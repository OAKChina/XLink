/**
 * @file    tcpip_host.c
 * @brief   TCP/IP helper definitions
*/

/* **************************************************************************/
/*      Include Files                                                       */
/* **************************************************************************/
#include "tcpip_host.h"

#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../PlatformDeviceFd.h"
#include "XLinkLog.h"

#if (defined(_WIN32) || defined(_WIN64))
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  /* Windows 7. */
#endif
#include <winsock2.h>
#include <Ws2tcpip.h>
#include <Iphlpapi.h>
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <net/if.h>
#include <netdb.h>
#include <ifaddrs.h>
#endif

/* **************************************************************************/
/*      Private Macro Definitions                                            */
/* **************************************************************************/

// Debug, uncomment first line for some printing
//#define HAS_DEBUG
#undef HAS_DEBUG

#define BROADCAST_UDP_PORT                  11491

#define MAX_IFACE_CHAR                      64
#define MAX_DEVICE_DISCOVERY_IFACE          10

#define MSEC_TO_USEC(x)                     (x * 1000)
#define DEVICE_DISCOVERY_RES_TIMEOUT_SEC    0.2
#define DEVICE_RES_TIMEOUT_MSEC             20

#ifdef HAS_DEBUG
#define DEBUG(...) do { printf(__VA_ARGS__); } while(0)
#else
#define DEBUG(fmt, ...) do {} while(0)
#endif


/* **************************************************************************/
/*      Private Function Definitions                                        */
/* **************************************************************************/
#if (defined(_WIN32) || defined(_WIN64) )
#include <win_time.h>
#endif
static inline double seconds()
{
    static double s;
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    if(!s)
        s = ts.tv_sec + ts.tv_nsec * 1e-9;
    return ts.tv_sec + ts.tv_nsec * 1e-9 - s;
}

static XLinkDeviceState_t tcpip_convert_device_state(uint32_t state)
{
    if(state == TCPIP_HOST_STATE_BOOTED)
    {
        return X_LINK_BOOTED;
    }
    else if(state == TCPIP_HOST_STATE_BOOTLOADER)
    {
        return X_LINK_BOOTLOADER;
    }
    else if(state == TCPIP_HOST_STATE_FLASH_BOOTED)
    {
        return X_LINK_FLASH_BOOTED;
    }
    else
    {
        return X_LINK_ANY_STATE;
    }
}


static tcpipHostError_t tcpip_create_socket(TCPIP_SOCKET* out_sock, bool broadcast, int timeout_ms)
{
    TCPIP_SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
 #if (defined(_WIN32) || defined(_WIN64) )
    if(sock == INVALID_SOCKET)
    {
        return TCPIP_HOST_ERROR;
    }
 #else
    if(sock < 0)
    {
        return TCPIP_HOST_ERROR;
    }
#endif

    // add socket option for broadcast
    int rc = 1;
    if(broadcast)
    {
        if(setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char *) &rc, sizeof(rc)) < 0)
        {
            return TCPIP_HOST_ERROR;
        }
    }

#if (defined(_WIN32) || defined(_WIN64) )
#else
    int reuse_addr = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(int)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }
#endif

    // Specify timeout
#if (defined(_WIN32) || defined(_WIN64) )
    int read_timeout = timeout_ms;
#else
    struct timeval read_timeout;
    read_timeout.tv_sec = 0;
    read_timeout.tv_usec = MSEC_TO_USEC(timeout_ms);
#endif
    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *) &read_timeout, sizeof(read_timeout)) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    *out_sock = sock;
    return TCPIP_HOST_SUCCESS;
}

static tcpipHostError_t tcpip_create_socket_broadcast(TCPIP_SOCKET* out_sock)
{
    return tcpip_create_socket(out_sock, true, DEVICE_RES_TIMEOUT_MSEC);
}



