#include "AudioActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <BoardConfig.h>
#include <HalAudioOutput.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "AudioDuration.h"
#include "CrossPointSettings.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/SubpageLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr uint32_t kSampleRate = 16000;
constexpr size_t kMaxRecordingBytes = 30u * 60u * kSampleRate * sizeof(int16_t);
constexpr const char* kVolumePath = "/.crosspoint/audio-volume";
HalFile* playbackFile = nullptr;

int readPlayback(uint8_t* dst, const size_t length) {
  if (!playbackFile || !playbackFile->isOpen()) return -1;
  return playbackFile->read(dst, length);
}

bool seekPlayback(const size_t position) {
  return playbackFile && playbackFile->isOpen() && playbackFile->seek(position);
}

bool hasMp3Extension(const std::string& path) {
  if (path.size() < 4) return false;
  return std::tolower(static_cast<unsigned char>(path[path.size() - 3])) == 'm' &&
         std::tolower(static_cast<unsigned char>(path[path.size() - 2])) == 'p' &&
         std::tolower(static_cast<unsigned char>(path[path.size() - 1])) == '3' && path[path.size() - 4] == '.';
}

audioduration::Duration readDuration(HalFile& file, const bool mp3) {
  uint8_t bytes[512];
  if (!file.seek(0)) return {};
  int count = file.read(bytes, sizeof(bytes));
  const size_t fileSize = file.size();
  if (count < 12) return {};
  if (mp3) {
    size_t offset = 0;
    if (!memcmp(bytes, "ID3", 3)) {
      if ((bytes[6] | bytes[7] | bytes[8] | bytes[9]) & 0x80) return {};
      offset = 10 + (size_t(bytes[6]) << 21) + (size_t(bytes[7]) << 14) + (size_t(bytes[8]) << 7) + bytes[9];
      if (bytes[5] & 0x10) offset += 10;
      if (offset >= fileSize || !file.seek(offset)) return {};
      count = file.read(bytes, sizeof(bytes));
    }
    for (int i = 0; i + 4 <= count; ++i) {
      const auto result = audioduration::mp3(bytes + i, count - i, fileSize - offset - i);
      if (result.seconds) return result;
    }
  } else if (!memcmp(bytes, "RIFF", 4) && !memcmp(bytes + 8, "WAVE", 4)) {
    size_t position = 12;
    uint32_t byteRate = 0;
    for (int chunk = 0; chunk < 64 && position + 8 <= fileSize; ++chunk) {
      if (!file.seek(position) || file.read(bytes, 8) != 8) return {};
      const uint32_t length = audioduration::le32(bytes + 4);
      if (length > fileSize - position - 8) return {};
      if (!memcmp(bytes, "fmt ", 4) && length >= 16) {
        if (file.read(bytes, 16) != 16) return {};
        byteRate = audioduration::le32(bytes + 8);
      } else if (!memcmp(bytes, "data", 4) && byteRate)
        return {length / byteRate, false};
      position += 8 + size_t(length) + (length & 1);
    }
  }
  return {};
}

void writeLe32(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value >> 16);
  dst[3] = static_cast<uint8_t>(value >> 24);
}

void makeWavHeader(uint8_t header[44], uint32_t pcmBytes) {
  memset(header, 0, 44);
  memcpy(header, "RIFF", 4);
  writeLe32(header + 4, pcmBytes + 36);
  memcpy(header + 8, "WAVEfmt ", 8);
  writeLe32(header + 16, 16);
  header[20] = 1;
  header[22] = 1;
  writeLe32(header + 24, kSampleRate);
  writeLe32(header + 28, kSampleRate * 2);
  header[32] = 2;
  header[34] = 16;
  memcpy(header + 36, "data", 4);
  writeLe32(header + 40, pcmBytes);
}

}  // namespace

void AudioActivity::onEnter() {
  Activity::onEnter();
  HalAudioOutput::shutdown();
  state_.store(State::Home, std::memory_order_release);
  samplesWritten_.store(0, std::memory_order_relaxed);
  lastRenderSecond_ = UINT32_MAX;
  volume_ = 45;
  volumeDirty_ = false;
  {
    HalFile setting;
    uint8_t saved = 45;
    if (Storage.openFileForRead("Audio", kVolumePath, setting) && setting.read(&saved, 1) == 1 && saved <= 100)
      volume_ = saved;
  }
  path_.clear();
  entries_.clear();
  selectedHomeItem_ = 0;
  selectedEntry_ = 0;
  playbackFromBrowser_ = false;
  errorFromBrowser_ = false;
  // Do not scan FAT free space here: it can block the UI on large cards.
  if (musicApp_) openBrowser(true);
  requestUpdate();
}

