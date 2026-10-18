#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
// Head/tail silence trimming for the voice-assistant recording. The decision
// only needs per-window peak levels, so this stays a pure, allocation-free
// header: the caller streams the WAV from storage and passes a compact peak
// array in. Upload time on this device is ~15.7 KB/s (lwIP TCP window vs. RTT),
// so every silent byte removed is a byte not waited for.
namespace readerai {
// 16 kHz / 16-bit / mono, the format HalAudioOutput::beginCapture(16000) writes.
constexpr uint32_t kSampleRate = 16000;
// 10 ms analysis window. Long enough that a single-sample click or a codec
// pop cannot flip a window to "voiced" (the peak is smoothed over 160 samples),
// short enough that snapping to a window edge costs at most 10 ms of padding.
constexpr uint32_t kWindow = kSampleRate / 100;
// Absolute noise gate, in raw int16 units (300 / 32768 = -40.8 dBFS). Below
// this a window is hiss or converter idle, never speech: intelligible speech
// peaks far above -40 dBFS even with a modest mic preamp. It only binds on a
// very quiet recording, where the failure mode is "no trim", not "cut audio".
constexpr int32_t kSilenceFloor = 300;
// Relative gate: a window counts as voiced only at >= 2% (-34 dB) of the
// recording's own peak window. Within one utterance speech stays well inside
// 30 dB of its peak, so this keeps every phoneme while discarding silence that
// a quiet room raises above kSilenceFloor (fans, breathing, handling noise).
constexpr int32_t kSilencePeakDivisor = 50;
// 200 ms kept on each side of the voiced span. ASR needs the onset and release
// of the first/last phoneme: a plosive or fricative ramps up over tens of ms
// and its first window can still sit under the gate, so cutting flush to the
// detected edge clips the very sound being transcribed. 200 ms costs 12.8 KB
// (~0.4 s of upload) — cheap insurance against a lost word.
constexpr uint32_t kPadSamples = kSampleRate / 5;
// Do not rewrite a file at all unless it drops below this many samples: an
// accidental 10 ms "trim" would cost a full rewrite plus a second file on the
// card and save nothing.
constexpr uint32_t kMinKeepSamples = kSampleRate * 3 / 10;

// Largest |sample| in a block of little-endian PCM. memcpy + int16_t keeps the
// unaligned read defined (RISC-V faults on a raw reinterpret_cast in place).
// Number of peak windows needed to describe `samples` samples.
//
// Rounded UP on purpose. The scan writes the window a sample belongs to, so the
// highest index it can touch is (samples - 1) / kWindow, which equals
// samples / kWindow when the recording is not a whole number of windows. Sizing
// the array with the truncated quotient therefore left it one element short and
// the scan wrote two bytes past its end -- straight into the next heap block's
// header, which only surfaced much later as
// `assert failed: multi_heap_free multi_heap_poisoning.c:279 (head != NULL)`.
constexpr uint32_t windowCount(uint32_t samples) {
  return samples == 0 ? 0 : (samples + kWindow - 1) / kWindow;
}

inline uint16_t windowPeak(const uint8_t* pcm, size_t bytes) {
  uint16_t peak = 0;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    int16_t sample = 0;
    memcpy(&sample, pcm + i, sizeof(sample));
    const int32_t magnitude = sample < 0 ? -static_cast<int32_t>(sample) : sample;
    if (magnitude > peak) peak = static_cast<uint16_t>(magnitude);
  }
  return peak;
}

// Sample range [start, end) to keep for one recording. `end == total` and
// `start == 0` also mean "keep everything".
struct Trim {
  uint32_t start = 0;
  uint32_t end = 0;
};

// Decide the keep range from the per-window peaks of `total` samples. Returns
// the whole recording unchanged when there is nothing to gain: no window clears
// the gate, or the gated span plus padding would be shorter than kMinKeepSamples.
// Pure and allocation-free, so the host test can drive it directly.
inline Trim trimRange(const uint16_t* peaks, uint32_t count, uint32_t total) {
  const Trim keepAll{0, total};
  if (!peaks || count == 0 || total == 0) return keepAll;
  uint16_t maxPeak = 0;
  for (uint32_t i = 0; i < count; ++i)
    if (peaks[i] > maxPeak) maxPeak = peaks[i];
  const int32_t relative = maxPeak / kSilencePeakDivisor;
  const int32_t gate = relative > kSilenceFloor ? relative : kSilenceFloor;
  uint32_t first = 0, last = 0;
  bool any = false;
  for (uint32_t i = 0; i < count; ++i) {
    if (peaks[i] < gate) continue;
    if (!any) first = i;
    last = i;
    any = true;
  }
  if (!any) return keepAll;
  // Pad outside the gated span but never outside the recording; unsigned
  // arithmetic keeps the clamp well defined at both ends.
  const uint32_t padWindows = kPadSamples / kWindow;
  const uint32_t startWindow = first > padWindows ? first - padWindows : 0;
  const uint32_t endWindow = last + padWindows + 1 < count ? last + padWindows + 1 : count;
  const Trim kept{startWindow * kWindow, endWindow * kWindow < total ? endWindow * kWindow : total};
  if (kept.end - kept.start < kMinKeepSamples) return keepAll;
  return kept;
}
}  // namespace readerai
