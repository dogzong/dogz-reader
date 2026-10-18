#pragma once
#include <cstdint>
// Two-frame interpolation; streaming and independent of file/decoder buffers.
class AudioResampler {
 public:
  void reset(uint32_t rate) {
    rate_ = rate;
    phase_ = 0;
    started_ = ended_ = tail_ = false;
  }
  template <class Read>
  bool read(Read&& next, int16_t& out) {
    if (!started_) {
      if (!next(a_)) return false;
      if (!next(b_)) {
        b_ = a_;
        tail_ = true;
      }
      started_ = true;
    }
    if (ended_ || rate_ == 0) return false;
    out =
        static_cast<int16_t>((static_cast<int64_t>(a_) * (16000 - phase_) + static_cast<int64_t>(b_) * phase_) / 16000);
    phase_ += rate_;
    while (phase_ >= 16000) {
      phase_ -= 16000;
      if (tail_) {
        ended_ = true;
        break;
      }
      a_ = b_;
      if (!next(b_)) {
        b_ = a_;
        tail_ = true;
      }
    }
    return true;
  }

 private:
  uint32_t rate_ = 16000, phase_ = 0;
  int16_t a_ = 0, b_ = 0;
  bool started_ = false, ended_ = false, tail_ = false;
};
