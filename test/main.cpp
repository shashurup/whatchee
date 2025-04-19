#include <stdio.h>
#include "../main/typography.h"
#include "../main/ter_x20b_pcf20pt.h"

class MockDisplay {
private:
  const GFXfont* font;
public:
  void setFont(const GFXfont *f) {
    font = f;
  }

  void drawChar(int16_t x, int16_t y, unsigned char c,
           uint16_t color, uint16_t bg, uint8_t size) {
    GFXglyph *glyph = &font->glyph[c - font->first];
    for (int y = 0; y < glyph->height; ++y) {
      for (int x = 0; x < glyph->width; ++x) {
        int bit_no = y * glyph->width + x;
        int byte_no = bit_no / 8;
        int rem = bit_no % 8;
        bool bit = 1;
        if ((0x80 >> rem) & font->bitmap[byte_no + glyph->bitmapOffset])
          putchar('#');
        else
          putchar(' ');
      }
      putchar('\n');
    }
  }
};

MockDisplay display;
Typography<MockDisplay> typography(display);

int main(int argc, char** argv) {
  typography.SetFont(&ter_x20b_pcf20pt[0],
                     sizeof(ter_x20b_pcf20pt) / sizeof(ter_x20b_pcf20pt[0]));
  typography.Print("Heeey!!!\n");
  typography.Print("Привет\nшрифт");
}
