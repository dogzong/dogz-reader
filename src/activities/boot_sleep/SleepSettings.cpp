#include "SleepSettings.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace sleepscreen {
namespace {
constexpr const char* kConfig = "/.crosspoint/sleep/settings.json";
bool decode(JsonVariantConst d, Settings& settings) {
  const int mode = d["mode"] | 0;
  const char* image = d["image"] | "";
  if ((mode != 0 && mode != 1) || strlen(image) >= sizeof(settings.image) ||
      (image[0] && (image[0] != '/' || strstr(image, "..") || strpbrk(image, "\r\n"))))
    return false;
  Settings result;
  result.mode = mode;
  result.crop = (d["crop"] | 0) != 0;
  result.modules = (d["modules"] | int(kModules)) & kModules;
  snprintf(result.image, sizeof(result.image), "%s", image);
  settings = result;
  return true;
}
}  // namespace
bool Settings::load() {
  std::string json;
  JsonDocument d;
  // Bounded JSON allocation, released before any display decoding; no framebuffer copy.
  if (!Storage.readFileToString("SLP", kConfig, 1024, json) || deserializeJson(d, json) || d["version"].as<int>() != 1)
    return false;
  return decode(d.as<JsonVariantConst>(), *this);
}
bool Settings::save() const {
  JsonDocument d;
  d["version"] = 1;
  d["mode"] = mode;
  d["crop"] = int(crop);
  d["modules"] = modules;
  d["image"] = image;
  Settings validated;
  if (!decode(d.as<JsonVariantConst>(), validated) || !Storage.ensureDirectoryExists(kDir)) return false;
  const char* part = "/.crosspoint/sleep/settings.json.part";
  {
    auto file = Storage.open(part, O_WRITE | O_CREAT | O_TRUNC);
    if (!file || serializeJson(d, file) != measureJson(d)) return false;
    file.flush();
  }
  return Storage.replaceFile(part, kConfig);
}
void provision(const String& json) {
  JsonDocument d;
  Settings settings;
  const bool okay = json.length() <= 1024 && !deserializeJson(d, json) && decode(d.as<JsonVariantConst>(), settings) &&
                    settings.save();
  logSerial.println(okay ? "SLEEP_CONFIG:OK" : "SLEEP_CONFIG:FAILED");
}
}  // namespace sleepscreen
