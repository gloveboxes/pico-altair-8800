# Raspberry Pi Pico Altair 8800

## Clone With Submodules

This project bundles Pimoroni's Pico helper libraries as a git submodule.
Clone (or update) with:

```shell
git clone --recurse-submodules https://github.com/gloveboxes/Raspberyy-Pi-Pico-2-W-Altair-8800.git
# or, if already cloned
git submodule update --init --recursive
```

## Serial Terminal

### macos

1. screen /dev/tty.usbmodem101 115200
   
   - screen is built into macOS
   - Exit: Press <kbd>ctrl+c</kbd> then <kbd>K</kbd>

2. picocom /dev/tty.usbmodem101 -b 115200

    - Install

        ```shell
        brew install picocom
        ```

    - Exit: Press <kbd>ctrl+a</kbd>, then <kbd>ctrl+x</kbd>


## Wi-Fi Console

1. Provide credentials via the CMake cache variables (preferred):
    ```shell
    cmake -B build -DWIFI_SSID="MyNetwork" -DWIFI_PASSWORD="secretpass"
    ```
    The values are injected as preprocessor macros. (You can still fall back to defining `PICO_DEFAULT_WIFI_SSID`/`PICO_DEFAULT_WIFI_PASSWORD` elsewhere if you prefer.)
2. Enable the optional network console when configuring:
    ```shell
    cmake -B build -DALTAIR_ENABLE_WEBSOCKET=ON [...other flags...]
    ```
    Leaving it `OFF` (default) keeps the firmware USB-only.
3. Build and flash as usual. On boot the Pico W connects to Wi-Fi and starts a WebSocket console on port `8082`.
4. Point a browser at `http://<pico-ip>:8082/` to load the bundled console UI, or use any WebSocket-capable client (e.g., `wscat`) to connect to `ws://<pico-ip>:8082/` and interact with the Altair terminal alongside USB serial.
5. USB serial stays active; terminal I/O is mirrored between USB and the WebSocket session.

## Selecting a Target Board

`PICO_BOARD` now defaults to `pico2_w` (RP2350 Pico 2 W). Override it when configuring to target the non-W version or other boards:

```shell
cmake -B build -DPICO_BOARD=pico2 [...other flags...]
```

- Non-W boards (e.g., `pico2`) do not have a CYW43 radio, so leave `-DALTAIR_ENABLE_WEBSOCKET=OFF` for a USB-only firmware.
- The RP2040-based Pico (original) still builds, but its 264 KB of SRAM is very tight for the full CP/M image—expect to trim features if you go that route.

### CMake Configuration Options

| Option | Default | Purpose |
| --- | --- | --- |
| `-DWIFI_SSID=""` | empty | Wi-Fi SSID for the Pico W (passed to firmware at build time). |
| `-DWIFI_PASSWORD=""` | empty | Wi-Fi password accompanying the SSID. |
| `-DENABLE_INKY_DISPLAY=ON` | ON | Pulls in the Pimoroni Inky Pack driver and shows the welcome/IP screen. Set to `OFF` to save flash/RAM when the display isn't connected. |
| `-DALTAIR_ENABLE_WEBSOCKET=ON` | OFF | Builds the Wi-Fi/WebSocket console firmware path (adds CYW43 + pico-ws-server stack). |
| `-DPICO_BOARD=pico2_w` | pico2_w | Selects the Pico variant (e.g., `pico2`, `pico2_w`). WebSockets require a board with CYW43 (the `_w` models). |
| `-DCMAKE_BUILD_TYPE=Release` | Debug | Usual CMake switch for optimized builds (recommended). |

## Regenerate Disk Image Header

1. Copy the .dsk file to the disks folder
2. Run the following command

    ```shell
    python3 dsk_to_header.py --input cpm63k.dsk --output Altair8800/cpm63k_disk.h --symbol cpm63k_dsk
    ```

3. Copy the .h file to the Altair8800 folder
4. Rebuild and deploy


## Rebuild for Performance

cmake -B build -DCMAKE_BUILD_TYPE=Release regenerated the build directory with CMAKE_BUILD_TYPE explicitly set to Release (confirmed by the “Build type is Release” line). That enables the Pico SDK’s release optimization flags (-O3, no extra debug helpers).
cmake --build build then rebuilt everything with those settings. The log shows only Release-config targets being built and the final altair.elf linked successfully with no errors—just the usual picotool fetch/install noise and a warning about duplicate errors/liberrors.a, which the SDK always emits.


```shell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Building App

There are two build tasks

1. Build Altair (Release): Default build task, does clean then build.
2. Build Altair (Debug)L Create a debug build.
