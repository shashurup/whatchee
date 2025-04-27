#pragma once

#include "main_queue.h"
#include <inttypes.h>
#include <time.h>

#define BUTTON_MENU 0
#define BUTTON_BACK 1
#define BUTTON_UP 2
#define BUTTON_DOWN 3
#define BUTTON_CHANGED 1u
#define BUTTON_PRESSED 2u
#define BUTTON_RELEASED 3u

void vibrate(uint8_t intervalMs, uint8_t length);

void setup_misc_hw();

bool handle_misc_hw_events(Message msg);

bool get_rtc_time(tm* t);

void set_rtc_time(tm* t);

void clear_rtc_timer();

void set_rtc_timer(uint8_t minutes);

void append_nvs_block(const char *name, void *buf, size_t size);

class Battery {
 private:
  int start_voltage;
  time_t start_time;
  int prev_voltage = 0;
  time_t prev_time = 0;
  uint8_t prev_min = 0;
  uint8_t discharge_rate = 0;
  uint16_t log[100];
  uint8_t log_idx = 0;
  void append_log(uint16_t subj);
 public:
  void measure(struct tm* now);
  uint8_t get_level();
  int get_voltage();
  uint8_t get_discharge_rate();
  void flush();
};
