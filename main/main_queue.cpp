#include "main_queue.h"

QueueHandle_t main_queue = NULL;

void setup_main_queue() {
  main_queue = xQueueCreate(7, sizeof(struct Message));
}
