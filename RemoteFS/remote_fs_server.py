#!/usr/bin/env python3
"""
Remote File System Server for Altair 8800 Emulator

This server handles disk sector read/write requests from Pico clients over TCP.
Each client (identified by IP address) gets their own copy of the disk images,
allowing multiple Altair emulators to operate independently.

Protocol:
- INIT (0x03): Initialize connection with client IP (len + bytes), copies disk files if first time for this client
- READ_SECTOR (0x01): drive(1) + track(1) + sector(1) -> status(1) + data(137)
- WRITE_SECTOR (0x02): drive(1) + track(1) + sector(1) + data(137) -> status(1)

Response status:
- 0x00: OK
- 0xFF: Error
"""

import os
import sys
import socket
import shutil
import struct
import threading
import argparse
import logging
import mmap
import time
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
from functools import lru_cache
from collections import deque

# Protocol constants
CMD_READ_SECTOR = 0x01
CMD_WRITE_SECTOR = 0x02
CMD_INIT = 0x03

RESP_OK = 0x00
RESP_ERROR = 0xFF

# Disk geometry (8" floppy)
SECTOR_SIZE = 137
SECTORS_PER_TRACK = 32
MAX_TRACKS = 77
TRACK_SIZE = SECTORS_PER_TRACK * SECTOR_SIZE
DISK_SIZE = MAX_TRACKS * TRACK_SIZE

# Drive configuration
MAX_DRIVES = 4
DISK_NAMES = ["cpm63k.dsk", "bdsc-v1.60.dsk", "escape-posix.dsk", "blank.dsk"]

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S'
)
logger = logging.getLogger(__name__)


class DiskImage:
    """Represents a single disk image file with memory-mapped I/O for performance"""
    
    def __init__(self, filepath: Path):
        self.filepath = filepath
        self.lock = threading.Lock()
        self._file = None
        self._mmap = None
        self._last_flush = time.time()
        self._dirty = False
        self._open()
    
    def _open(self):
        """Open the file and create memory mapping"""
        try:
            # Ensure file exists and is the right size
            if not self.filepath.exists():
                with open(self.filepath, 'wb') as f:
                    f.write(bytes(DISK_SIZE))
            
            # Open file for read/write
            self._file = open(self.filepath, 'r+b')
            self._mmap = mmap.mmap(self._file.fileno(), DISK_SIZE)
            logger.debug(f"Opened disk image: {self.filepath}")
        except Exception as e:
            logger.error(f"Failed to open disk image {self.filepath}: {e}")
            self._mmap = None
            self._file = None
    
    def close(self):
        """Close the memory mapping and file"""
        with self.lock:
            if self._mmap and self._dirty:
                try:
                    # Force synchronous flush before closing
                    self._mmap.flush()
                except Exception as e:
                    logger.error(f"Error flushing on close: {e}")
                    
            if self._mmap:
                self._mmap.close()
                self._mmap = None
            if self._file:
                self._file.close()
                self._file = None
        
    def read_sector(self, track: int, sector: int) -> bytes:
        """Read a sector from the disk image"""
        if track >= MAX_TRACKS or sector >= SECTORS_PER_TRACK:
            logger.warning(f"Invalid sector address: track={track}, sector={sector}")
            return bytes(SECTOR_SIZE)
        
        if not self._mmap:
            return bytes(SECTOR_SIZE)
            
        offset = track * TRACK_SIZE + sector * SECTOR_SIZE
        
        with self.lock:
            try:
                return self._mmap[offset:offset + SECTOR_SIZE]
            except Exception as e:
                logger.error(f"Error reading sector: {e}")
                return bytes(SECTOR_SIZE)
    
    def write_sector(self, track: int, sector: int, data: bytes) -> bool:
        """Write a sector to the disk image"""
        if track >= MAX_TRACKS or sector >= SECTORS_PER_TRACK:
            logger.warning(f"Invalid sector address: track={track}, sector={sector}")
            return False
            
        if len(data) != SECTOR_SIZE:
            logger.warning(f"Invalid sector data size: {len(data)}")
            return False
        
        if not self._mmap:
            return False
            
        offset = track * TRACK_SIZE + sector * SECTOR_SIZE
        
        with self.lock:
            try:
                self._mmap[offset:offset + SECTOR_SIZE] = data
                self._dirty = True
                
                # Flush every 2 seconds or immediately if it's been a while
                now = time.time()
                if now - self._last_flush > 2.0:
                    self._mmap.flush(0, 0)  # MS_ASYNC: non-blocking async flush
                    self._last_flush = now
                    self._dirty = False
                    
                return True
            except Exception as e:
                logger.error(f"Error writing sector: {e}")
                return False