static tcpipHostError_t tcpip_send_broadcast(TCPIP_SOCKET sock){

#if (defined(_WIN32) || defined(_WIN64) )

    DWORD rv, size = 0;
    PMIB_IPADDRTABLE ipaddrtable;

    rv = GetIpAddrTable(NULL, &size, 0);
    if (rv != ERROR_INSUFFICIENT_BUFFER) {
        return TCPIP_HOST_ERROR;
    }
    ipaddrtable = (PMIB_IPADDRTABLE) malloc(size);
    if (!ipaddrtable)
        return TCPIP_HOST_ERROR;

    rv = GetIpAddrTable(ipaddrtable, &size, 0);
    if (rv != NO_ERROR) {
        free(ipaddrtable);
        return TCPIP_HOST_ERROR;
    }

    tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
    struct sockaddr_in broadcast = { 0 };
    broadcast.sin_addr.s_addr = INADDR_BROADCAST;
    broadcast.sin_family = AF_INET;
    broadcast.sin_port = htons(BROADCAST_UDP_PORT);
    for (DWORD i = 0; i < ipaddrtable->dwNumEntries; ++i) {

        // Get broadcast IP
        MIB_IPADDRROW addr = ipaddrtable->table[i];
        broadcast.sin_addr.s_addr = (addr.dwAddr & addr.dwMask)
            | (addr.dwMask ^ (DWORD)0xffffffff);
        sendto(sock, (const char *) &send_buffer, sizeof(send_buffer), 0, (struct sockaddr*) & broadcast, sizeof(broadcast));

#ifdef HAS_DEBUG
        char ip_broadcast_str[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &((struct sockaddr_in*) & broadcast)->sin_addr, ip_broadcast_str, sizeof(ip_broadcast_str));
        DEBUG("Interface up and running. Broadcast IP: %s\n", ip_broadcast_str);
#endif

    }

    free(ipaddrtable);

    return TCPIP_HOST_SUCCESS;

#else

    struct ifaddrs *ifaddr = NULL;
    if(getifaddrs(&ifaddr) < 0)
    {
        return TCPIP_HOST_ERROR;
    }

    // iterate linked list of interface information
    int socket_count = 0;
    for(struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
    {
        // check for ipv4 family, and assign only AFTER ifa_addr != NULL
        sa_family_t family;
        if(ifa->ifa_addr != NULL && ((family = ifa->ifa_addr->sa_family) == AF_INET))
        {
            // Check if interface is up and running
            struct ifreq if_req;
            strncpy(if_req.ifr_name, ifa->ifa_name, sizeof(if_req.ifr_name));
            ioctl(sock, SIOCGIFFLAGS, &if_req);

            DEBUG("interface name %s, (flags: %hu). ", ifa->ifa_name, if_req.ifr_flags);

            if((if_req.ifr_flags & IFF_UP) && (if_req.ifr_flags & IFF_RUNNING)){
                // Interface is up and running
                // Calculate broadcast address (IPv4, OR negated mask)
                struct sockaddr_in ip_broadcast = *((struct sockaddr_in*) ifa->ifa_addr);
                struct sockaddr_in ip_netmask = *((struct sockaddr_in*) ifa->ifa_netmask);
                ip_broadcast.sin_addr.s_addr |= ~ip_netmask.sin_addr.s_addr;

                #ifdef HAS_DEBUG
                    char ip_broadcast_str[INET_ADDRSTRLEN] = {0};
                    inet_ntop(family, &((struct sockaddr_in *)&ip_broadcast)->sin_addr, ip_broadcast_str, sizeof(ip_broadcast_str));
                    DEBUG("Up and running. Broadcast IP: %s", ip_broadcast_str);
                #endif

                // send broadcast message
                struct sockaddr_in broadcast_addr;
                broadcast_addr.sin_family = family;
                broadcast_addr.sin_addr.s_addr = ip_broadcast.sin_addr.s_addr;
                broadcast_addr.sin_port = htons(BROADCAST_UDP_PORT);

                tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;
                if(sendto(sock, &send_buffer, sizeof(send_buffer), 0, (struct sockaddr *) &broadcast_addr, sizeof(broadcast_addr)) < 0)
                {
                    // Ignore if not successful. The devices on that interface won't be found
                }
            } else {
                DEBUG("Not up and running.");
            }

            DEBUG("\n");

        }
    }
    // Release interface addresses
    freeifaddrs(ifaddr);

    return TCPIP_HOST_SUCCESS;
#endif
}


/* **************************************************************************/
/*      Public Function Definitions                                         */
/* **************************************************************************/
tcpipHostError_t tcpip_close_socket(TCPIP_SOCKET sock)
{
#if (defined(_WIN32) || defined(_WIN64) )
    if(sock != INVALID_SOCKET)
    {
        closesocket(sock);
        return TCPIP_HOST_SUCCESS;
    }
#else
    if(sock != -1)
    {
        close(sock);
        return TCPIP_HOST_SUCCESS;
    }
#endif
    return TCPIP_HOST_ERROR;
}

xLinkPlatformErrorCode_t tcpip_get_devices(const deviceDesc_t in_deviceRequirements, deviceDesc_t* devices, size_t devices_size, unsigned int* device_count)
{
    // Name signifies ip in TCP_IP protocol case
    const char* target_ip = in_deviceRequirements.name;
    XLinkDeviceState_t target_state = in_deviceRequirements.state;
    const char* target_mxid = in_deviceRequirements.mxid;

    // Socket
    TCPIP_SOCKET sock;

    bool check_target_ip = false;
    if(target_ip != NULL && strlen(target_ip) > 0){
        check_target_ip = true;
    }
    bool check_target_mxid = false;
    if(target_mxid != NULL && strlen(target_mxid) > 0){
        check_target_mxid = true;
    }

    // If IP is specified, do UNICAST
    if(check_target_ip) {

        // Create socket for UDP unicast
        if(tcpip_create_socket(&sock, false, 100) != TCPIP_HOST_SUCCESS){
            return X_LINK_PLATFORM_ERROR;
        }

        // TODO(themarpe) - Add IPv6 capabilities
        // send unicast device discovery
        struct sockaddr_in device_address;
        device_address.sin_family = AF_INET;
        device_address.sin_port = htons(BROADCAST_UDP_PORT);


        // Convert address to binary
        #if (defined(_WIN32) || defined(__USE_W32_SOCKETS)) && (_WIN32_WINNT <= 0x0501)
            device_address.sin_addr.s_addr = inet_addr(target_ip);  // for XP
        #else
            inet_pton(AF_INET, target_ip, &device_address.sin_addr.s_addr);
        #endif

        tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_DEVICE_DISCOVER;

        if(sendto(sock, &send_buffer, sizeof(send_buffer), 0, (struct sockaddr *) &device_address, sizeof(device_address)) < 0)
        {
            tcpip_close_socket(sock);
            return X_LINK_PLATFORM_ERROR;
        }

    } else {
        // do a broadcast search

        // Create ANY receiving socket first
        if(tcpip_create_socket_broadcast(&sock) != TCPIP_HOST_SUCCESS){
            return X_LINK_PLATFORM_ERROR;
        }

        // Then send broadcast
        if (tcpip_send_broadcast(sock) != TCPIP_HOST_SUCCESS) {
            tcpip_close_socket(sock);
            return X_LINK_PLATFORM_ERROR;
        }

    }


    // loop to receive message response from devices
    unsigned num_devices_match = 0;
    // Loop through all sockets and received messages that arrived
    double t1 = seconds();
    do {
        if(num_devices_match >= devices_size){
            // Enough devices matched, exit the loop
            break;
        }

        char ip_addr[INET_ADDRSTRLEN] = {0};
        tcpipHostDeviceDiscoveryResp_t recv_buffer = {0};
        struct sockaddr_in dev_addr;
        uint32_t len = sizeof(dev_addr);

        int ret = recvfrom(sock, (char *) &recv_buffer, sizeof(recv_buffer), 0, (struct sockaddr*) & dev_addr, &len);
        if(ret > 0)
        {
            DEBUG("Received UDP response, length: %d\n", ret);
            XLinkDeviceState_t foundState = tcpip_convert_device_state(recv_buffer.state);
            if(recv_buffer.command == TCPIP_HOST_CMD_DEVICE_DISCOVER && (target_state == X_LINK_ANY_STATE || target_state == foundState))
            {
                // Correct device found, increase matched num and save details

                // convert IP address in binary into string
                inet_ntop(AF_INET, &dev_addr.sin_addr, ip_addr, sizeof(ip_addr));
                // if(state == X_LINK_BOOTED){
                //     strncat(ip_addr, ":11492", 16);
                // }

                // Check IP if needed
                if(check_target_ip && strcmp(target_ip, ip_addr) != 0){
                    // IP doesn't match, skip this device
                    continue;
                }
                // Check MXID if needed
                if(check_target_mxid && strcmp(target_mxid, recv_buffer.mxid)){
                    // MXID doesn't match, skip this device
                    continue;
                }

                // copy device information
                // Status
                devices[num_devices_match].status = X_LINK_SUCCESS;
                // IP
                memset(devices[num_devices_match].name, 0, sizeof(devices[num_devices_match].name));
                strncpy(devices[num_devices_match].name, ip_addr, sizeof(devices[num_devices_match].name));
                // MXID
                memset(devices[num_devices_match].mxid, 0, sizeof(devices[num_devices_match].mxid));
                strncpy(devices[num_devices_match].mxid, recv_buffer.mxid, sizeof(devices[num_devices_match].mxid));
                // Platform
                devices[num_devices_match].platform = X_LINK_MYRIAD_X;
                // Protocol
                devices[num_devices_match].protocol = X_LINK_TCP_IP;
                // State
                devices[num_devices_match].state = foundState;

                num_devices_match++;
            }
        }
    } while(seconds() - t1 < DEVICE_DISCOVERY_RES_TIMEOUT_SEC);

    tcpip_close_socket(sock);

    // Filter out duplicates - routing table will decide through which interface the packets will traverse
    // TODO(themarpe) - properly separate interfaces.
    // Either bind to interface addr, or SO_BINDTODEVICE Linux, IP_BOUND_IF macOS, and prefix interface name
    int write_index = 0;
    for(int i = 0; i < (int) num_devices_match; i++){
        bool duplicate = false;
        for(int j = i - 1; j >= 0; j--){
            // Check if duplicate
            if(strcmp(devices[i].name, devices[j].name) == 0 && strcmp(devices[i].mxid, devices[j].mxid) == 0){
                duplicate = true;
                break;
            }
        }
        if(!duplicate){
            devices[write_index] = devices[i];
            write_index++;
        }
    }
    // return total device found
    *device_count = write_index;

    // Return success if search was successful (even if no devices found)
    return X_LINK_PLATFORM_SUCCESS;
}


xLinkPlatformErrorCode_t tcpip_boot_bootloader(const char* name){
    if(name == NULL || name[0] == 0){
        return X_LINK_PLATFORM_ERROR;
    }

    // Create socket for UDP unicast
    TCPIP_SOCKET sock;
    if(tcpip_create_socket(&sock, false, 100) != TCPIP_HOST_SUCCESS){
        return X_LINK_PLATFORM_ERROR;
    }

    // TODO(themarpe) - Add IPv6 capabilities
    // send unicast reboot to bootloader
    struct sockaddr_in device_address;
    device_address.sin_family = AF_INET;
    device_address.sin_port = htons(BROADCAST_UDP_PORT);

    // Convert address to binary
    #if (defined(_WIN32) || defined(__USE_W32_SOCKETS)) && (_WIN32_WINNT <= 0x0501)
        device_address.sin_addr.s_addr = inet_addr(name);  // for XP
    #else
        inet_pton(AF_INET, name, &device_address.sin_addr.s_addr);
    #endif

    tcpipHostCommand_t send_buffer = TCPIP_HOST_CMD_RESET;
    if (sendto(sock, (const char *)&send_buffer, sizeof(send_buffer), 0, (struct sockaddr *)&device_address, sizeof(device_address)) < 0)
    {
        return X_LINK_PLATFORM_ERROR;
    }

    tcpip_close_socket(sock);

    return X_LINK_PLATFORM_SUCCESS;
}


int tcpipPlatformClose(void *fdKey)
{
#if defined(USE_TCP_IP)

    int status = 0;

    void* tmpsockfd = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpsockfd)){
        mvLog(MVLOG_FATAL, "Cannot find file descriptor by key");
        return -1;
    }
    TCPIP_SOCKET sock = (TCPIP_SOCKET) (uintptr_t) tmpsockfd;

