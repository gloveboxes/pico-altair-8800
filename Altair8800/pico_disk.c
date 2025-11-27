#include "pico_disk.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Global disk controller instance
pico_disk_controller_t pico_disk_controller;

static inline void set_status(uint8_t bit)
{
    pico_disk_controller.current->status &= ~bit;
}

// Helper: Set status bit to FALSE (set bit for active-low)
static inline void clear_status(uint8_t bit)
{
    pico_disk_controller.current->status |= bit;
}

static const uint8_t STATUS_DEFAULT = STATUS_ENWD | STATUS_MOVE_HEAD |
                                      STATUS_HEAD | STATUS_IE |
                                      STATUS_TRACK_0 | STATUS_NRDA;

static sector_patch_t *find_patch(pico_disk_t *disk, uint16_t index)
{
    sector_patch_t *node = disk->patches;
    while (node) {
        if (node->index == index) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static void clear_patches(pico_disk_t *disk)
{
    sector_patch_t *node = disk->patches;
    while (node) {
        sector_patch_t *next = node->next;
        free(node);
        node = next;
    }
    disk->patches = NULL;
}

static sector_patch_t *get_patch(pico_disk_t *disk, uint16_t index)
{
    sector_patch_t *existing = find_patch(disk, index);
    if (existing) {
        return existing;
    }
    sector_patch_t *node = (sector_patch_t *)malloc(sizeof(sector_patch_t));
    if (!node) {
        printf("[PATCH] ERROR: Failed to allocate patch for sector %u\n", index);
        return NULL;
    }
    node->index = index;
    node->next = disk->patches;
    disk->patches = node;
    memset(node->data, 0, SECTOR_SIZE);
    return node;
}

static void flush_sector(pico_disk_t *disk)
{
    if (!disk->sector_dirty) {
        return;
    }

    uint16_t sector_index = (uint16_t)(disk->disk_pointer / SECTOR_SIZE);
    sector_patch_t *patch = get_patch(disk, sector_index);
    if (patch) {
        memcpy(patch->data, disk->sector_data, SECTOR_SIZE);
    }

    disk->sector_dirty = false;
    disk->have_sector_data = false;
    disk->sector_pointer = 0;
}

// Helper: Seek to current track
static void seek_to_track(void)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return;
    }
    
    flush_sector(disk);
    
    uint32_t seek_offset = disk->track * TRACK_SIZE;
    disk->disk_pointer = seek_offset;
    disk->have_sector_data = false;
    disk->sector_pointer = 0;
    disk->sector = 0;
}

// Initialize disk controller
void pico_disk_init(void)
{
    memset(&pico_disk_controller, 0, sizeof(pico_disk_controller_t));
    
    // Initialize all drives
    for (int i = 0; i < MAX_DRIVES; i++) {
        pico_disk_controller.disk[i].status = STATUS_DEFAULT;
        pico_disk_controller.disk[i].track = 0;
        pico_disk_controller.disk[i].sector = 0;
        pico_disk_controller.disk[i].disk_loaded = false;
        pico_disk_controller.disk[i].disk_image_flash = NULL;
        pico_disk_controller.disk[i].patches = NULL;
    }
    
    // Select drive 0 by default
    pico_disk_controller.current = &pico_disk_controller.disk[0];
    pico_disk_controller.current_disk = 0;
    
}

// Load disk image for specified drive (Copy-on-Write)
bool pico_disk_load(uint8_t drive, const uint8_t *disk_image, uint32_t size)
{
    if (drive >= MAX_DRIVES) {
        return false;
    }
    
    pico_disk_t *disk = &pico_disk_controller.disk[drive];
    clear_patches(disk);
    
    // Copy-on-Write: Keep flash pointer, allocate RAM on first write
    disk->disk_image_flash = disk_image;
    disk->disk_size = size;
    disk->disk_loaded = true;
    disk->disk_pointer = 0;
    disk->sector = 0;
    disk->track = 0;
    disk->sector_pointer = 0;
    disk->sector_dirty = false;
    disk->have_sector_data = false;
    disk->write_status = 0;
    disk->patches = NULL;
    
    // Start from default hardware reset value, then reflect initial state
    disk->status = STATUS_DEFAULT;
    disk->status &= (uint8_t)~STATUS_MOVE_HEAD;
    disk->status &= (uint8_t)~STATUS_TRACK_0;  // head at track 0 (active-low)
    disk->status &= (uint8_t)~STATUS_SECTOR;   // sector true
    
    return true;
}

