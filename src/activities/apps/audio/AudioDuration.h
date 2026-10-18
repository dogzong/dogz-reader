#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audioduration {
struct Duration {
  uint32_t seconds = 0;
  bool estimated = false;
};
inline uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t le32(const uint8_t* p) {
  return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
// Read only the first MPEG Layer III frame, with no song-sized allocation.
inline Duration mp3(const uint8_t* frame, size_t available, size_t audioBytes) {
  if (available < 4) return {};
  const uint32_t header = be32(frame);
  const unsigned version = (header >> 19) & 3, layer = (header >> 17) & 3;
  const unsigned bitrateIndex = (header >> 12) & 15, rateIndex = (header >> 10) & 3;
  if ((header & 0xffe00000) != 0xffe00000 || version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 ||
      rateIndex == 3)
    return {};
  constexpr uint16_t rates[] = {44100, 48000, 32000};
  constexpr uint16_t mpeg1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  constexpr uint16_t mpeg2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
  const unsigned rate = rates[rateIndex] / (version == 3 ? 1 : version == 2 ? 2 : 4);
  const unsigned samples = version == 3 ? 1152 : 576;
  const bool mono = ((header >> 6) & 3) == 3;
  const size_t sideInfo = version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
  const size_t xing = 4 + (((header >> 16) & 1) ? 0 : 2) + sideInfo;
  uint32_t frames = 0;
  if (available >= xing + 12 && (!memcmp(frame + xing, "Xing", 4) || !memcmp(frame + xing, "Info", 4)) &&
      (be32(frame + xing + 4) & 1))
    frames = be32(frame + xing + 8);
  if (!frames && available >= 54 && !memcmp(frame + 36, "VBRI", 4)) frames = be32(frame + 50);
  if (frames) return {static_cast<uint32_t>(uint64_t(frames) * samples / rate), false};
  const unsigned kbps = version == 3 ? mpeg1[bitrateIndex] : mpeg2[bitrateIndex];
  // Files without Xing/VBRI are estimates, explicitly marked in the UI.
  return {static_cast<uint32_t>(uint64_t(audioBytes) * 8 / (kbps * 1000)), true};
}
}  // namespace audioduration
