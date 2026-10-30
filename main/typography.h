#pragma once
#include <cstdint>
#include <gfxfont.h>
#include "utils.h"


#define SET_FONT(t, f) t.SetFont(&f[0], sizeof(f)/ sizeof(f[0]))

struct UnicodeFont {
  GFXfont base_font;
  unsigned page, first, last;
};

template<typename T>
class Typography {
 private:
  const UnicodeFont *font;
  int font_page_count;
  T &target;
  const UnicodeFont* current_page = 0;
  // Distance from the top of a text line to the baseline.  It must be the
  // same for all glyphs and all pages of a font, otherwise glyphs with a
  // small bitmap (':', '-', ...) end up aligned to the top of the line.
  int16_t baseline = 0;
  int16_t x = 0;
  int16_t y = 0;
  uint16_t fg = 0;
  uint16_t bg = 0xFF;
  float interval = 1.1;

  bool updateFont(uint32_t c) {
    if (current_page && c >= current_page->first && c <= current_page->last)
      return true;
    for (int i = 0; i < font_page_count; ++i)
      if (c >= font[i].first && c <= font[i].last) {
        current_page = &font[i];
        target.setFont(&current_page->base_font);
        return true;
      }
    return false;
  }

 public:

  Typography(T& tgt): target(tgt) {}

  void SetFont(const UnicodeFont *subj, int page_count) {
    font = subj;
    font_page_count = page_count;
    current_page = 0;
    // Ascent of the font: the largest distance between the baseline and the
    // topmost pixel of a glyph.  Note that the glyph array of a page is
    // indexed from 0, while base_font.first/last are character codes.
    baseline = 0;
    for (int p = 0; p < page_count; ++p) {
      const GFXfont &f = subj[p].base_font;
      for (int i = 0; i <= f.last - f.first; ++i)
        if (-f.glyph[i].yOffset > baseline)
          baseline = -f.glyph[i].yOffset;
    }
  }

  void SetCursor(int16_t _x, int16_t _y) {
    x = _x;
    y = _y;
  }

  void SetColors(uint16_t _fg, uint16_t _bg) {
    fg = _fg;
    bg = _bg;
  }

  void SetInterval(float subj) {
    interval = subj;
  }

  void Print(const char* subj,
             uint16_t &actual_width,
             uint16_t &actual_height,
             bool dry_run,
             bool fit,
             int16_t x_start,
             int16_t y_start,
             uint16_t max_width,
             uint16_t max_height) {
    actual_height = 0;
    actual_width = 0;
    x = x_start;
    y = y_start;
    int line_height = 0;
    for (const char* c = subj; *c; c = next_ut8_symbol(c)) {
      uint32_t sym = decode_utf8(c);
      if (sym == 0xd) {
        x = x_start;
        continue;
      }
      if (sym == 0xa) {
        x = x_start;
        y += line_height * interval + font[0].base_font.yAdvance;
        line_height = 0;
        continue;
      }
      if (!updateFont(sym)) {
        sym = '?';
        updateFont(sym);
      }
      int idx = sym - current_page->page - current_page->base_font.first;
      GFXglyph *glyph = &current_page->base_font.glyph[idx];
      if (fit && (x + glyph->xAdvance) > (x_start + max_width)) {
        x = x_start;
        y += line_height * interval + font[0].base_font.yAdvance;
        line_height = 0;
      }
      if (fit && y > (y_start + max_height))
        return;
      if (!dry_run)
        target.drawChar(x, y + baseline,
                        sym - current_page->page, fg, bg, 1);
      x += glyph->xAdvance;
      if (glyph->height > line_height)
        line_height = glyph->height;
      if (x > actual_width)
        actual_width = x;
      if ((y + line_height) > actual_height)
        actual_height = y + line_height;
    }
  }

  void Print(const char* subj) {
    uint16_t dummy = 0;
    Print(subj, dummy, dummy, false, false, x, y, 0, 0);
  }

  void FitText(const char* subj,
               int16_t x_start,
               int16_t y_start,
               uint16_t max_width,
               uint16_t max_height) {
    uint16_t dummy = 0;
    Print(subj, dummy, dummy, false, true,
          x_start, y_start, max_width, max_height);
  }

  void TextDimensions(const char* subj, uint16_t &width, uint16_t &height) {
    int16_t backup_x = x;
    int16_t backup_y = y;
    Print(subj, width, height, true, false, 0, 0, 0, 0);
    x = backup_x;
    y = backup_y;
  }

  uint16_t PrintCentered(const char* subj, int16_t y) {
    uint16_t width;
    uint16_t height;
    TextDimensions(subj, width, height);
    x = (target.width() - width) / 2;
    SetCursor(x, y);
    Print(subj);
    return height;
  }
};
