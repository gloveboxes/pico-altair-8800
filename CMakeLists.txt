cmake_minimum_required(VERSION 3.13)

# Wi-Fi credentials
set(WIFI_SSID "" CACHE STRING "Wi-Fi network SSID for CYW43 station mode")
set(WIFI_PASSWORD "" CACHE STRING "Wi-Fi network password for CYW43 station mode")

option(ALTAIR_ENABLE_WEBSOCKET "Enable WebSocket console and server support" ON)

# Propagate the option to every translation unit so IntelliSense sees it too
add_compile_definitions(ALTAIR_ENABLE_WEBSOCKET=$<BOOL:${ALTAIR_ENABLE_WEBSOCKET}>)

# 💡 Select board BEFORE importing SDK (safer)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico2_w" CACHE STRING "Target Pico board (e.g., pico2, pico2_w)" FORCE)
endif()


# Use custom mbedtls config from project root
set(PICO_MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/mbedtls_config.h" CACHE FILEPATH "Custom mbedTLS config")

# Import Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(altair C CXX ASM)
pico_sdk_init()

add_library(lwipopts_provider INTERFACE)
target_include_directories(lwipopts_provider INTERFACE ${CMAKE_CURRENT_LIST_DIR})

set(ALTAIR_SOURCES
    main.c
    Altair8800/intel8080.c
    Altair8800/memory.c
    Altair8800/pico_disk.c
    io_ports.c
    PortDrivers/time_io.c
    PortDrivers/utility_io.c
    websocket_console.c
)

set(ALTAIR_LIBS)

if(ALTAIR_ENABLE_WEBSOCKET)
    set(PICO_WS_SERVER_DIR ${CMAKE_CURRENT_LIST_DIR}/lib/pico-ws-server)

    add_library(pico_ws_server
        ${PICO_WS_SERVER_DIR}/src/client_connection.cpp
        ${PICO_WS_SERVER_DIR}/src/http_handler.cpp
        ${PICO_WS_SERVER_DIR}/src/web_socket_frame_builder.cpp
        ${PICO_WS_SERVER_DIR}/src/web_socket_handler.cpp
        ${PICO_WS_SERVER_DIR}/src/web_socket_message_builder.cpp
        ${PICO_WS_SERVER_DIR}/src/web_socket_server.cpp
        ${PICO_WS_SERVER_DIR}/src/web_socket_server_internal.cpp
    )

    target_include_directories(pico_ws_server BEFORE
        PUBLIC ${PICO_WS_SERVER_DIR}/include
        PUBLIC ${CMAKE_CURRENT_LIST_DIR}
        PRIVATE ${PICO_WS_SERVER_DIR}/src
    )

    target_compile_definitions(pico_ws_server PUBLIC DEBUG_PRINT=0)

    # Add compatibility header for mbedtls API changes
    target_compile_options(pico_ws_server PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/mbedtls_compat.h>
    )

    target_link_libraries(pico_ws_server
        PUBLIC
            pico_stdlib
            pico_cyw43_driver
            pico_lwip_nosys
            pico_mbedtls
            lwipopts_provider
    )

    list(APPEND ALTAIR_SOURCES wifi.c ws.cpp)
    list(APPEND ALTAIR_LIBS pico_ws_server)
endif()

add_executable(altair ${ALTAIR_SOURCES})

# Make sure all CYW43 headers are visible and include Altair8800 directory
target_include_directories(altair PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Altair8800
    ${CMAKE_CURRENT_LIST_DIR}/PortDrivers
)

if(ALTAIR_ENABLE_WEBSOCKET)
    target_include_directories(altair PRIVATE
        ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_arch/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_driver/include
        ${PICO_SDK_PATH}/lib/cyw43-driver/src        # 👈 this one is for cyw43.h
    )
endif()

target_compile_definitions(altair PRIVATE
    WIFI_SSID=\"${WIFI_SSID}\"
    WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
    ALTAIR_ENABLE_WEBSOCKET=$<BOOL:${ALTAIR_ENABLE_WEBSOCKET}>
)

# USB stdio on, UART stdio off
pico_enable_stdio_usb(altair 1)
pico_enable_stdio_uart(altair 0)

# Link stdlib + CYW43 (for onboard LED)
target_link_libraries(altair
    pico_stdlib
    pico_multicore
    ${ALTAIR_LIBS}
)

if(ALTAIR_ENABLE_WEBSOCKET)
    target_link_libraries(altair pico_cyw43_arch_lwip_poll)
endif()

# Generate .uf2, .elf, etc.
pico_add_extra_outputs(altair)