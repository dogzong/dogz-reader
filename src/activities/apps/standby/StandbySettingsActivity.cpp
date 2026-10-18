#include "StandbySettingsActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>

#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/home/FileBrowserActivity.h"
#include "components/UITheme.h"

namespace {
constexpr StrId mainNames[] = {StrId::STR_SMART_STANDBY_ENTER,
                               StrId::STR_SMART_STANDBY_PAGE,
                               StrId::STR_SMART_STANDBY_CLOCK,
                               StrId::STR_SMART_STANDBY_ONLINE_MODULES,
                               StrId::STR_SMART_STANDBY_OFFLINE_MODULES,
                               StrId::STR_SMART_STANDBY_OFFLINE_CLOCK,
                               StrId::STR_SMART_STANDBY_POEM_INTERVAL,
                               StrId::STR_SMART_STANDBY_IMAGE_PICK,
                               StrId::STR_SMART_STANDBY_CROP,
                               StrId::STR_SMART_STANDBY_CONNECTION};
constexpr StrId moduleNames[] = {StrId::STR_SMART_STANDBY_WEATHER,
                                 StrId::STR_SMART_STANDBY_QUOTA,
                                 StrId::STR_SMART_STANDBY_POEM,
                                 StrId::STR_SMART_STANDBY_LUNAR,
                                 StrId::STR_BATTERY,
                                 StrId::STR_SMART_STANDBY_FORECAST,
                                 StrId::STR_SMART_STANDBY_AIR,
                                 StrId::STR_SMART_STANDBY_RAIN};
constexpr uint16_t flags[] = {smartstandby::Weather, smartstandby::Quota,    smartstandby::Poem, smartstandby::Lunar,
                              smartstandby::Battery, smartstandby::Forecast, smartstandby::Air,  smartstandby::Rain};
constexpr int offlineRows[] = {0, 2, 3, 4, 5, 6, 7, 0};
}  // namespace
void StandbySettingsActivity::onEnter() {
  UiListActivity::onEnter();
  settings_.load();
  for (int i = 0; i < listCount(); ++i) {
    rows_[i].label = I18N.get(group_ ? moduleNames[group_ == 1 ? i : offlineRows[i]] : mainNames[i]);
    rows_[i].actionValue = i;
  }
}
const char* StandbySettingsActivity::headerTitle() const { return tr(STR_STANDBY_TITLE); }
bool StandbySettingsActivity::handleCustomInput() {
  if (!gate_) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
      !mappedInput.isPressed(MappedInputManager::Button::Confirm))
    gate_ = false;
  return true;
}
void StandbySettingsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (group_) {
    const uint16_t flag = flags[group_ == 1 ? index : offlineRows[index]];
    (group_ == 1 ? settings_.online : settings_.offline) ^= flag;
  } else {
    switch (index) {
      case 0:
        settings_.save();
        activityManager.goToStandby();
        return;
      case 1:
        settings_.face = (settings_.face + 1) % 5;
        break;
      case 2:
        settings_.clock ^= 1;
        break;
      case 3:
      case 4:
        startActivityForResultWith<StandbySettingsActivity>(
            [this](const ActivityResult&) {
              settings_.load();
              gate_ = true;
              requestUpdate();
            },
            index == 3 ? 1 : 2);
        return;
      case 5:
        settings_.offlineMinute ^= 1;
        break;
      case 6:
        settings_.poemMinutes = settings_.poemMinutes == 30 ? 60 : settings_.poemMinutes == 60 ? 0 : 30;
        break;
      case 7:
        Storage.ensureDirectoryExists("/Pictures");
        Storage.ensureDirectoryExists("/Pictures/Standby");
        Storage.ensureDirectoryExists(smartstandby::kDir);
        startActivityForResultWith<FileBrowserActivity>(
            [this](const ActivityResult& result) {
              gate_ = true;
              const auto* path = std::get_if<FilePathResult>(&result.data);
              if (!result.isCancelled && path && path->path.size() < sizeof(settings_.image)) {
                bool ready = true;
                if (FsHelpers::hasPngExtension(path->path)) {
                  GfxRenderer::FrameBufferLoan loan(renderer);
                  ready = PngToBmpConverter::pngFileToBmpFile(path->path.c_str(), "/.crosspoint/standby/image.bmp.part",
                                                              true) &&
                          Storage.replaceFile("/.crosspoint/standby/image.bmp.part", "/.crosspoint/standby/image.bmp");
                }
                if (ready) {
                  snprintf(settings_.image, sizeof(settings_.image), "%s", path->path.c_str());
                  settings_.save();
                }
              }
              requestUpdate();
            },
            "/Pictures/Standby", FileBrowserActivity::Mode::PickImage);
        return;
      case 8:
        settings_.crop ^= 1;
        break;
      case 9:
        settings_.save();
        activityManager.goToStandbyFace(2);
        return;
    }
  }
  if (!settings_.save()) LOG_ERR("STANDBY", "Cannot save settings");
  requestUpdate();
}
void StandbySettingsActivity::buildScreen(UiScreen& screen) {
  namespace fui = freeink::ui;
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  if (group_) {
    for (int i = 0; i < listCount(); ++i) {
      bool enabled = (group_ == 1 ? settings_.online : settings_.offline) & flags[group_ == 1 ? i : offlineRows[i]];
      rows_[i].value = enabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      GUI.setCheckboxRow(rows_[i], enabled);
    }
  } else {
    constexpr StrId faces[] = {StrId::STR_FACE_SLOPPY_CLOCK, StrId::STR_FACE_CHINESE_CALENDAR,
                               StrId::STR_SMART_STANDBY_INFO, StrId::STR_SMART_STANDBY_QUIET,
                               StrId::STR_SMART_STANDBY_IMAGE};
    rows_[1].value = I18N.get(faces[settings_.face]);
    rows_[2].value = settings_.clock ? tr(STR_SMART_STANDBY_DIGITAL) : tr(STR_SMART_STANDBY_FLIP);
    rows_[5].value = settings_.offlineMinute ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    snprintf(interval_, sizeof(interval_), "%u", settings_.poemMinutes);
    rows_[6].value = settings_.poemMinutes ? interval_ : tr(STR_SMART_STANDBY_MANUAL);
    const char* slash = strrchr(settings_.image, '/');
    rows_[7].value = slash ? slash + 1 : tr(STR_NOT_SET);
    rows_[8].value = settings_.crop ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    rows_[9].value = settings_.token[0] ? "" : tr(STR_NOT_SET);
  }
  fui::ListProps props;
  props.items = rows_;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 2;
  syncListViewport(screen, props);
  screen.list(props);
}
