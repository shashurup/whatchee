#pragma once

#include <OpenFontRender.h>
#include <vector>

const unsigned NOTIFICATIONS_BUFFER_SIZE = 2048;

struct NotificationBuffer {
  char* current;
  char* top;
  char buffer[NOTIFICATIONS_BUFFER_SIZE];

  void add(const char* notification);
  void clear();
  void prev();
  void next();
};

class MyFontRender: public virtual OpenFontRender {
 public:
  void drawStringBreakLines(const char* subj, int32_t x, int32_t y, uint32_t width, uint32_t height);
  void drawStringCentered(const char* subj, int32_t y, uint32_t width);
};
