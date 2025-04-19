#include "utils.h"

const char* next_ut8_symbol(const char* str) {

  uint8_t first = *str;
    
  if ((first & 0x80) == 0)
    return str + 1;

  if ((first & 0xE0) == 0xC0)
    return str + 2;
    
  if ((first & 0xF0) == 0xE0)
    return str + 3;
    
  if ((first & 0xF8) == 0xF0)
    return str + 4;

  return str + 1;
}

uint32_t decode_utf8(const char* str) {
    if (!str)
        return 0;

    uint8_t first = *str;
    
    // ASCII символ (1 байт)
    if ((first & 0x80) == 0)
      return first;
    
    // 2-байтовый символ
    if ((first & 0xE0) == 0xC0) {
        if (str[1] == '\0' || (str[1] & 0xC0) != 0x80)
            return 0;  // Неверный символ продолжения
        return ((first & 0x1F) << 6) | (str[1] & 0x3F);
    }
    
    // 3-байтовый символ
    if ((first & 0xF0) == 0xE0) {
        if (str[1] == '\0' || str[2] == '\0' || 
            (str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80)
            return 0;  // Неверный символ продолжения
        return ((first & 0x0F) << 12) | 
               ((str[1] & 0x3F) << 6) | 
               (str[2] & 0x3F);
    }
    
    // 4-байтовый символ
    if ((first & 0xF8) == 0xF0) {
        if (str[1] == '\0' || str[2] == '\0' || str[3] == '\0' ||
            (str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 || 
            (str[3] & 0xC0) != 0x80)
            return 0;  // Неверный символ продолжения
        return ((first & 0x07) << 18) | 
               ((str[1] & 0x3F) << 12) | 
               ((str[2] & 0x3F) << 6) | 
               (str[3] & 0x3F);
    }
    
    return 0;  // Неверный символ UTF-8
}
