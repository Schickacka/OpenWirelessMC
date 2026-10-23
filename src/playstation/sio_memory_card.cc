#include "sio_memory_card.h"

#include "storage/storage.h"
#include "logging.h"
#include "playstation/sio.h"
#include "playstation/spi.h"

#include <esp_log.h>
#include <esp_attr.h>
#include <esp32/rom/ets_sys.h>

#include <stdio.h>

//#define MC_DEBUG

// Directory-status fidelity (see CORRECOES-v1.4.md, C5).
// A real PS1 card reports 0x08 ("directory not read") again after sector 0 is
// committed, because the directory may have changed. The original OpenMC did
// this in Commit(); this fork had dropped it. Keep the switch so the old
// behaviour can be restored without touching the logic.
#ifndef MC_STRICT_DIRECTORY_FLAG
#define MC_STRICT_DIRECTORY_FLAG 1
#endif

namespace openmc
{
  namespace sio
  {
    namespace memory_card
    {
#ifdef MC_DEBUG
      static char debug_string[] = "MC event %2X\n";
#endif
      uint8_t DRAM_ATTR memory_card_ram[131072] = {};
      uint16_t DRAM_ATTR sector_version[1024] = {};

      uint8_t DRAM_ATTR flag = Flags::kDirectoryUnread;

      static uint8_t DRAM_ATTR current_command;
      // 32-bit: the long/fast read (0x42) can stream for far more than 65535
      // bytes without ever ending. A 16-bit counter wrapped back into the
      // command header and corrupted the stream.
      static uint32_t DRAM_ATTR command_ticks;

      uint16_t DRAM_ATTR sector;
      static uint8_t DRAM_ATTR sector_offset;

      static uint8_t DRAM_ATTR checksum_in;
      static uint8_t DRAM_ATTR checksum_out;

      static bool DRAM_ATTR uncommited_soft_write = false;
      bool DRAM_ATTR committed_to_storage = true;
      bool DRAM_ATTR skip_ack = false;
      uint64_t volatile DRAM_ATTR last_tick;
      uint64_t volatile DRAM_ATTR last_write_tick;

      static uint8_t DRAM_ATTR data_buffer[128];

      uint8_t DRAM_ATTR game_id[256];
      uint8_t DRAM_ATTR game_id_length = 0;
      volatile bool DRAM_ATTR game_id_updated = false;

      McTraceEntry DRAM_ATTR trace_ring[32] = {};
      volatile uint32_t DRAM_ATTR trace_write_idx = 0;
      volatile uint32_t DRAM_ATTR trace_read_idx = 0;
      volatile uint32_t DRAM_ATTR pad_poll_count = 0;
      volatile uint32_t DRAM_ATTR abort_byte0_count = 0;

      static bool DRAM_ATTR trace_active = false;
      static uint8_t DRAM_ATTR trace_addr = 0;
      static uint8_t DRAM_ATTR trace_cmd = 0;
      static uint8_t DRAM_ATTR trace_flag_sent = 0;
      static uint8_t DRAM_ATTR trace_status = 0;

      void IRAM_ATTR Commit()
      {
        bool sector_changed = false;
        uint32_t sector_offset = (sector * 128);

        if (uncommited_soft_write)
        {
          // Write buffer to memory page
          for (int i = 0; i < 128; i++)
          {
            if (memory_card_ram[sector_offset + i] != data_buffer[i])
            {
              sector_changed = true;
              memory_card_ram[sector_offset + i] = data_buffer[i];
            }
          }


          // Clear (soft)buffer status before return
          uncommited_soft_write = false;

          if (sector_changed)
          {
            // Order matters: data first, then the version, then the dirty flag.
            sector_version[sector]++;
            __sync_synchronize();
            committed_to_storage = false;
#if MC_STRICT_DIRECTORY_FLAG
            // The directory may have changed: a real card tells the console to
            // re-read it on the next access. OpenMC did this; the 1.3 fork lost it.
            if (sector == 0x0000)
            {
              flag = Flags::kDirectoryUnread;
            }
#endif
          }
          last_write_tick = sio::event_ticks;
        }

        return;
      }

