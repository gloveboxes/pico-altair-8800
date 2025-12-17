cmake_minimum_required(VERSION 3.13)

# 💡 Select board BEFORE importing SDK (safer)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico2_w" CACHE STRING "Target Pico board (e.g., pico, pico_w, pico2, pico2_w, pimoroni_pico_plus2_w_rp2350)" FORCE)
endif()

# Wi-Fi credentials are configured at runtime via flash storage
# Use serial console to configure credentials on boot
# PICO_CYW43_SUPPORTED is automatically set by the SDK based on board type

# Import Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(altair C CXX ASM)
pico_sdk_init()

# Generate build version header
set(VERSION_FILE ${CMAKE_CURRENT_LIST_DIR}/build_version.txt)
set(TEMPLATE_FILE ${CMAKE_CURRENT_LIST_DIR}/build_version.h.in)
set(OUTPUT_FILE ${CMAKE_CURRENT_BINARY_DIR}/build_version.h)

add_custom_target(generate_version
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${VERSION_FILE}
        -DTEMPLATE_FILE=${TEMPLATE_FILE}
        -DOUTPUT_FILE=${OUTPUT_FILE}
        $<$<BOOL:${SKIP_VERSION_INCREMENT}>:-DSKIP_VERSION_INCREMENT=1>
        -P ${CMAKE_CURRENT_LIST_DIR}/generate_version.cmake
    BYPRODUCTS ${OUTPUT_FILE}
    COMMENT "Generating build version header"
    VERBATIM
)

add_library(lwipopts_provider INTERFACE)
target_include_directories(lwipopts_provider INTERFACE ${CMAKE_CURRENT_LIST_DIR})

set(ALTAIR_SOURCES
    main.c
    cpu_state.c
    Altair8800/intel8080.c
    Altair8800/memory.c
    Altair8800/pico_disk.c
    io_ports.c
    PortDrivers/time_io.c
    PortDrivers/utility_io.c
    websocket_console.c
    wifi_config.c
    comms_mgr.c
)

set(ALTAIR_LIBS)

if(PICO_CYW43_SUPPORTED)
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

# Ensure build version is generated before compiling
add_dependencies(altair generate_version)

# Make sure all CYW43 headers are visible and include Altair8800 directory
target_include_directories(altair PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Altair8800
    ${CMAKE_CURRENT_LIST_DIR}/PortDrivers
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(PICO_CYW43_SUPPORTED)
    target_include_directories(altair PRIVATE
        ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_arch/include
        ${PICO_SDK_PATH}/src/rp2_common/pico_cyw43_driver/include
        ${PICO_SDK_PATH}/lib/cyw43-driver/src        # 👈 this one is for cyw43.h
    )
endif()

# USB stdio on, UART stdio off
pico_enable_stdio_usb(altair 1)
pico_enable_stdio_uart(altair 0)

# Link stdlib + CYW43 (for onboard LED) + hardware_flash for WiFi config storage
target_link_libraries(altair
    pico_stdlib
    pico_multicore
    pico_rand
    hardware_flash
    hardware_sync
    ${ALTAIR_LIBS}
)

if(PICO_CYW43_SUPPORTED)
    target_link_libraries(altair pico_cyw43_arch_lwip_poll)
endif()

# Generate .uf2, .elf, etc.
pico_add_extra_outputs(altair)