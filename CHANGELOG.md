# Changelog - Build 896 (2026-01-08)

## 📺 Display Driver Architecture Refactor
- **Generic ST7789 Support**: Renamed `display_2_8` module to `display_st7789` to properly support all ST7789-based displays (Pimoroni 2.8", Waveshare 2", etc.).
- **Variant System**: Introduced `ST7789_VARIANT` CMake variable to select between display types at compile time (e.g., `WAVESHARE_2`).
- **Unified Interface**: Both display variants now share the same high-level `display_st7789.h` interface for initialization and updates.

## 🐛 Waveshare 2" Display Fixes
- **New Driver Variant**: Added `st7789vw_async.c` specifically for Waveshare 2" display (ST7789VW controller) with correct pinning (SPI1) and initialization sequence.
- **Missing Includes Fixed**: Resolved compilation errors in `st7789vw_async.c` by adding missing hardware headers (`spi.h`, `dma.h`, `gpio.h`).
- **CMake Logic Fix**: Corrected priority logic in `CMakeLists.txt` to ensure `ST7789_VARIANT` correctly selects the Waveshare driver source file.
- **Build Script Fix**: Updated `build_all_boards.sh` to correctly pass `DISPLAY_ST7789_SUPPORT=ON` for Waveshare 2" builds (previously was defaulting to OFF).

## 🛠 Build System Improvements
- **Legacy Compatibility**: Added CMake shims to support old build flags (`DISPLAY_2_8_SUPPORT`, `WAVESHARE_2_DISPLAY`) while warning users to upgrade to new flags.
- **Stability**: Lowered SPI clock to 50MHz for Pimoroni 2.8" display to improve signal integrity on RP2350.

---

# Changelog - Build 862 (2026-01-08)

## ⚙️ Generic Configuration Module

Refactored WiFi configuration into a generic configuration system that stores multiple settings in flash.

### Breaking Change
- **New Flash Format**: Configuration data now uses magic number `0x43464730` ("CFG0") instead of `0x57494649` ("WIFI"). Existing stored credentials will need to be re-entered on first boot.

### New Features
- **Runtime RFS Server IP**: Remote FS server IP address is now configured at runtime via the serial console prompt, no longer hardcoded at compile time.
- **Extended Configuration Prompt**: Boot prompts now request WiFi SSID, password, **and** RFS server IP address.
- **Cached IP Access**: Added `config_get_rfs_ip()` for fast runtime access to stored server IP.

### Files Changed
- **Renamed**: `wifi_config.c` → `config.c`, `wifi_config.h` → `config.h`
- **Updated**: `remote_fs.c` now uses `config_get_rfs_ip()` instead of compile-time macro
- **Updated**: `CMakeLists.txt` removed `RFS_SERVER_IP` variable and compile definition
- **Updated**: `build_all_boards.sh` removed hardcoded `-DRFS_SERVER_IP=...` parameter

---

# Changelog - Build 856 (2026-01-07)

## 📊 New Diagnostics Tool: `pico.com`
- **New App**: Created a standalone command-line tool `Apps/PICO/PICO.C` (compiles to `pico.com`) providing instant system insights.
- **Features**: Displays Emulator Version, System Uptime (hh:mm), lwIP Network Statistics, and Remote FS Performance.
- **Build Script**: Added `Apps/PICO/PICO.SUB` for easy compilation on the emulator.

## 📈 Stats Display & Telemetry
- **RFS Stats Categorization**: Separated Remote FS cache statistics into a dedicated section in `ONBOARD.C` for better readability.
- **32-bit Counter**: Upgraded the main loop counter in `ONBOARD.C` to a 32-bit long integer to investigate and prevent 16-bit overflow issues during long-running tests.
- **Uptime Reporting**: Implemented port-based uptime reporting (Seconds and Format) for client applications.

