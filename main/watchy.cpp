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
#include "ter_x28b_pcf28pt.h"
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
RTC_DATA_ATTR time_t boot_time = 0;
RTC_DATA_ATTR uint32_t step_base = 0;
const char *wdays[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
const char *months[] = {"Янв", "Фев", "Мар", "Апр", "Мая", "Июня",
                        "Июля", "Авг", "Сен", "Окт", "Ноя", "Дек"};

#define MINUTE_CHANGED 1
#define DECADE_CHANGED 2
#define HOUR_CHANGED 4
#define DAY_CHANGED 8
#define SCREEN_CHANGED 16
#define CONNECTION_CHANGED 32

#define NOTHING 0
#define LAST_DIGIT 1
#define LAST_TWO_DIGITS 2
#define WHOLE_SCREEN 3
#define REFRESH_SCREEN 4

void sync_current_time(tm* subj, int* ts_day) {
  if (subj->tm_mday != *ts_day) {
    *ts_day = subj->tm_mday;
    set_rtc_time(subj);
  }
}

// index of the minutes units digit in the "HH:MM" string
#define MIN_UNIT_IDX 4

void draw_main_screen(tm* time,
                      bool is_time_valid,
                      bool is_sleeping,
                      uint32_t step_counter,
                      uint8_t battery_level,
                      bool connected,
                      int update_mode) {
  if (update_mode == NOTHING)
    return;
  display.fillScreen(EPD_WHITE);
  char hour_min[] = "--:--";
  if (is_time_valid) {
    sprintf(hour_min, "%02d:%02d", time->tm_hour, time->tm_min);
    // During deep sleep the screen is updated only every 10 minutes,
    // so the units of minutes are unknown most of the time
    if (is_sleeping)
      hour_min[MIN_UNIT_IDX] = '-';
  }
  SET_FONT(typography, C059_Bold37pt);
  uint16_t y = 40;
  uint16_t hm_y = y;
  uint16_t hm_h = typography.PrintCentered(hour_min, y);
  y += hm_h;

  display.drawRect(10, 104, 180, 5, EPD_BLACK);
  display.fillRect(10, 105, 180 * battery_level / 100, 4, EPD_BLACK);
  
  char day_month[11] = "---";
  if (is_time_valid)
    sprintf(day_month, "%s %d %s",
            wdays[time->tm_wday], time->tm_mday, months[time->tm_mon]);
  SET_FONT(typography, ter_x32b_pcf32pt);
  y += 20;
  y += typography.PrintCentered(day_month, y);

  y += 10;
  if (is_sleeping)
    typography.PrintCentered("сон", y);
  else if (!connected)
    typography.PrintCentered("разрыв", y);
  else {
    char counter_str[20];
    sprintf(counter_str, "%lu", step_counter);
    typography.PrintCentered(counter_str, y);
  }

  if (update_mode == REFRESH_SCREEN)
    display.update();
  else {
    // only last digit by defalt
    // TODO update only last digit
    uint32_t upd_x = GDEH0154D67_WIDTH / 2;
    uint32_t upd_w = GDEH0154D67_WIDTH / 2;
    uint32_t upd_y = hm_y;
    // add 15 pixel, for some reason lower part doesn't update
    uint32_t upd_h = hm_h + 15;
    if (update_mode == LAST_TWO_DIGITS) {
      // last two digits every 10 minutes
      upd_x = GDEH0154D67_WIDTH / 2;
      upd_w = GDEH0154D67_WIDTH / 2;
    }
    else if (update_mode == WHOLE_SCREEN) {
      upd_x = 0;
      upd_y = 0;
      upd_w = GDEH0154D67_WIDTH;
      upd_h = GDEH0154D67_HEIGHT;
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
  bool space_before = true;
  for (const char* p = subj; *p; ++p) {
    if (isdigit(*p))
      result += *p;
    else {
      if (space_before && result.size() <= 6 && result.size() >= 4)
        return result;
      space_before = isspace(*p);
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
    SET_FONT(typography, C059_Bold29pt);
    y += typography.PrintCentered(code.c_str(), y);
    y += 10;
  }
  SET_FONT(typography, ter_x28b_pcf28pt);
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
    notifications.add(subj->text);
    // TODO find better solution
    // TODO handle phone ring start
    vibrate(100, 10);
    return true;
  } else if (subj->icon == 2) {
    // TODO handle phone ring end
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

void draw_info(unsigned disconnect_count) {
  display.fillScreen(EPD_WHITE);
  char buf[22];
  SET_FONT(typography, ter_x28b_pcf28pt);
  typography.SetCursor(5,5);
  if (boot_time) {
    struct tm* bt = localtime(&boot_time);
    strftime(buf, sizeof(buf), " %d.%m %H:%M\n", bt);
    typography.Print(buf);
  }
  snprintf(buf, 22, " %u%% (%d)\n",
           battery.get_level(), battery.get_voltage());
  typography.Print(buf);
  snprintf(buf, 22, " %u%% в день\n", battery.get_discharge_rate());
  typography.Print(buf);
  snprintf(buf, 22, " %u разрывов\n", disconnect_count);
  typography.Print(buf);
  display.updateWindow(0, 0, GDEH0154D67_WIDTH, GDEH0154D67_HEIGHT, false);
}

// Deep sleep period in minutes, wake ups are aligned to it
const uint8_t sleep_interval_min = 10;

// Seconds left until the nearest moment when the number of minutes
// is a multiple of sleep_interval_min.
// One extra second is added to avoid waking up slightly too early,
// which would cause an additional short sleep cycle.
uint32_t time_to_next_interval(const tm& now) {
  uint32_t interval = (uint32_t)sleep_interval_min * 60;
  uint32_t passed = (now.tm_min % sleep_interval_min) * 60 + now.tm_sec;
  return interval - passed + 1;
}

void deep_sleep() {
  battery.flush();
  struct tm now;
  uint64_t sleep_us = (uint64_t)sleep_interval_min * 60 * 1000000;
  if (get_rtc_time(&now))
    sleep_us = (uint64_t)time_to_next_interval(now) * 1000000;
  else
    ESP_LOGW(TAG, "No RTC time, sleeping for the whole interval");
  esp_sleep_enable_ext1_wakeup_io(BIT(ACCEL_GPIO),
                                  ESP_EXT1_WAKEUP_ANY_HIGH);
  ESP_LOGI(TAG, "Deeply sleeping for %u seconds", (unsigned)(sleep_us / 1000000));
  esp_err_t err = esp_deep_sleep_try(sleep_us);
  ESP_LOGE(TAG, "Failed to fall asleep deeply, %d", err);
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

int time_changes(tm* subj, int changes) {
  static RTC_DATA_ATTR uint8_t prev_day = 0;
  static RTC_DATA_ATTR uint8_t prev_hour = 0;
  static RTC_DATA_ATTR uint8_t prev_minute = 0;
  if (subj->tm_min != prev_minute)
    changes |= MINUTE_CHANGED;
  if (subj->tm_min / 10 != prev_minute / 10)
    changes |= DECADE_CHANGED;
  if (subj->tm_hour != prev_hour)
    changes |= HOUR_CHANGED;
  if (subj->tm_mday != prev_day)
    changes |= DAY_CHANGED;
  prev_day = subj->tm_mday;
  prev_hour = subj->tm_hour;
  prev_minute = subj->tm_min;
  return changes;
}

int calc_update_mode(bool valid, int changes) {
  if (!valid)
    return REFRESH_SCREEN;
  else if (changes & DAY_CHANGED)
    return REFRESH_SCREEN;
  else if (changes & SCREEN_CHANGED)
    return WHOLE_SCREEN;
  else if (changes & CONNECTION_CHANGED)
    return WHOLE_SCREEN;
  else if (changes & HOUR_CHANGED)
    return WHOLE_SCREEN;
  else if (changes & DECADE_CHANGED)
    return LAST_TWO_DIGITS;
  else if (changes & MINUTE_CHANGED)
    return LAST_DIGIT;
  return NOTHING;
}

void update_display(tm* now, bool valid,
                    int screen, int changes,
                    bool connected, unsigned disconnect_count) {
  if (changes) {
    ESP_LOGI(TAG, "Applying changes: %x", changes);
    if (screen == INFO_SCREEN) {
      draw_info(disconnect_count);
      display.deepSleep();
    } else if (screen == NOTIFICATION_SCREEN) {
      if (notifications.get_current()) {
        draw_notifications();
        display.deepSleep();
      }
    } else if (screen == MAIN_SCREEN) {
      int update_mode = calc_update_mode(valid, changes);
      ESP_LOGI(TAG, "Updating main screen, mode: %d", update_mode);
      draw_main_screen(now, valid, false,
                       step_counter() + step_base,
                       battery.get_level(),
                       connected,
                       update_mode);
      display.deepSleep();
    }
  }
}

void draw_sleeping_screen() {
  struct tm now;
  bool valid = get_rtc_time(&now);
  int changes =  valid ? time_changes(&now, 0) : 0;
  int update_mode = calc_update_mode(valid, changes);
  if (changes & DAY_CHANGED)
    step_base = 0;
  ESP_LOGI(TAG, "Updating main screen, mode: %d", update_mode);
  draw_main_screen(&now, valid, true, step_base,
                   battery.get_level(), false, update_mode);
  display.deepSleep();
}

extern "C" void app_main()
{
  ESP_LOGI(TAG, "Enter app_main()");
  // esp_deep_sleep_start();

  int time_sync_day = 0;
  int screen = MAIN_SCREEN;
  bool connected = false;
  unsigned disconnect_count = 0;

  uint32_t wakeup_reason = esp_sleep_get_wakeup_causes(); // get wake up reason
  ESP_LOGI(TAG, "Wakeup reason: %d", wakeup_reason);
  bool poweron = wakeup_reason == 0;
  setup_nvs();
  setup_misc_hw();
  add_accel_device();
  display.init();
  display.setRefresh(poweron);
  if (wakeup_reason & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
    draw_sleeping_screen();
    deep_sleep();
  }
  setup_pm();
  setup_main_queue();
  setup_accel();
  accel_wait_idle();
  setup_ble("Whatcheee");

  // notifications.add("Довольно короткое сообщение №1 123456 code");
  // notifications.add("Quite short test message 2\nwith line breaks и т.д. и т.р. and so on");
  // screen = NOTIFICATION_SCREEN;
  
  ESP_LOGI(TAG, "Battery voltage: %d", battery.get_voltage());
  
  int interval = minute / portTICK_PERIOD_MS;
  int changes = SCREEN_CHANGED;
  while(true) {
    struct tm now;
    bool valid = get_rtc_time(&now);
    if (valid) {
      changes = time_changes(&now, changes);

      if (!boot_time) {
        ESP_LOGI(TAG, "RTC time: %u-%u-%u %u:%u:%u (%d)",
                 now.tm_year, now.tm_mon, now.tm_mday,
                 now.tm_hour, now.tm_min, now.tm_sec, valid);
        boot_time = mktime(&now);
      }
    }

    battery.measure(valid ? &now : 0);

    if (changes & HOUR_CHANGED) {
      send_battery(battery.get_level());
      send_step_counter(step_counter() + step_base);
    }
    if (changes & DAY_CHANGED) {
      reset_step_counter();
      step_base = 0;
    }

    update_display(&now, valid, screen, changes,
                   connected, disconnect_count);

    changes = 0;
    struct Message msg;
    if (xQueueReceive(main_queue, &msg, interval) == pdTRUE) {
      if (handle_misc_hw_events(msg))
        continue;
      switch (msg.type) {
      case BUTTON_PRESSED:
        ESP_LOGI(TAG, "Button %u pressed", (unsigned)msg.data);
        switch ((unsigned)msg.data) {
        case BUTTON_BACK:
          screen = MAIN_SCREEN;
          changes |= SCREEN_CHANGED;
          break;
        case BUTTON_UP:
          screen = NOTIFICATION_SCREEN;
          notifications.prev();
          changes |= SCREEN_CHANGED;
          break;
        case BUTTON_DOWN:
          screen = NOTIFICATION_SCREEN;
          notifications.next();
          changes |= SCREEN_CHANGED;
          break;
        case BUTTON_MENU:
          screen = INFO_SCREEN;
          changes |= SCREEN_CHANGED;
          break;
        }
        break;
      case BUTTON_RELEASED:
        ESP_LOGI(TAG, "Button %u released", (unsigned)msg.data);
        break;
      case ACCEL_NO_MOTION:
        ESP_LOGI(TAG, "No motion for 2.5 minutes, falling asleep");
        step_base += step_counter();
        hibernate_accel();
        draw_main_screen(&now, valid, true, step_base,
                         battery.get_level(), false, WHOLE_SCREEN);
        display.deepSleep();
        deep_sleep();
        break;
      case CLIENT_SUBSCRIBED:
        send_info();
        send_battery(battery.get_level());
        send_step_counter(step_counter() + step_base);
        break;
      case CLIENT_TIME: {
        tm* t = (tm *)msg.data;
        ESP_LOGI(TAG, "Got time: %u-%u-%u %u:%u:%u",
                 t->tm_year, t->tm_mon, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
        sync_current_time(t, &time_sync_day);
        delete t;
        break;
      }
      case CLIENT_NOTIFICATION: {
        Notification *notif = (Notification *)msg.data;
        if (handle_notification(notif)) {
          screen = NOTIFICATION_SCREEN;
          changes |= SCREEN_CHANGED;
        }
        delete notif;
        break;
      }
      case CLIENT_FIND:
        vibrate(50, 10);
        break;
      case CLIENT_CONNECTED:
        connected = true;
        changes |= CONNECTION_CHANGED;
        break;
      case CLIENT_DISCONNECTED:
        connected = false;
        ++disconnect_count;
        changes |= CONNECTION_CHANGED;
        break;
      }
    }
    // uart_wait_tx_done(UART_NUM_2, 200 / portTICK_PERIOD_MS);
  }
}