class ClientSession:
    """Handles a single client connection"""
    
    def __init__(self, conn: socket.socket, addr: tuple, server: "RemoteFSServer"):
        self.conn = conn
        self.addr = addr
        self.client_ip = addr[0]
        self.server = server
        self.client_id = None
        self.client_dir = None
        self.disks = None
        self.running = True
        # Set socket timeout to 300 seconds (5 minutes) to detect hung connections
        self.conn.settimeout(300)
        
    def handle(self):
        """Main handler loop for client connection"""
        logger.info(f"Client connected: {self.client_ip}")
        
        try:
            while self.running:
                # Read command byte
                cmd_data = self._recv_exact(1)
                if not cmd_data:
                    break
                    
                cmd = cmd_data[0]
                
                if cmd == CMD_INIT:
                    self._handle_init()
                elif cmd == CMD_READ_SECTOR:
                    self._handle_read_sector()
                elif cmd == CMD_WRITE_SECTOR:
                    self._handle_write_sector()
                else:
                    logger.warning(f"Unknown command: 0x{cmd:02X}")
                    self.conn.sendall(bytes([RESP_ERROR]))
                    
        except ConnectionResetError:
            logger.info(f"Client disconnected: {self.client_ip}")
        except Exception as e:
            logger.error(f"Error handling client {self.client_ip}: {e}")
        finally:
            self.conn.close()
            logger.info(f"Connection closed: {self.client_ip}")
    
    def _recv_exact(self, size: int) -> bytes:
        """Receive exactly 'size' bytes from the connection"""
        try:
            # Use MSG_WAITALL for more efficient single recv call
            data = self.conn.recv(size, socket.MSG_WAITALL)
            if len(data) != size:
                return None
            return data
        except (socket.error, OSError):
            return None
    
    def _handle_init(self):
        """Handle INIT command"""
        id_len_data = self._recv_exact(1)
        if not id_len_data:
            return
        id_len = id_len_data[0]
        if id_len == 0:
            logger.warning(f"[{self.client_ip}] INIT missing client ID")
            self.conn.sendall(bytes([RESP_ERROR]))
            return
        id_bytes = self._recv_exact(id_len)
        if not id_bytes or len(id_bytes) != id_len:
            logger.warning(f"[{self.client_ip}] INIT client ID read failed")
            self.conn.sendall(bytes([RESP_ERROR]))
            return

        client_id = id_bytes.decode('ascii', errors='replace').strip()
        if not client_id:
            logger.warning(f"[{self.client_ip}] INIT empty client ID")
            self.conn.sendall(bytes([RESP_ERROR]))
            return

        self.client_id = client_id
        self.client_dir = self.server.get_client_dir(client_id)
        self.disks = self.server.get_client_disks(client_id)

        logger.info(f"INIT from {self.client_ip} (id={client_id})")
        self.conn.sendall(bytes([RESP_OK]))
    
    def _handle_read_sector(self):
        """Handle READ_SECTOR command"""
        # Read drive, track, sector
        params = self._recv_exact(3)
        if not params:
            return
            
        drive, track, sector = params[0], params[1], params[2]

        if self.disks is None:
            logger.warning(f"[{self.client_ip}] READ before INIT")
            self.conn.sendall(bytes([RESP_ERROR]))
            return

        if drive >= MAX_DRIVES or drive >= len(self.disks):
            logger.warning(f"Invalid drive: {drive}")
            self.conn.sendall(bytes([RESP_ERROR]))
            return
            
        disk = self.disks[drive]
        data = disk.read_sector(track, sector)
        
        # Send response: status + data
        response = bytes([RESP_OK]) + data
        self.conn.sendall(response)
        
        logger.debug(f"[{self.client_ip}] READ:  drive={drive}, track={track:02d}, sector={sector:02d}")
    
    def _handle_write_sector(self):
        """Handle WRITE_SECTOR command"""
        # Read drive, track, sector, data
        params = self._recv_exact(3)
        if not params:
            return
            
        drive, track, sector = params[0], params[1], params[2]

        data = self._recv_exact(SECTOR_SIZE)
        if not data:
            return

        if self.disks is None:
            logger.warning(f"[{self.client_ip}] WRITE before INIT")
            self.conn.sendall(bytes([RESP_ERROR]))
            return
            
        if drive >= MAX_DRIVES or drive >= len(self.disks):
            logger.warning(f"Invalid drive: {drive}")
            self.conn.sendall(bytes([RESP_ERROR]))
            return
            
        disk = self.disks[drive]
        success = disk.write_sector(track, sector, data)
        
        # No response sent for writes (Async/Fire-and-forget)
        
        logger.debug(f"[{self.client_ip}] WRITE: drive={drive}, track={track:02d}, sector={sector:02d}, success={success}")