#ifdef _WIN32
    status = shutdown(sock, SD_BOTH);
    if (status == 0) { status = closesocket(sock); }
#else
    if(sock != -1)
    {
        status = shutdown(sock, SHUT_RDWR);
        if (status == 0) { status = close(sock); }
    }
#endif

    if(destroyPlatformDeviceFdKey(fdKey)){
        mvLog(MVLOG_FATAL, "Cannot destory file descriptor key");
        return -1;
    }

    return status;

#endif
    return -1;
}

int tcpipPlatformRead(void *fdKey, void *data, int size)
{
#if defined(USE_TCP_IP)
    int nread = 0;

    void* tmpsockfd = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpsockfd)){
        mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
        return -1;
    }
    TCPIP_SOCKET sock = (TCPIP_SOCKET) (uintptr_t) tmpsockfd;

    while(nread < size)
    {
        int rc = recv(sock, &((char*)data)[nread], size - nread, 0);
        if(rc <= 0)
        {
            return -1;
        }
        else
        {
            nread += rc;
        }
    }
#endif
    return 0;
}

int tcpipPlatformWrite(void *fdKey, void *data, int size)
{
#if defined(USE_TCP_IP)
    int byteCount = 0;

    void* tmpsockfd = NULL;
    if(getPlatformDeviceFdFromKey(fdKey, &tmpsockfd)){
        mvLog(MVLOG_FATAL, "Cannot find file descriptor by key: %" PRIxPTR, (uintptr_t) fdKey);
        return -1;
    }
    TCPIP_SOCKET sock = (TCPIP_SOCKET) (uintptr_t) tmpsockfd;

    while(byteCount < size)
    {
        // Use send instead of write and ignore SIGPIPE
        //rc = write((intptr_t)fd, &((char*)data)[byteCount], size - byteCount);

        int flags = 0;
        #if defined(MSG_NOSIGNAL)
            // Use flag NOSIGNAL on send call
            flags = MSG_NOSIGNAL;
        #endif

        int rc = send(sock, &((char*)data)[byteCount], size - byteCount, flags);
        if(rc <= 0)
        {
            return -1;
        }
        else
        {
            byteCount += rc;
        }
    }
#endif
    return 0;
}



