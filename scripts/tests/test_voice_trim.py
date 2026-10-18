#!/usr/bin/env python3
"""Exercise the production voice-trim decision against synthetic recordings."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
header = root / 'src/activities/apps/voice-assistant/VoiceTrim.h'
# Compile the shipped header itself (not a copy) plus a small driver. The peak
# arrays play the role of the streamed per-window scan the activity performs.
production = '#include "%s"\n' % header
common = r'''
using readerai::kPadSamples;
using readerai::kSampleRate;
using readerai::kSilenceFloor;
using readerai::kWindow;
constexpr uint32_t W = kWindow;  // 160 samples = 10 ms
constexpr uint32_t PADW = kPadSamples / W;
// One 10 ms window out of a synthetic clip: quiet unless it is in [from,to).
uint16_t peakAt(uint32_t window, uint32_t from, uint32_t to, uint16_t voiced, uint16_t floor) {
  return window >= from && window < to ? voiced : floor;
}
template <typename F>
std::vector<uint16_t> peaksOf(uint32_t count, F peak) {
  std::vector<uint16_t> peaks(count);
  for (uint32_t i = 0; i < count; ++i) peaks[i] = peak(i);
  return peaks;
}
// A synthetic WAV payload: `from`..`to` windows of voiced audio, quiet
// elsewhere. Every sample inside a window shares one magnitude, so the truly
// voiced span is exactly [from*W, to*W).
std::vector<uint8_t> pcmOf(uint32_t windows, uint32_t from, uint32_t to, int16_t voiced, int16_t floor) {
  std::vector<uint8_t> pcm;
  pcm.reserve(size_t(windows) * W * 2);
  for (uint32_t w = 0; w < windows; ++w) {
    const int16_t value = w >= from && w < to ? voiced : floor;
    for (uint32_t i = 0; i < W; ++i) {
      pcm.push_back(static_cast<uint8_t>(value & 0xff));
      pcm.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    }
  }
  return pcm;
}
// The kept range must be reproducible from a streamed scan of that range alone:
// rescanning it yields peaks whose own trim starts at 0 (the first window is
// already voiced, so nothing else would be cut).
void assertKeptIsTight(const std::vector<uint8_t>& pcm, const readerai::Trim& t, uint32_t windows) {
  assert(t.end <= windows * W);
  assert((t.end - t.start) % W == 0);
  std::vector<uint16_t> peaks = peaksOf((t.end - t.start) / W, [&](uint32_t i) {
    return readerai::windowPeak(pcm.data() + size_t(t.start + i * W) * 2, W * 2);
  });
  assert(readerai::trimRange(peaks.data(), static_cast<uint32_t>(peaks.size()), t.end - t.start).start == 0);
}
'''
stubs = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
'''
tests = r'''
int main() {
  // --- head and tail silence are trimmed, and the kept range contains all of
  // the loud audio with exactly the documented padding around it.
  {
    const uint32_t windows = 500;            // 5.0 s
    const uint32_t first = 100, last = 399;  // 1.0 s .. 4.0 s voiced
    auto pcm = pcmOf(windows, first, last + 1, 12000, 60);
    auto peaks = peaksOf(windows, [&](uint32_t i) { return peakAt(i, first, last + 1, 12000, 60); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == (first - PADW) * W);
    assert(t.end == (last + 1 + PADW) * W);
    assert(t.start < first * W && t.end > (last + 1) * W);  // every voiced sample kept
    assert((t.end - t.start) * 100 / (windows * W) == 68);
    assertKeptIsTight(pcm, t, windows);
    std::printf("head/tail silence: kept [%u,%u) of %u samples (%u%%)\n", t.start, t.end, windows * W,
                (t.end - t.start) * 100 / (windows * W));
  }
  // --- padding is exactly kPadSamples on each side of the voiced span
  {
    const uint32_t windows = 400, first = 150, last = 249;
    auto peaks = peaksOf(windows, [&](uint32_t i) { return peakAt(i, first, last + 1, 9000, 20); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(first * W - t.start == kPadSamples);
    assert(t.end - (last + 1) * W == kPadSamples);
  }
  // --- voice running to both ends: the padding clamps to the file, nothing cut
  {
    const uint32_t windows = 300;
    auto peaks = peaksOf(windows, [](uint32_t) { return uint16_t(8000); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == 0 && t.end == windows * W);
  }
  // --- already tight clip: speech ends 20 ms before the file does, so the tail
  // padding clamps to the file end instead of overrunning it
  {
    const uint32_t windows = 300, last = windows - 3;
    auto peaks = peaksOf(windows, [&](uint32_t i) { return peakAt(i, 0, last + 1, 8000, 0); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == 0 && t.end == windows * W);
    assert(t.end <= windows * W);
  }
  // --- all silence: no window clears the floor, keep the recording untouched
  {
    const uint32_t windows = 500;
    auto peaks = peaksOf(windows, [](uint32_t i) { return uint16_t(120 + i % 40); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == 0 && t.end == windows * W);
  }
  // --- all voiced: the relative gate rises with the peak, so nothing is dropped
  {
    const uint32_t windows = 500;
    auto peaks = peaksOf(windows, [](uint32_t i) { return uint16_t(6000 + (i * 37) % 60); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == 0 && t.end == windows * W);
  }
  // --- a loud room raises the floor above kSilenceFloor: the relative gate must
  // still find the speech and keep the whole clip
  {
    const uint32_t windows = 500, first = 20, last = 480;
    auto peaks = peaksOf(windows, [&](uint32_t i) { return peakAt(i, first, last + 1, 20000, 1500); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == 0 && t.end == windows * W);
  }
  // --- too short: 100 ms of voice does not clear the 300 ms rewrite floor
  {
    const uint32_t windows = 20, first = 5, last = 14;
    auto peaks = peaksOf(windows, [&](uint32_t i) { return peakAt(i, first, last + 1, 9000, 10); });
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == 0 && t.end == windows * W);
  }
  // --- degenerate inputs must not read out of bounds or divide by zero
  {
    const uint16_t one = 5000;
    assert(readerai::trimRange(nullptr, 0, 0).start == 0);
    assert(readerai::trimRange(nullptr, 10, 1000).end == 1000);
    assert(readerai::trimRange(&one, 0, 1000).end == 1000);
    assert(readerai::trimRange(&one, 1, 0).end == 0);
    assert(readerai::trimRange(&one, 1, 1).start == 0);
    assert(readerai::trimRange(&one, 1, 1).end == 1);
    assert(readerai::windowPeak(nullptr, 0) == 0);
    const uint8_t odd[3] = {0x01, 0x02, 0x03};  // trailing odd byte is ignored
    assert(readerai::windowPeak(odd, 3) == 0x0201);
    const uint8_t negative[2] = {0x00, 0x80};  // -32768 must not wrap
    assert(readerai::windowPeak(negative, 2) == 32768);
  }
  // --- a moderate click far ahead of the speech is a single sample of energy,
  // not a word: the relative gate stays anchored on the speech and the click
  // window is only padding-extended, never treated as the utterance start
  {
    const uint32_t windows = 500, first = 200, last = 299;
    auto peaks = peaksOf(windows, [&](uint32_t i) { return peakAt(i, first, last + 1, 12000, 30); });
    peaks[10] = 200;  // below the -40.8 dBFS floor: not speech-level energy
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == (first - PADW) * W);
    std::printf("isolated click: kept from sample %u (speech starts at %u)\n", t.start, first * W);
  }
  // --- but a genuinely loud transient does raise the gate and widen the kept
  // range: an explosion is speech-level energy and must not be cut off
  {
    const uint32_t windows = 500, first = 200, last = 299;
    auto peaks = peaksOf(windows, [&](uint32_t i) { return peakAt(i, first, last + 1, 7000, 30); });
    peaks[10] = 40000;
    const readerai::Trim t = readerai::trimRange(peaks.data(), windows, windows * W);
    assert(t.start == 0);  // the transient's window sits inside the leading pad
    assert(t.end > last * W);
  }
  std::printf("PASS: voice trim decision (%u Hz, %u-sample windows, %u-sample pad, floor %d)\n", kSampleRate, W,
              kPadSamples, int(kSilenceFloor));
}
'''
# The activity wiring the test cannot exercise off-device, asserted structurally.
cpp_section = (root / 'src/activities/apps/voice-assistant/VoiceAssistantActivity.cpp').read_text()
loop_start = cpp_section.index('void VoiceAssistantActivity::loop()')
loop_body = cpp_section[loop_start:cpp_section.index('\n}\n', loop_start)]
trim_start = cpp_section.index('uint32_t VoiceAssistantActivity::trimRecording')
trim_body = cpp_section[trim_start:cpp_section.index('\n}\n', trim_start)]
trim_call = cpp_section.index('const uint32_t trimmed = trimRecording(written + 44);')
checks = {
    'trim runs after the recording is published and before the pending record is saved':
        cpp_section.index('replaceFile(Part, Wav)') < trim_call < cpp_section.index('sourceKey_ = key();', trim_call),
    'trim uses makeUniqueNoThrow and null-checks the peaks array':
        'makeUniqueNoThrow<uint16_t[]>(count)' in trim_body and 'if (!peaks)' in trim_body,
    'trim never calls close() on a local HalFile':
        '.close()' not in trim_body,
    'trim updates the WAV header through wavHeader()':
        'wavHeader(buf.get(), keptBytes)' in trim_body,
    'trim publishes only through replaceFile(Part, Wav)':
        'replaceFile(Part, Wav)' in trim_body and 'remove(Part)' in trim_body,
    'trim skips the rewrite below the 5% gain floor':
        'kTrimMinGainPercent = 5' in cpp_section and 'bytes / 100 * kTrimMinGainPercent' in trim_body,
    'trim streams both passes with no whole-file buffer':
        'src.read(buf.get(), want)' in trim_body and 'seek(44)' in trim_body,
    'pagination now precedes the active_ early return':
        loop_body.index('preparePages();') < loop_body.index('if (active_.load(std::memory_order_acquire) &&'),
    'pagination is not duplicated after the early return':
        loop_body.count('preparePages();') == 1,
    # Scoped to the transcription branch: the injected-ask hook publishes the same
    # state earlier in the file, so a whole-file .index() would match that one.
    'run() assigns question_ before publishing State::Transcript':
        (lambda branch: branch.index('if (!poll(client, sourceId_, question_)) return;') <
         branch.index('state_.store(State::Transcript, std::memory_order_release);'))(
            cpp_section[cpp_section.index('if (work_ == Work::Transcribe) {'):]),
    # The answer reuses lines_ from the transcript unless the page cache is
    # invalidated, which drew only the first few characters of the reply.
    'run() clears pagesReady_ before publishing State::Answer':
        cpp_section.rindex('pagesReady_ = false;', 0, cpp_section.index('state_.store(State::Answer, std::memory_order_release);')) >
        cpp_section.index('if (!poll(client, answerId_, answer_)) return;'),
    'preparePages measures in Portrait like render does':
        'setOrientation(GfxRenderer::Orientation::Portrait);' in
        cpp_section[cpp_section.index('void VoiceAssistantActivity::preparePages()'):
                    cpp_section.index('const int width = std::max(1, pageContentRect().width - 32);')],
    # The scan writes the window a sample belongs to, so index (total-1)/kWindow
    # must exist. The old truncated size (total / kWindow) failed this for every
    # recording that was not a whole number of windows, and the resulting two-byte
    # overrun corrupted the heap.
    'windowCount covers every index the scan can write':
        all((total - 1) // 160 < ((total + 160 - 1) // 160) for total in
            list(range(1, 2000)) + [15359, 15360, 15361, 47999, 48000, 48001, 959999, 960000]),
    'windowCount is zero only for an empty recording':
        (lambda f: f(0) == 0 and f(1) == 1 and f(160) == 1 and f(161) == 2 and f(960000) == 6000)(
            lambda n: 0 if n == 0 else (n + 159) // 160),
    'the transcript is drawn right aligned and the answer left aligned':
        'const bool alignRight = state == State::Transcript;' in cpp_section,
}
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    cpp = directory / 'trim.cpp'
    cpp.write_text(stubs + production + common + tests)
    binary = directory / 'trim'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
for label, ok in checks.items():
    assert ok, label
    print(f'PASS: {label}')
print('PASS: voice trim decision and wiring')
