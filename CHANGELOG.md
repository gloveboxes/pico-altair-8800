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