void AudioActivity::onExit() {
  if (state_.load(std::memory_order_acquire) == State::Recording) requestStopRecording();
  while (workerActive_.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(10));
  HalAudioOutput::shutdown();
  playbackFile = nullptr;
  if (file_.isOpen()) file_.close();
  if (volumeDirty_ && Storage.ensureDirectoryExists("/.crosspoint")) {
    HalFile setting;
    if (Storage.openFileForWrite("Audio", kVolumePath, setting)) setting.write(&volume_, 1);
  }
  Activity::onExit();
}

void AudioActivity::startRecording() {
  errorFromBrowser_ = false;
  if (!Storage.ready() || !Storage.ensureDirectoryExists("/Recordings")) {
    state_.store(State::Error, std::memory_order_release);
    requestUpdate();
    return;
  }
  char filename[64];
  bool foundAvailableName = false;
  const uint32_t stamp = millis();
  for (uint8_t suffix = 0; suffix < 100; ++suffix) {
    snprintf(filename, sizeof(filename), "/Recordings/record-%lu-%u.wav", static_cast<unsigned long>(stamp), suffix);
    char partName[72];
    snprintf(partName, sizeof(partName), "%s.part", filename);
    if (!Storage.exists(filename) && !Storage.exists(partName)) {
      foundAvailableName = true;
      break;
    }
  }
  if (!foundAvailableName) {
    state_.store(State::Error, std::memory_order_release);
    requestUpdate();
    return;
  }
  path_ = filename;
  const std::string partPath = path_ + ".part";
  if (!Storage.openFileForWrite("Audio", partPath, file_)) {
    state_.store(State::Error, std::memory_order_release);
    requestUpdate();
    return;
  }
  uint8_t header[44];
  makeWavHeader(header, 0);
  if (file_.write(header, sizeof(header)) != sizeof(header)) {
    file_.close();
    Storage.remove(partPath.c_str());
    state_.store(State::Error, std::memory_order_release);
    requestUpdate();
    return;
  }
  state_.store(State::Recording, std::memory_order_release);
  workerActive_.store(true, std::memory_order_release);
  if (xTaskCreate(&AudioActivity::recordingTask, "AudioRecord", 4096, this, 1, nullptr) != pdPASS) {
    workerActive_.store(false, std::memory_order_release);
    file_.close();
    Storage.remove(partPath.c_str());
    state_.store(State::Error, std::memory_order_release);
  }
  requestUpdate();
}

void AudioActivity::openBrowser(const bool music) {
  browserMusic_ = music;
  errorFromBrowser_ = false;
  entries_.clear();
  selectedEntry_ = 0;
  if (!Storage.ready()) {
    state_.store(State::Error, std::memory_order_release);
    requestUpdate();
    return;
  }
  const char* directory = music ? "/Music" : "/Recordings";
  if (!Storage.exists(directory) && music) Storage.ensureDirectoryExists(directory);
  const auto files = Storage.listFiles(directory, 200);
  entries_.reserve(files.size());
  for (const String& name : files) {
    std::string item(name.c_str());
    // AppleDouble metadata and other hidden files are not audio tracks.
    if (item.empty() || item.front() == '.') continue;
    const size_t dot = item.find_last_of('.');
    if (dot == std::string::npos) continue;
    std::string extension = item.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if ((!music && extension == ".wav") || (music && (extension == ".mp3" || extension == ".wav"))) {
      entries_.push_back(std::move(item));
    }
  }
  std::sort(entries_.begin(), entries_.end());
  state_.store(State::Browser, std::memory_order_release);
  requestUpdate();
}

void AudioActivity::stopPlayback() {
  HalAudioOutput::shutdown();
  playbackFile = nullptr;
  if (file_.isOpen()) file_.close();
  state_.store(State::Browser, std::memory_order_release);
  requestUpdate();
}

bool AudioActivity::playBrowserEntry(const size_t index) {
  if (index >= entries_.size()) return false;
  selectedEntry_ = index;
  playbackFromBrowser_ = true;
  errorFromBrowser_ = true;
  path_ = std::string(browserMusic_ ? "/Music/" : "/Recordings/") + entries_[index];
  return startPlayback();
}

