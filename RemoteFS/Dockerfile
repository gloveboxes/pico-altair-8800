FROM python:3.11-alpine

WORKDIR /app

# Create directories
RUN mkdir -p /app/clients /app/disks

# Copy server script
COPY RemoteFS/remote_fs_server.py /app/

# Copy disk templates
COPY disks/*.dsk /app/disks/

# Expose port
EXPOSE 8080

# Run server
# Note: volumes should be mounted at /app/clients and /app/disks
CMD ["python3", "-u", "remote_fs_server.py", "--host", "0.0.0.0", "--port", "8080", "--clients-dir", "/app/clients", "--template-dir", "/app/disks"]