class RemoteFSServer:
    """Remote File System Server"""
    
    def __init__(self, host: str, port: int, template_dir: Path, clients_dir: Path):
        self.host = host
        self.port = port
        self.template_dir = template_dir
        self.clients_dir = clients_dir
        self.running = False
        self.server_socket = None
        self.client_disks = {}  # client_id -> [DiskImage, ...]
        self.lock = threading.Lock()
        # Thread pool for handling clients (max 50 concurrent connections)
        self.executor = ThreadPoolExecutor(max_workers=50, thread_name_prefix="client-")
        
    def get_client_dir(self, client_id: str) -> Path:
        """Get the directory for a specific client, create if needed"""
        safe_id = client_id.replace(':', '_').replace('.', '_')
        client_dir = self.clients_dir / safe_id
        
        with self.lock:
            if not client_dir.exists():
                # First time for this client - copy template disks
                logger.info(f"Creating disk folder for new client: {client_id}")
                client_dir.mkdir(parents=True, exist_ok=True)
                
                # Copy disk images from template directory
                for disk_name in DISK_NAMES:
                    src = self.template_dir / disk_name
                    dst = client_dir / disk_name
                    if src.exists():
                        logger.info(f"  Copying {disk_name}...")
                        shutil.copy2(src, dst)
                    else:
                        # Create empty disk if template doesn't exist
                        logger.warning(f"  Template {disk_name} not found, creating empty disk")
                        with open(dst, 'wb') as f:
                            f.write(bytes(DISK_SIZE))
                            
        return client_dir

    def get_client_disks(self, client_id: str) -> list:
        """Get or create disk images for a client"""
        with self.lock:
            if client_id in self.client_disks:
                return self.client_disks[client_id]
                
        client_dir = self.get_client_dir(client_id)
        
        disks = []
        for disk_name in DISK_NAMES:
            disk_path = client_dir / disk_name
            disks.append(DiskImage(disk_path))
            
        with self.lock:
            self.client_disks[client_id] = disks
            
        return disks
    
    def start(self):
        """Start the server"""
        # Ensure directories exist
        self.clients_dir.mkdir(parents=True, exist_ok=True)
        
        if not self.template_dir.exists():
            logger.error(f"Template directory not found: {self.template_dir}")
            logger.error("Please ensure disk images are available in the 'disks' directory")
            sys.exit(1)
            
        # Check for at least one disk image
        found_disks = [d for d in DISK_NAMES if (self.template_dir / d).exists()]
        if not found_disks:
            logger.error(f"No disk images found in {self.template_dir}")
            logger.error(f"Expected disk names: {', '.join(DISK_NAMES)}")
            sys.exit(1)
        
        logger.info(f"Found {len(found_disks)} disk image(s) in template directory")
        
        # Create server socket
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        
        # Increase socket buffer sizes for better throughput (1MB each)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
        
        self.server_socket.bind((self.host, self.port))
        # Increased backlog from 5 to 128 to handle connection bursts
        self.server_socket.listen(128)
        
        self.running = True
        logger.info(f"Remote FS Server listening on {self.host}:{self.port}")
        logger.info(f"Template directory: {self.template_dir}")
        logger.info(f"Client data directory: {self.clients_dir}")
        
        try:
            while self.running:
                try:
                    conn, addr = self.server_socket.accept()
                    client_ip = addr[0]
                    
                    # Optimize socket for low latency and reliability
                    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # Disable Nagle's algorithm
                    conn.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)   # Enable keepalive
                    
                    # Increase client socket buffer sizes (256KB each, suitable for embedded clients)
                    conn.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
                    conn.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 256 * 1024)
                    
                    # Configure graceful shutdown (wait up to 1 second for pending data)
                    conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 1))
                    
                    # Handle client using thread pool (more efficient than creating threads)
                    session = ClientSession(conn, addr, self)
                    self.executor.submit(session.handle)
                    
                except KeyboardInterrupt:
                    break
                    
        finally:
            self.stop()
    
    def stop(self):
        """Stop the server"""
        self.running = False
        
        # Shutdown thread pool
        logger.info("Shutting down thread pool...")
        self.executor.shutdown(wait=True, cancel_futures=False)
        
        # Flush and close all disk images
        logger.info("Flushing disk images...")
        with self.lock:
            for client_id, disks in self.client_disks.items():
                for disk in disks:
                    disk.close()
        
        if self.server_socket:
            self.server_socket.close()
            
        logger.info("Server stopped")


def main():
    parser = argparse.ArgumentParser(
        description="Remote File System Server for Altair 8800 Emulator"
    )
    parser.add_argument(
        '--host', default='0.0.0.0',
        help='Host address to bind to (default: 0.0.0.0)'
    )
    parser.add_argument(
        '--port', type=int, default=8085,
        help='Port to listen on (default: 8085)'
    )
    parser.add_argument(
        '--template-dir', type=Path,
        default=Path(__file__).parent.parent / 'disks',
        help='Directory containing template disk images (default: ../disks)'
    )
    parser.add_argument(
        '--clients-dir', type=Path,
        default=Path(__file__).parent / 'clients',
        help='Directory for per-client disk storage (default: ./clients)'
    )
    parser.add_argument(
        '--debug', action='store_true',
        help='Enable debug logging'
    )
    
    args = parser.parse_args()
    
    if args.debug:
        logging.getLogger().setLevel(logging.DEBUG)
    
    server = RemoteFSServer(
        host=args.host,
        port=args.port,
        template_dir=args.template_dir,
        clients_dir=args.clients_dir
    )
    
    try:
        server.start()
    except KeyboardInterrupt:
        logger.info("Interrupted by user")
    except Exception as e:
        logger.error(f"Server error: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
