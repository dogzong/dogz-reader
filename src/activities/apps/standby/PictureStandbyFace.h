#pragma once
#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "SmartStandby.h"
#include "fontIds.h"

class PictureStandbyFace final : public StandbyFace {
 public:
  void onEnter() override { settings_.load(); }
  TickResult tick() override { return TickResult::None; }
  uint32_t secondsUntilNextWake() const override { return UINT32_MAX; }
  StrId titleId() const override { return StrId::STR_SMART_STANDBY_IMAGE; }
  bool wantsGrayscale() const override { return true; }
  void render(GfxRenderer& r, const Rect& v) override {
    const char* path = FsHelpers::hasPngExtension(std::string_view(settings_.image)) ? "/.crosspoint/standby/image.bmp"
                                                                                     : settings_.image;
    auto file = Storage.open(path);
    if (file) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const float scale =
            std::min(1.0f, std::min(float(v.width) / bitmap.getWidth(), float(v.height) / bitmap.getHeight()));
        const int x = v.x + (v.width - int(bitmap.getWidth() * scale)) / 2;
        const int y = v.y + (v.height - int(bitmap.getHeight() * scale)) / 2;
        if (settings_.crop ? r.drawBitmapCropToFill(bitmap, v.x, v.y, v.width, v.height)
                           : r.drawBitmap(bitmap, x, y, v.width, v.height))
          return;
      }
    }
    r.drawCenteredText(UI_12_FONT_ID, v.y + v.height / 2, tr(STR_INVALID_IMAGE_FILE));
  }

 private:
  smartstandby::Settings settings_;
};
