#include "SleepSettingsActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>

#include "SleepActivity.h"
#include "activities/ActivityManager.h"
#include "activities/ActivityResult.h"
#include "activities/home/FileBrowserActivity.h"
#include "components/UITheme.h"

namespace {
constexpr StrId mainNames[] = {StrId::STR_SLEEP_PREVIEW, StrId::STR_SMART_STANDBY_PAGE, StrId::STR_SLEEP_MODULES,
                               StrId::STR_SMART_STANDBY_IMAGE_PICK, StrId::STR_SMART_STANDBY_CROP};
constexpr StrId moduleNames[] = {StrId::STR_SMART_STANDBY_WEATHER, StrId::STR_SMART_STANDBY_FORECAST,
                                 StrId::STR_SMART_STANDBY_AIR,     StrId::STR_SMART_STANDBY_RAIN,
                                 StrId::STR_SMART_STANDBY_POEM,    StrId::STR_BATTERY};
constexpr uint16_t flags[] = {smartstandby::Weather, smartstandby::Forecast, smartstandby::Air,
                              smartstandby::Rain,    smartstandby::Poem,     smartstandby::Battery};
}  // namespace
void SleepSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  settings_.load();
  for (int i = 0; i < listCount(); ++i) {
    rows_[i].label = I18N.get(group_ ? moduleNames[i] : mainNames[i]);
    rows_[i].actionValue = i;
  }
}
const char* SleepSettingsActivity::headerTitle() const { return tr(STR_SLEEP_SCREEN); }
bool SleepSettingsActivity::handleCustomInput() {
  if (!gate_) return false;
  if (!mappedInput.isPressed(MappedInputManager::Button::Back) &&
      !mappedInput.isPressed(MappedInputManager::Button::Confirm))
    gate_ = false;
  return true;
}
void SleepSettingsActivity::activateIndex(int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  if (group_) {
    settings_.modules ^= flags[index];
  } else {
    switch (index) {
      case 0:
        startActivityForResultWith<SleepActivity>(
            [this](const ActivityResult&) {
              gate_ = true;
              requestUpdate();
            },
            false, true);
        return;
      case 1:
        settings_.mode ^= 1;
        break;
      case 2:
        startActivityForResultWith<SleepSettingsActivity>(
            [this](const ActivityResult&) {
              settings_.load();
              gate_ = true;
              requestUpdate();
            },
            1);
        return;
      case 3:
        Storage.ensureDirectoryExists("/Pictures");
        Storage.ensureDirectoryExists("/Pictures/Sleep");
        Storage.ensureDirectoryExists(sleepscreen::kDir);
        startActivityForResultWith<FileBrowserActivity>(
            [this](const ActivityResult& result) {
              gate_ = true;
              const auto* path = std::get_if<FilePathResult>(&result.data);
              if (!result.isCancelled && path && path->path.size() < sizeof(settings_.image)) {
                const bool png = FsHelpers::hasPngExtension(path->path);
                const char* part = "/.crosspoint/sleep/image.bmp.part";
                const char* backup = "/.crosspoint/sleep/image.bmp.bak";
                bool ready = true, hadPrevious = false, replaced = false;
                if (png) {
                  {
                    GfxRenderer::FrameBufferLoan loan(renderer);
                    ready = PngToBmpConverter::pngFileToBmpFile(path->path.c_str(), part, true);
                  }
                  if (ready) {
                    Storage.remove(backup);
                    hadPrevious = Storage.exists(sleepscreen::kImage);
                    ready = !hadPrevious || Storage.rename(sleepscreen::kImage, backup);
                    if (ready) {
                      replaced = Storage.rename(part, sleepscreen::kImage);
                      ready = replaced;
                    }
                  }
                } else {
                  auto file = Storage.open(path->path.c_str());
                  Bitmap bitmap(file);
                  ready = file && bitmap.parseHeaders() == BmpReaderError::Ok;
                }
                if (ready) {
                  auto next = settings_;
                  snprintf(next.image, sizeof(next.image), "%s", path->path.c_str());
                  next.mode = 1;
                  ready = next.save();
                  if (ready) settings_ = next;
                }
                if (png) {
                  Storage.remove(part);
                  if (!ready && replaced) Storage.remove(sleepscreen::kImage);
                  if (!ready && hadPrevious && Storage.exists(backup)) Storage.rename(backup, sleepscreen::kImage);
                  if (ready) Storage.remove(backup);
                }
                if (!ready) {
                  LOG_ERR("SLP", "Cannot install selected image");
                  GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
                  delay(1000);
                }
              }
              requestUpdate();
            },
            "/Pictures/Sleep", FileBrowserActivity::Mode::PickImage);
        return;
      case 4:
        settings_.crop = !settings_.crop;
        break;
    }
  }
  if (!settings_.save()) {
    LOG_ERR("SLP", "Cannot save settings");
    settings_.load();
  }
  requestUpdate();
}
void SleepSettingsActivity::buildScreen(UiScreen& screen) {
  namespace fui = freeink::ui;
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  if (group_) {
    for (int i = 0; i < listCount(); ++i) {
      const bool enabled = settings_.modules & flags[i];
      rows_[i].value = enabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      GUI.setCheckboxRow(rows_[i], enabled);
    }
  } else {
    rows_[1].value = settings_.mode ? tr(STR_SMART_STANDBY_IMAGE) : tr(STR_SMART_STANDBY_OFFLINE);
    const char* slash = strrchr(settings_.image, '/');
    rows_[3].value = slash ? slash + 1 : tr(STR_NOT_SET);
    rows_[4].value = settings_.crop ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
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
