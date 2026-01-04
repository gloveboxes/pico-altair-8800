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
- **Increased Cache Size**: Expanded write-through sector cache to **160KB** (~1,080 sectors), significantly improving read performance.
- **Added Write Deduplication**: Writes are now compared against cached data; redundant network writes are skipped, reducing latency and network traffic.
- **Cache Statistics**: Added periodic reporting (every 30s) of cache hits, misses, and write skips to the console.

### Robustness & Reliability
- **Improved Auto-Reconnect Logic**: Fixed issue where RFS client would stay disconnected if idle. Now triggers immediate reconnection on *any* disk request (Read/Write/Connect).
- **Resilient Disk Controller**: Updated `pico_88dcdd_remote_fs.c` to wait for timeout (25s) instead of aborting immediately on network errors, allowing transparent background reconnection without crashing the emulated program.
- **Optimized Display Refresh**: Tuned virtual front panel refresh rate to **~30Hz** (33ms) to balance CPU usage.

### Server & Docker Support
- **Dockerized RFS Server**: Added `Dockerfile` and `docker-compose.yml` for easy deployment.
  - Switches to `python:3.11-alpine` for a lightweight footprint (~50MB).
  - Supports persistent client storage via bind mounts.
  - Embeds template disk images directly into the container.
- **Reduced Logging**: Updated `remote_fs_server.py` to use `DEBUG` level for individual sector read/write logs, reducing console noise. Connection events remain at `INFO`.
- **Systemd Service**: Added `altair-rfs.service` and `install_service.sh` for native Linux background service deployment.

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
