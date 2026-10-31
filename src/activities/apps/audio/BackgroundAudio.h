#pragma once
#include <HalStorage.h>
#include <cstdint>
#include <string>

// State owned independently of the foreground Activity; accessed by the UI
// task. The audio task only reads file via the existing playback callback.
namespace BackgroundAudio {
struct Session {
  HalFile file;
  std::string path, title, artist, cover;
  uint32_t duration = 0;
  bool estimated = false;
  uint8_t volume = 45;
  bool music = false;
};
Session& session();
bool active();
void stop();
void cleanupFinished();
}
