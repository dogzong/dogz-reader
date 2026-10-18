#include "SleepActivity.h"

#include <Bitmap.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cmath>

namespace {
HalDisplay::GrayscaleMode sleepGrayscaleMode(const GfxRenderer& renderer) {
  return renderer.grayscaleCapabilities(HalDisplay::GrayscaleMode::Direct).supported()
             ? HalDisplay::GrayscaleMode::Direct
             : HalDisplay::GrayscaleMode::Absolute;
}

struct BitmapPlacement {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;
};

BitmapPlacement calculateBitmapPlacement(const int bitmapWidth, const int bitmapHeight, const GfxRenderer& renderer,
                                         bool crop) {
  BitmapPlacement placement;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (crop || bitmapWidth > pageWidth || bitmapHeight > pageHeight) {
    float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (crop) {
        placement.cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - placement.cropX) * static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
      }
      placement.x = 0;
      placement.y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (crop) {
        placement.cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmapWidth) / ((1.0f - placement.cropY) * static_cast<float>(bitmapHeight));
      }
      placement.x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      placement.y = 0;
    }
  } else {
    placement.x = (pageWidth - bitmapWidth) / 2;
    placement.y = (pageHeight - bitmapHeight) / 2;
  }

  return placement;
}

}  // namespace
void SleepActivity::onEnter() {
  Activity::onEnter();
  settings_.load();
  display.setInverted(false);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.setRenderMode(GfxRenderer::BW);
  if (auto* cache = renderer.getFontCacheManager()) cache->releaseSdFontCaches();
  if (settings_.mode == 1) {
    const char* path =
        FsHelpers::hasPngExtension(std::string_view(settings_.image)) ? sleepscreen::kImage : settings_.image;
    auto file = Storage.open(path);
    if (file) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderBitmapSleepScreen(bitmap, false, true);
        LOG_DBG("SLP", "Static image rendered once; no timer/network");
        return;
      }
    }
    LOG_ERR("SLP", "Invalid selected image; using cached information");
  }
  renderDefaultSleepScreen();
}
void SleepActivity::loop() {
  if (!preview_) return;
  if (gate_) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
        !mappedInput.isPressed(MappedInputManager::Button::Confirm))
      gate_ = false;
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm))
    finish();
}
void SleepActivity::renderDefaultSleepScreen() const {
  renderer.clearScreen();
  // Activity member storage avoids putting two ~3 KiB snapshots on the small task stack.
  face_.prepareSleep(settings_.modules);
  face_.render(renderer, Rect(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()));
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  LOG_DBG("SLP", "Cached information rendered once; no clock/timer/network");
}
void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preserveBackground,
                                            const bool nativeCustomImage) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto placement = calculateBitmapPlacement(bitmap.getWidth(), bitmap.getHeight(), renderer, settings_.crop);
  const int x = placement.x;
  const int y = placement.y;
  const float cropX = placement.cropX;
  const float cropY = placement.cropY;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  if (!preserveBackground) renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale();

  if (nativeCustomImage && !preserveBackground && renderer.getGrayscaleLevels() == 16) {
    const bool displayed =
        renderer.beginGrayscale16() &&
        renderer.drawBitmapGrayscale16(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, settings_.crop) &&
        renderer.commitGrayscale16();
    renderer.cancelGrayscale16();
    if (!displayed) {
      LOG_ERR("SLP", "Native grayscale sleep image failed");
      renderDefaultSleepScreen();
    }
    return;
  }

  if (!(settings_.crop ? renderer.drawBitmapCropToFill(bitmap, 0, 0, pageWidth, pageHeight)
                       : renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, preserveBackground))) {
    renderDefaultSleepScreen();
    return;
  }

  const bool absolute = hasGreyscale && renderer.grayscaleCapabilities(sleepGrayscaleMode(renderer)).supported();
  if (absolute) {
    if (!renderer.displayGrayscaleBase(sleepGrayscaleMode(renderer))) return;
  } else if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (hasGreyscale) {
    bool ready = true;
    for (const auto plane : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      if (bitmap.rewindToData() != BmpReaderError::Ok) {
        ready = false;
        break;
      }
      if (!absolute || !preserveBackground) renderer.clearScreen(absolute ? 0xFF : 0x00);
      renderer.setRenderMode(plane);
      if (!(settings_.crop
                ? renderer.drawBitmapCropToFill(bitmap, 0, 0, pageWidth, pageHeight)
                : renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, preserveBackground))) {
        ready = false;
        break;
      }
      if (plane == GfxRenderer::GRAYSCALE_LSB)
        renderer.copyGrayscaleLsbBuffers();
      else
        renderer.copyGrayscaleMsbBuffers();
    }
    if (ready)
      renderer.displayGrayBuffer();
    else
      LOG_ERR("SLP", "Incomplete grayscale image; keeping the current display");
    renderer.setRenderMode(GfxRenderer::BW);
  }
}
