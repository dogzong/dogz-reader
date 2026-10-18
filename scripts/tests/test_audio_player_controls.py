#!/usr/bin/env python3
"""Exercise the production playing-state key handler (press + release gestures)."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
source=(root/'src/activities/apps/audio/AudioActivity.cpp').read_text()
start=source.index('  // Front Left/Right adjust volume;')
end=source.index('  if (state == State::Playing && mappedInput.wasReleased(MappedInputManager::Button::Confirm))',start)
production=source[start:end]
stubs=r'''
#include <atomic>
#include <cassert>
#include <cstddef>
#include <vector>
struct MappedInputManager {
 enum class Button { Up,Down,Left,Right,Confirm };
 unsigned press=0,release=0;
 bool wasPressed(Button b)const{return press&(1u<<unsigned(b));}
 bool wasReleased(Button b)const{return release&(1u<<unsigned(b));}
};
namespace HalAudioOutput { void shutdown() {} }
void* playbackFile=nullptr;
struct File {bool isOpen(){return false;}void close(){}};
struct Player {
 enum class State {Playing,Browser};
 std::atomic<State> state_{State::Playing};
 MappedInputManager mappedInput;
 bool playbackFromBrowser_=true;
 std::vector<int> entries_{0,1,2};
 size_t selectedEntry_=1;
 File file_;
 int volume=50,volumeActions=0,trackActions=0;
 void changeVolume(int delta){volume+=delta;++volumeActions;}
 void playBrowserEntry(size_t index){selectedEntry_=index;++trackActions;state_=State::Playing;}
 void handle(){const State state=state_.load();
'''
tests=r'''
 }
 void gesture(MappedInputManager::Button b){
  mappedInput.press=1u<<unsigned(b);mappedInput.release=0;handle();
  mappedInput.press=0;mappedInput.release=1u<<unsigned(b);handle();
  mappedInput.release=0;
 }
};
int main(){
 using B=MappedInputManager::Button;
 Player p;
 p.gesture(B::Left);assert(p.volume==55&&p.volumeActions==1&&p.trackActions==0);
 p.gesture(B::Right);assert(p.volume==50&&p.volumeActions==2&&p.trackActions==0);
 p.gesture(B::Up);assert(p.selectedEntry_==0&&p.trackActions==1&&p.volumeActions==2);
 p.gesture(B::Up);assert(p.selectedEntry_==2&&p.trackActions==2&&p.volume==50);
 p.gesture(B::Down);assert(p.selectedEntry_==0&&p.trackActions==3&&p.volume==50);
 p.entries_.clear();p.gesture(B::Down);assert(p.trackActions==3);
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp=Path(directory)/'controls.cpp';binary=Path(directory)/'controls'
    cpp.write_text(stubs+production+tests)
    subprocess.run(['c++','-std=c++17',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: front Left louder / Right quieter; side Up/Down tracks; wraparound and empty list; one action per gesture')
