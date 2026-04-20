#pragma once

#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

FRESULT ws_f_mount(FATFS* fs, const TCHAR* path, BYTE opt);
FRESULT ws_f_open(FIL* fp, const TCHAR* path, BYTE mode);
FRESULT ws_f_close(FIL* fp);
FRESULT ws_f_read(FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT ws_f_write(FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT ws_f_sync(FIL* fp);
FRESULT ws_f_lseek(FIL* fp, FSIZE_t ofs);
FRESULT ws_f_lseek_read(FIL* fp, FSIZE_t ofs, void* buff, UINT btr, UINT* br);
FRESULT ws_f_lseek_write_sync(FIL* fp, FSIZE_t ofs, const void* buff, UINT btw, UINT* bw);
FRESULT ws_f_lseek_write_sync_lseek(FIL* fp, FSIZE_t write_ofs, const void* buff, UINT btw,
									UINT* bw, FSIZE_t seek_ofs, FRESULT* seek_fr);

#ifdef __cplusplus
}
#endif