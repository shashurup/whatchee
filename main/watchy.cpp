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
#include <time.h>

#include "main_queue.h"
#include "ble.h"
#include "misc_hw.h"
#include "binaryttf.h"
#include "utils.h"

static const char* TAG = "main";

#define MAIN_SCREEN 0
#define NOTIFICATION_SCREEN 1

EpdSpi io;
Gdeh0154d67 display(io);
MyFontRender font_renderer;
NotificationBuffer notifications;
int time_sync_day = 0;
struct tm display_time;
int screen = MAIN_SCREEN;
char* displayed_notification = 0;


void sync_current_time(tm* subj) {
  if (subj->tm_mday != time_sync_day) {
    time_sync_day = subj->tm_mday;
    set_rtc_time(subj);
  }
}

void draw_main_screen(tm* time, uint8_t battery, bool refresh) {
  display.fillScreen(EPD_WHITE);
  char hour_min[6];
  sprintf(hour_min, "%02d:%02d", time->tm_hour, time->tm_min);
  font_renderer.setFontSize(64);
  uint32_t hm_width = font_renderer.getTextWidth(hour_min);
  uint32_t hm_height = font_renderer.getTextHeight(hour_min);
  ESP_LOGI(TAG, "hm_width: %lu, hm_height: %lu", hm_width, hm_height);
  uint32_t hm_x = (int32_t) (GDEH0154D67_WIDTH - hm_width) / 2;
  uint32_t hm_y = 40;
  font_renderer.drawString(hour_min, hm_x, hm_y, 0, 0xffff);

  char day_month[11];
  strftime(day_month, 11, "%a %e %b", time);
  font_renderer.setFontSize(32);
  uint32_t dm_width = font_renderer.getTextWidth(day_month);
  uint32_t dm_height = font_renderer.getTextHeight(day_month);
  font_renderer.drawString(day_month,
                           (int32_t)(200 - dm_width) / 2,
                           60 + hm_height,
                           0, 0xffff);

  font_renderer.setCursor(20, 60 + hm_height + 15 + dm_height);
  font_renderer.setFontSize(24);
  font_renderer.printf("%u", battery);
  if (refresh)
    display.update();
  else {
    // uint32_t upd_x = 0;
    // uint32_t upd_y = 0;
    // uint32_t upd_w = GDEH0154D67_WIDTH;
    // uint32_t upd_h = GDEH0154D67_HEIGHT;
    // only last digit by defalt
    // uint32_t upd_x = hm_x + hm_width * 3 / 4;
    uint32_t upd_x = hm_x + hm_width * 3 / 4 + 5;
    uint32_t upd_y = hm_y;
    uint32_t upd_w = hm_width / 4;
    // uint32_t upd_h = hm_height;
    uint32_t upd_h = hm_height + 15;
    if (display_time.tm_hour != time->tm_hour) {
      // the whole screen each hour
      upd_x = 0;
      upd_y = 0;
      upd_w = GDEH0154D67_WIDTH;
      upd_h = GDEH0154D67_HEIGHT;
    } else if (display_time.tm_min / 10 != time->tm_min / 10) {
      // last two digits every 10 minutes
      upd_x = hm_x + hm_width / 2;
      // upd_w = hm_width / 2;
      upd_w = hm_width / 2 + 5;
    }
    ESP_LOGI(TAG, "upd_x: %lu, upd_y: %lu, upd_w: %lu, upd_h: %lu",
             upd_x, upd_y, upd_w, upd_h);
    display.updateWindow(upd_x, upd_y, upd_w, upd_h, false);
  }
}

void draw_notifications() {
  display.fillScreen(EPD_WHITE);
  font_renderer.setCursor(5, 5);
  font_renderer.setFontSize(20);
  font_renderer.drawStringBreakLines(notifications.current,
                                     5, 5, 190, 190);
  display.updateWindow(0, 0, GDEH0154D67_WIDTH, GDEH0154D67_HEIGHT, false);
}

