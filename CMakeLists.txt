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
# Pico Display 2.8" support (off by default)
option(DISPLAY_2_8_SUPPORT "Enable Pico Display 2.8 inch LCD support" OFF)

# SD Card support (on by default)
option(SD_CARD_SUPPORT "Enable SD Card support" OFF)

# Waveshare 3.5" display support (off by default)
# This display uses spi1 with different pins than Pimoroni displays
option(WAVESHARE_3_5_DISPLAY "Enable Waveshare 3.5 inch LCD support (uses spi1)" OFF)
option(WAVESHARE_2_DISPLAY "Enable Waveshare 2.0 inch display" OFF)
option(DISPLAY_ST7789_SUPPORT "Enable ST7789 LCD support" OFF)
option(REMOTE_FS_SUPPORT "Enable Remote FS support" OFF)
option(BLUETOOTH_KEYBOARD_SUPPORT "Enable Bluetooth LE keyboard support" OFF)
option(VT100_DISPLAY "Enable VT100 terminal on Waveshare 3.5 display (replaces front panel)" OFF)

# Ensure only one display is enabled at a time
if(INKY_SUPPORT AND DISPLAY_2_8_SUPPORT)
    message(FATAL_ERROR "Cannot enable both INKY_SUPPORT and DISPLAY_2_8_SUPPORT at the same time. Please choose one display type.")
endif()

# Warn about SD Card and Display 2.8 pin conflict (only when NOT using Waveshare)
# Waveshare 3.5" display shares SPI with SD card on spi1, so no conflict
if(SD_CARD_SUPPORT AND DISPLAY_2_8_SUPPORT AND NOT WAVESHARE_3_5_DISPLAY)
    message(WARNING "⚠️  SD_CARD_SUPPORT and DISPLAY_2_8_SUPPORT have a pin conflict!")
    message(WARNING "    SD Card MISO (GPIO 16) conflicts with Display 2.8 DC (GPIO 16)")
    message(WARNING "    This configuration will NOT work correctly.")
    message(WARNING "    Choose either SD card OR Display 2.8, not both.")
    message(FATAL_ERROR "Cannot enable both SD_CARD_SUPPORT and DISPLAY_2_8_SUPPORT due to pin conflict on GPIO 16.")
endif()

# Import Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(altair C CXX ASM)
pico_sdk_init()

# Import Pimoroni Pico libraries if Inky or Display 2.8 support is enabled
if(INKY_SUPPORT OR DISPLAY_2_8_SUPPORT OR SD_CARD_SUPPORT)
    set(PIMORONI_PICO_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pimoroni-pico)
    include(${CMAKE_CURRENT_LIST_DIR}/lib/pimoroni-pico/pimoroni_pico_import.cmake)
    
    # Add local sdcard/fatfs drivers BEFORE pimoroni-pico drivers
    # This ensures our local copies are used instead of the submodule versions
    # (the cmake files use "if (NOT TARGET ...)" guards so first definition wins)
    if(SD_CARD_SUPPORT)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs local-drivers/fatfs)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard local-drivers/sdcard)
    endif()
    
    # Only build the drivers and libraries we need, not examples
    add_subdirectory(${PIMORONI_PICO_PATH}/common pimoroni-pico/common)
    add_subdirectory(${PIMORONI_PICO_PATH}/drivers pimoroni-pico/drivers)
    add_subdirectory(${PIMORONI_PICO_PATH}/libraries pimoroni-pico/libraries)

    # Suppress old K&R-style function definition warnings from vendored zlib
    # sources in pngdec (GCC 15 enables -Wold-style-definition by default)
    if(TARGET pngdec)
        target_compile_options(pngdec PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-old-style-definition>)
    endif()
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
    ansi_input.c
    wifi.c
    cpu_state.c
    FrontPanels/virtual_monitor.c
    FrontPanels/inky_display.cpp
    i8080_disasm.c
    Altair8800/intel8080.c
    Altair8800/memory.c
    io_ports.c
    PortDrivers/time_io.c
    PortDrivers/utility_io.c
    PortDrivers/files_io.c
    PortDrivers/stats_io.c
    websocket_console.c
    config.c
    core1_io_mgr.c
)

