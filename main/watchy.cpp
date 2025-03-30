#include <stdio.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "epd.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <epdspi.h>
#include <gdeh0154d67.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <OpenFontRender.h>

#include "main_queue.h"
#include "ble.h"
#include "misc_hw.h"
#include "binaryttf.h"

static const char* TAG = "main";

EpdSpi io;
Gdeh0154d67 display(io);
OpenFontRender fontRender;

void idle_tasks() {
  // check screen needs updating
  // check battery level needs to be sent
}

void setup_pm() {
  esp_pm_config_t pm_config = {
    .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    .min_freq_mhz = CONFIG_XTAL_FREQ,
    .light_sleep_enable = false
    // .light_sleep_enable = true
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
  esp_sleep_enable_gpio_wakeup();
}

extern "C" void app_main()
{
  ESP_LOGI(TAG, "Enter app_main()");

  setup_main_queue();
  setup_ble("Whatcheee");
  setup_misc_hw();
  setup_pm();

  fontRender.setDrawer(display);
  if (fontRender.loadFont(binaryttf, sizeof(binaryttf)))
    ESP_LOGE(TAG, "Error loading font");

  display.init(false);
  display.fillScreen(EPD_WHITE);
  display.setTextColor(EPD_BLACK);
  // display.setFont(&FreeSans12pt7b);
  // display.println("");
  // display.println("Hi, I'm Whatcheee");
  // display.println("");
  // display.setFont(&FreeMonoBold24pt7b);
  // display.println(" 20:59");

  fontRender.setFontColor(0);
  fontRender.setFontSize(20);
  fontRender.printf("Привет, Whatchee!!!\n");
  fontRender.setFontSize(64);
  fontRender.printf("20:59");

  display.update();
  display.deepSleep();

  ESP_LOGI(TAG, "Battery voltage: %d", get_battery_millivolts());
  
  while(true) {
    struct Message msg;
    if (xQueueReceive(main_queue, &msg, 60 * 1000 / portTICK_PERIOD_MS) == pdTRUE) {
      if (!handle_misc_hw_events(msg)) {
        switch (msg.type) {
        case BUTTON_PRESSED:
          ESP_LOGI(TAG, "Button %u pressed", (unsigned)msg.data);
          break;
        case BUTTON_RELEASED:
          ESP_LOGI(TAG, "Button %u released", (unsigned)msg.data);
          if ((unsigned)msg.data == BUTTON_BACK) {
            ESP_LOGI(TAG, "Entering light sleep");
            vibrate(75, 6);
            esp_light_sleep_start();
          }
          break;
        case CLIENT_SUBSCRIBED:
          send_info();
          send_battery(get_battery_millivolts());
        }
        }
      idle_tasks();
    }
    else 
      idle_tasks();
    // uart_wait_tx_done(UART_NUM_2, 200 / portTICK_PERIOD_MS);
  }
}