      void IRAM_ATTR GoIdle()
      {
        if (uncommited_soft_write)
        {
          Commit();
        }
        // GameID byte layout, counted from the byte that follows 0x21:
        //   tick 0 = 0x21 itself, tick 1 = reserved, tick 2 = length,
        //   tick 3.. = characters. GoIdle() therefore has to subtract 3,
        //   not 2 (C1 in CORRECOES-v1.4.md).
        if (current_command == Commands::kGameID && command_ticks >= 3 && game_id_length > 0)
        {
          uint32_t chars_received = command_ticks - 3;
          if (chars_received > game_id_length)
          {
            chars_received = game_id_length;
          }
          if (chars_received > 0 && !game_id_updated)
          {
            if (chars_received >= (sizeof(game_id) - 1)) chars_received = sizeof(game_id) - 2;
            game_id[chars_received] = '\0';
            game_id_length = chars_received;
            __sync_synchronize();
            game_id_updated = true;
          }
        }
        if (trace_active)
        {
          static uint32_t DRAM_ATTR read2_skip = 0;
          bool should_record = (trace_cmd != Commands::kPing);
          if (trace_cmd == Commands::kRead && command_ticks == 2)
          {
            should_record = ((read2_skip++ & 31) == 0);
          }
          if (should_record)
          {
            uint32_t idx = trace_write_idx & 31;
            trace_ring[idx].event_tick = (uint32_t)sio::event_ticks;
            trace_ring[idx].addr = trace_addr;
            trace_ring[idx].cmd = trace_cmd;
            trace_ring[idx].flag_sent = trace_flag_sent;
            trace_ring[idx].status = trace_status;
            trace_ring[idx].abort_reason = spi::last_abort_reason;
            trace_ring[idx].ticks = command_ticks;
            trace_ring[idx].sector = sector;
            trace_ring[idx].chk_in = checksum_in;
            trace_ring[idx].chk_out = checksum_out;
            trace_write_idx++;
          }
          trace_active = false;
        }
        current_command = Commands::kNone;
        command_ticks = 0;
        sector_offset = 0;
        skip_ack = false;
      }

      uint8_t IRAM_ATTR ProcessEvents(uint8_t data_in)
      {
        uint8_t data_out = 0xFF;
        bool command_routed = false;

        last_tick = sio::event_ticks;

        //Loop until command is properly routed
        while (!command_routed)
        {
          switch (current_command)
          {
            // No command yet
          case Commands::kNone:
            // Store incoming byte as command
            current_command = data_in;
            // Store flag byte in outbound variable
            data_out = flag;
            skip_ack = false;
            trace_active = true;
            trace_addr = data_in;
            trace_cmd = 0;
            trace_flag_sent = flag;
            trace_status = 0;
            sector = 0;
            checksum_in = 0;
            checksum_out = 0;
            // Safe to exit interpret loop
            command_routed = true;
#ifdef MC_DEBUG
            ets_printf(debug_string, data_in);
#endif
            break;

          // Last command was MC Access, expecting
          case Commands::kAccess:
            trace_cmd = data_in;
            switch (data_in)
            {
            case Commands::kRead:
            case Commands::kFastRead:
            case Commands::kWrite:
            case Commands::kGetID:
            case Commands::kGameID:
            case Commands::kPing:
            case Commands::kReset:
              current_command = data_in;
              // Re-evaluate command
              // command_routed = false;
#ifdef MC_DEBUG
              ets_printf(debug_string, data_in);
#endif
              break;

            default:
              data_out = Responses::kIdleHighZ;
              GoIdle();
              skip_ack = true;
              command_routed = true;
              spi::Disable();
#ifdef MC_DEBUG
              ets_printf(debug_string, data_in);
#endif
              break;
            }
            break;

          case Commands::kRead:
          case Commands::kFastRead:
            data_out = TickReadCommand(data_in);
            command_routed = true;
            break;

          case Commands::kWrite:
            data_out = TickWriteCommand(data_in);
            command_routed = true;
            break;

          // https://gitlab.com/chriz2600/ps1-game-id-transmission
          // PS1 Game ID transmisson (MemCard PRO / PS1Digital)
          case Commands::kGameID:
            data_out = TickGameIDCommand(data_in);
            command_routed = true;
            break;

          case Commands::kPing:
            data_out = TickPingCommand(data_in);
            command_routed = true;
            break;

          case Commands::kGetID:
            data_out = TickGetIDCommand(data_in);
            command_routed = true;
            break;

          case Commands::kReset:
            data_out = TickResetCommand(data_in);
            command_routed = true;
            break;

          case Commands::kError: // Unexpected/Unsupported command
          default:
            data_out = Responses::kIdleHighZ;
            GoIdle();
            skip_ack = true;
            command_routed = true;
            spi::Disable();
            break;
          }
        }

        return data_out;
      }