// TODO add IPv6 to tcpipPlatformConnect()
int tcpipPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd)
{
#if defined(USE_TCP_IP)
    if (!devPathWrite || !fd)
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    TCPIP_SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock < 0)
    {
        tcpip_close_socket(sock);
        return -1;
    }

    // Disable sigpipe reception on send
    #if defined(SO_NOSIGPIPE)
        const int set = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof(set));
    #endif

    struct sockaddr_in serv_addr = { 0 };

    size_t len = strlen(devPathWrite);
    if (!len)
        return X_LINK_PLATFORM_INVALID_PARAMETERS;
    char *const devPathWriteBuff = (char *)malloc(len + 1);
    if (!devPathWriteBuff)
        return X_LINK_PLATFORM_ERROR;
    strncpy(devPathWriteBuff, devPathWrite, len);
    devPathWriteBuff[len] = 0;

    char* serv_ip = strtok(devPathWriteBuff, ":");
    char* serv_port = strtok(NULL, ":");

    // Parse port, or use default
    uint16_t port = TCPIP_LINK_SOCKET_PORT;
    if(serv_port != NULL){
        port = atoi(serv_port);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    int ret = inet_pton(AF_INET, serv_ip, &serv_addr.sin_addr);
    free(devPathWriteBuff);

    if(ret <= 0)
    {
        tcpip_close_socket(sock);
        return -1;
    }

    if(connect(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
    {
        tcpip_close_socket(sock);
        return -1;
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) sock);

#endif
    return 0;
}

int tcpipPlatformBootBootloader(const char *name)
{
    return tcpip_boot_bootloader(name);
}

int tcpipPlatformBootFirmware(const deviceDesc_t* deviceDesc, const char* firmware, size_t length){
    // TCPIP doesn't support a boot mechanism
    return -1;
}


int tcpipPlatformInitialize() {
    int ret = 0;

#if (defined(_WIN32) || defined(_WIN64)) && defined(USE_TCP_IP)
    WSADATA wsa_data;
    ret = WSAStartup(MAKEWORD(2,2), &wsa_data);
#endif

    return ret;
}