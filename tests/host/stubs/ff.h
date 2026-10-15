#pragma once
#include <stdint.h>
typedef struct { uint32_t n_fatent, csize, ssize; } FATFS; typedef uint32_t DWORD; typedef int FRESULT; enum { FR_OK = 0 };
#define FF_MAX_SS 512
#define FF_MIN_SS 512
#define FF_SS 512
FRESULT f_getfree(const char*, DWORD*, FATFS**);
