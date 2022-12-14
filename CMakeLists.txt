# Copyright (C) 2018-2021 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
#
cmake_minimum_required(VERSION 3.2)
set(TARGET_NAME "XLink")
project(${TARGET_NAME} LANGUAGES C CXX)

include(cmake/XLink.cmake)
include(cmake/Flags.cmake)

# Add option - MX ID naming instead of usb port
option(XLINK_USE_MX_ID_NAME "Use MX ID name instead of usb port" ON)
# Build examples
option(XLINK_BUILD_EXAMPLES "Build XLink examples" OFF)
# Build tests
option(XLINK_BUILD_TESTS "Build XLink tests" OFF)

add_library(${TARGET_NAME} ${XLINK_SOURCES})
add_flag_source(src/shared/XLinkDevice.c "-Werror=switch-enum")

if(WIN32)
    target_compile_definitions(${TARGET_NAME} PRIVATE WIN32_LEAN_AND_MEAN)
else()
    target_include_directories(${TARGET_NAME}
        PRIVATE "${LIBUSB_INCLUDE_DIR}"
    )

    target_link_libraries(${TARGET_NAME}
        PRIVATE Threads::Threads
        PUBLIC ${LIBUSB_LIBRARY}
    )
endif()

if(MINGW)
    target_link_libraries(${TARGET_NAME}
        PUBLIC
            libwinusb.a
            libsetupapi.a
            ws2_32
            Iphlpapi.lib
    )
endif()

target_include_directories(${TARGET_NAME}
    PUBLIC
        $<INSTALL_INTERFACE:include>
        "$<BUILD_INTERFACE:${XLINK_INCLUDE}>"
    PRIVATE
        ${XLINK_PLATFORM_INCLUDE}
        ${XLINK_PRIVATE_INCLUDE}
        ${XLINK_INCLUDE}/XLink
    )

target_compile_definitions(${TARGET_NAME}
    PRIVATE
        HAVE_STRUCT_TIMESPEC
        _CRT_SECURE_NO_WARNINGS
        USE_USB_VSC
        USE_TCP_IP
    PUBLIC
        __PC__
        XLINK_USE_MX_ID_NAME=${XLINK_USE_MX_ID_NAME}
)

if (ENABLE_MYRIAD_NO_BOOT)
    target_compile_definitions(${TARGET_NAME}
        PRIVATE
        NO_BOOT)
endif()

# Set C99 standard
set_property(TARGET ${TARGET_NAME} PROPERTY C_STANDARD 99)
# Set compiler features (c++11), and disables extensions (g++11)
set_property(TARGET ${TARGET_NAME} PROPERTY CXX_STANDARD 11)
set_property(TARGET ${TARGET_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${TARGET_NAME} PROPERTY CXX_EXTENSIONS OFF)
# Add interface transitive property (C++11)
if(${CMAKE_VERSION} VERSION_LESS "3.8.0")
    target_compile_features(${TARGET_NAME} INTERFACE cxx_range_for)
else()
    target_compile_features(${TARGET_NAME} INTERFACE cxx_std_11)
endif()

# Check if pthread_getname_np exists
if(NOT WIN32)
    include(CheckSymbolExists)
    set(_TMP_CMAKE_REQUIRED_DEFINITIONS "${CMAKE_REQUIRED_DEFINITIONS}")
    set(_TMP_CMAKE_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
    set(_TMP_CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
    list(APPEND CMAKE_REQUIRED_LIBRARIES "-lpthread")
    # This flag breaks check_symbol_exists()
    string(REPLACE " -Werror " " " CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")

    # perfom check for pthread_getname_np symbol
    check_symbol_exists(pthread_getname_np pthread.h HAVE_PTHREAD_GETNAME_NP)

    set(CMAKE_REQUIRED_DEFINITIONS "${_TMP_CMAKE_REQUIRED_DEFINITIONS}")
    set(CMAKE_REQUIRED_LIBRARIES "${_TMP_CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_C_FLAGS "${_TMP_CMAKE_C_FLAGS}")
    unset(_TMP_CMAKE_REQUIRED_DEFINITIONS)
    unset(_TMP_CMAKE_REQUIRED_LIBRARIES)
    unset(_TMP_CMAKE_C_FLAGS)

    # Set the define
    if(HAVE_PTHREAD_GETNAME_NP)
        target_compile_definitions(${TARGET_NAME} PRIVATE HAVE_PTHREAD_GETNAME_NP)
    endif()
endif()

# Examples
if(XLINK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Tests
if(XLINK_BUILD_TESTS)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS ${TARGET_NAME}
    EXPORT "${TARGET_NAME}Config"
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

set(config_install_dir "${CMAKE_INSTALL_LIBDIR}/cmake/${TARGET_NAME}")
export(TARGETS
        ${TARGET_NAME}
    FILE "${config_install_dir}/${TARGET_NAME}Config.cmake"
)
install(EXPORT
    "${TARGET_NAME}Config"
    DESTINATION "${config_install_dir}"
)