void preprocess_notification(Notification* subj) {
  for (int i = 0; i < strlen(subj->text); ++i) {
    char ch = subj->text[i];
    if (ch == '\n' || ch == '\r' || ch == '\t') {
      subj->text[i] = ' ';
    }
  }
}

void new_notification(Notification* subj) {
  ESP_LOGI(TAG, "New notification: %s", subj->text);
  preprocess_notification(subj);
  notifications.add(subj->text);
  delete subj;
  vibrate(75, 6);
}

void idle_tasks() {
  if (screen == NOTIFICATION_SCREEN &&
      notifications.current &&
      notifications.current != displayed_notification) {
    draw_notifications();
    display.deepSleep();
    displayed_notification = notifications.current;
    display_time = {};
  }
  else if (screen == MAIN_SCREEN) {
    struct tm now;
    bool valid = get_rtc_time(&now);
    if (valid && now.tm_hour != display_time.tm_hour) {
      send_battery(get_battery_level());
    }
    if (valid && now.tm_min != display_time.tm_min) {
      ESP_LOGI(TAG, "Updating time");
      draw_main_screen(&now, get_battery_level(), false);
      display_time = now;
      display.deepSleep();
    }
  }
}

void setup_pm() {
  esp_pm_config_t pm_config = {
    .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    .min_freq_mhz = CONFIG_XTAL_FREQ,
    // .light_sleep_enable = false
    .light_sleep_enable = true
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

  bool valid = get_rtc_time(&display_time);
  ESP_LOGI(TAG, "RTC time: %u-%u-%u %u:%u:%u (%d)",
           display_time.tm_year, display_time.tm_mon, display_time.tm_mday,
           display_time.tm_hour, display_time.tm_min, display_time.tm_sec, valid);

  font_renderer.setDrawer(display);
  if (font_renderer.loadFont(binaryttf, sizeof(binaryttf)))
    ESP_LOGE(TAG, "Error loading font");
  display.init(false);
  font_renderer.setFontColor(0);

  if (valid) {
    draw_main_screen(&display_time, get_battery_level(), true);
    display.deepSleep();
  }

  // notifications.add("Довольно короткое сообщение №1");
  // notifications.add("Quite short test message 2\nwith line breaks и т.д. и т.р. and so on");
  // screen = NOTIFICATION_SCREEN;
  
  ESP_LOGI(TAG, "Battery voltage: %d", get_battery_millivolts());
  
  while(true) {
    struct Message msg;
    if (xQueueReceive(main_queue, &msg, 60 * 1000 / portTICK_PERIOD_MS) == pdTRUE) {
      if (!handle_misc_hw_events(msg)) {
        switch (msg.type) {
        case BUTTON_PRESSED:
          ESP_LOGI(TAG, "Button %u pressed", (unsigned)msg.data);
          switch ((unsigned)msg.data) {
          case BUTTON_BACK:
            screen = MAIN_SCREEN;
            break;
          case BUTTON_UP:
            screen = NOTIFICATION_SCREEN;
            notifications.prev();
            break;
          case BUTTON_DOWN:
            screen = NOTIFICATION_SCREEN;
            notifications.next();
            break;
          }
          break;
        case BUTTON_RELEASED:
          ESP_LOGI(TAG, "Button %u released", (unsigned)msg.data);
          if ((unsigned)msg.data == BUTTON_MENU) {
            ESP_LOGI(TAG, "Entering light sleep");
            vibrate(75, 6);
            esp_light_sleep_start();
          }
          break;
        case CLIENT_SUBSCRIBED:
          send_info();
          send_battery(get_battery_level());
          break;
        case CLIENT_TIME: {
          tm* t = (tm *)msg.data;
          ESP_LOGD(TAG, "Got time: %u-%u-%u %u:%u:%u",
                   t->tm_year, t->tm_mon, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
          sync_current_time(t);
          delete t;
          break;
        }
        case CLIENT_NOTIFICATION:
          new_notification((Notification *)msg.data);
          screen = NOTIFICATION_SCREEN;
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
