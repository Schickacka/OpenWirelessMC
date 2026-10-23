#ifndef _SPI_H
#define _SPI_H

#include "pins.h"

#include <stdint.h>
#include <driver/gpio.h>
#include <esp32/rom/ets_sys.h>

// Macro borrowed from Arduino wiring.h
#define lowByte(w) ((uint8_t)((w)&0xff))
#define highByte(w) ((uint8_t)((w) >> 8))

#define MAX_TIMEOUT_TICKS 0xFFFF // No idea how long this is, need to time/adjust

namespace openmc
{
    namespace spi
    {
        extern uint8_t SPDR;
        extern bool selected;
        extern bool enabled;
        extern uint8_t last_abort_reason;
        extern volatile uint32_t sel_glitch_count;

        uint32_t InterruptHandler(uint32_t cause);
        void ActiveMode();
        void Disable();
        void Enable();
        void Init();
        void InstallInterrupt();
        void PassiveMode();

        // Confirm SEL is genuinely HIGH (debounced against capacitive crosstalk on GPIO35)
        inline bool IRAM_ATTR ConfirmUnselected()
        {
            for (int k = 0; k < 8; k++)
            {
                if ((GPIO_REG_READ(GPIO_IN1_REG) & kSEL_Bitmask) == 0)
                {
                    sel_glitch_count++;
                    return false;
                }
            }
            return true;
        }

        inline bool IRAM_ATTR SendAck()
        {
            // Delay ~10uS from last clock pulse
            for (int i = 0; i < 84; i++)
            {
                if (GPIO_REG_READ(GPIO_IN1_REG) & kSEL_Bitmask)
                {
                    last_abort_reason = 50;
                    // Return false, ack not sent
                    return false;
                }
            }

            // Drop ACK to GND (open-drain: data=0, enable output)
            REG_WRITE(GPIO_OUT1_W1TC_REG, kACK_Bitmask);
            REG_WRITE(GPIO_ENABLE1_W1TS_REG, kACK_Bitmask);

            // Delay ~2uS
            for (int i = 0; i < 20; i++)
            {
                if (GPIO_REG_READ(GPIO_IN1_REG) & kSEL_Bitmask)
                {
                    last_abort_reason = 51;
                    // Device not selected anymore, break early
                    break;
                }
            }

            // Release ACK back to High-Z (open-drain release: disable output)
            REG_WRITE(GPIO_ENABLE1_W1TC_REG, kACK_Bitmask);

            // Return true, ack sent
            return true;
        }

        // Inlined for speed
        inline uint8_t IRAM_ATTR Transceive(uint8_t data_out)
        {
            // Bit hacky, but, if disabled, abort up the chain
            if (!enabled)
            {
                last_abort_reason = 1;
                selected = false;
                return 0xFF;
            }
            uint8_t data_in = 0x00;

            uint32_t timeout_ticks;

            for (int bitPos = 0; bitPos < 8; bitPos++)
            {
                timeout_ticks = 0;

                // Wait for clock to go low
                while (true)
                {
                    uint32_t in1 = GPIO_REG_READ(GPIO_IN1_REG);
                    if ((in1 & kCLK_Bitmask) == 0)
                    {
                        break;
                    }
                    // Abort if genuinely unselected
                    if ((in1 & kSEL_Bitmask) && ConfirmUnselected())
                    {
                        last_abort_reason = (bitPos == 0) ? 2 : (10 + bitPos);
                        selected = false;
                        return 0xFF;
                    }

                    timeout_ticks++;
                    if (timeout_ticks >= MAX_TIMEOUT_TICKS)
                    {
                        last_abort_reason = 20 + bitPos;
                        selected = false;
                        return 0xFF;
                    }
                }

                // Write bit out immediately while clock is low
                if (data_out & (1 << bitPos))
                {
                    // if bit is set, write to the bit set register
                    REG_WRITE(GPIO_OUT1_W1TS_REG, kMISO_Bitmask);
                }
                else
                {
                    // if bit is clear, write to the bit clear register
                    REG_WRITE(GPIO_OUT1_W1TC_REG, kMISO_Bitmask);
                }

                // Reset timeout counter before entering next loop cycle
                timeout_ticks = 0;

                // Wait for clock to go high
                uint32_t in1;
                while (true)
                {
                    in1 = GPIO_REG_READ(GPIO_IN1_REG);
                    if ((in1 & kCLK_Bitmask) != 0)
                    {
                        break;
                    }
                    // Abort if genuinely unselected
                    if ((in1 & kSEL_Bitmask) && ConfirmUnselected())
                    {
                        last_abort_reason = 30 + bitPos;
                        selected = false;
                        return 0xFF;
                    }

                    timeout_ticks++;
                    if (timeout_ticks >= MAX_TIMEOUT_TICKS)
                    {
                        last_abort_reason = 40 + bitPos;
                        selected = false;
                        return 0xFF;
                    }
                }

                // Store current bit state from rising edge snapshot
                if (in1 & kMOSI_Bitmask)
                {
                    data_in |= (1 << bitPos);
                }
            }

            return data_in;
        }
    }
}

#endif // _SPI_H