#ifndef BLE_H
#define BLE_H

#include <stddef.h>
#include <inttypes.h>

#define CLIENT_SUBSCRIBED 11u
#define CLIENT_NOTIFICATION 12u
#define CLIENT_TIME 13u
#define CLIENT_FIND 14u
#define CLIENT_CONNECTED 15u
#define CLIENT_DISCONNECTED 16u

struct Notification {
  uint8_t icon;
  uint8_t state;
  char* text;

  Notification(size_t text_size);
  ~Notification();
};

void setup_ble(const char* name);

void ble_sleep();

void ble_wakeup();

void send_info();

void send_battery(uint8_t level);

#endif // BLE_H
