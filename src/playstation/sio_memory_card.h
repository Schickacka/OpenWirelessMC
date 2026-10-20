#ifndef SIO_MEMORY_CARD_H
#define SIO_MEMORY_CARD_H

#include <stdint.h>

namespace openmc
{
    namespace sio
    {
        namespace memory_card
        {

            // Implement actual bit manipulation later
            // Need more info on flag bits.
            enum Flags : uint8_t
            {
                kDirectoryUnread = 0x08, // Initial power on value
                kDirectoryRead = 0x00    // Cleared after good MC Write
                                         // (test write sector 3F = 0x1F80) offset
            };

            enum Commands : uint8_t
            {
                kAccess = 0x81, // Memory Card Select
                kFastRead = 0x42, // 'B' Fast / Long Read Command
                kRead = 0x52,   // Read Command
                kGetID = 0x53,  // Get ID Command
                kWrite = 0x57,  // Write Command
                kNone = 0x00,   // No command, idle state
                kError = 0xFF,  // Bad command

                // https://gitlab.com/chriz2600/ps1-game-id-transmission
                // PS1 Game ID transmisson (MemCard PRO / PS1Digital)
                // To-do: Some commands may require ACK skip behavior?
                // To-do: Consider disabling some commands/implementations as non-applicable
                kPing = 0x20,            // Ping Command
                kGameID = 0x21,          // GAMEID
                kPreviousChannel = 0x22, // mount the previous channel of the Virtual Memory Card
                kNextChannel = 0x23,     // mount the next channel of the Virtual Memory Card
                kPreviousCard = 0x24,    // mount the previous folder (card)
                kNextCard = 0x25,        // mount the next folder (card)
                kGetCardName = 0x26,     // pass the name of the currently mounted card
                kReset = 0x27            // MMCE Reset
            };

            enum Responses : uint8_t
            {
                kIdleHighZ = 0xFF,           // High default state
                kDummy = 0x00,               // Filler Data
                kID1 = 0x5A,                 // Memory Card ID1
                kID2 = 0x5D,                 // Memory Card ID2
                kCommandAcknowledge1 = 0x5C, // Command Acknowledge 1
                kCommandAcknowledge2 = 0x5D, // Command Acknowledge 2
                kGoodReadWrite = 0x47,       // Good Read/Write
                kBadChecksum = 0x4E,         // Bad Checksum during Write
                kBadSector = 0xFF,           // Bad Memory Card Sector
                                             // https://gitlab.com/chriz2600/ps1-game-id-transmission
                                             // PS1 Game ID transmisson (MemCard PRO / PS1Digital)
                kSuccess = 0x20,             // Success
                kCardPresent = 0x27,         // Card Present
                kReserved = 0x00,            // Reserved
                kTerminationSignal = 0xFF    // Termination Signal

            };

            extern uint8_t memory_card_ram[131072];

            // Per-sector change counter. Only the SIO core (Commit) writes it; the storage task compares it
            // against the version it last saved to the SD card, so it can write only the sectors that changed
            // and never loses a write that happens while a flush is in progress.
            extern uint16_t sector_version[1024];

            extern uint8_t flag;
            extern uint16_t sector;

            extern uint8_t game_id[256];
            extern uint8_t game_id_length;
            extern volatile bool game_id_updated;

            extern uint64_t volatile last_tick;
            extern uint64_t volatile last_write_tick;
            extern bool committed_to_storage;
            extern bool skip_ack;

            struct McTraceEntry
            {
                uint32_t event_tick;
                uint8_t addr;
                uint8_t cmd;
                uint8_t flag_sent;
                uint8_t status;
                uint8_t abort_reason;
                uint16_t ticks;
                uint16_t sector;
                uint8_t chk_in;
                uint8_t chk_out;
            };

            extern McTraceEntry trace_ring[32];
            extern volatile uint32_t trace_write_idx;
            extern volatile uint32_t trace_read_idx;
            extern volatile uint32_t pad_poll_count;
            extern volatile uint32_t abort_byte0_count;

            void Commit();
            void GoIdle();
            uint8_t ProcessEvents(uint8_t);
            uint8_t TickReadCommand(uint8_t &DataIn);
            uint8_t TickWriteCommand(uint8_t &DataIn);

            // https://gitlab.com/chriz2600/ps1-game-id-transmission
            // PS1 Game ID transmisson (MemCard PRO / PS1Digital)
            uint8_t TickGameIDCommand(uint8_t &DataIn);
            uint8_t TickPingCommand(uint8_t &DataIn);
            uint8_t TickGetIDCommand(uint8_t &DataIn);
            uint8_t TickResetCommand(uint8_t &DataIn);
            uint8_t TickChannelCommand(uint8_t &DataIn);
        } // namespace memory_card

    } //namespace sio

} // namespace openmc

#endif //SIO_MEMORY_CARD_H