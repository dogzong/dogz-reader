#include <cassert>
#include <cmath>

#include "AudioResampler.h"
int main() {
  for (unsigned rate : {8000u, 16000u, 22050u, 44100u, 48000u}) {
    AudioResampler r;
    r.reset(rate);
    unsigned pos = 0, count = 0;
    int16_t out;
    auto read = [&](int16_t& v) {
      if (pos++ >= rate) return false;
      v = 1234;
      return true;
    };
    while (r.read(read, out)) {
      assert(out == 1234);
      ++count;
    }
    assert(count == 16000);
  }
  AudioResampler r;
  r.reset(48000);
  unsigned pos = 0, count = 0;
  int16_t out;
  auto tone = [&](int16_t& v) {
    if (pos >= 48000) return false;
    v = static_cast<int16_t>(10000 * std::sin(2 * 3.141592653589793 * 440 * pos++ / 48000));
    return true;
  };
  while (r.read(tone, out)) {
    assert(std::abs(out - 10000 * std::sin(2 * 3.141592653589793 * 440 * count / 16000)) < 3);
    ++count;
  }
  assert(count == 16000);
}