if(BLUETOOTH_KEYBOARD_SUPPORT)
    if(NOT PICO_CYW43_SUPPORTED)
        message(FATAL_ERROR "BLUETOOTH_KEYBOARD_SUPPORT requires a CYW43-based board such as pico_w or pico2_w.")
    endif()

    list(APPEND ALTAIR_SOURCES
        drivers/bluetooth/bt_keyboard.c
    )
endif()

# Conditionally add Display 2.8 sources
if(DISPLAY_2_8_SUPPORT)
    list(APPEND ALTAIR_SOURCES
        FrontPanels/display_2_8.c
        FrontPanels/st7789_async.c
    )
endif()

# Conditionally add ST7789 Display sources
if(DISPLAY_ST7789_SUPPORT)
    list(APPEND ALTAIR_SOURCES
        FrontPanels/display_st7789.c
    )
    if(WAVESHARE_2_DISPLAY)
        list(APPEND ALTAIR_SOURCES FrontPanels/st7789vw_async.c)
    else()
        list(APPEND ALTAIR_SOURCES FrontPanels/st7789_async.c)
    endif()
endif()

# Conditionally add Waveshare 3.5" display + SD drivers
if(WAVESHARE_3_5_DISPLAY)
    list(APPEND ALTAIR_SOURCES
        drivers/waveshare/ws_spi1_bus.c
        drivers/waveshare/ws_ili9488.c
        drivers/waveshare/ws_display.c
    )
    if(VT100_DISPLAY)
        list(APPEND ALTAIR_SOURCES
            FrontPanels/vt100_display.c
        )
    endif()
    if(SD_CARD_SUPPORT)
        list(APPEND ALTAIR_SOURCES
            drivers/waveshare/ws_sdcard.c
            drivers/waveshare/ws_fatfs.c
        )
    endif()
endif()


# Conditionally add disk controller based on SD card support
if(SD_CARD_SUPPORT)
    if(WAVESHARE_3_5_DISPLAY)
        list(APPEND ALTAIR_SOURCES Altair8800/pico_88dcdd_sd_card_ws.c)
    else()
        list(APPEND ALTAIR_SOURCES Altair8800/pico_88dcdd_sd_card.c)
    endif()
elseif(REMOTE_FS_SUPPORT)
    list(APPEND ALTAIR_SOURCES 
        Altair8800/pico_88dcdd_remote_fs.c
        Altair8800/remote_fs.c
    )
else()
    list(APPEND ALTAIR_SOURCES Altair8800/pico_88dcdd_flash.c)
endif()

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

    # Override submodule's PUBLIC DEBUG_PRINT with PRIVATE to avoid
    # leaking it to btstack (which redefines it as a function-like macro).
    get_target_property(_defs pico_ws_server INTERFACE_COMPILE_DEFINITIONS)
    if(_defs)
        list(REMOVE_ITEM _defs "DEBUG_PRINT=0")
        set_target_properties(pico_ws_server PROPERTIES INTERFACE_COMPILE_DEFINITIONS "${_defs}")
    endif()
    target_compile_definitions(pico_ws_server PRIVATE DEBUG_PRINT=0)

    target_link_libraries(pico_ws_server
        PUBLIC
            pico_stdlib
            pico_cyw43_driver
            pico_lwip_nosys
            pico_mbedtls
            lwipopts_provider
    )

    list(APPEND ALTAIR_SOURCES 
        ws.cpp
        captive_portal/captive_portal.c
        ${PICO_SDK_PATH}/lib/lwip/src/apps/http/http_client.c
    )
    list(APPEND ALTAIR_LIBS pico_ws_server pico_lwip_mdns)
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