## 🛠 Build System & Compatibility
- **RFS Build Target**: Updated `build_all_boards.sh` to change `pico2_w` to `pico2_w_rfs`, ensuring the Pico 2 W build includes Remote FS support by default.
- **Conditional Compilation**: Fixed `stats_io.c` to properly handle non-WiFi and non-RFS build configurations, resolving link errors for standard Pico targets.

---

# Changelog - Build 742 (2026-01-05)

## 📡 Remote File System (RFS) & Memory Stability
- **Platform-Specific Optimization (RAM Cache)**:
    - **RP2040 (Pico W)**: 80KB Cache (Optimized for stability).
    - **RP2350 (Pico 2 W)**: 180KB Cache (High performance).
- **Critical Fix**: Resolved premature HTTP connection closure that caused chunked large file transfers to fail.
- **Asynchronous Writes**: Implemented "Fire-and-Forget" write strategy for RFS, significantly improving disk I/O latency.

## 🌐 Network Stack (lwIP) Tuning
Comprehensive memory tuning to eliminate "Out of Memory" panics while maximizing throughput.

| Tuning Parameter | Old Value | New Value | Reason |
| :--- | :--- | :--- | :--- |
| **MEM_LIBC_MALLOC** | `1` | `0` | **CRITICAL**: Use lwIP's bounded pool instead of system heap to prevent OOM crashes. |
| **MEM_SIZE** | `12000` | `26000` | Increased heap for TCP Window buffering during bursts. |
| **PBUF_POOL_SIZE** | `20` | `22` | Added buffer headroom for concurrent RFS + HTTP traffic. |
| **MEMP_NUM_TCP_PCB** | `16` | `10` | Reduced max PCB count to save RAM (Max observed usage: 6). |
| **TCP_WND** | `4 * MSS` | `8 * MSS` | Increased window size for better throughput. |

## 🛠 Build System & Toolchain
- **Automated Artifact Management**: `build_all_boards.sh` now aggregates all builds into `Releases/` folder.
- **New Release Targets**:
    - `altair_pico_w_inky.uf2`: Pico W + Inky Pack (RFS enabled).
    - `altair_pico2_w_display28_rfs.uf2`: Pico 2 W + 2.8" Display + RFS.
- **Monitoring**: Added real-time lwIP memory statistics (Heap, PBUF, SEG, PCB) to serial output for performance validation.

## ⚙️ Configuration & Defaults
- **Default Server**: Updated default RFS Server IP to `192.168.1.151` in build scripts.
- **Release Optimization**: `build_all_boards.sh` now forces `ALTAIR_DEBUG=OFF` and skips copying bulky `.elf` files, producing a clean set of release-ready `.uf2` binaries.

---

# Changelog - Build 694 (2026-01-04)

## 🎉 New Feature: Remote File System (RFS)
This major update introduces the **Remote File System**, allowing the Altair emulator to boot and run CP/M from disk images stored on a remote server over WiFi. This eliminates the need for an SD card on WiFi-enabled boards (Pico W / Pico 2 W).

- **Network-Attached Storage**: Mount `.dsk` images hosted on a Python server (PC/macOS/Linux/Raspberry Pi).
- **Split-Core Architecture**: 
  - **Core 0**: Handles Emulation and transparent Disk Controller I/O.
  - **Core 1**: dedicated to Networking and RFS protocol, ensuring emulation performance isn't impacted by network latency.
- **Async I/O**: Inter-core ring buffers allow non-blocking disk requests.
- **Centralized Management**: Multiple Pico emulators can boot from the same server, each getting their own private copy of the disk images (copy-on-connect).

## Remote File System (RFS) Enhancements

### Performance & Caching
- **Implemented Transparent RFS Cache**: Moved sector cache from disk controller to RFS layer (`remote_fs.c`), making it transparent to upper layers.
- **Async "Fire-and-Forget" Writes**: Write operations now return immediately after sending data, removing network round-trip latency and significantly improving write performance.
- **Increased Cache Size**: Expanded write-through sector cache to **160KB** (~1,080 sectors), significantly improving read performance.
- **Added Write Deduplication**: Writes are now compared against cached data; redundant network writes are skipped, reducing latency and network traffic.
- **Cache Statistics**: Added periodic reporting (every 30s) of cache hits, misses, and write skips to the console.

