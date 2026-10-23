#include "playstation/sio.h"

#include "pins.h"
#include "playstation/sio_controller.h"
#include "playstation/sio_memory_card.h"
#include "playstation/sio_net_yaroze.h"
#include "playstation/spi.h"

#include <esp32/rom/ets_sys.h>
#include <soc/gpio_struct.h>

namespace openmc
{
    namespace sio
    {
        uint8_t DRAM_ATTR current_command = PS1_SIOCommands::Idle;

        bool DRAM_ATTR memory_card_enabled = true;
        bool DRAM_ATTR pad_enabled = false;
        bool DRAM_ATTR net_yaroze_enabled = false;

        uint64_t DRAM_ATTR event_ticks;

        void IRAM_ATTR GoIdle()
        {
            // Reset emulated device commands/variables
            controller::GoIdle();
            memory_card::GoIdle();
            net_yaroze::GoIdle();
        }

        void IRAM_ATTR Init()
        {
            event_ticks = 0;
            memory_card::last_write_tick = 0;
            GoIdle();
        }

        void IRAM_ATTR ProcessEvents()
        {
            uint8_t data_in = 0x00;
            uint8_t data_out = 0xFF;

            // If ignore, do nothing
            if (current_command == PS1_SIOCommands::Ignore)
            {
                return;
            }
            else // Enter SIO Loop
            {

                // Check SPI status register
                // Store incoming data to variable
                data_in = spi::SPDR;

                // Nothing done yet, check first address byte before driving MISO
                if (current_command == PS1_SIOCommands::Idle || current_command == PS1_SIOCommands::Wait)
                {
                    current_command = data_in;
                    if ((current_command == PS1_SIOCommands::MC_Access && memory_card_enabled) ||
                        (current_command == PS1_SIOCommands::PAD_Access && pad_enabled) ||
                        (current_command == PS1_SIOCommands::NY_Access && net_yaroze_enabled))
                    {
                        // Double check SEL_MC1 is genuinely LOW before driving MISO!
                        // If SEL is HIGH, this address byte was meant for Slot 2!
                        if ((GPIO_REG_READ(GPIO_IN1_REG) & kSEL_Bitmask) == 0)
                        {
                            spi::ActiveMode();
                        }
                        else
                        {
                            current_command = PS1_SIOCommands::Ignore;
                            spi::Disable();
                            return;
                        }
                    }
                }

                // Interpret incoming command
                switch (current_command)
                {

                // Console requests memory card, continue interpreting command
                case PS1_SIOCommands::MC_Access:
                    if (memory_card_enabled)
                    {
                        data_out = memory_card::ProcessEvents(data_in);
                    }
                    else
                    {
                        current_command = PS1_SIOCommands::Ignore;
                        spi::Disable();
                        return;
                    }

                    break;

                case PS1_SIOCommands::PAD_Access:
                    memory_card::pad_poll_count++;
                    if (pad_enabled)
                    {
                        data_out = controller::ProcessEvents(data_in);
                    }
                    else
                    {
                        current_command = PS1_SIOCommands::Ignore;
                        spi::Disable();
                        return;
                    }
                    break;

                case PS1_SIOCommands::NY_Access:
                    if (net_yaroze_enabled)
                    {
                        data_out = net_yaroze::ProcessEvents(data_in);
                    }
                    else
                    {
                        current_command = PS1_SIOCommands::Ignore;
                        spi::Disable();
                        return;
                    }
                    break;

                default: // Bad/Unexpected/Unsupported slave select command
                    current_command = PS1_SIOCommands::Ignore;
                    spi::Disable();
                    return;
                }

                // Push outbound data to the SPI Data Register
                // Data will be transferred in the next byte pair
                spi::SPDR = data_out;
                if (spi::enabled && current_command != PS1_SIOCommands::Ignore && !memory_card::skip_ack)
                {
                    if (spi::SendAck() == false) // SendAck returns false if ack not sent, i.e. slave no longer selected
                    {
                        spi::selected = false;
                    }
                }
            }
        }

        void IRAM_ATTR TickEventCounter()
        {
            event_ticks++;
            // Roll-over detection
            if (event_ticks == 0)
            {
                memory_card::last_write_tick = 0;
            }
        }

        void SetMemoryCardEmulation(bool enable)
        {
            if (enable)
            {
                current_command = PS1_SIOCommands::Idle;
                GoIdle();
                memory_card_enabled = true;
                spi::Enable();
                // Clear any pending interrupt on SEL pin before enabling
                REG_WRITE(GPIO_STATUS1_W1TC_REG, (1ULL << (kSEL_Pin - 32)));
                GPIO.pin[kSEL_Pin].int_ena = BIT(0);
            }
            else
            {
                memory_card_enabled = false;
                spi::Disable();
                GPIO.pin[kSEL_Pin].int_ena = 0;
                // Clear any pending interrupt on SEL pin while disabled
                REG_WRITE(GPIO_STATUS1_W1TC_REG, (1ULL << (kSEL_Pin - 32)));
                current_command = PS1_SIOCommands::Idle;
                GoIdle();
            }
        }
    } // namespace sio
} // namespace openmc