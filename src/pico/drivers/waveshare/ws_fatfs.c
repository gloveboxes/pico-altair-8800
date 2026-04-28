#include "ws_fatfs.h"

#include "ws_spi1_bus.h"

FRESULT ws_f_mount(FATFS* fs, const TCHAR* path, BYTE opt)
{
    ws_spi1_begin_sd_session();
    FRESULT fr = f_mount(fs, path, opt);
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_open(FIL* fp, const TCHAR* path, BYTE mode)
{
    ws_spi1_begin_sd_session();
    FRESULT fr = f_open(fp, path, mode);
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_close(FIL* fp)
{
    ws_spi1_begin_sd_session();
    FRESULT fr = f_close(fp);
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
    ws_spi1_begin_sd_session();
    FRESULT fr = f_read(fp, buff, btr, br);
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
    ws_spi1_begin_sd_session();
    FRESULT fr = f_write(fp, buff, btw, bw);
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_sync(FIL* fp)
{
    ws_spi1_begin_sd_session();
    FRESULT fr = f_sync(fp);
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_lseek(FIL* fp, FSIZE_t ofs)
{
    ws_spi1_begin_sd_session();
    FRESULT fr = f_lseek(fp, ofs);
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_lseek_read(FIL* fp, FSIZE_t ofs, void* buff, UINT btr, UINT* br)
{
    if (br)
    {
        *br = 0;
    }

    ws_spi1_begin_sd_session();
    FRESULT fr = f_lseek(fp, ofs);
    if (fr == FR_OK)
    {
        fr = f_read(fp, buff, btr, br);
    }
    ws_spi1_end_sd_session();
    return fr;
}

FRESULT ws_f_lseek_write_sync(FIL* fp, FSIZE_t ofs, const void* buff, UINT btw, UINT* bw)
{
    if (bw)
    {
        *bw = 0;
    }

    ws_spi1_begin_sd_session();
    FRESULT fr = f_lseek(fp, ofs);
    if (fr == FR_OK)
    {
        fr = f_write(fp, buff, btw, bw);
    }
    if (fr == FR_OK)
    {
        fr = f_sync(fp);
    }
    ws_spi1_end_sd_session();
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

    ws_spi1_begin_sd_session();
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

    ws_spi1_end_sd_session();
    return fr;
}