### Robustness & Reliability
- **Improved Auto-Reconnect Logic**: Fixed issue where RFS client would stay disconnected if idle. Now triggers immediate reconnection on *any* disk request (Read/Write/Connect).
- **Resilient Disk Controller**: Updated `pico_88dcdd_remote_fs.c` to wait for timeout (25s) instead of aborting immediately on network errors, allowing transparent background reconnection without crashing the emulated program.
- **Optimized Display Refresh**: Tuned virtual front panel refresh rate to **~30Hz** (33ms) to balance CPU usage.

### Server & Docker Support
- **Unified RFS & Web Container**:
  - Combined **RFS Server** (Port 8085) and **Nginx Web Server** (Port 8086) into a single lightweight Alpine container (~50MB).
  - **RFS Server**: Moved default port to **8085** to follow Intel CPU naming conventions. 8085 came after 8080.
  - **Web Server**: Serves local `Apps/` directory with auto-indexing enabled, allowing direct file access for the emulator via `http_get`.
- **Docker Configuration**: Updated `docker-compose.yml` and `entrypoint.sh` for multi-service support and correct Alpine Nginx paths.
- **Reduced Logging**: Updated `remote_fs_server.py` to use `DEBUG` level for individual sector read/write logs.
- **Systemd Service**: Added `altair-rfs.service` and `install_service.sh` for native Linux background service deployment.

### Build Configuration
- **Added `pico2_w_display28_rfs`**: New build target for Pico 2 W + 2.8" Display + Remote FS support.

---

# Changelog - Build 631 (2026-01-04)

## Memory Management Improvements

### Disk Controller Static Patch Pool
- **Changed**: Replaced dynamic `malloc()` with static pre-allocated patch pool
- **Size**: 1200 sectors (~166KB) for copy-on-write disk modifications
- **Impact**: Prevents heap exhaustion that caused system crashes during heavy disk operations
- **Files**: `Altair8800/pico_88dcdd_flash.h`, `Altair8800/pico_88dcdd_flash.c`

**Details:**
- Pool is shared across all disks (A, B, C, D)
- Pool size configurable via `PATCH_POOL_SIZE` in `pico_88dcdd_flash.h`
- Added exhaustion detection with warning message
- Added stats function `pico_disk_get_patch_stats()` for monitoring

### lwIP Network Stack Optimizations
- **Changed**: Reduced memory allocation for network buffers
- **Files**: `lwipopts.h`

| Setting | Before | After |
|---------|--------|-------|
| `MEM_SIZE` | 16000 | 12000 |
| `PBUF_POOL_SIZE` | 24 | 16 |
| `MEMP_NUM_TCP_SEG` | 32 | 24 |
| `TCP_WND` | 6×MSS | 4×MSS |
| `TCP_SND_BUF` | 6×MSS | 4×MSS |
| `LWIP_ICMP` | 1 | 0 |
| `LWIP_RAW` | 1 | 0 |

---

## Build Configuration Updates

### Removed Pico Display 2.8 Support
- **Removed**: `pico_display28` build target from build scripts
- **Reason**: Display 2.8 requires ~440KB RAM, Pico/Pico W only have 264KB
- **Files**: `build_all_boards.sh`, `.vscode/tasks.json`

**Display 2.8 now requires RP2350-based boards:**
- pico2, pico2_w
- pimoroni_pico_plus2_w_rp2350

---

## Memory Usage (pico2_w with Display 2.8)

| Component | Size |
|-----------|------|
| Framebuffer (RGB565) | 150 KB |
| Disk patch pool | 166 KB |
| Altair memory | 64 KB |
| Other (stack, network, etc.) | ~61 KB |
| **Total RAM used** | 440.7 KB |
| **Free heap** | 79.3 KB |