      uint8_t IRAM_ATTR TickReadCommand(uint8_t &data_in)
      {
        uint8_t data_out = 0xFF;

        // 0x42 'B' (long/fast read). The 1.3 firmware treated it as a normal
        // 0x52 read: it sent one sector, then checksum + 0x47 and went Hi-Z.
        // A real card (and sd2psXtd) streams 128-byte pages back-to-back until
        // SEL rises, with no per-page checksum and no 0x47 terminator.
        // C2 in CORRECOES-v1.4.md. Keep this branch before the switch: after
        // tick 8 the normal read state machine must not run for 0x42.
        if (current_command == Commands::kFastRead && command_ticks >= 8)
        {
          if (command_ticks == 8)
          {
            sector_offset = 0;
          }
          if (sector >= 1024)
          {
            skip_ack = true;
            data_out = Responses::kIdleHighZ;
            spi::PassiveMode();
            return data_out;
          }
          else
          {
            data_out = memory_card_ram[(sector * (uint16_t)128) + sector_offset];
          }

          checksum_out ^= data_out;
          sector_offset++;
          if (sector_offset >= 128)
          {
            sector_offset = 0;
            sector++;
          }

          command_ticks++;
          return data_out;
        }

        switch (command_ticks)
        {
          //Data is sent and received simultaneously,
          //so the data we send isn't received by the system
          //until the next bytes are exchanged. In this way,
          //you have to basically respond one byte earlier than
          //actually occurs between Slave and Master.
          //Offset "Send" bytes noted from nocash's psx specs.

        case 0: //52h
          data_out = Responses::kID1;
          break;

        case 1: //00h
          data_out = Responses::kID2;
          break;

        case 2: //00h
          data_out = Responses::kDummy;
          break;

        case 3: //MSB
          //Store upper 8 bits of sector
          sector = (data_in << 8);
          //Reply with (pre)
          data_out = data_in;
          break;

        case 4: //LSB
          //Store lower 8 bits of sector
          sector |= data_in;
          data_out = Responses::kCommandAcknowledge1;
          break;

        case 5: //00h
          data_out = Responses::kCommandAcknowledge2;
          break;

        case 6: //00h
          //Confirm MSB
          data_out = sector >> 8;
          break;

        case 7: //00h
          //Confirm LSB
          data_out = (sector & 0xFF);
          checksum_out = highByte(sector) ^ lowByte(sector);
          break;

          // Cases 8 through 135 overloaded to default operator below

        case 136:
          data_out = checksum_out;
          break;

        case 137:
          data_out = Responses::kGoodReadWrite;
          trace_status = data_out;
          if (sector == 0x0000)
          {
            flag = Flags::kDirectoryRead;
          }
          break;

        case 138:
        default:
          if (command_ticks >= 8 && command_ticks <= 135) //Stay here for 128 bytes
          {
            if (sector >= 1024)
            {
              data_out = 0x00;
            }
            else
            {
              data_out = memory_card_ram[(sector * (uint16_t)128) + sector_offset];
            }

            checksum_out ^= data_out;
            sector_offset++;
          }
          else
          {
            // Real card: after 0x47 (tick 137), release the bus to Hi-Z and skip ACK
            skip_ack = true;
            data_out = Responses::kIdleHighZ;
            spi::PassiveMode();
          }
          break;
        }

        command_ticks++;

        return data_out;
      }

      uint8_t IRAM_ATTR TickWriteCommand(uint8_t &data_in)
      {
        uint8_t data_out = 0xFF;

        switch (command_ticks)
        {
          // Data is sent and received simultaneously,
          // so the data we send isn't received by the system
          // until the next bytes are exchanged. In this way,
          // you have to basically respond one byte earlier than
          // actually occurs between Slave and Master.
          // Offset "Send" bytes noted from nocash's psx specs.

        case 0: // 57h 'W' Write command
          data_out = Responses::kID1;
          break;

        case 1: // 00h
          data_out = Responses::kID2;
          break;

        case 2: // 00h
          data_out = Responses::kDummy;
          break;

        case 3: // MSB
          // Store upper 8 bits of sector
          sector = (data_in << 8);
          // Reply with (pre)
          data_out = data_in;
          break;

        case 4: // LSB
          // Store lower 8 bits of sector
          sector |= data_in;
          checksum_out = (sector >> 8) ^ (sector & 0xFF);
          data_out = data_in;
          break;

        case 133: // CHK
          checksum_in = data_in;
          data_out = Responses::kCommandAcknowledge1;
          break;

        case 134: // 00h
          data_out = Responses::kCommandAcknowledge2;
          break;

        case 135: // 00h
          if (sector >= 1024)
          {
            data_out = Responses::kBadSector;
          }
          else if (checksum_in == checksum_out)
          {
            // Any successful write clears the flag to ready (0x00 / kDirectoryRead).
            // A real PS1 card / sd2psX never stays at 0x08 during active saves.
            flag = Flags::kDirectoryRead;
            data_out = Responses::kGoodReadWrite;
            // If the incoming sector is within our storage, store it
            uncommited_soft_write = true;
          }
          else
          {
            data_out = Responses::kBadChecksum;
          }
          trace_status = data_out;
          break;

        case 136:
        default:
          if (command_ticks >= 136)
          {
            skip_ack = true;
            data_out = Responses::kIdleHighZ;
            spi::PassiveMode();
            Commit();
          }
          else if (command_ticks >= 5 && command_ticks <= 132)
          {
            // Store data
            data_buffer[command_ticks - 5] = data_in;
            // Calculate checksum
            checksum_out ^= data_in;
            // Reply with (pre)
            data_out = data_in;
          }
          else
          {
            GoIdle();
            current_command = PS1_SIOCommands::Ignore;
            spi::Disable();
          }

          break;
        }

        command_ticks++;

        return data_out;
      }

