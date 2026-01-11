/*
 * FT Protocol Test Client
 *
 * Desktop application to test the Remote FT Server protocol
 * without needing to deploy to Pico hardware.
 *
 * Usage: ./ft_test_client <server_ip> <filename> [output_file]
 *
 * Build: gcc -o ft_test_client ft_test_client.c
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Protocol constants (must match remote_ft_server.py) */
#define FT_SERVER_PORT 8090

#define CMD_SET_FILENAME 0x01
#define CMD_GET_CHUNK 0x02
#define CMD_CLOSE 0x03

#define RESP_OK 0x00
#define RESP_EOF 0x01
#define RESP_ERROR 0xFF

#define CHUNK_SIZE 256

/* Receive exactly n bytes */
static int recv_exact(int sock, void* buf, size_t n)
{
    size_t received = 0;
    while (received < n)
    {
        ssize_t r = recv(sock, (char*)buf + received, n - received, 0);
        if (r <= 0)
        {
            if (r == 0)
            {
                fprintf(stderr, "Connection closed by server\n");
            }
            else
            {
                perror("recv");
            }
            return -1;
        }
        received += r;
    }
    return 0;
}

int main(int argc, char* argv[])
{
    const char* server_ip;
    const char* filename;
    const char* output_file = NULL;
    int sock;
    struct sockaddr_in server_addr;
    uint8_t send_buf[260];
    uint8_t recv_buf[CHUNK_SIZE];
    size_t total_bytes = 0;
    int chunks = 0;
    FILE* fp = NULL;
    clock_t start_time, end_time;
    int flag = 1;

    printf("FT Protocol Test Client v1.0\n");
    printf("============================\n\n");

    if (argc < 3)
    {
        printf("Usage: %s <server_ip> <filename> [output_file]\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s 192.168.1.100 test.txt\n", argv[0]);
        printf("  %s 192.168.1.100 test.txt downloaded.txt\n", argv[0]);
        return 1;
    }

    server_ip = argv[1];
    filename = argv[2];
    if (argc >= 4)
    {
        output_file = argv[3];
    }

    /* Create socket */
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return 1;
    }

    /* Disable Nagle's algorithm */
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* Connect to server */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(FT_SERVER_PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0)
    {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        close(sock);
        return 1;
    }

    printf("Connecting to %s:%d...\n", server_ip, FT_SERVER_PORT);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("Connected!\n\n");

    /* Open output file if specified */
    if (output_file)
    {
        fp = fopen(output_file, "wb");
        if (!fp)
        {
            perror("fopen");
            close(sock);
            return 1;
        }
        printf("Saving to: %s\n", output_file);
    }

    start_time = clock();

    /* Step 1: Send SET_FILENAME command */
    printf("Step 1: SET_FILENAME '%s'\n", filename);

    size_t name_len = strlen(filename);
    send_buf[0] = CMD_SET_FILENAME;
    memcpy(&send_buf[1], filename, name_len + 1); /* include null terminator */

    if (send(sock, send_buf, 1 + name_len + 1, 0) < 0)
    {
        perror("send SET_FILENAME");
        goto cleanup;
    }

    /* Receive status */
    if (recv_exact(sock, recv_buf, 1) < 0)
    {
        goto cleanup;
    }

    printf("  Response: 0x%02X ", recv_buf[0]);
    if (recv_buf[0] == RESP_OK)
    {
        printf("(OK)\n");
    }
    else if (recv_buf[0] == RESP_ERROR)
    {
        printf("(ERROR - file not found?)\n");
        goto cleanup;
    }
    else
    {
        printf("(UNKNOWN)\n");
        goto cleanup;
    }

    /* Step 2: Request chunks until EOF */
    printf("\nStep 2: GET_CHUNK loop\n");

    while (1)
    {
        /* Send GET_CHUNK command */
        send_buf[0] = CMD_GET_CHUNK;
        if (send(sock, send_buf, 1, 0) < 0)
        {
            perror("send GET_CHUNK");
            goto cleanup;
        }

        /* First receive just the status byte */
        if (recv_exact(sock, recv_buf, 1) < 0)
        {
            goto cleanup;
        }

        uint8_t status = recv_buf[0];
        chunks++;

        /* Always receive the count byte next */
        uint8_t count_byte;
        if (recv_exact(sock, &count_byte, 1) < 0)
        {
            goto cleanup;
        }

        /* If error, skip payload */
        size_t valid_bytes = 0;
        if (status == RESP_ERROR)
        {
            valid_bytes = 0;
        }
        else
        {
            /* Decode count: 0 means 256, otherwise actual count */
            valid_bytes = (count_byte == 0) ? 256 : count_byte;

            /* Receive exactly the payload size */
            if (valid_bytes > 0)
            {
                if (recv_exact(sock, recv_buf, valid_bytes) < 0)
                {
                    goto cleanup;
                }
            }
        }

        printf("  Chunk %d: status=0x%02X (%s), %zu valid bytes\n", chunks, status,
               status == RESP_OK    ? "MORE"
               : status == RESP_EOF ? "EOF"
                                    : "ERR",
               valid_bytes);

        /* Write only valid data to file */
        if (fp && valid_bytes > 0)
        {
            fwrite(recv_buf, 1, valid_bytes, fp);
        }

        total_bytes += valid_bytes;

        if (status == RESP_EOF || status == RESP_ERROR)
        {
            break;
        }
    }

    end_time = clock();

    /* Step 3: Send CLOSE command */
    printf("\nStep 3: CLOSE\n");
    send_buf[0] = CMD_CLOSE;
    if (send(sock, send_buf, 1, 0) < 0)
    {
        perror("send CLOSE");
        goto cleanup;
    }

    if (recv_exact(sock, recv_buf, 1) < 0)
    {
        goto cleanup;
    }
    printf("  Response: 0x%02X (%s)\n", recv_buf[0], recv_buf[0] == RESP_OK ? "OK" : "ERROR");

    /* Summary */
    double elapsed = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    printf("\n============================\n");
    printf("Transfer complete!\n");
    printf("  File: %s\n", filename);
    printf("  Chunks: %d\n", chunks);
    printf("  Total bytes: %zu\n", total_bytes);
    printf("  Time: %.3f seconds\n", elapsed);
    if (elapsed > 0)
    {
        printf("  Speed: %.1f KB/s\n", (total_bytes / 1024.0) / elapsed);
    }
    if (output_file)
    {
        printf("  Saved to: %s\n", output_file);
    }

cleanup:
    if (fp)
        fclose(fp);
    close(sock);
    return 0;
}
