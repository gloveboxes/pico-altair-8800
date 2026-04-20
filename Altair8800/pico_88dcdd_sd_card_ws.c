#include "pico_88dcdd_sd_card.h"

#include "drivers/waveshare/ws_spi1_bus.h"

// MITS 88-DCDD Disk Controller Emulation for Pico with Waveshare SD support
// Uses Waveshare SPI1 session coordination around FatFs file I/O.

sd_disk_controller_t sd_disk_controller;

static void writeSector(sd_disk_t* pDisk);

static const uint8_t STATUS_DEFAULT =
    STATUS_ENWD | STATUS_MOVE_HEAD | STATUS_HEAD | STATUS_IE | STATUS_TRACK_0 | STATUS_NRDA;

static inline void set_status(uint8_t bit)
{
    sd_disk_controller.current->status &= ~bit;
}

static inline void clear_status(uint8_t bit)
{
    sd_disk_controller.current->status |= bit;
}

static void seek_to_track(void)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    ws_spi1_begin_sd_session();

    if (disk->sectorDirty)
    {
        writeSector(disk);
    }

    uint32_t seek_offset = disk->track * TRACK_SIZE;
    FRESULT fr = f_lseek(&disk->fil, seek_offset);

    ws_spi1_end_sd_session();

    if (fr != FR_OK)
    {
        printf("[WS_SD_DISK] Seek failed for track %u, error: %d\n", disk->track, fr);
    }

    disk->diskPointer = seek_offset;
    disk->haveSectorData = false;
    disk->sectorPointer = 0;
    disk->sector = 0;
}

void sd_disk_init(void)
{
    memset(&sd_disk_controller, 0, sizeof(sd_disk_controller_t));

    for (int i = 0; i < MAX_DRIVES; i++)
    {
        sd_disk_controller.disk[i].status = STATUS_DEFAULT;
        sd_disk_controller.disk[i].track = 0;
        sd_disk_controller.disk[i].sector = 0;
        sd_disk_controller.disk[i].disk_loaded = false;
    }

    sd_disk_controller.current = &sd_disk_controller.disk[0];
    sd_disk_controller.currentDisk = 0;
}

bool sd_disk_load(uint8_t drive, const char* disk_path)
{
    if (drive >= MAX_DRIVES)
    {
        printf("[WS_SD_DISK] Invalid drive number: %u\n", drive);
        return false;
    }

    sd_disk_t* disk = &sd_disk_controller.disk[drive];

    if (disk->disk_loaded)
    {
        ws_spi1_begin_sd_session();
        f_close(&disk->fil);
        ws_spi1_end_sd_session();
        disk->disk_loaded = false;
    }

    ws_spi1_begin_sd_session();
    FRESULT fr = f_open(&disk->fil, disk_path, FA_READ | FA_WRITE);
    if (fr != FR_OK)
    {
        ws_spi1_end_sd_session();
        printf("[WS_SD_DISK] Failed to open %s, error: %d\n", disk_path, fr);
        return false;
    }

    FSIZE_t file_size = f_size(&disk->fil);
    ws_spi1_end_sd_session();
    if (file_size < DISK_SIZE)
    {
        printf("[WS_SD_DISK] Warning: %s is smaller than expected (%lu bytes)\n",
               disk_path, (unsigned long)file_size);
    }

    disk->disk_loaded = true;
    disk->diskPointer = 0;
    disk->sector = 0;
    disk->track = 0;
    disk->sectorPointer = 0;
    disk->sectorDirty = false;
    disk->haveSectorData = false;
    disk->write_status = 0;

    disk->status = STATUS_DEFAULT;
    disk->status &= (uint8_t)~STATUS_MOVE_HEAD;
    disk->status &= (uint8_t)~STATUS_TRACK_0;
    disk->status &= (uint8_t)~STATUS_SECTOR;

    return true;
}

void sd_disk_select(uint8_t drive)
{
    uint8_t select = drive & DRIVE_SELECT_MASK;

    if (select < MAX_DRIVES)
    {
        sd_disk_controller.currentDisk = select;
        sd_disk_controller.current = &sd_disk_controller.disk[select];
    }
    else
    {
        sd_disk_controller.currentDisk = 0;
        sd_disk_controller.current = &sd_disk_controller.disk[0];
    }
}

uint8_t sd_disk_status(void)
{
    return sd_disk_controller.current->status;
}

