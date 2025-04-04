#ifndef BLE_H
#define BLE_H

#include <stddef.h>
#include <inttypes.h>

#define CLIENT_SUBSCRIBED 11u
#define CLIENT_NOTIFICATION 12u
#define CLIENT_TIME 13u

struct Notification {
  uint8_t icon;
  uint8_t state;
  char* text;

  Notification(size_t text_size);
  ~Notification();
};

void setup_ble(const char* name);

void send_info();

void send_battery(uint8_t level);

#endif // BLE_H
