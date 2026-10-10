#ifndef _SDCARD_H
#define _SDCARD_H

#include <esp_err.h>

namespace openmc
{
    namespace storage
    {
        namespace sdcard
        {
            extern bool sd_mounted;

            esp_err_t Init(void);
            void Task_MountSDCard(void *params);
            void unmount_sdcard(void);
        } // sdcard
    } // storage
} // openmc

#endif // _SDCARD_H