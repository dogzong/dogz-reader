#pragma once
#include "SmartStandby.h"
#include "activities/UiListActivity.h"

class StandbySettingsActivity final : public UiListActivity {
 public:
  StandbySettingsActivity(GfxRenderer& r, MappedInputManager& i, int group = 0)
      : UiListActivity("StandbySettings", r, i), group_(group) {}
  void onEnter() override;

 private:
  int group_;
  smartstandby::Settings settings_;
  freeink::ui::ListItem rows_[10]{};
  char interval_[8]{};
  bool gate_ = true;
  int listCount() const override { return group_ ? (group_ == 1 ? 5 : 7) : 10; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
};
