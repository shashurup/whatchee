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
#include <time.h>

#include "main_queue.h"
#include "ble.h"
#include "misc_hw.h"
#include "binaryttf.h"

static const char* TAG = "main";

#define NOTIFICATIONS_BUFFER_SIZE 2048

struct NotificationBuffer {
  char* current;
  char* top;
  char buffer[NOTIFICATIONS_BUFFER_SIZE];

  void add(const char* notification) {
    if (!top)
      top = buffer;
    size_t len = strlen(notification);
    const char* end = buffer + NOTIFICATIONS_BUFFER_SIZE;
    if (top + len + 1 > end) {
      for (char* c = top; c < end; c++)
        *c = 0;
      top = buffer;
    }
    bool overlap = top[len];
    current = strcpy(top, notification);
    top += len + 1;
    if (overlap)
      for (char* c = top; *c && c < end; c++)
        *c = 0;
  }

  void clear() {
    current = 0;
    top = buffer;
  }

  void prev() {
    if (!current)
      return;
    char *c = current;
    if (c == buffer)
      c = buffer + NOTIFICATIONS_BUFFER_SIZE - 1;
    for (; !*c && c >= buffer; --c);
    for (; *c && c >= buffer; --c);
    current = c;
  }

  void next() {
    if (!current)
      return;
    const char* end = buffer + NOTIFICATIONS_BUFFER_SIZE;
    char *c = current;
    for (; *c && c < end; ++c);
    for (; !*c && c < end; ++c);
    if (c < end)
      current = c;
    else
      current = buffer;
  }
};

EpdSpi io;
Gdeh0154d67 display(io);
OpenFontRender fontRender;
NotificationBuffer notifications;

void update_current_time(tm* subj) {
  set_rtc_time(subj);
}

void new_notification(Notification* subj) {
  notifications.add(subj->text);
  delete subj;
}

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

  struct tm now;
  bool valid = get_rtc_time(&now);
  ESP_LOGI(TAG, "RTC time: %u-%u-%u %u:%u:%u (%d)",
           now.tm_year, now.tm_mon, now.tm_mday,
           now.tm_hour, now.tm_min, now.tm_sec, valid);

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
          break;
        case CLIENT_TIME: {
          tm* t = (tm *)msg.data;
          ESP_LOGD(TAG, "Got time: %u-%u-%u %u:%u:%u",
                   t->tm_year, t->tm_mon, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
          update_current_time(t);
          delete t;
          break;
        }
        case CLIENT_NOTIFICATION:
          new_notification((Notification *)msg.data);
          break;
        }
      }
      idle_tasks();
    }
    else 
      idle_tasks();
    // uart_wait_tx_done(UART_NUM_2, 200 / portTICK_PERIOD_MS);
  }
}
