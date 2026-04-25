#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif

#include "Altair8800/intel8080.h"
#include "Altair8800/memory.h"
#include "PortDrivers/host_files_io.h"
#include "ansi_input.h"
#include "io_ports.h"
#include "universal_88dcdd.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
#endif

#define ASCII_MASK_7BIT 0x7f

#ifndef LOCAL_RUNNER_REPO_ROOT
#define LOCAL_RUNNER_REPO_ROOT ".."
#endif

static intel8080_t cpu;
static struct termios saved_termios;
static int saved_stdin_flags = -1;
static bool terminal_configured = false;
static volatile sig_atomic_t keep_running = 1;

static const char *drive_a_path = LOCAL_RUNNER_REPO_ROOT "/Disks/cpm63k.dsk";
static const char *drive_b_path = LOCAL_RUNNER_REPO_ROOT "/Disks/bdsc-v1.60.dsk";
static const char *drive_c_path = LOCAL_RUNNER_REPO_ROOT "/Disks/blank.dsk";
static const char *apps_root_path = LOCAL_RUNNER_REPO_ROOT "/Apps";

static uint32_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0;
    }

    return (uint32_t)((now.tv_sec * 1000u) + (now.tv_nsec / 1000000u));
}

static void restore_terminal(void)
{
    if (!terminal_configured)
    {
        return;
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    if (saved_stdin_flags >= 0)
    {
        fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags);
    }
    terminal_configured = false;
}

static void handle_signal(int signum)
{
    (void)signum;
    keep_running = 0;
}

static void prefer_efficiency_core(void)
{
#ifdef __APPLE__
    int rc;

    rc = pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
    if (rc != 0)
    {
        fprintf(stderr, "altair-local: failed to set utility QoS (%d)\n", rc);
    }
#endif
}

static bool configure_terminal(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO))
    {
        return true;
    }

    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0)
    {
        perror("tcgetattr");
        return false;
    }

    raw = saved_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~(OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    {
        perror("tcsetattr");
        return false;
    }

    saved_stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (saved_stdin_flags < 0 || fcntl(STDIN_FILENO, F_SETFL, saved_stdin_flags | O_NONBLOCK) != 0)
    {
        perror("fcntl");
        restore_terminal();
        return false;
    }

    terminal_configured = true;
    atexit(restore_terminal);
    return true;
}

static uint8_t terminal_read(void)
{
    unsigned char ch;
    ssize_t n;

    n = read(STDIN_FILENO, &ch, 1);
    if (n <= 0)
    {
        return ansi_input_process(0x00, monotonic_ms());
    }

    ch &= ASCII_MASK_7BIT;
    if (ch == 0x1d)
    {
        keep_running = 0;
        return 0x00;
    }
    ch = ansi_input_process(ch, monotonic_ms());
    if (ch == '\n')
    {
        return '\r';
    }
    return ch;
}

static void terminal_write(uint8_t c)
{
    unsigned char ch = (unsigned char)(c & ASCII_MASK_7BIT);

    if (write(STDOUT_FILENO, &ch, 1) < 0 && errno != EAGAIN)
    {
        keep_running = 0;
    }
}

static uint8_t sense_switches(void)
{
    return 0xff;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--drive-a PATH] [--drive-b PATH] [--drive-c PATH] [--apps-root PATH]\n"
            "\n"
            "Defaults reference the repository Disks and Apps folders:\n"
            "  A: %s\n"
            "  B: %s\n"
            "  C: %s\n"
            "  Apps: %s\n",
            program, drive_a_path, drive_b_path, drive_c_path, apps_root_path);
}

static bool parse_args(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--drive-a") == 0 && i + 1 < argc)
        {
            drive_a_path = argv[++i];
        }
        else if (strcmp(argv[i], "--drive-b") == 0 && i + 1 < argc)
        {
            drive_b_path = argv[++i];
        }
        else if (strcmp(argv[i], "--drive-c") == 0 && i + 1 < argc)
        {
            drive_c_path = argv[++i];
        }
        else if (strcmp(argv[i], "--apps-root") == 0 && i + 1 < argc)
        {
            apps_root_path = argv[++i];
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return false;
        }
        else
        {
            print_usage(argv[0]);
            return false;
        }
    }

    return true;
}

int main(int argc, char **argv)
{
    disk_controller_t controller;

    if (!parse_args(argc, argv))
    {
        return argc > 1 ? 1 : 0;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    if (!configure_terminal())
    {
        return 1;
    }
    prefer_efficiency_core();

    if (!host_disk_init(drive_a_path, drive_b_path, drive_c_path))
    {
        restore_terminal();
        fprintf(stderr, "altair-local: failed to open disk images\n");
        fprintf(stderr, "  A: %s\n  B: %s\n  C: %s\n", drive_a_path, drive_b_path, drive_c_path);
        return 1;
    }

    host_files_init(apps_root_path);
    controller = host_disk_controller();

    memset(memory, 0x00, 64 * 1024);
    loadDiskLoader(0xff00);
    i8080_reset(&cpu, terminal_read, terminal_write, sense_switches, &controller, io_port_in, io_port_out);
    i8080_examine(&cpu, 0xff00);

    while (keep_running)
    {
        i8080_cycle(&cpu);
    }

    host_disk_close();
    restore_terminal();
    return 0;
}
