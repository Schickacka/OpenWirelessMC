#ifndef _WIFI_H
#define _WIFI_H

#include <stdbool.h>
#include <stdint.h>

namespace openmc
{
    namespace wifi
    {
        enum Mode : uint8_t
        {
            kAcessPoint = 0,
            kClient = 1,
            kNone = 2
        };
        extern bool ready;
        extern char ip_address[16];
        // 32 characters + NUL. esp_wifi_config.ssid is a 32-byte, NON
        // NUL-terminated field; this status copy must be terminated because it
        // is printed and serialized to JSON (C7).
        extern uint8_t ssid[33];
    } // wifi
} // openmc

#endif // _WIFI_H