#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace smartstandby {
struct PoemData {
  char title[64]{}, author[48]{}, rasterId[25]{};
  std::array<std::array<char, 96>, 4> lines{};
};
struct Day {
  char date[11]{}, description[48]{};
  int16_t low = 0, high = 0;
};
struct Snapshot {
  char id[25]{};
  int64_t weatherAt = 0, quotaAt = 0;
  int16_t temperature = 0, feels = 0, humidity = -1, aqi = -1, rain = -1;
  int16_t remaining[2] = {-1, -1};
  int64_t resetAt[2] = {};
  char city[32]{}, description[48]{}, sunset[8]{};
  std::array<Day, 3> days{};
  std::array<PoemData, 5> poems{};
  uint8_t dayCount = 0, poemCount = 0;
  bool weatherValid = false;
  bool quotaFresh = false;
};
bool parse(const char* json, size_t bytes, Snapshot& out);
}  // namespace smartstandby