// Select disk drive
void pico_disk_select(uint8_t drive)
{
    uint8_t select = drive & DRIVE_SELECT_MASK;
    
    if (select < MAX_DRIVES) {
        pico_disk_controller.current_disk = select;
        pico_disk_controller.current = &pico_disk_controller.disk[select];
    } else {
        pico_disk_controller.current_disk = 0;
        pico_disk_controller.current = &pico_disk_controller.disk[0];
    }
}

// Get disk status
uint8_t pico_disk_status(void)
{
    uint8_t status = pico_disk_controller.current->status;
    return status;
}

// Disk control function
void pico_disk_function(uint8_t control)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return;
    }
    
    // Step in (increase track)
    if (control & CONTROL_STEP_IN) {
        if (disk->track < MAX_TRACKS - 1) {
            disk->track++;
        }
        if (disk->track != 0) {
            clear_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }
    
    // Step out (decrease track)
    if (control & CONTROL_STEP_OUT) {
        if (disk->track > 0) {
            disk->track--;
        }
        if (disk->track == 0) {
            set_status(STATUS_TRACK_0);
        }
        seek_to_track();
    }
    
    // Head load
    if (control & CONTROL_HEAD_LOAD) {
        set_status(STATUS_HEAD);
        set_status(STATUS_NRDA);
    }
    
    // Head unload
    if (control & CONTROL_HEAD_UNLOAD) {
        clear_status(STATUS_HEAD);
    }
    
    // Write enable
    if (control & CONTROL_WE) {
        set_status(STATUS_ENWD);
        disk->write_status = 0;
    }
}

// Get current sector
uint8_t pico_disk_sector(void)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return 0xC0;  // Invalid sector
    }
    
    // Wrap sector to 0 after reaching end of track
    if (disk->sector == SECTORS_PER_TRACK) {
        disk->sector = 0;
    }
    
    flush_sector(disk);
    
    uint32_t seek_offset = disk->track * TRACK_SIZE + disk->sector * SECTOR_SIZE;
    disk->disk_pointer = seek_offset;
    disk->sector_pointer = 0;
    disk->have_sector_data = false;
    
    // Format sector number (88-DCDD specification)
    // D7-D6: Always 1
    // D5-D1: Sector number (0-31)
    // D0: Sector True bit (0 at sector start, 1 otherwise)
    uint8_t ret_val = 0xC0;  // Set D7-D6
    ret_val |= (disk->sector << SECTOR_SHIFT_BITS);  // D5-D1
    ret_val |= (disk->sector_pointer == 0) ? 0 : 1;  // D0
    
    disk->sector++;
    return ret_val;
}

// Write byte to disk (Copy-on-Write)
void pico_disk_write(uint8_t data)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return;
    }
    
    if (disk->sector_pointer >= SECTOR_SIZE + 2) {
        disk->sector_pointer = SECTOR_SIZE + 1;
    }
    
    disk->sector_data[disk->sector_pointer++] = data;
    disk->sector_dirty = true;
    disk->have_sector_data = true;
    
    if (disk->write_status == SECTOR_SIZE) {
        flush_sector(disk);
        disk->write_status = 0;
        clear_status(STATUS_ENWD);
    } else {
        disk->write_status++;
    }
}

// Read byte from disk
uint8_t pico_disk_read(void)
{
    pico_disk_t *disk = pico_disk_controller.current;
    
    if (!disk->disk_loaded) {
        return 0x00;
    }
    
    // Load sector data if not already loaded
    if (!disk->have_sector_data) {
        disk->sector_pointer = 0;
        memset(disk->sector_data, 0x00, SECTOR_SIZE);
        
        uint32_t offset = disk->disk_pointer;
        if (offset + SECTOR_SIZE <= disk->disk_size) {
            memcpy(disk->sector_data, &disk->disk_image_flash[offset], SECTOR_SIZE);
            disk->have_sector_data = true;
            uint16_t sector_index = (uint16_t)(offset / SECTOR_SIZE);
            sector_patch_t *patch = find_patch(disk, sector_index);
            if (patch) {
                memcpy(disk->sector_data, patch->data, SECTOR_SIZE);
            }
        }
    }
    
    // Return current byte and advance pointer within sector
    // Note: Sector positioning is controlled by pico_disk_sector() (port 0x09), not here
    return disk->sector_data[disk->sector_pointer++];
}
