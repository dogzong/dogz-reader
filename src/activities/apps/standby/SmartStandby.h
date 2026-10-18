#pragma once
#include <Arduino.h>

#include <array>
#include <atomic>

#include "SmartStandbyData.h"
#include "StandbyFace.h"

namespace smartstandby {
constexpr const char* kDir = "/.crosspoint/standby";
enum Module : uint16_t {
  Weather = 1,
  Quota = 2,
  Poem = 4,
  Lunar = 8,
  Battery = 16,
  Forecast = 32,
  Air = 64,
  Rain = 128,
  Details = 256
};
struct Settings {
  uint8_t face = 2, clock = 0, offlineMinute = 0, poemMinutes = 30, crop = 0;
  uint16_t online = Weather | Quota | Poem | Lunar | Battery;
  uint16_t offline = Weather | Poem | Forecast | Air | Rain | Battery;
  char endpoint[257] = "https://www.dogzong.com/standby/v1/snapshot";
  char token[257] = {};
  char image[257] = {};
  bool load();
  bool save() const;
};
void provision(const String& json);
}  // namespace smartstandby

class SmartStandbyFace final : public StandbyFace {
 public:
  explicit SmartStandbyFace(bool quiet = false) : quiet_(quiet) {}
  void onEnter() override;
  void prepareSleep(uint16_t modules);
  void onExit() override;
  void onPageNext() override;
  void onPagePrev() override { onPageNext(); }
  TickResult tick() override;
  void render(GfxRenderer& renderer, const Rect& viewport) override;
  StrId titleId() const override;
  uint32_t secondsUntilNextWake() const override;

 private:
  smartstandby::Settings settings_;
  smartstandby::Snapshot snapshot_, pending_;
  smartstandby::PoemData displayedPoem_{};
  bool hasPoem_ = false, receivedPoems_ = false;
  bool sleep_ = false, quiet_ = false, offline_ = false, authError_ = false;
  uint8_t failures_ = 0, poem_ = 0;
  uint32_t nextRequest_ = 0, nextPoem_ = 0, lastClock_ = UINT32_MAX;
  time_t shownTime_ = 0;
  std::atomic<bool> cancel_{false}, done_{false}, running_{false};
  int status_ = 0;
  bool valid_ = false;
  void startRequest();
  void fetch();
  void renderSleep(GfxRenderer& renderer, const Rect& viewport);
  static void worker(void* self);
};
