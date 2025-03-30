#ifndef BLE_H
#define BLE_H

#define CLIENT_SUBSCRIBED 11u

void setup_ble(const char* name);

void send_info();

void send_battery(int millivolts);

#endif // BLE_H
