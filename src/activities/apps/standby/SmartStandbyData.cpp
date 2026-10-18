#include "SmartStandbyData.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstring>
namespace smartstandby {
namespace {
bool text(JsonVariantConst v, char* dst, size_t cap) {
  const char* value = v.as<const char*>();
  if (!value || strlen(value) >= cap) return false;
  memcpy(dst, value, strlen(value) + 1);
  return true;
}
}  // namespace
bool parse(const char* json, size_t bytes, Snapshot& out) {
  if (!json || bytes > 8192) return false;
  JsonDocument d;
  if (deserializeJson(d, json, bytes) || d["schemaVersion"].as<int>() != 1) return false;
  // Reset in place: a full Snapshot temporary would consume >3 KiB of the
  // network task stack. This record contains only scalars and fixed arrays.
  memset(&out, 0, sizeof(out));
  out.humidity = out.aqi = out.rain = -1;
  out.remaining[0] = out.remaining[1] = -1;
  text(d["snapshotId"], out.id, sizeof(out.id));
  auto wx = d["weather"];
  out.weatherAt = wx["updatedAt"] | int64_t(0);
  out.weatherValid = wx["current"].is<int>() && out.weatherAt >= 1700000000 &&
                     strcmp(wx["status"] | "unavailable", "unavailable") != 0;
  out.temperature = wx["current"] | 0;
  out.feels = wx["feels"] | 0;
  out.humidity = wx["humidity"] | -1;
  out.aqi = wx["aqi"] | -1;
  out.rain = wx["precip"] | -1;
  text(wx["city"], out.city, sizeof(out.city));
  text(wx["description"], out.description, sizeof(out.description));
  text(wx["sunset"], out.sunset, sizeof(out.sunset));
  for (auto day : wx["days"].as<JsonArrayConst>()) {
    if (out.dayCount == out.days.size()) break;
    auto& dest = out.days[out.dayCount];
    if (!text(day["date"], dest.date, sizeof(dest.date)) || strlen(dest.date) != 10) continue;
    text(day["description"], dest.description, sizeof(dest.description));
    dest.low = day["minimum"] | 0;
    dest.high = day["maximum"] | 0;
    ++out.dayCount;
  }
  auto q = d["codex"];
  out.quotaAt = q["updatedAt"] | int64_t(0);
  out.quotaFresh = strcmp(q["status"] | "", "fresh") == 0;
  for (int i = 0; i < 2; ++i) {
    auto w = q[i == 0 ? "fiveHour" : "sevenDay"];
    if (w["remainingPercent"].is<float>() || w["remainingPercent"].is<int>()) {
      const float n = w["remainingPercent"].as<float>();
      if (std::isfinite(n) && n >= 0 && n <= 100) out.remaining[i] = std::lround(n);
    }
    out.resetAt[i] = w["resetsAt"] | int64_t(0);
  }
  for (auto p : d["poems"].as<JsonArrayConst>()) {
    if (out.poemCount == out.poems.size()) break;
    const bool full = strcmp(p["kind"] | "", "full") == 0;
    const bool quote = strcmp(p["kind"] | "", "quote") == 0;
    const size_t count = p["lines"].size();
    if ((!full && !quote) || (full && count != 4) || (quote && (count < 1 || count > 2))) continue;
    auto& dest = out.poems[out.poemCount];
    text(p["id"], dest.rasterId, sizeof(dest.rasterId));
    if (strlen(dest.rasterId) != 24 || strspn(dest.rasterId, "0123456789abcdef") != 24) dest.rasterId[0] = 0;
    bool ok = text(p["title"], dest.title, sizeof(dest.title)) && text(p["author"], dest.author, sizeof(dest.author));
    for (size_t i = 0; i < count; ++i) ok = text(p["lines"][i], dest.lines[i].data(), dest.lines[i].size()) && ok;
    if (ok) ++out.poemCount;
  }
  return true;
}
}  // namespace smartstandby
