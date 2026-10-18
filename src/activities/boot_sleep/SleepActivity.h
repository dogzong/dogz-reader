#pragma once
#include "SleepSettings.h"
#include "activities/Activity.h"
#include "activities/apps/standby/SmartStandby.h"
class Bitmap;
class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& r, MappedInputManager& i, bool fromTimeout = false, bool preview = false)
      : Activity("Sleep", r, i), preview_(preview) {
    (void)fromTimeout;
  }
  void onEnter() override;
  void loop() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap&, bool preserveBackground = false, bool nativeCustomImage = false) const;
  sleepscreen::Settings settings_;
  mutable SmartStandbyFace face_;
  bool preview_ = false, gate_ = true;
};
