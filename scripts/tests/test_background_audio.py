#!/usr/bin/env python3
"""Exercise the real background session lifecycle against a fake audio worker."""
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    (d / 'HalStorage.h').write_text("""#pragma once
extern int event;
class HalFile { bool open_ = true; public: bool isOpen() const { return open_; } void close() { if (event != 1) __builtin_trap(); event=2; open_=false; } };
""")
    (d / 'HalAudioOutput.h').write_text('#pragma once\nnamespace HalAudioOutput { bool isPlaying(); void shutdown(); }\n')
    (d / 'test.cpp').write_text("""#include \"BackgroundAudio.h\"
#include <cassert>
int event=0; bool playing=true;
namespace HalAudioOutput { bool isPlaying(){return playing;} void shutdown(){event=1;playing=false;} }
int main(){auto& s=BackgroundAudio::session(); s.music=true; assert(BackgroundAudio::active()); BackgroundAudio::cleanupFinished(); assert(event==0); playing=false; BackgroundAudio::cleanupFinished(); assert(event==2); assert(!s.music); assert(!BackgroundAudio::active());}
""")
    subprocess.run(['c++', '-std=c++17', '-I'+str(d), '-I'+str(root/'src/activities/apps/audio'), str(root/'src/activities/apps/audio/BackgroundAudio.cpp'), str(d/'test.cpp'), '-o', str(d/'test')], check=True)
    subprocess.run([str(d/'test')], check=True)
print('PASS: background session stays alive; worker stops before file closes')
