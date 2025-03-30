#ifndef _MAIN_QUEUE_H
#define _MAIN_QUEUE_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct Message {
  unsigned type;
  unsigned value;
};

extern QueueHandle_t main_queue;

void setup_main_queue();

#endif // _MAIN_QUEUE_H