      uint8_t IRAM_ATTR TickGameIDCommand(uint8_t &data_in)
      {
        uint8_t data_out = Responses::kReserved;

        switch (command_ticks)
        {
        // Tick 0 sees 0x21 itself (ProcessEvents falls through from kAccess
        // into the new command), tick 1 is the reserved byte, tick 2 is the
        // length and tick 3 is the first character. The 1.3 code assumed tick
        // 0 was the reserved byte, so it stored the length as game_id[0] and
        // shifted every character by one. C1 in CORRECOES-v1.4.md.
        case 0: // 0x21 command byte
        case 1: // reserved
          data_out = Responses::kReserved;
          break;

        case 2: // length of the GameID (strlen + 1)
          game_id_length = data_in;
          // The spec's DATA column for the data bytes is "Prev Byte": the
          // first character byte must see the length echoed back, then each
          // following byte sees the previous character (sd2psXtd does this
          // with prev = length). This value is transferred on the next byte.
          data_out = data_in;
          break;

        default: // first and subsequent GameID characters
          {
            uint32_t char_idx = command_ticks - 3;
            if (game_id_length > 0 &&
                char_idx < (sizeof(game_id) - 1) &&
                char_idx < game_id_length)
            {
              game_id[char_idx] = data_in;
            }

            bool complete = (game_id_length > 0 && (char_idx + 1) >= game_id_length);
            bool terminated = (game_id_length > 0 && data_in == '\0');
            if (complete || terminated)
            {
              uint32_t term_idx = complete ? game_id_length : char_idx;
              if (term_idx >= sizeof(game_id)) term_idx = sizeof(game_id) - 1;
              game_id[term_idx] = '\0';
              __sync_synchronize();
              game_id_updated = true;
            }
            data_out = data_in; // echo the previous byte, like sd2psXtd
          }
          break;
        }

        command_ticks++;

        return data_out;
      }

      uint8_t IRAM_ATTR TickPingCommand(uint8_t &data_in)
      {
        uint8_t data_out = 0xFF;

        switch (command_ticks)
        {
        case 0:              // 20h Ping Command
        case 1:              // 00h Reserved
          data_out = kDummy; // 0x00 / Reserved
          break;

        case 2: // Card Present
          data_out = Responses::kCardPresent; // 0x27
          break;

        case 3: // Termination signal
          data_out = Responses::kTerminationSignal; // 0xFF
          trace_status = data_out;
          break;

        default:
          skip_ack = true;
          data_out = Responses::kIdleHighZ;
          spi::PassiveMode();
          break;
        }

        command_ticks++;

        return data_out;
      }

      uint8_t IRAM_ATTR TickGetIDCommand(uint8_t &data_in)
      {
        uint8_t data_out = 0xFF;

        switch (command_ticks)
        {
        case 0: // 53h
          data_out = Responses::kID1; // 0x5A
          break;

        case 1: // 00h
          data_out = Responses::kID2; // 0x5D
          break;

        case 2: // 00h
          data_out = Responses::kCommandAcknowledge1; // 0x5C
          break;

        case 3: // 00h
          data_out = Responses::kCommandAcknowledge2; // 0x5D
          break;

        case 4: // 00h
          data_out = 0x04;
          break;

        case 5: // 00h
          data_out = 0x00;
          break;

        case 6: // 00h
          data_out = 0x00;
          break;

        case 7: // 00h
          data_out = 0x80;
          trace_status = data_out;
          break;

        case 8: // 00h
        default:
          skip_ack = true;
          data_out = Responses::kIdleHighZ;
          spi::PassiveMode();
          break;
        }

        command_ticks++;

        return data_out;
      }

      uint8_t IRAM_ATTR TickResetCommand(uint8_t &data_in)
      {
        uint8_t data_out = 0xFF;

        switch (command_ticks)
        {
        case 0:
        case 1:
          data_out = Responses::kDummy; // 0x00
          break;

        case 2:
          data_out = Responses::kSuccess; // 0x20
          break;

        case 3:
          data_out = Responses::kTerminationSignal; // 0xFF
          flag = Flags::kDirectoryUnread; // Reset flag to 0x08 on PS1 reset
          trace_status = data_out;
          break;

        default:
          skip_ack = true;
          data_out = Responses::kIdleHighZ;
          spi::PassiveMode();
          break;
        }

        command_ticks++;

        return data_out;
      }
    } // namespace memory_card

  } // namespace sio
} // namespace openmc