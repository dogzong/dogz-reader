#!/usr/bin/env python3
"""Run the production audio worker against deterministic I2S pause/stop events."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root/'freeink-sdk/libs/hardware/AudioManager/src/AudioManager.cpp').read_text()
def method(signature, end):
    start = source.index(signature)
    return source[start:source.index(end,start)]
production = method('bool AudioManager::setPaused(', 'void AudioManager::stop()') + method('void AudioManager::taskLoop()', '\n}  // namespace freeink')
stubs = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <cstdio>
constexpr size_t READ_CHUNK = 1024;
using i2s_chan_handle_t = void*;
constexpr int ESP_OK = 0;
int pdMS_TO_TICKS(int n) { return n; }
void vTaskDelay(int) {}
void vTaskDelete(void*) {}
void log_e(const char*) {}
struct AudioConfig { int ampSettleMs; };
struct Board { AudioConfig audio; };
struct BoardConfig { inline static Board ACTIVE = {{0}}; };
class AudioManager {
public:
 enum class PlaybackState : uint8_t { Idle, Playing, Stopping, RestartPending };
 std::atomic<PlaybackState> playbackState_{PlaybackState::Playing};
 std::atomic<bool> paused_{false};
 std::atomic<uint32_t> playedFrames_{0};
 struct { std::function<int(uint8_t*,size_t)> read; std::function<bool(size_t)> seek; } source_;
 struct { uint32_t sampleRate=16000; uint16_t channels=1; size_t dataStart=0,dataLength=6*READ_CHUNK; } wav_;
 void* txChan_=nullptr;
 void* task_=nullptr;
 bool loop_=false,chanEnabled_=true;
 bool setPaused(bool);
 void taskLoop();
 void setAmp(bool) {}
};
AudioManager* current;
int reads=0,pcmWrites=0,pauseWrites=0;
bool stopWhilePaused=false;
int i2s_channel_disable(void*) { return ESP_OK; }
int i2s_channel_write(void*,const void* data,size_t size,size_t* written,int) {
 *written=size;
 const bool pcm=static_cast<const int16_t*>(data)[0]!=0;
 if(pcm) {
  ++pcmWrites;
  if(pcmWrites==1) assert(current->setPaused(true));
 } else if(current->paused_.load() && current->playbackState_.load()==AudioManager::PlaybackState::Playing) {
  ++pauseWrites;
  assert(reads==1); // paused silence never calls the source/advances decoder.
  if(pauseWrites==3) {
   if(stopWhilePaused) current->playbackState_.store(AudioManager::PlaybackState::Stopping);
   else assert(current->setPaused(false));
  }
 }
 return ESP_OK;
}
'''
tests = r'''
int main() {
 for(bool stop : {false,true}) {
  AudioManager audio; current=&audio; stopWhilePaused=stop;
  reads=pcmWrites=pauseWrites=0;
  audio.source_.read=[](uint8_t* p,size_t n) {
   ++reads; for(size_t i=0;i<n;i+=2) { int16_t value=1; memcpy(p+i,&value,2); } return int(n);
  };
  audio.source_.seek=[](size_t){return false;};
  audio.taskLoop();
  assert(pauseWrites==3);
  assert(audio.playbackState_.load()==AudioManager::PlaybackState::Idle);
  assert(!audio.paused_.load());
  assert(!audio.chanEnabled_);
  assert(reads==(stop?1:6));
  assert(audio.playedFrames_.load()==uint32_t(reads)*READ_CHUNK/2);
  assert(!audio.setPaused(true)); // idle cannot become stuck in a paused state.
 }
}
'''
with tempfile.TemporaryDirectory() as folder:
    cpp=Path(folder)/'pause.cpp';binary=Path(folder)/'pause'
    cpp.write_text(stubs+production+tests)
    subprocess.run(['c++','-std=c++17',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: production worker pause/resume keeps decoder position, frame count; stop while paused drains and exits')
