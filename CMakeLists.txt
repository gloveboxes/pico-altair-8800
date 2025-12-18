cmake_minimum_required(VERSION 3.13)

# 💡 Select board BEFORE importing SDK (safer)
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD "pico2_w" CACHE STRING "Target Pico board (e.g., pico, pico_w, pico2, pico2_w, pimoroni_pico_plus2_w_rp2350)" FORCE)
endif()

# Wi-Fi credentials are configured at runtime via flash storage
# Use serial console to configure credentials on boot
# PICO_CYW43_SUPPORTED is automatically set by the SDK based on board type

# Debug option (off by default)
option(ALTAIR_DEBUG "Enable debug logging" OFF)

# Pico Inky support (off by default)
option(INKY_SUPPORT "Enable Pico Inky E-Ink display support" OFF)

# Pico Display 2.8" support (off by default)
option(DISPLAY_2_8_SUPPORT "Enable Pico Display 2.8 inch LCD support" OFF)

# Ensure only one display is enabled at a time
if(INKY_SUPPORT AND DISPLAY_2_8_SUPPORT)
    message(FATAL_ERROR "Cannot enable both INKY_SUPPORT and DISPLAY_2_8_SUPPORT at the same time. Please choose one display type.")
endif()

# Import Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(altair C CXX ASM)
pico_sdk_init()

# Import Pimoroni Pico libraries if Inky or Display 2.8 support is enabled
if(INKY_SUPPORT OR DISPLAY_2_8_SUPPORT)
    set(PIMORONI_PICO_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pimoroni-pico)
    include(${CMAKE_CURRENT_LIST_DIR}/lib/pimoroni-pico/pimoroni_pico_import.cmake)
    # Only build the drivers and libraries we need, not examples
    add_subdirectory(${PIMORONI_PICO_PATH}/common pimoroni-pico/common)
    add_subdirectory(${PIMORONI_PICO_PATH}/drivers pimoroni-pico/drivers)
    add_subdirectory(${PIMORONI_PICO_PATH}/libraries pimoroni-pico/libraries)
endif()

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
    FrontPanels/virtual_monitor.c
    FrontPanels/inky_display.cpp
    FrontPanels/display_2_8.cpp
    i8080_disasm.c
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

# Add debug definition if enabled
if(ALTAIR_DEBUG)
    target_compile_definitions(altair PRIVATE ALTAIR_DEBUG=1)
endif()

# Add Inky support definition if enabled
if(INKY_SUPPORT)
    target_compile_definitions(altair PRIVATE INKY_SUPPORT=1)
endif()

# Add Display 2.8 support definition if enabled
if(DISPLAY_2_8_SUPPORT)
    target_compile_definitions(altair PRIVATE DISPLAY_2_8_SUPPORT=1)
endif()

# Make sure all CYW43 headers are visible and include Altair8800 directory
target_include_directories(altair PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Altair8800
    ${CMAKE_CURRENT_LIST_DIR}/PortDrivers
    ${CMAKE_CURRENT_LIST_DIR}/FrontPanels
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

# Link Pimoroni Inky libraries if enabled
if(INKY_SUPPORT)
    target_link_libraries(altair
        uc8151          # Inky display driver
        pico_graphics   # Graphics library
        button          # Button support
    )
endif()

# Link Pimoroni Display 2.8 libraries if enabled
if(DISPLAY_2_8_SUPPORT)
    target_link_libraries(altair
        st7789          # ST7789 LCD driver
        pico_graphics   # Graphics library
        pico_display_28 # Pico Display 2.8 board definitions
        rgbled          # RGB LED driver
    )
endif()

# Generate .uf2, .elf, etc.
pico_add_extra_outputs(altair)