void sd_disk_function(uint8_t control)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    if (control & CONTROL_STEP_IN)
    {
        if (disk->track < MAX_TRACKS - 1)
        {
            disk->track++;
        }
        if (disk->track != 0)
        {
            clear_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }

    if (control & CONTROL_STEP_OUT)
    {
        if (disk->track > 0)
        {
            disk->track--;
        }
        if (disk->track == 0)
        {
            set_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }

    if (control & CONTROL_HEAD_LOAD)
    {
        set_status(STATUS_HEAD);
        set_status(STATUS_NRDA);
    }

    if (control & CONTROL_HEAD_UNLOAD)
    {
        clear_status(STATUS_HEAD);
    }

    if (control & CONTROL_WE)
    {
        set_status(STATUS_ENWD);
        disk->write_status = 0;
    }
}

uint8_t sd_disk_sector(void)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0xC0;
    }

    if (disk->sector == SECTORS_PER_TRACK)
    {
        disk->sector = 0;
    }

    ws_spi1_begin_sd_session();

    if (disk->sectorDirty)
    {
        writeSector(disk);
    }

    uint32_t seek_offset = disk->track * TRACK_SIZE + disk->sector * SECTOR_SIZE;
    FRESULT fr = f_lseek(&disk->fil, seek_offset);

    ws_spi1_end_sd_session();

    if (fr != FR_OK)
    {
        printf("[WS_SD_DISK] Seek failed for sector, error: %d\n", fr);
    }

    disk->diskPointer = seek_offset;
    disk->sectorPointer = 0;
    disk->haveSectorData = false;

    uint8_t ret_val = 0xC0;
    ret_val |= (disk->sector << SECTOR_SHIFT_BITS);
    ret_val |= (disk->sectorPointer == 0) ? 0 : 1;

    disk->sector++;
    return ret_val;
}

void sd_disk_write(uint8_t data)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return;
    }

    if (disk->sectorPointer >= SECTOR_SIZE + 2)
    {
        disk->sectorPointer = SECTOR_SIZE + 1;
    }

    disk->sectorData[disk->sectorPointer++] = data;
    disk->sectorDirty = true;
    disk->haveSectorData = true;

    if (disk->write_status == SECTOR_SIZE)
    {
        writeSector(disk);
        disk->write_status = 0;
        clear_status(STATUS_ENWD);
    }
    else
    {
        disk->write_status++;
    }
}

uint8_t sd_disk_read(void)
{
    sd_disk_t* disk = sd_disk_controller.current;

    if (!disk->disk_loaded)
    {
        return 0x00;
    }

    if (!disk->haveSectorData)
    {
        disk->sectorPointer = 0;
        memset(disk->sectorData, 0x00, SECTOR_SIZE);

        UINT bytes_read;
        ws_spi1_begin_sd_session();
        FRESULT fr = f_lseek(&disk->fil, disk->diskPointer);
        if (fr == FR_OK)
        {
            fr = f_read(&disk->fil, disk->sectorData, SECTOR_SIZE, &bytes_read);
        }
        ws_spi1_end_sd_session();

        if (fr != FR_OK)
        {
            printf("[WS_SD_DISK] Sector read failed, error: %d\n", fr);
            disk->haveSectorData = false;
        }
        else if (bytes_read != SECTOR_SIZE)
        {
            printf("[WS_SD_DISK] Sector read incomplete: read %u of %u bytes\n",
                   bytes_read, SECTOR_SIZE);
            disk->haveSectorData = (bytes_read > 0);
        }
        else
        {
            disk->haveSectorData = true;
        }
    }

    return disk->sectorData[disk->sectorPointer++];
}

static void writeSector(sd_disk_t* pDisk)
{
    if (!pDisk->sectorDirty)
    {
        return;
    }

    UINT bytes_written;
    bool owns_session = !ws_spi1_sd_session_active();
    if (owns_session)
    {
        ws_spi1_begin_sd_session();
    }

    FRESULT fr = f_lseek(&pDisk->fil, pDisk->diskPointer);
    if (fr == FR_OK)
    {
        fr = f_write(&pDisk->fil, pDisk->sectorData, SECTOR_SIZE, &bytes_written);
    }

    if (fr != FR_OK)
    {
        printf("[WS_SD_DISK] Sector write failed, error: %d\n", fr);
    }
    else if (bytes_written != SECTOR_SIZE)
    {
        printf("[WS_SD_DISK] Sector write incomplete: wrote %u of %u bytes\n",
               bytes_written, SECTOR_SIZE);
    }
    else
    {
        f_sync(&pDisk->fil);
    }

    if (owns_session)
    {
        ws_spi1_end_sd_session();
    }

    pDisk->sectorPointer = 0;
    pDisk->sectorDirty = false;
}