# Add ST7789 Support definition
if(DISPLAY_ST7789_SUPPORT)
    target_compile_definitions(altair PRIVATE DISPLAY_ST7789_SUPPORT=1)
endif()

# Add Remote FS Support definition
if(REMOTE_FS_SUPPORT)
    target_compile_definitions(altair PRIVATE REMOTE_FS_SUPPORT=1)
endif()

if(SD_CARD_SUPPORT)
    target_compile_definitions(altair PRIVATE SD_CARD_SUPPORT=1)
endif()

if(WAVESHARE_3_5_DISPLAY)
    target_compile_definitions(altair PRIVATE WAVESHARE_3_5_DISPLAY=1)
endif()

if(VT100_DISPLAY)
    if(NOT WAVESHARE_3_5_DISPLAY)
        message(FATAL_ERROR "VT100_DISPLAY requires WAVESHARE_3_5_DISPLAY to be enabled.")
    endif()
    target_compile_definitions(altair PRIVATE VT100_DISPLAY=1)
endif()

if(WAVESHARE_2_DISPLAY)
    target_compile_definitions(altair PRIVATE WAVESHARE_2_DISPLAY=1)
endif()

if(BLUETOOTH_KEYBOARD_SUPPORT)
    if(PICO_BOARD STREQUAL "pico_w")
        set(ALTAIR_BT_FLASH_BANK_STORAGE_OFFSET 0x1FD000)
    elseif(PICO_BOARD STREQUAL "pico2_w")
        set(ALTAIR_BT_FLASH_BANK_STORAGE_OFFSET 0x3FD000)
    elseif(PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350")
        set(ALTAIR_BT_FLASH_BANK_STORAGE_OFFSET 0x1FD000)
    else()
        message(FATAL_ERROR "BLUETOOTH_KEYBOARD_SUPPORT flash layout is only configured for pico_w, pico2_w, and pimoroni_pico_plus2_w_rp2350.")
    endif()

    target_compile_definitions(altair PRIVATE
        BLUETOOTH_KEYBOARD_SUPPORT=1
        PICO_FLASH_BANK_STORAGE_OFFSET=${ALTAIR_BT_FLASH_BANK_STORAGE_OFFSET}
    )
endif()

# Make sure all CYW43 headers are visible and include Altair8800 directory
target_include_directories(altair PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Altair8800
    ${CMAKE_CURRENT_LIST_DIR}/PortDrivers
    ${CMAKE_CURRENT_LIST_DIR}/FrontPanels
    ${CMAKE_CURRENT_LIST_DIR}/drivers/bluetooth
    ${CMAKE_CURRENT_LIST_DIR}/drivers/waveshare
    ${CMAKE_CURRENT_LIST_DIR}/captive_portal
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

if(DISPLAY_ST7789_SUPPORT)
    target_link_libraries(altair
        hardware_spi
        hardware_dma
        hardware_pwm
    )
endif()

if(WAVESHARE_3_5_DISPLAY)
    target_link_libraries(altair
        hardware_spi
        hardware_dma
        hardware_pwm
    )
endif()

if(PICO_CYW43_SUPPORTED)
    # Use threadsafe_background for interrupt-driven WiFi polling
    # This automatically services CYW43 via timer interrupt, eliminating
    # the sawtooth latency pattern seen with manual polling
    target_link_libraries(altair pico_cyw43_arch_lwip_threadsafe_background)
endif()

if(BLUETOOTH_KEYBOARD_SUPPORT)
    target_link_libraries(altair
        pico_btstack_ble
        pico_btstack_cyw43
    )
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

if(SD_CARD_SUPPORT)
    if(WAVESHARE_3_5_DISPLAY)
        # Waveshare has its own SD card driver — only link FatFs
        target_link_libraries(altair fatfs)
    else()
        target_link_libraries(altair sdcard)
    endif()
endif()

# Generate .uf2, .elf, etc.
pico_add_extra_outputs(altair)
