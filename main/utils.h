#pragma once

#include <deque>
#include <string>

const int NOTIFICATION_LIMIT = 8;

struct NotificationBuffer {
  std::deque<std::string> store;
  int current = 0;

  const char* get_current() {
    if (store.empty())
      return 0;
    return store[current].c_str();
  }
  
  void add(const char* notification) {
    store.push_back(notification);
    while (store.size() > NOTIFICATION_LIMIT)
      store.pop_front();
    current = store.size() - 1;
  }

  void clear() {
    store.clear();
  }

  void prev() {
    if (current-- == 0)
      current = store.size() - 1;
  }

  void next() {
    if (++current == store.size())
      current = 0;
  }
};

const char* next_ut8_symbol(const char* str);
uint32_t decode_utf8(const char* str);
