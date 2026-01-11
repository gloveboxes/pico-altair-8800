# FT Protocol Test Client

Desktop test application to verify the Remote FT Server protocol without deploying to Pico hardware.

## Build

```bash
cd test/ft_client
make
```

Or manually:
```bash
gcc -o ft_test_client ft_test_client.c -Wall
```

## Usage

```bash
./ft_test_client <server_ip> <filename> [output_file]
```

## Examples

```bash
# Test with the Python server running locally
cd RemoteFS
python3 remote_ft_server.py &

# Run the test client
cd ../test/ft_client
./ft_test_client 127.0.0.1 test.txt

# Save the downloaded file
./ft_test_client 127.0.0.1 test.txt downloaded.txt
```

## Sample Output

```
FT Protocol Test Client v1.0
============================

Connecting to 127.0.0.1:8090...
Connected!

Step 1: SET_FILENAME 'test.txt'
  Response: 0x00 (OK)

Step 2: GET_CHUNK loop
  Chunk 1: status=0x01 (EOF), 256 bytes

Step 3: CLOSE
  Response: 0x00 (OK)

============================
Transfer complete!
  File: test.txt
  Chunks: 1
  Total bytes: 256
  Time: 0.001 seconds
  Speed: 256.0 KB/s
```
