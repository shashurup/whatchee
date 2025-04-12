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
const char* displayed_notification = 0;
Battery battery;
bool connected = false;
bool prev_connected = true;
unsigned disconnect_count = 0;


void sync_current_time(tm* subj) {
  if (subj->tm_mday != time_sync_day) {
    time_sync_day = subj->tm_mday;
    set_rtc_time(subj);
  }
}

void draw_main_screen(tm* time, bool valid, bool refresh) {
  display.fillScreen(EPD_WHITE);
  char hour_min[6] = "--:--";
  if (valid)
    sprintf(hour_min, "%02d:%02d", time->tm_hour, time->tm_min);
  font_renderer.setFontSize(64);
  uint32_t hm_h = font_renderer.getTextHeight(hour_min);
  uint32_t hm_y = 40;
  font_renderer.drawStringCentered(hour_min, hm_y, GDEH0154D67_WIDTH);

  char day_month[11];
  strftime(day_month, 11, "%a %e %b", time);
  font_renderer.setFontSize(32);
  uint32_t dm_h = font_renderer.getTextHeight(day_month);
  if (valid)
    font_renderer.drawStringCentered(day_month, 60 + hm_h, GDEH0154D67_WIDTH);

  font_renderer.setCursor(20, 60 + hm_h + 15 + dm_h);
  font_renderer.setFontSize(24);
  if (connected)
    font_renderer.printf("  %u (%u)",
                         battery.get_level(),
                         battery.get_discharge_rate());
  else
    font_renderer.printf("not connected");

  if (refresh || display_time.tm_mday != time->tm_mday)
    display.update();
  else {
    // only last digit by defalt
    // TODO update only last digit
    uint32_t upd_x = GDEH0154D67_WIDTH / 2;
    uint32_t upd_w = GDEH0154D67_WIDTH / 2;
    uint32_t upd_y = hm_y;
    // add 15 pixel, for some reason lower part doesn't update
    uint32_t upd_h = hm_h + 15;
    if (display_time.tm_hour != time->tm_hour ||
        prev_connected != connected) {
      // the whole screen each hour
      upd_x = 0;
      upd_y = 0;
      upd_w = GDEH0154D67_WIDTH;
      upd_h = GDEH0154D67_HEIGHT;
    } else if (display_time.tm_min / 10 != time->tm_min / 10) {
      // last two digits every 10 minutes
      upd_x = GDEH0154D67_WIDTH / 2;
      upd_w = GDEH0154D67_WIDTH / 2;
    }
    ESP_LOGI(TAG, "upd_x: %lu, upd_y: %lu, upd_w: %lu, upd_h: %lu",
             upd_x, upd_y, upd_w, upd_h);
    display.updateWindow(upd_x, upd_y, upd_w, upd_h, false);
  }
}

// find from 4 up to 6 consequtive digits
// treat them like 2FA code
std::string find_code(const char* subj) {
  std::string result;
  for (const char* p = subj; *p; ++p) {
    if (isdigit(*p))
      result += *p;
    else {
      if (result.size() <= 6 && result.size() >= 4)
        return result;
      result.clear();
    }
  }
  if (result.size() < 4 || result.size() > 6)
    result.clear();
  return result;
}

void draw_notifications() {
  display.fillScreen(EPD_WHITE);
  const char* notification = notifications.get_current();
  auto code = find_code(notification);
  uint32_t y = 5;
  if (!code.empty()) {
    font_renderer.setFontSize(48);
    uint32_t h = font_renderer.getTextHeight(code.c_str());
    font_renderer.drawStringCentered(code.c_str(), y, GDEH0154D67_WIDTH);
    y += h + 5;
  }
  font_renderer.setFontSize(20);
  font_renderer.drawStringBreakLines(notification,
                                     5, y, 190, GDEH0154D67_HEIGHT - y - 5);
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

bool notification_is_empty(Notification* subj) {
  for (const char* p = subj->text; *p; ++p)
    if (!isspace(*p))
      return false;
  return true;
}

bool handle_notification(Notification* subj) {
  ESP_LOGI(TAG, "New notification, icon: %u, state: %u, %s",
           subj->icon, subj->state, subj->text);
  if (subj->icon == 1) {
    //incoming call
  } else if (subj->icon == 2) {
    // cancel incomint call
    return false; // its empty no need to display
  }
  preprocess_notification(subj);
  if (!notification_is_empty(subj)) {
    notifications.add(subj->text);
    vibrate(75, 6);
    return true;
  }
  return false;
}

void idle_tasks() {
  battery.measure();
  if (screen == NOTIFICATION_SCREEN &&
      notifications.get_current() &&
      notifications.get_current() != displayed_notification) {
    draw_notifications();
    display.deepSleep();
    displayed_notification = notifications.get_current();
    // provoke whole screen update when back to main screen
    display_time.tm_hour = -1;
  }
  else if (screen == MAIN_SCREEN) {
    struct tm now;
    bool valid = get_rtc_time(&now);
    if (valid && now.tm_hour != display_time.tm_hour) {
      send_battery(battery.get_level());
    }
    if (now.tm_min != display_time.tm_min) {
      ESP_LOGI(TAG, "Updating time");
      draw_main_screen(&now, valid, false);
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

  draw_main_screen(&display_time, valid, true);
  display.deepSleep();

  // notifications.add("Довольно короткое сообщение №1");
  // notifications.add("Quite short test message 2\nwith line breaks и т.д. и т.р. and so on");
  // screen = NOTIFICATION_SCREEN;
  
  ESP_LOGI(TAG, "Battery voltage: %d", battery.get_voltage());
  
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
          send_battery(battery.get_level());
          break;
        case CLIENT_TIME: {
          tm* t = (tm *)msg.data;
          ESP_LOGD(TAG, "Got time: %u-%u-%u %u:%u:%u",
                   t->tm_year, t->tm_mon, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
          sync_current_time(t);
          delete t;
          break;
        }
        case CLIENT_NOTIFICATION: {
          Notification *notif = (Notification *)msg.data;
          if (handle_notification(notif))
            screen = NOTIFICATION_SCREEN;
          delete notif;
          break;
        }
        case CLIENT_FIND:
          vibrate(50, 10);
          break;
        case CLIENT_CONNECTED:
          prev_connected = connected;
          connected = true;
          break;
        case CLIENT_DISCONNECTED:
          prev_connected = connected;
          connected = false;
          ++disconnect_count;
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
