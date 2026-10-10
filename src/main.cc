#include "gui/gui.h"
#include "logging.h"
#include "oled/ssd1306.h"
#include "pins.h"
#include "playstation/sio.h"
#include "playstation/sio_memory_card.h"
#include "playstation/spi.h"
#include "storage/config.h"
#include "storage/storage.h"
#include "system/settings.h"
#include "system/timer.h"
#include "touch_input/touch.h"
#include "web/file_server.h"
#include "wifi/access_point.h"
#include "wifi/client.h"
#include "wifi/wifi.h"

#include "baremetal_core1/core0_stall.h"
#include "baremetal_core1/bare_metal_app_cpu.h"

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_attr.h>
#include <esp_intr_alloc.h>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <sdkconfig.h>
#include <stdio.h>

// Task sizes - there should be approximately 5% free space to allow for growth
//#define FILE_WRITE_TASK_SIZE 1760
#define GUI_TASK_SIZE 2048
#define TOUCH_INPUT_TASK_SIZE 2048
#define FILE_WRITE_TASK_SIZE 3096

//#define STARTUP_DELAY_MS 500

// rama
// sickle <3
// Nicolas Noble
// danhans42

extern "C"
{
    void app_main(void);
}

namespace openmc
{
    void main(void);
    void SetupInterrupts();

    void SetupInterrupts()
    {
        spi::InstallInterrupt();
    }

    void main(void)
    {
        ESP_LOGI(kLogPrefix, "=== OpenMC ULTIMATE PS1 Starting ===");
        sio::memory_card_enabled = true;
        sio::pad_enabled = false;
        sio::net_yaroze_enabled = false;

        // Turn off LED
        // To-do: Illuminate when card image is uncommitted to storage
        gpio_set_direction(kPin_LED, GPIO_MODE_OUTPUT);
        gpio_set_level(kPin_LED, 0);

        system::timer::Init();

#ifndef ENABLE_OLED
#define ENABLE_OLED 0
#endif

#if ENABLE_OLED
        gpio_set_direction(kOLEDPin_Backlight, GPIO_MODE_OUTPUT);
        gpio_set_level(kOLEDPin_Backlight, 0);

        oled::Init(); // Init oled screen
        xTaskCreatePinnedToCore(gui::Task_UpdateScreen, "screen_update_task_core_0", GUI_TASK_SIZE, NULL, 0, NULL, 0);

        touch_input::SetCallback_LeftPressed(gui::Callback_Left);
        touch_input::SetCallback_ConfirmPressed(gui::Callback_Confirm);
        touch_input::SetCallback_RightPressed(gui::Callback_Right);
        xTaskCreatePinnedToCore(touch_input::Task_TouchInput, "touch_pad_input_task_core_0", TOUCH_INPUT_TASK_SIZE, NULL, 0, NULL, 0);
#endif

        // Load defaults first so WiFi and settings are ready
        system::LoadDefaultSettings();

        // Start Wi-Fi Access Point immediately so user can connect even without SD
        if (system::settings.wifi_mode == wifi::Mode::kAcessPoint)
        {
            ESP_LOGI(kLogPrefix, "Starting Wi-Fi Access Point (SSID: %s)...", system::settings.ssid);
            wifi::access_point::Init();
        }
        else if (system::settings.wifi_mode == wifi::Mode::kClient)
        {
            ESP_LOGI(kLogPrefix, "Connecting to Wi-Fi client...");
            wifi::client::Init();
        }

        storage::Init();

        // Auto-write task
        xTaskCreatePinnedToCore(storage::Task_Write, "write_task_core_0", FILE_WRITE_TASK_SIZE, NULL, 0, NULL, 0);

        sio::Init();       // Init the SIO state machine to a default state.
        spi::Init();       // Setup the pins for bitbanged SPI
        spi::Enable();     // Enable SPI
        SetupInterrupts(); // Create a task to install our interrupt handler on Core 1

        core0_stall_init();
        start_app_cpu();

        // Try loading config from /sdcard/config.txt, else defaults set above remain in place
        if (storage::ready)
        {
            storage::config::LoadConfig();
        }

        gpio_set_direction(kSDPin_Detect, GPIO_MODE_INPUT);
        gpio_set_pull_mode(kSDPin_Detect, GPIO_PULLUP_ONLY);

        if (system::settings.wifi_mode == wifi::Mode::kClient || system::settings.wifi_mode == wifi::Mode::kAcessPoint)
        {
            while (wifi::ready == false || storage::ready == false)
            {
                ESP_LOGI(kLogPrefix, "Waiting for SD card to mount before launching Web Server... (wifi=%d, storage=%d)", wifi::ready, storage::ready);
                vTaskDelay(3000 / portTICK_PERIOD_MS);
            }

            ESP_ERROR_CHECK(web::file_server::start_file_server(storage::base_path));
            ESP_LOGI(kLogPrefix, "Web File Server started at http://192.168.4.1 !");
        }

        ESP_LOGI(kLogPrefix, "Minimum Free Heap = %i\n", esp_get_minimum_free_heap_size());
    }
}

extern "C" void app_main(void)
{
    printf("\n\n>>> OpenMC app_main entered successfully! <<<\n\n");
    openmc::main();
}