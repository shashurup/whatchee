#ifndef _MISCHW_H
#define _MISCHW_H

#include "main_queue.h"
#include <inttypes.h>

#define BUTTON_MENU 0
#define BUTTON_BACK 1
#define BUTTON_UP 2
#define BUTTON_DOWN 3
#define BUTTON_CHANGED 1u
#define BUTTON_PRESSED 2u
#define BUTTON_RELEASED 3u

void vibrate(uint8_t intervalMs, uint8_t length);

int get_battery_millivolts();

void setup_misc_hw();

bool handle_misc_hw_events(Message msg);

#endif /* _RTCI2C_H */
