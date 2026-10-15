#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_err.h"
#include "playstation/sio_memory_card.h"
#include "playstation/sio.h"
#include "playstation/spi.h"
int g_log_quiet = 0;
const char *kLogPrefix = "T";
namespace openmc {
 namespace spi { volatile uint32_t sel_glitch_count; }
 namespace system { namespace timer { uint64_t timestamp; } }
 namespace sio {
   uint64_t event_ticks; bool memory_card_enabled = true; bool pad_enabled; bool net_yaroze_enabled;
   int set_calls = 0;
   void SetMemoryCardEmulation(bool e){ memory_card_enabled = e; set_calls++; }
   namespace memory_card {
     uint8_t memory_card_ram[131072]; uint16_t sector_version[1024]; uint8_t flag; uint16_t sector;
     uint8_t game_id[256]; uint8_t game_id_length; volatile bool game_id_updated;
     uint64_t volatile last_tick; uint64_t volatile last_write_tick; bool committed_to_storage = true; bool skip_ack;
     McTraceEntry trace_ring[32]; volatile uint32_t trace_write_idx, trace_read_idx, pad_poll_count, abort_byte0_count;
     void GoIdle(){}
   }
 }
 namespace storage { namespace sdcard {
   bool sd_ok = true; int unmounts = 0;
   esp_err_t Init(void){ return sd_ok ? ESP_OK : ESP_FAIL; }
   void unmount_sdcard(void){ unmounts++; }
 } }
}