void AudioActivity::confirmDeleteSelected() {
  if (selectedEntry_ >= entries_.size() || workerActive_.load(std::memory_order_acquire)) return;
  const std::string fullPath = std::string(browserMusic_ ? "/Music/" : "/Recordings/") + entries_[selectedEntry_];
  const std::string displayName = entries_[selectedEntry_];
  startActivityForResultWith<ConfirmationActivity>(
      [this, fullPath](const ActivityResult& result) {
        if (!result.isCancelled && Storage.remove(fullPath.c_str())) {
          openBrowser(browserMusic_);
        } else {
          requestUpdate();
        }
      },
      tr(STR_DELETE) + std::string("? "), displayName);
}

void AudioActivity::recordingTask(void* context) {
  auto* self = static_cast<AudioActivity*>(context);
  if (!HalAudioOutput::beginCapture(kSampleRate)) {
    self->file_.close();
    const std::string partPath = self->path_ + ".part";
    Storage.remove(partPath.c_str());
    self->state_.store(State::Error, std::memory_order_release);
    self->requestUpdate();
    // Publish completion last: onExit may destroy self as soon as it is false.
    self->workerActive_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
  }

  int16_t pcm[64];
  uint32_t written = 0;
  bool failed = false;
  while (self->state_.load(std::memory_order_acquire) == State::Recording && written < kMaxRecordingBytes) {
    const int count = HalAudioOutput::readCapture(pcm, 64, 100);
    if (count < 0) {
      failed = true;
      break;
    }
    if (count == 0) continue;
    const size_t bytes = static_cast<size_t>(count) * sizeof(int16_t);
    if (self->file_.write(reinterpret_cast<const uint8_t*>(pcm), bytes) != bytes) {
      failed = true;
      break;
    }
    written += static_cast<uint32_t>(bytes);
    self->samplesWritten_.store(written / 2, std::memory_order_relaxed);
  }

  HalAudioOutput::endCapture();
  uint8_t header[44];
  makeWavHeader(header, written);
  const std::string partPath = self->path_ + ".part";
  if (!failed && self->file_.seek(0) && self->file_.write(header, sizeof(header)) == sizeof(header)) {
    self->file_.flush();
  } else {
    failed = true;
  }
  self->file_.close();
  if (!failed && !Storage.replaceFile(partPath.c_str(), self->path_.c_str())) failed = true;
  if (failed) Storage.remove(partPath.c_str());
  self->state_.store(failed ? State::Error : State::Saved, std::memory_order_release);
  self->requestUpdate();
  self->workerActive_.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void AudioActivity::requestStopRecording() {
  State expected = State::Recording;
  if (state_.compare_exchange_strong(expected, State::Stopping, std::memory_order_acq_rel)) requestUpdate();
}

bool AudioActivity::startPlayback() {
  if (path_.empty() || !Storage.openFileForRead("Audio", path_, file_)) {
    state_.store(State::Error, std::memory_order_release);
    requestUpdate();
    return false;
  }
  playbackFromBrowser_ = state_.load(std::memory_order_acquire) == State::Browser;
  const auto duration = readDuration(file_, hasMp3Extension(path_));
  durationSeconds_ = duration.seconds;
  durationEstimated_ = duration.estimated;
  file_.seek(0);
  playbackFile = &file_;
  const bool started = hasMp3Extension(path_) ? HalAudioOutput::playMp3(readPlayback, seekPlayback, volume_)
                                              : HalAudioOutput::playWav(readPlayback, seekPlayback, false, volume_);
  if (!started) {
    playbackFile = nullptr;
    file_.close();
    state_.store(State::Error, std::memory_order_release);
    errorFromBrowser_ = playbackFromBrowser_;
    requestUpdate();
    return false;
  }
  const size_t slash = path_.find_last_of('/');
  songTitle_ = path_.substr(slash == std::string::npos ? 0 : slash + 1);
  const size_t suffix = songTitle_.find_last_of('.');
  if (suffix != std::string::npos) songTitle_.resize(suffix);
  artist_.clear();
  if (musicApp_) {
    // Filename convention: song-artist.mp3; ID3 tag extraction is separate.
    const size_t separator = songTitle_.find_last_of('-');
    if (separator != std::string::npos && separator > 0 && separator + 1 < songTitle_.size()) {
      artist_ = songTitle_.substr(separator + 1);
      songTitle_.resize(separator);
    }
  }
  coverPath_.clear();
  if (musicApp_) {
    const std::string candidate = path_.substr(0, path_.find_last_of('.')) + ".bmp";
    if (Storage.exists(candidate.c_str()))
      coverPath_ = candidate;
    else if (Storage.exists("/Music/cover.bmp"))
      coverPath_ = "/Music/cover.bmp";
  }
  lastPlaybackRefresh_ = millis();
  state_.store(State::Playing, std::memory_order_release);
  requestUpdate();
  return true;
}

void AudioActivity::changeVolume(const int delta) {
  const uint8_t next = static_cast<uint8_t>(std::max(0, std::min(100, static_cast<int>(volume_) + delta)));
  if (next == volume_) return;
  volume_ = next;
  volumeDirty_ = true;
  HalAudioOutput::setVolume(volume_);
  requestUpdate();
}

void AudioActivity::loop() {
  // Input changes strings/list selections shared with the render task.
  RenderLock lock(*this);
  const State state = state_.load(std::memory_order_acquire);
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == State::Recording)
      requestStopRecording();
    else if (musicApp_ && (state == State::Browser || (state == State::Error && !errorFromBrowser_))) {
      activityManager.goToApps();
    } else if (state == State::Browser || state == State::Saved || state == State::Error) {
      if (state == State::Error && errorFromBrowser_) {
        errorFromBrowser_ = false;
        state_.store(State::Browser, std::memory_order_release);
      } else {
        state_.store(State::Home, std::memory_order_release);
      }
      requestUpdate();
    } else if (state == State::Playing)
      stopPlayback();
    else
      activityManager.goToApps();
    return;
  }
  if (state == State::Home) {
    if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
      selectedHomeItem_ = (selectedHomeItem_ + 1) % 2;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::NavNext)) {
      selectedHomeItem_ = (selectedHomeItem_ + 1) % 2;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedHomeItem_ == 0)
        startRecording();
      else
        openBrowser(false);
      return;
    }
  }
  if (state == State::Browser) {
    if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, 800) && !entries_.empty()) {
      confirmDeleteSelected();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious) && !entries_.empty()) {
      selectedEntry_ = (selectedEntry_ + entries_.size() - 1) % entries_.size();
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) && !entries_.empty()) {
      selectedEntry_ = (selectedEntry_ + 1) % entries_.size();
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !entries_.empty()) {
      playBrowserEntry(selectedEntry_);
      return;
    }
  }
  // Front Left/Right adjust volume; side Up/Down switch tracks.
  // The front-left key raises the volume; the front-right key lowers it.
  // NavPrevious/NavNext combine both axes and must not be used while playing.
  if (state == State::Playing && mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    changeVolume(-5);
    return;
  }
  if (state == State::Playing && mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    changeVolume(5);
    return;
  }
  if (state == State::Playing && mappedInput.wasReleased(MappedInputManager::Button::Up) && playbackFromBrowser_ &&
      !entries_.empty()) {
    HalAudioOutput::shutdown();
    playbackFile = nullptr;
    if (file_.isOpen()) file_.close();
    state_.store(State::Browser, std::memory_order_release);
    const size_t previous = (selectedEntry_ + entries_.size() - 1) % entries_.size();
    playBrowserEntry(previous);
    return;
  }
  if (state == State::Playing && mappedInput.wasReleased(MappedInputManager::Button::Down) && playbackFromBrowser_ &&
      !entries_.empty()) {
    HalAudioOutput::shutdown();
    playbackFile = nullptr;
    if (file_.isOpen()) file_.close();
    state_.store(State::Browser, std::memory_order_release);
    const size_t next = (selectedEntry_ + 1) % entries_.size();
    playBrowserEntry(next);
    return;
  }
  if (state == State::Playing && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    HalAudioOutput::setPaused(!HalAudioOutput::isPaused());
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (state == State::Recording)
      requestStopRecording();
    else if (state == State::Saved)
      startPlayback();
    return;
  }
  if (state == State::Playing && !HalAudioOutput::isPlaying()) {
    HalAudioOutput::shutdown();
    playbackFile = nullptr;
    if (file_.isOpen()) file_.close();
    state_.store(playbackFromBrowser_ ? State::Browser : State::Saved, std::memory_order_release);
    requestUpdate();
    return;
  }
  if (state == State::Playing && !HalAudioOutput::isPaused() && millis() - lastPlaybackRefresh_ >= 5000) {
    lastPlaybackRefresh_ = millis();
    requestUpdate();
  }
  if (state == State::Recording) {
    const uint32_t seconds = samplesWritten_.load(std::memory_order_relaxed) / kSampleRate;
    if (seconds != lastRenderSecond_) {
      lastRenderSecond_ = seconds;
      requestUpdate();
    }
  }
}

void AudioActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, GUI.buttonHintsVisible(), false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight};
  const State state = state_.load(std::memory_order_acquire);
  const char* pageTitle = state == State::Browser ? (browserMusic_ ? tr(STR_AUDIO_MUSIC) : tr(STR_AUDIO_RECORDINGS))
                                                  : (musicApp_ ? tr(STR_AUDIO_MUSIC) : tr(STR_AUDIO_TITLE));
  if (state == State::Playing) {
    // The player owns its compact status line and uses the full safe height.
    const Rect player{safe.x + 20, safe.y + metrics.topPadding, std::max(1, safe.width - 40),
                      std::max(1, safe.height - metrics.topPadding)};
    renderPlayer(player);
    return;
  }
  GUI.drawHeader(renderer, header, pageTitle);
  const Rect body = pageContentRect();
  const Rect list{body.x + 12, body.y + 12, std::max(1, body.width - 24), std::max(1, body.height - 24)};
  if (state == State::Home) {
    const char* rows[] = {tr(STR_AUDIO_RECORD), tr(STR_AUDIO_RECORDINGS)};
    GUI.drawList(renderer, list, 2, selectedHomeItem_, [&rows](int i) { return std::string(rows[i]); });
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  if (state == State::Browser) {
    if (entries_.empty()) {
      const char* empty = tr(STR_AUDIO_NO_FILES);
      renderer.drawText(UI_12_FONT_ID, body.x + 16, body.y + body.height / 2, empty, true);
    } else {
      GUI.drawList(renderer, list, entries_.size(), selectedEntry_, [this](int i) { return entries_[i]; });
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    if (!entries_.empty()) {
      const char* hint = tr(STR_AUDIO_DELETE_HINT);
      renderer.drawText(UI_10_FONT_ID, body.x + 12, body.y + body.height - 12, hint, true);
    }
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }
  if (state == State::Playing) {
    renderPlayer(body);
    return;
  }
  const char* message = tr(STR_AUDIO_READY);
  if (state == State::Recording)
    message = tr(STR_AUDIO_RECORDING);
  else if (state == State::Stopping)
    message = tr(STR_AUDIO_SAVING);
  else if (state == State::Saved)
    message = tr(STR_AUDIO_SAVED);
  else if (state == State::Playing)
    message = musicApp_ ? tr(STR_AUDIO_MUSIC_PLAYING) : tr(STR_AUDIO_PLAYING);
  else if (state == State::Error)
    message = tr(STR_AUDIO_ERROR);
  const int font = UI_12_FONT_ID;
  const int width = renderer.getTextWidth(font, message);
  renderer.drawText(font, body.x + (body.width - width) / 2, body.y + body.height / 2, message, true);

  char detail[64];
  if (state == State::Recording) {
    const uint32_t seconds = samplesWritten_.load(std::memory_order_relaxed) / kSampleRate;
    snprintf(detail, sizeof(detail), "%02lu:%02lu", static_cast<unsigned long>(seconds / 60),
             static_cast<unsigned long>(seconds % 60));
  } else if (!path_.empty()) {
    snprintf(detail, sizeof(detail), "%s", path_.c_str());
  } else {
    detail[0] = '\0';
  }
  const char* action = state == State::Recording || state == State::Playing ? tr(STR_AUDIO_STOP)
                       : state == State::Saved                              ? tr(STR_AUDIO_PLAY)
                                                                            : "";
  const auto labels = mappedInput.mapLabels(
      tr(STR_BACK), action, state == State::Playing && playbackFromBrowser_ ? tr(STR_AUDIO_PREVIOUS) : "",
      state == State::Playing && playbackFromBrowser_ ? tr(STR_AUDIO_NEXT) : "");
  if (detail[0] != '\0') {
    const std::string shownDetail = renderer.truncatedText(UI_10_FONT_ID, detail, body.width);
    const int detailWidth = renderer.getTextWidth(UI_10_FONT_ID, shownDetail.c_str());
    renderer.drawText(UI_10_FONT_ID, body.x + (body.width - detailWidth) / 2, body.y + body.height / 2 + 34,
                      shownDetail.c_str(), true);
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

namespace {
void drawDiscRing(const GfxRenderer& renderer, int cx, int cy, int radius) {
  int previousX = cx + radius, previousY = cy;
  for (int step = 1; step <= 128; ++step) {
    const float angle = step * (6.28318530718f / 128);
    const int px = cx + static_cast<int>(radius * std::cos(angle));
    const int py = cy + static_cast<int>(radius * std::sin(angle));
    renderer.drawLine(previousX, previousY, px, py, false);
    previousX = px;
    previousY = py;
  }
}
void drawTriangle(const GfxRenderer& renderer, int cx, int cy, int width, int height, bool right) {
  for (int column = 0; column < width; ++column) {
    const int half = (right ? width - 1 - column : column) * height / (2 * width);
    renderer.drawLine(cx - width / 2 + column, cy - half, cx - width / 2 + column, cy + half);
  }
}
void drawSpeaker(const GfxRenderer& renderer, int x, int cy, bool loud) {
  renderer.fillRect(x, cy - 4, 5, 8);
  for (int column = 0; column < 8; ++column)
    renderer.drawLine(x + 4 + column, cy - 4 - column / 2, x + 4 + column, cy + 4 + column / 2);
  renderer.drawArc(10, x + 10, cy, 1, -1, 2, true);
  renderer.drawArc(10, x + 10, cy, 1, 1, 2, true);
  if (loud) {
    renderer.drawArc(16, x + 10, cy, 1, -1, 2, true);
    renderer.drawArc(16, x + 10, cy, 1, 1, 2, true);
  }
}
}  // namespace

void AudioActivity::renderPlayer(const Rect& body) {
  const bool paused = HalAudioOutput::isPaused();
  const char* heading = tr(STR_AUDIO_NOW_PLAYING);
  renderer.drawText(UI_10_FONT_ID, body.x + (body.width - renderer.getTextWidth(UI_10_FONT_ID, heading)) / 2,
                    body.y + 6, heading, true);
  renderer.drawLine(body.x + 2, body.y + 11, body.x + 8, body.y + 17, 2, true);
  renderer.drawLine(body.x + 8, body.y + 17, body.x + 14, body.y + 11, 2, true);
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawBatteryRight(
      renderer, Rect{body.x + body.width - metrics.batteryWidth, body.y, metrics.batteryWidth, metrics.batteryHeight},
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);

  // Leave room for metadata, two thin sliders and physical-key controls.
  const int size = std::max(1, std::min(body.width - 24, body.height - 300));
  const int x = body.x + (body.width - size) / 2;
  const int y = body.y + 38;
  bool coverDrawn = false;
  if (!coverPath_.empty()) {
    HalFile cover;
    if (Storage.openFileForRead("AudioCover", coverPath_, cover)) {
      Bitmap bitmap(cover, true);
      coverDrawn =
          bitmap.parseHeaders() == BmpReaderError::Ok && renderer.drawBitmapCropToFill(bitmap, x, y, size, size);
    }
    if (coverDrawn) renderer.maskRoundedRectOutsideCorners(x, y, size, size, 12, Color::White);
  }
  if (!coverDrawn) {
    renderer.fillRoundedRect(x, y, size, size, 12, Color::LightGray);
    const int cx = x + size / 2, cy = y + size / 2;
    const int radius = size * 5 / 12;
    renderer.fillRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, radius, Color::Black);
    for (int inset = 16; inset <= 32 && radius > inset; inset += 8) drawDiscRing(renderer, cx, cy, radius - inset);
    // Sparse highlights stay monochrome, unlike a phone's smooth gradient.
    for (int dy = -radius; dy < radius; ++dy) {
      for (int dx = -radius; dx < radius; ++dx) {
        if (dx * dx + dy * dy < radius * radius && std::abs(dx + 2 * dy) < radius / 6 && ((dx + 3 * dy) % 8 == 0))
          renderer.drawPixel(cx + dx, cy + dy, false);
      }
    }
    const int label = std::max(12, size / 8);
    renderer.fillRoundedRect(cx - label, cy - label, label * 2, label * 2, label, Color::White);
    const char* brand = tr(STR_AUDIO_DISC_LABEL);
    renderer.drawText(UI_12_FONT_ID, cx - renderer.getTextWidth(UI_12_FONT_ID, brand) / 2, cy - label / 2, brand, true);
    renderer.fillRoundedRect(cx - 3, cy + label / 2, 6, 6, 3, Color::Black);
  }
  const int textX = body.x + 12, textWidth = std::max(1, body.width - 24);
  const int titleY = y + size + 20;
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, songTitle_.c_str(), textWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, textX, titleY, title.c_str(), true, EpdFontFamily::BOLD);
  const std::string artist =
      renderer.truncatedText(UI_10_FONT_ID, artist_.empty() ? tr(STR_AUDIO_LOCAL_TRACK) : artist_.c_str(), textWidth);
  renderer.drawText(UI_10_FONT_ID, textX, titleY + 34, artist.c_str(), true);

  uint32_t elapsed = HalAudioOutput::positionMs() / 1000;
  if (durationSeconds_) elapsed = std::min(elapsed, durationSeconds_);
  // Distribute the controls proportionally down to the safe bottom edge.
  // Hidden button hints release their full height rather than leaving a blank strip.
  const int controlSpan = std::max(212, body.y + body.height - 32 - titleY);
  const int progressY = titleY + controlSpan * 78 / 212;
  renderer.fillRoundedRect(textX, progressY, textWidth, 6, 3, Color::LightGray);
  if (durationSeconds_ && elapsed > 0) {
    const int played = static_cast<int>(uint64_t(elapsed) * textWidth / durationSeconds_);
    if (played > 0) renderer.fillRoundedRect(textX, progressY, played, 6, 3, Color::Black);
  }
  char time[32];
  snprintf(time, sizeof(time), "%lu:%02lu", static_cast<unsigned long>(elapsed / 60),
           static_cast<unsigned long>(elapsed % 60));
  renderer.drawText(UI_10_FONT_ID, textX, progressY + 14, time, true);
  if (durationSeconds_) {
    const uint32_t remaining = durationSeconds_ - elapsed;
    snprintf(time, sizeof(time), "%s-%lu:%02lu", durationEstimated_ ? "~" : "",
             static_cast<unsigned long>(remaining / 60), static_cast<unsigned long>(remaining % 60));
  } else
    snprintf(time, sizeof(time), "--:--");
  renderer.drawText(UI_10_FONT_ID, textX + textWidth - renderer.getTextWidth(UI_10_FONT_ID, time), progressY + 14, time,
                    true);

  const int controlY = titleY + controlSpan * 147 / 212;
  const int center = body.x + body.width / 2;
  for (const int direction : {-1, 1}) {
    const int cx = center + direction * body.width / 3;
    drawTriangle(renderer, cx - 10, controlY, 20, 30, direction > 0);
    drawTriangle(renderer, cx + 10, controlY, 20, 30, direction > 0);
  }
  if (paused)
    drawTriangle(renderer, center + 3, controlY, 32, 42, true);
  else {
    renderer.fillRoundedRect(center - 17, controlY - 20, 10, 40, 3, Color::Black);
    renderer.fillRoundedRect(center + 7, controlY - 20, 10, 40, 3, Color::Black);
  }
  const int volumeY = titleY + controlSpan;
  drawSpeaker(renderer, textX, volumeY, false);
  drawSpeaker(renderer, textX + textWidth - 28, volumeY, true);
  const int volumeX = textX + 38, volumeWidth = std::max(1, textWidth - 78);
  renderer.fillRoundedRect(volumeX, volumeY - 3, volumeWidth, 6, 3, Color::LightGray);
  const int filled = volumeWidth * volume_ / 100;
  if (filled > 0) renderer.fillRoundedRect(volumeX, volumeY - 3, filled, 6, 3, Color::Black);
  renderer.fillRoundedRect(volumeX + filled - 4, volumeY - 4, 8, 8, 4, Color::Black);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), paused ? tr(STR_AUDIO_PLAY) : tr(STR_AUDIO_PAUSE),
                                            tr(STR_AUDIO_VOLUME_UP), tr(STR_AUDIO_VOLUME_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
