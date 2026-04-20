#include "ws_fatfs.h"

#include "ws_spi1_bus.h"

static bool ws_fatfs_begin_if_needed(void)
{
    bool owns_session = !ws_spi1_sd_session_active();
    if (owns_session)
    {
        ws_spi1_begin_sd_session();
    }
    return owns_session;
}

static void ws_fatfs_end_if_needed(bool owns_session)
{
    if (owns_session)
    {
        ws_spi1_end_sd_session();
    }
}

FRESULT ws_f_mount(FATFS* fs, const TCHAR* path, BYTE opt)
{
    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_mount(fs, path, opt);
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_open(FIL* fp, const TCHAR* path, BYTE mode)
{
    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_open(fp, path, mode);
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_close(FIL* fp)
{
    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_close(fp);
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_read(fp, buff, btr, br);
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_write(fp, buff, btw, bw);
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_sync(FIL* fp)
{
    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_sync(fp);
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_lseek(FIL* fp, FSIZE_t ofs)
{
    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_lseek(fp, ofs);
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_lseek_read(FIL* fp, FSIZE_t ofs, void* buff, UINT btr, UINT* br)
{
    if (br)
    {
        *br = 0;
    }

    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_lseek(fp, ofs);
    if (fr == FR_OK)
    {
        fr = f_read(fp, buff, btr, br);
    }
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_lseek_write_sync(FIL* fp, FSIZE_t ofs, const void* buff, UINT btw, UINT* bw)
{
    if (bw)
    {
        *bw = 0;
    }

    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_lseek(fp, ofs);
    if (fr == FR_OK)
    {
        fr = f_write(fp, buff, btw, bw);
    }
    if (fr == FR_OK)
    {
        fr = f_sync(fp);
    }
    ws_fatfs_end_if_needed(owns_session);
    return fr;
}

FRESULT ws_f_lseek_write_sync_lseek(FIL* fp, FSIZE_t write_ofs, const void* buff, UINT btw,
                                    UINT* bw, FSIZE_t seek_ofs, FRESULT* seek_fr)
{
    if (bw)
    {
        *bw = 0;
    }
    if (seek_fr)
    {
        *seek_fr = FR_OK;
    }

    bool owns_session = ws_fatfs_begin_if_needed();
    FRESULT fr = f_lseek(fp, write_ofs);
    if (fr == FR_OK)
    {
        fr = f_write(fp, buff, btw, bw);
    }
    if (fr == FR_OK)
    {
        fr = f_sync(fp);
    }

    FRESULT local_seek_fr = f_lseek(fp, seek_ofs);
    if (seek_fr)
    {
        *seek_fr = local_seek_fr;
    }

    ws_fatfs_end_if_needed(owns_session);
    return fr;
}