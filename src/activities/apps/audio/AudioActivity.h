#pragma once

#include <HalStorage.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

class AudioActivity final : public Activity {
 public:
  AudioActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool musicApp = false)
      : Activity(musicApp ? "Music" : "Audio", renderer, mappedInput), musicApp_(musicApp) {}

  void onEnter() override;
  bool ownsAudio() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return workerActive_.load(std::memory_order_acquire) || state_.load(std::memory_order_acquire) == State::Playing;
  }

 private:
  enum class State : uint8_t { Home, Browser, Recording, Stopping, Saved, Playing, Error };
  static void recordingTask(void* context);
  void startRecording();
  void requestStopRecording();
  bool startPlayback();
  void openBrowser(bool music);
  void stopPlayback();
  void renderPlayer(const Rect& body);
  void changeVolume(int delta);
  void confirmDeleteSelected();
  bool playBrowserEntry(size_t index);

  const bool musicApp_;
  HalFile file_;
  std::string path_;
  std::vector<std::string> entries_;
  size_t selectedEntry_ = 0;
  uint8_t selectedHomeItem_ = 0;
  uint8_t volume_ = 45;
  bool volumeDirty_ = false;
  std::string songTitle_;
  std::string artist_;
  std::string coverPath_;
  uint32_t durationSeconds_ = 0;
  bool durationEstimated_ = false;
  uint32_t lastPlaybackRefresh_ = 0;
  bool browserMusic_ = false;
  bool playbackFromBrowser_ = false;
  bool errorFromBrowser_ = false;
  std::atomic<State> state_{State::Home};
  std::atomic<bool> workerActive_{false};
  std::atomic<uint32_t> samplesWritten_{0};
  uint32_t lastRenderSecond_ = UINT32_MAX;
};
