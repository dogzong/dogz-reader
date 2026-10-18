#!/usr/bin/env python3
"""Check the production MPEG duration parser against real CBR/VBR encoder output."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)
    cpp = folder / 'duration.cpp'
    cpp.write_text(r'''
#include "AudioDuration.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc, char** argv) {
  uint8_t invalid[64] = {};
  assert(!audioduration::mp3(invalid, sizeof(invalid), 4096).seconds);
  // Reserved version and free-format headers must not divide by zero.
  uint8_t reserved[] = {0xff, 0xeb, 0x90, 0x00};
  assert(!audioduration::mp3(reserved, 4, 4096).seconds);
  uint8_t freeformat[] = {0xff, 0xfb, 0x00, 0x00};
  assert(!audioduration::mp3(freeformat, 4, 4096).seconds);
  for (int n = 1; n < argc; ++n) {
    std::ifstream in(argv[n], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
    size_t offset = 0;
    if (bytes.size() >= 10 && !memcmp(bytes.data(), "ID3", 3))
      offset = 10 + (size_t(bytes[6]) << 21) + (size_t(bytes[7]) << 14) + (size_t(bytes[8]) << 7) + bytes[9];
    auto result = audioduration::mp3(bytes.data() + offset, bytes.size() - offset, bytes.size() - offset);
    assert(result.seconds >= 9 && result.seconds <= 11);
    assert(!result.estimated); // ffmpeg emits frame-count metadata for both CBR and VBR.
    std::printf("%s: %u seconds, frame-count duration\n", argv[n], result.seconds);
  }
}
''')
    files = []
    for label, options in [('cbr', ['-b:a', '128k']), ('vbr', ['-q:a', '4'])]:
        path = folder / f'{label}.mp3'
        subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'sine=frequency=440:duration=10',
                        '-codec:a', 'libmp3lame', *options, str(path)], check=True)
        files.append(str(path))
    binary = folder / 'duration'
    subprocess.run(['c++', '-std=c++17', '-I', str(root/'src/activities/apps/audio'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary), *files], check=True)
print('PASS: real CBR/VBR MP3 durations, invalid and reserved headers')
