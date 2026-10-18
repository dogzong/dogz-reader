#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "network/ReaderAiClient.h"
namespace readerai {
struct Line {
  uint32_t start, end;
  // True for a line of the user's question, false for a line of the answer: the
  // transcript draws the first right aligned and the second left aligned, so the
  // alignment has to travel with the line once a page mixes turns.
  bool question = false;
};
// Memoises single-glyph width measurements.
//
// paginate() asks for one glyph at a time while wrapping, and a renderer
// measurement is expensive: resolveTextFontId() rescans the run to pick a face,
// and a glyph the resolved face does not cover falls through to onGlyphMiss(),
// which opens the .cpfont and reads glyph metadata. Repeating that for every
// occurrence of the same character dominated the layout -- a 36-line answer
// stalled the UI task for ~20 s (measured: `[LOOP] New max loop duration: 19620`).
// Only single-codepoint runs are cached; longer runs are measured directly, so
// wrapping cannot change. The table is 2 KiB of the Activity's heap, against a
// layout pass that already reserves 1024 lines twice over.
class GlyphWidthCache {
 public:
  int measure(const std::string& run, const std::function<int(const std::string&)>& measure) {
    const int cp = singleCodepoint(run);
    if (cp <= 0) return measure(run);
    const size_t base = (static_cast<uint32_t>(cp) * 2654435761u) % kSlots;
    for (size_t probe = 0; probe < kProbes; ++probe) {
      Entry& entry = slots_[(base + probe) % kSlots];
      if (entry.cp == cp) return entry.width;
      if (entry.cp == 0) {
        entry.width = measure(run);
        entry.cp = cp;
        return entry.width;
      }
    }
    return measure(run);
  }

 private:
  static constexpr size_t kSlots = 256;
  static constexpr size_t kProbes = 4;
  struct Entry {
    int cp = 0;
    int width = 0;
  };
  Entry slots_[kSlots];

  // Returns the codepoint when `run` holds exactly one, else 0.
  static int singleCodepoint(const std::string& run) {
    if (run.empty() || run.size() > 4) return 0;
    const auto* bytes = reinterpret_cast<const uint8_t*>(run.data());
    uint32_t cp = 0;
    size_t expected = 0;
    const uint8_t lead = bytes[0];
    if (lead < 0x80) {
      cp = lead;
      expected = 1;
    } else if ((lead & 0xe0) == 0xc0) {
      cp = lead & 0x1f;
      expected = 2;
    } else if ((lead & 0xf0) == 0xe0) {
      cp = lead & 0x0f;
      expected = 3;
    } else if ((lead & 0xf8) == 0xf0) {
      cp = lead & 0x07;
      expected = 4;
    } else {
      return 0;
    }
    if (run.size() != expected) return 0;
    for (size_t i = 1; i < expected; ++i) {
      if ((bytes[i] & 0xc0) != 0x80) return 0;
      cp = (cp << 6) | (bytes[i] & 0x3f);
    }
    return cp > 0 ? static_cast<int>(cp) : 0;
  }
};

inline bool paginate(const std::string& text, int width, const std::function<int(const std::string&)>& measure,
                     std::vector<Line>& lines) {
  lines.clear();
  if (text.size() > MaxTextBytes || !validUtf8(text) || width <= 0) return false;
  // At most 8 KiB of line offsets for a 24 KiB answer; no duplicate full text.
  lines.reserve(1024);
  size_t start = 0, i = 0;
  int used = 0;
  auto emit = [&](size_t end) {
    if (lines.size() >= 1024) return false;
    lines.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(end)});
    return true;
  };
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i]);
    const size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (c == '\n' || c == '\r') {
      if (!emit(i)) return false;
      i += n;
      if (c == '\r' && i < text.size() && text[i] == '\n') ++i;
      start = i;
      used = 0;
      continue;
    }
    const int glyphWidth = std::max(1, measure(text.substr(i, n)));
    if (i > start && used + glyphWidth > width) {
      // Per-glyph widths are only a hint. The renderer resolves one face for a
      // whole run, so summing single-glyph measurements under-reports what
      // drawText() actually paints and the tail of the line falls off screen.
      // Keep the longest prefix that still measures within width.
      size_t end = i;
      while (end > start && measure(text.substr(start, end - start)) > width) {
        size_t previous = end - 1;
        while (previous > start && (static_cast<unsigned char>(text[previous]) & 0xc0) == 0x80) --previous;
        end = previous;
      }
      if (end == start) end = i;  // A single glyph wider than the line still advances.
      if (!emit(end)) return false;
      start = end;
      used = 0;
      i = end;
      continue;
    }
    used += glyphWidth;
    i += n;
  }
  return emit(i);
}
}  // namespace readerai
