#include "utils.h"
#include "esp_log.h"
#include <string>
#include <vector>

struct Context {
  std::vector<unsigned> breaks;
  long offset = 0;
  uint32_t width;
};

void MyFontRender::drawStringBreakLines(const char* subj, int32_t x, int32_t y,
                                        uint32_t width, uint32_t height) {
  FT_BBox bbox;
  FT_Error err;

  Context ctx;
  ctx.width = width;
  auto checkBreak = []
    (unsigned pos, uint16_t ch, FT_BBox &glyph_bbox, void* arg) {
    Context *ctx = (Context *)arg;
    if (glyph_bbox.xMax - ctx->offset >= ctx->width ||
        glyph_bbox.xMin < ctx->offset) {
      ctx->breaks.push_back(pos);
      ctx->offset = glyph_bbox.xMin;
    }
    ctx->offset = std::min(ctx->offset, glyph_bbox.xMin);
  };
  drawHString(subj, 0, 0, getFontColor(), getBackgroundColor(),
              Align::TopLeft, Drawing::Skip, bbox, err, checkBreak, &ctx);

  uint32_t dy = (uint32_t) getFontMaxHeight() * getLineSpaceRatio();
  const char *base = subj;
  unsigned count = 0;
  for (const unsigned &pos: ctx.breaks) {
    const char *cur = base;
    for (; count <= pos; ++cur)
      if (*cur < 0x80 || *cur >= 0b11000000)
        count++;
    --cur;
    std::string copy(base, cur - base);
    drawString(copy.c_str(), x, y, getFontColor(), getBackgroundColor());
    y += dy;
    base = cur;
  }
  drawString(base, x, y, getFontColor(), getBackgroundColor());
}

void MyFontRender::drawStringCentered(const char* subj, int32_t y, uint32_t width) {
  uint32_t subj_width = getTextWidth(subj);
  uint32_t x = (int32_t) (width - subj_width) / 2;
  drawString(subj, x, y, getFontColor(), getBackgroundColor());
}
