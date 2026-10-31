#include "BackgroundAudio.h"
#include <HalAudioOutput.h>

namespace BackgroundAudio {
Session& session() { static Session value; return value; }
bool active() { return session().music && HalAudioOutput::isPlaying(); }
void stop() {
  auto& s = session();
  HalAudioOutput::shutdown(); // joins audio worker before closing its file
  if (s.file.isOpen()) s.file.close();
  s.music = false;
}
void cleanupFinished() {
  if (session().music && !HalAudioOutput::isPlaying()) stop();
}
}
