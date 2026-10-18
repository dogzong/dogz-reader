#pragma once
#include "SleepSettings.h"
#include "activities/UiListActivity.h"

class SleepSettingsActivity final : public UiListActivity {
 public:
  SleepSettingsActivity(GfxRenderer& r, MappedInputManager& i, int group = 0)
      : UiListActivity("SleepSettings", r, i), group_(group) {}
  void onEnter() override;

 private:
  int group_;
  sleepscreen::Settings settings_;
  freeink::ui::ListItem rows_[6]{};
  bool gate_ = true;
  int listCount() const override { return group_ ? 6 : 5; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
};
