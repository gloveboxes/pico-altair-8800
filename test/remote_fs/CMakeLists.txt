cmake_minimum_required(VERSION 3.13)

# This is a standalone test for the Remote FS module
# Build with: cmake .. -DPICO_BOARD=pico2_w && make

# 💡 Select board BEFORE importing SDK
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico2_w" CACHE STRING "Target Pico board" FORCE)
endif()

# Import Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(remote_fs_test C CXX ASM)
pico_sdk_init()

# Remote FS server configuration
set(RFS_SERVER_IP "192.168.1.22" CACHE STRING "Remote FS server IP address")
set(RFS_SERVER_PORT "8080" CACHE STRING "Remote FS server port")

# lwipopts.h location
add_library(lwipopts_provider INTERFACE)
target_include_directories(lwipopts_provider INTERFACE ${CMAKE_CURRENT_LIST_DIR}/../..)

# Test sources
add_executable(remote_fs_test
    main.c
    ../../Altair8800/remote_fs.c
)

target_include_directories(remote_fs_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../..
    ${CMAKE_CURRENT_LIST_DIR}/../../Altair8800
)

target_compile_definitions(remote_fs_test PRIVATE
    REMOTE_FS_SUPPORT=1
    RFS_SERVER_IP="${RFS_SERVER_IP}"
    RFS_SERVER_PORT=${RFS_SERVER_PORT}
)

# USB stdio on, UART stdio off
pico_enable_stdio_usb(remote_fs_test 1)
pico_enable_stdio_uart(remote_fs_test 0)

target_link_libraries(remote_fs_test
    pico_stdlib
    pico_cyw43_arch_lwip_poll
    pico_multicore
)

pico_add_extra_outputs(remote_fs_test)
