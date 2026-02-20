/*
 * pico.c - Pico Stats Display Tool
 *
 * Displays system, lwIP network and Remote FS statistics
 * from the emulator's onboard sensors using I/O ports.
 *
 * To compile with BDS C:
 * cc pico
 * clink pico

 * Pico Stats support for Altair 8800
 * BDS C 1.6 on CP/M
 *
 * Rewritten for BDS C constraints:
 *  - All symbols unique within first 7 characters
 *  - K&R style definitions only
 *  - No support for casts
 *  - No support for goto labels named 'end'
 */

#include <stdio.h>

/* LWIP Stats port and data values */
#define STATS_PORT 50
#define STATS_HEAP 0
#define STATS_PBUF 1
#define STATS_SEG 2
#define STATS_PCB 3

/* RFS Stats port and data values */
#define RFS_PORT 51 /* Remote FS stats port */
#define RFS_TYPE 0  /* Cache stats type */

/* Global buffer */
char buffer[256];

/* Function prototypes */
int main();
int read_string_from_port();
int cputs();
int chput();
int bios(), bdos(), inp(), outp();
int atol(), itol(), ldiv(), lmod(), ltoa();

/* Long integer helper variables */
char luptime[4];
char l3600[4], l60[4];
char lhours[4], lrem[4], lmins[4];
char bufNum[16];

int main()
{
    int i;

    /* Display Header */
    cputs("\r\nPico Stats\r\n");

    /* Get Device ID - Port 46, data 0 */
    outp(46, 0);
    read_string_from_port(buffer, 255);
    cputs("\r\nHostname:         ");
    cputs(buffer);

    /* Get WiFi IP Address - Port 46, data 1 */
    outp(46, 1);
    read_string_from_port(buffer, 255);
    cputs("\r\nWiFi IP address:  ");
    cputs(buffer);

    /* Get Physical Device ID - Port 46, data 2 */
    outp(46, 2);
    read_string_from_port(buffer, 255);
    cputs("\r\nDevice ID:        ");
    cputs(buffer);

    /* Get Emulator Version - Port 70 */
    outp(70, 0);
    read_string_from_port(buffer, 255);
    cputs("\r\nEmulator version: ");
    cputs(buffer);

    /* Get Uptime - Port 41 (returns seconds string) */
    outp(41, 1);
    read_string_from_port(buffer, 255);
    cputs("\r\nUptime in secs:   ");
    cputs(buffer);

    /* Parse uptime to long for calculation */
    atol(luptime, buffer);
    itol(l3600, 3600);
    itol(l60, 60);

    /* Calculate Hours: luptime / 3600 */
    ldiv(lhours, luptime, l3600);

    /* Calculate Remainder: luptime % 3600 */
    lmod(lrem, luptime, l3600);

    /* Calculate Minutes: remainder / 60 */
    ldiv(lmins, lrem, l60);

    cputs("\r\nUptime hrs:mins:  ");
    ltoa(bufNum, lhours);
    cputs(bufNum);
    chput(':');

    /* Format minutes with leading zero if needed */
    ltoa(bufNum, lmins);
    if (bufNum[1] == 0) /* Single digit? */
    {
        chput('0');
    }
    cputs(bufNum);
    cputs("\r\n");

    /* lwIP Network Statistics */
    cputs("\r\n---- lwIP Network Statistics ----\r\n");

    /* Heap stats */
    outp(STATS_PORT, STATS_HEAP);
    read_string_from_port(buffer, 255);
    cputs(buffer);
    cputs("\r\n");

    /* PBUF pool stats */
    outp(STATS_PORT, STATS_PBUF);
    read_string_from_port(buffer, 255);
    cputs(buffer);
    cputs("\r\n");

    /* TCP Segment stats */
    outp(STATS_PORT, STATS_SEG);
    read_string_from_port(buffer, 255);
    cputs(buffer);
    cputs("\r\n");

    /* TCP PCB stats */
    outp(STATS_PORT, STATS_PCB);
    read_string_from_port(buffer, 255);
    cputs(buffer);
    cputs("\r\n");

    /* Remote FS Statistics */
    cputs("\r\n---- Remote FS Statistics ----\r\n");

    /* RFS Cache stats */
    outp(RFS_PORT, RFS_TYPE);
    read_string_from_port(buffer, 255);
    cputs(buffer);
    cputs("\r\n");

    return 0;
}

/*
 * Read string data from port 200 until null character
 */
int read_string_from_port(buf, max_len)
char* buf;
int max_len;
{
    int i, ch;

    i = 0;
    ch = inp(200);

    while (ch != 0 && i < max_len - 1)
    {
        buf[i] = ch;
        i++;
        ch = inp(200);
    }

    buf[i] = 0; /* Null terminate */
    return i;
}

/*
 * Output a string to console
 */
int cputs(s)
char* s;
{
    while (*s)
    {
        chput(*s);
        s++;
    }
    return 0;
}

/*
 * Output a character to console using BIOS
 */
int chput(c)
char c;
{
    return bios(4, c);
}
