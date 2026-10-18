#pragma once
#include <Arduino.h>

#include <cstdint>

namespace sleepscreen {
constexpr const char* kDir = "/.crosspoint/sleep";
constexpr const char* kImage = "/.crosspoint/sleep/image.bmp";
// Weather, poem, battery, forecast, air and rain; no quota or lunar clock.
constexpr uint16_t kModules = 1 | 4 | 16 | 32 | 64 | 128;
struct Settings {
  uint8_t mode = 0;
  bool crop = false;
  uint16_t modules = kModules;
  char image[257]{};
  bool load();
  bool save() const;
};
void provision(const String& json);
}  // namespace sleepscreen
