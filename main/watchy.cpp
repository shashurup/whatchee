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
#include <nvs_flash.h>
#include <time.h>

#include "main_queue.h"
#include "ble.h"
#include "misc_hw.h"
#include "utils.h"
#include "typography.h"
#include "ter_x20b_pcf20pt.h"
#include "ter_x32b_pcf32pt.h"
#include "c509_bold37pt.h"
#include "c509_bold29pt.h"

static const char* TAG = "main";

#define MAIN_SCREEN 0
#define NOTIFICATION_SCREEN 1
#define INFO_SCREEN 2
const int minute = 60 * 1000;

EpdSpi io;
Gdeh0154d67 display(io);
Typography typography(display);
NotificationBuffer notifications;
Battery battery;
int time_sync_day;
RTC_DATA_ATTR time_t boot_time = 0;
RTC_DATA_ATTR uint8_t prev_day = 0;
RTC_DATA_ATTR uint8_t prev_hour = 0;
RTC_DATA_ATTR uint8_t prev_minute = 0;
int screen;
bool screen_changed;
const char* displayed_notification;
bool connected;
bool prev_connected;
unsigned disconnect_count;
bool ringing;
bool sleeping;
const char *wdays[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
const char *months[] = {"Янв", "Фев", "Мар", "Апр", "Мая", "Июня",
                        "Июля", "Авг", "Сен", "Окт", "Ноя", "Дек"};


void sync_current_time(tm* subj) {
  if (subj->tm_mday != time_sync_day) {
    time_sync_day = subj->tm_mday;
    set_rtc_time(subj);
  }
}

void draw_main_screen(tm* time, bool valid) {
  display.fillScreen(EPD_WHITE);
  char hour_min[6] = "--:--";
  if (valid)
    sprintf(hour_min, "%02d:%02d", time->tm_hour, time->tm_min);
  typography.SetFont(&C059_Bold37pt[0],
                     sizeof(C059_Bold37pt)/ sizeof(C059_Bold37pt[0]));
  uint16_t y = 40;
  uint16_t hm_y = y;
  uint16_t hm_h = typography.PrintCentered(hour_min, y);
  y += hm_h;

  char day_month[11] = "---";
  if (valid)
    sprintf(day_month, "%s %d %s",
            wdays[time->tm_wday], time->tm_mday, months[time->tm_mon]);
  typography.SetFont(&ter_x32b_pcf32pt[0],
                     sizeof(ter_x32b_pcf32pt) / sizeof(ter_x32b_pcf32pt[0]));
  y += 12;
  y += typography.PrintCentered(day_month, y);

  typography.SetFont(&ter_x20b_pcf20pt[0],
                     sizeof(ter_x20b_pcf20pt) / sizeof(ter_x20b_pcf20pt[0]));
  y += 20;
  if (connected) {
    typography.SetCursor(20, y);
    char batt[10] = {};
    sprintf(batt, "%u (%u)",
            battery.get_level(), battery.get_discharge_rate());
    typography.Print(batt);
  }
  else if (sleeping)
    typography.PrintCentered("sleeping", y);
  else
    typography.PrintCentered("disconnected", y);

  if (valid && prev_day != time->tm_mday)
    display.update();
  else {
    // only last digit by defalt
    // TODO update only last digit
    uint32_t upd_x = GDEH0154D67_WIDTH / 2;
    uint32_t upd_w = GDEH0154D67_WIDTH / 2;
    uint32_t upd_y = hm_y;
    // add 15 pixel, for some reason lower part doesn't update
    uint32_t upd_h = hm_h + 15;
    if (!valid ||
        prev_hour != time->tm_hour ||
        screen_changed ||
        prev_connected != connected) {
      // the whole screen each hour
      upd_x = 0;
      upd_y = 0;
      upd_w = GDEH0154D67_WIDTH;
      upd_h = GDEH0154D67_HEIGHT;
    } else if (prev_minute / 10 != time->tm_min / 10) {
      // last two digits every 10 minutes
      upd_x = GDEH0154D67_WIDTH / 2;
      upd_w = GDEH0154D67_WIDTH / 2;
    }
    ESP_LOGD(TAG, "upd_x: %lu, upd_y: %lu, upd_w: %lu, upd_h: %lu",
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
  uint16_t y = 5;
  if (!code.empty()) {
    typography.SetFont(&C059_Bold29pt[0],
                       sizeof(C059_Bold29pt)/ sizeof(C059_Bold29pt[0]));
    y += typography.PrintCentered(code.c_str(), y);
    y += 10;
  }
  typography.SetFont(&ter_x20b_pcf20pt[0],
                     sizeof(ter_x20b_pcf20pt) / sizeof(ter_x20b_pcf20pt[0]));
  typography.FitText(notification, 5, y, 190, GDEH0154D67_HEIGHT - y - 5);
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
    ringing = true;
    notifications.add(subj->text);
    // TODO find better solution
    vibrate(100, 10);
    return true;
  } else if (subj->icon == 2) {
    ringing = false;
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

void draw_info() {
  display.fillScreen(EPD_WHITE);
  char buf[22];
  typography.SetFont(&ter_x20b_pcf20pt[0],
                     sizeof(ter_x20b_pcf20pt) / sizeof(ter_x20b_pcf20pt[0]));
  typography.SetCursor(5,5);
  if (boot_time) {
    struct tm* bt = localtime(&boot_time);
    strftime(buf, sizeof(buf), "boot: %d.%m %H:%M\n", bt);
    typography.Print(buf);
  }
  snprintf(buf, 22, "batt: %u%% (%d)\n",
           battery.get_level(), battery.get_voltage());
  typography.Print(buf);
  snprintf(buf, 22, "discharge: %u%%\n", battery.get_discharge_rate());
  typography.Print(buf);
  snprintf(buf, 22, "disconnects: %u\n", disconnect_count);
  typography.Print(buf);
  display.updateWindow(0, 0, GDEH0154D67_WIDTH, GDEH0154D67_HEIGHT, false);
}

bool sleeping_hours(struct tm& now) {
  return now.tm_hour < 8 || now.tm_hour >= 21;
}

void deep_sleep() {
  battery.flush();
  esp_sleep_enable_timer_wakeup(600000000); // 10 mins
  // esp_sleep_enable_ext1_wakeup(
  //     BTN_PIN_MASK,
  //     ESP_EXT1_WAKEUP_ANY_HIGH); // enable deep sleep wake on button press
  ESP_LOGI(TAG, "Deeply sleeping");
  esp_deep_sleep_start();
}

void idle_tasks() {
  struct tm now;
  bool valid = get_rtc_time(&now);
  if (valid) {
    battery.measure(&now);
    sleeping = sleeping_hours(now);
    if (now.tm_hour != prev_hour)
      send_battery(battery.get_level());
    if (!boot_time) {
      ESP_LOGI(TAG, "RTC time: %u-%u-%u %u:%u:%u (%d)",
               now.tm_year, now.tm_mon, now.tm_mday,
               now.tm_hour, now.tm_min, now.tm_sec, valid);
      boot_time = mktime(&now);
    }
  }
  else
    battery.measure(0);

  if (screen == NOTIFICATION_SCREEN) {
    if (screen_changed ||
        (notifications.get_current() &&
         notifications.get_current() != displayed_notification))
    draw_notifications();
    display.deepSleep();
    displayed_notification = notifications.get_current();
  }
  else if (screen == MAIN_SCREEN) {
    if (!valid ||
        now.tm_min != prev_minute ||
        prev_connected != connected ||
        screen_changed) {
      ESP_LOGI(TAG, "Updating main screen");
      draw_main_screen(&now, valid);
      display.deepSleep();
    }
  }
  else if (screen == INFO_SCREEN && screen_changed) {
    draw_info();
    display.deepSleep();
  }

  if (valid) {
    prev_day = now.tm_mday;
    prev_hour = now.tm_hour;
    prev_minute = now.tm_min;
  }
  screen_changed = false;

  if (ringing) {
    // TODO find better solution
    vibrate(100, 20);
    ringing = false;
  }
  if (sleeping)
    deep_sleep();
}

void setup_pm() {
  esp_pm_config_t pm_config = {
    .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    .min_freq_mhz = CONFIG_XTAL_FREQ,
    .light_sleep_enable = true
  };
  ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
  esp_sleep_enable_gpio_wakeup();
}

void setup_nvs() {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ESP_ERROR_CHECK(nvs_flash_init());
  }
}

extern "C" void app_main()
{
  ESP_LOGI(TAG, "Enter app_main()");

  time_sync_day = 0;
  screen = MAIN_SCREEN;
  screen_changed = true;
  displayed_notification = 0;
  connected = false;
  prev_connected = false;
  disconnect_count = 0;
  ringing = false;
  sleeping = false;
  bool refresh = false;

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause(); // get wake up reason
  ESP_LOGI(TAG, "Wakeup reason: %d", wakeup_reason);
  if (wakeup_reason == 0) {
    refresh = true;
    boot_time = 0;
    prev_day = 0;
    prev_hour = 0;
    prev_minute = 0;
  }
  setup_nvs();
  setup_misc_hw();
  display.init();
  display.setRefresh(refresh);
  idle_tasks();
  setup_pm();
  setup_main_queue();
  setup_ble("Whatcheee");

  // notifications.add("Довольно короткое сообщение №1 123456 code");
  // notifications.add("Quite short test message 2\nwith line breaks и т.д. и т.р. and so on");
  // screen = NOTIFICATION_SCREEN;
  
  ESP_LOGI(TAG, "Battery voltage: %d", battery.get_voltage());
  
  while(true) {
    struct Message msg;
    int interval = minute / portTICK_PERIOD_MS;
    if (xQueueReceive(main_queue, &msg, interval) == pdTRUE) {
      if (!handle_misc_hw_events(msg)) {
        switch (msg.type) {
        case BUTTON_PRESSED:
          ESP_LOGI(TAG, "Button %u pressed", (unsigned)msg.data);
          switch ((unsigned)msg.data) {
          case BUTTON_BACK:
            screen = MAIN_SCREEN;
            screen_changed = true;
            break;
          case BUTTON_UP:
            screen = NOTIFICATION_SCREEN;
            notifications.prev();
            break;
          case BUTTON_DOWN:
            screen = NOTIFICATION_SCREEN;
            notifications.next();
            break;
          case BUTTON_MENU:
            screen = INFO_SCREEN;
            screen_changed = true;
            break;
          }
          break;
        case BUTTON_RELEASED:
          ESP_LOGI(TAG, "Button %u released", (unsigned)msg.data);
          break;
        case CLIENT_SUBSCRIBED:
          send_info();
          send_battery(battery.get_level());
          break;
        case CLIENT_TIME: {
          tm* t = (tm *)msg.data;
          ESP_LOGI(TAG, "Got time: %u-%u-%u %u:%u:%u",
                   t->tm_year, t->tm_mon, t->tm_mday,
                   t->tm_hour, t->tm_min, t->tm_sec);
          sync_current_time(t);
          delete t;
          break;
        }
        case CLIENT_NOTIFICATION: {
          Notification *notif = (Notification *)msg.data;
          if (handle_notification(notif)) {
            screen = NOTIFICATION_SCREEN;
            screen_changed = true;
          }
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
