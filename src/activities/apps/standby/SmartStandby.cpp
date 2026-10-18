#include "SmartStandby.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <Util.h>
#include <WiFi.h>
#include <esp_system.h>

#include <algorithm>
#include <cmath>
#include <ctime>

#include "ChineseAlmanac.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "fonts/sleep_date.h"
#include "fonts/sleep_meta.h"
#include "fonts/sleep_number.h"
#include "fonts/sleep_poem.h"
#include "fonts/sleep_title.h"
#include "fonts/sleep_week.h"
#include "fonts/standby_clock.h"
#include "fonts/standby_meta.h"
#include "fonts/standby_weather.h"
#include "util/TimeUtils.h"

namespace smartstandby {
namespace {
constexpr const char* kConfig = "/.crosspoint/standby/settings.json";
bool text(JsonVariantConst v, char* dst, size_t cap) {
  const char* value = v.as<const char*>();
  if (!value || strlen(value) >= cap) return false;
  memcpy(dst, value, strlen(value) + 1);
  return true;
}
void rasterPath(const char* id, char* path, size_t cap) { snprintf(path, cap, "/.crosspoint/standby/poem-%s.bmp", id); }
void sleepRasterPath(const char* id, char* path, size_t cap) {
  snprintf(path, cap, "/.crosspoint/standby/sleep1-%s.bmp", id);
}
bool sleepTileValid(const char* path) {
  auto file = Storage.open(path);
  if (!file || file.size() != 13374) return false;
  Bitmap bitmap(file, true);
  return bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() == 416 && bitmap.getHeight() == 256 &&
         bitmap.is1Bit();
}
void cacheSleepTiles(freeink::SecureHttpClient& client, const Settings& settings, const Snapshot& snapshot,
                     const std::atomic<bool>& cancel, uint32_t start) {
  const size_t length = strlen(settings.endpoint);
  if (length < 9 || strcmp(settings.endpoint + length - 9, "/snapshot")) return;
  // Reuse the verified same-host TLS client; no new worker or response buffer.
  std::string base(settings.endpoint, length - 8);
  for (int i = 0; i < snapshot.poemCount && !cancel.load() && millis() - start < 26000; ++i) {
    const char* id = snapshot.poems[i].rasterId;
    if (!id[0]) continue;
    char path[80], part[88];
    sleepRasterPath(id, path, sizeof(path));
    if (sleepTileValid(path)) continue;
    snprintf(part, sizeof(part), "%s.part", path);
    bool complete = false;
    size_t bytes = 0;
    {
      auto file = Storage.open(part, O_WRONLY | O_CREAT | O_TRUNC);
      if (file && client.begin(base + "poem/" + id + "/sleep.bmp")) {
        client.setTimeout(std::min<uint32_t>(5000, 30000 - (millis() - start)));
        client.addHeader("Authorization", std::string("Bearer ") + settings.token);
        const int code = client.sendRequest(
            "GET", nullptr, 0,
            [&file, &bytes](const uint8_t* p, size_t n) {
              if (bytes + n > 13374 || file.write(p, n) != n) return false;
              bytes += n;
              return true;
            },
            [&cancel, start] { return cancel.load() || millis() - start >= 30000; });
        complete = code == 200 && client.responseComplete() && bytes == 13374 && !cancel.load();
      }
    }
    if (complete && sleepTileValid(part) && Storage.replaceFile(part, path))
      LOG_INF("STANDBY", "Large sleep poem cached (%u bytes)", unsigned(bytes));
    else
      Storage.remove(part);
  }
}
bool cacheRasters(const char* json, size_t bytes) {
  JsonDocument document;
  if (deserializeJson(document, json, bytes)) return false;
  for (auto poem : document["poems"].as<JsonArrayConst>()) {
    const char* id = poem["id"] | "";
    if (strlen(id) != 24 || strspn(id, "0123456789abcdef") != 24) continue;
    char path[80], part[88];
    rasterPath(id, path, sizeof(path));
    if (Storage.exists(path)) continue;
    auto raster = poem["raster"];
    const char* encoded = raster["data"] | "";
    const size_t length = strlen(encoded);
    const size_t rawBytes = raster["bytes"] | size_t(0);
    if (strcmp(raster["format"] | "", "bmp-deflate") || !length || length > 6144 || rawBytes != 9422) continue;
    // Transient ~14 KiB goes only to PSRAM, never the scarce TLS/internal heap.
    // On devices without PSRAM, keep the existing text fallback.
    auto compressed = HalMemory::allocatePsram(length * 3 / 4 + 3);
    auto raw = HalMemory::allocatePsram(rawBytes + 1);
    if (!compressed || !raw) {
      LOG_ERR("STANDBY", "No PSRAM for poem raster; retaining text");
      continue;
    }
    const int32_t decoded = freeink::content::base64Decode(encoded, length, compressed.get(), length * 3 / 4 + 3);
    if (decoded <= 0) continue;
    InflateReader inflate;
    inflate.init(false);
    inflate.setSource(compressed.get(), decoded);
    size_t produced = 0;
    if (inflate.readAtMost(raw.get(), rawBytes + 1, &produced) != InflateStatus::Done || produced != rawBytes ||
        raw[0] != 'B' || raw[1] != 'M')
      continue;
    snprintf(part, sizeof(part), "%s.part", path);
    bool saved = false;
    {
      auto file = Storage.open(part, O_WRONLY | O_CREAT | O_TRUNC);
      saved = file && file.write(raw.get(), rawBytes) == rawBytes;
    }
    if (saved)
      Storage.replaceFile(part, path);
    else
      Storage.remove(part);
  }
  return true;
}
void pruneRasters(const Snapshot& snapshot, const char* displayedId) {
  auto directory = Storage.open(kDir);
  if (!directory || !directory.isDirectory()) return;
  // At most five snapshot tiles plus the currently displayed tile survive.
  // No vector allocation: enumerate the bounded private cache directly.
  for (int i = 0; i < 64; ++i) {
    char name[80]{};
    {
      auto file = directory.openNextFile();
      if (!file) break;
      file.getName(name, sizeof(name));
    }
    const bool sleep = strlen(name) == 35 && !strncmp(name, "sleep1-", 7);
    if ((!sleep && (strlen(name) != 33 || strncmp(name, "poem-", 5))) || strcmp(name + strlen(name) - 4, ".bmp"))
      continue;
    name[strlen(name) - 4] = 0;
    const char* id = name + (sleep ? 7 : 5);
    if (strspn(id, "0123456789abcdef") != 24) continue;
    bool keep = strcmp(id, displayedId) == 0;
    for (int p = 0; p < snapshot.poemCount; ++p) keep = keep || strcmp(id, snapshot.poems[p].rasterId) == 0;
    if (!keep) {
      char path[80];
      if (sleep)
        sleepRasterPath(id, path, sizeof(path));
      else
        rasterPath(id, path, sizeof(path));
      Storage.remove(path);
    }
  }
}
bool endpointValid(const char* s) {
  return strncmp(s, "https://", 8) == 0 && strlen(s) > 10 && !strpbrk(s, "\r\n ?#@\\");
}
}  // namespace
bool Settings::load() {
  JsonDocument d;
  std::string json;
  if (!Storage.readFileToString("STANDBY", kConfig, 4096, json) || deserializeJson(d, json) ||
      d["version"].as<int>() != 1)
    return false;
  face = std::clamp(d["face"] | 2, 0, 4);
  clock = d["clock"] | 0;
  clock = clock ? 1 : 0;
  offlineMinute = (d["offlineMinute"] | 0) ? 1 : 0;
  poemMinutes = d["poemMinutes"] | 30;
  if (poemMinutes != 0 && poemMinutes != 30 && poemMinutes != 60) poemMinutes = 30;
  crop = (d["crop"] | 0) ? 1 : 0;
  online = (d["online"] | online) & 511;
  offline = (d["offline"] | offline) & (511 ^ Quota);
  text(d["endpoint"], endpoint, sizeof(endpoint));
  text(d["token"], token, sizeof(token));
  text(d["image"], image, sizeof(image));
  if (!endpointValid(endpoint) || strpbrk(token, "\r\n")) {
    token[0] = '\0';
    return false;
  }
  return true;
}
bool Settings::save() const {
  if (!endpointValid(endpoint) || strpbrk(token, "\r\n")) return false;
  JsonDocument d;
  d["version"] = 1;
  d["face"] = face;
  d["clock"] = clock;
  d["offlineMinute"] = offlineMinute;
  d["poemMinutes"] = poemMinutes;
  d["crop"] = crop;
  d["online"] = online;
  d["offline"] = offline;
  d["endpoint"] = endpoint;
  d["token"] = token;
  d["image"] = image;
  if (!Storage.ensureDirectoryExists(kDir)) return false;
  const char* part = "/.crosspoint/standby/settings.json.part";
  {
    auto f = Storage.open(part, O_WRITE | O_CREAT | O_TRUNC);
    if (!f || !serializeJson(d, f)) return false;
  }
  return Storage.replaceFile(part, kConfig);
}
void provision(const String& json) {
  if (json.length() > 20000) {
    logSerial.println("STANDBY_CONFIG:INVALID");
    return;
  }
  JsonDocument d;
  Settings s;
  s.load();
  if (deserializeJson(d, json) || !text(d["endpoint"], s.endpoint, sizeof(s.endpoint)) ||
      !text(d["token"], s.token, sizeof(s.token)) || !s.token[0]) {
    logSerial.println("STANDBY_CONFIG:INVALID");
    return;
  }
  if (d["image"].is<const char*>() && !text(d["image"], s.image, sizeof(s.image))) {
    logSerial.println("STANDBY_CONFIG:INVALID");
    return;
  }
  if (d["crop"].is<bool>()) s.crop = d["crop"].as<bool>();
  const char* ca = d["ca"] | "";
  if (d["clock"].is<int>()) s.clock = d["clock"].as<int>() == 1 ? 1 : 0;
  if (d["face"].is<int>()) s.face = std::clamp(d["face"].as<int>(), 0, 4);
  if (d["online"].is<int>()) s.online = d["online"].as<int>() & 511;
  if (d["offline"].is<int>()) s.offline = d["offline"].as<int>() & (511 ^ Quota);
  if (!endpointValid(s.endpoint) || strpbrk(s.token, "\r\n") || strlen(ca) > 16384 ||
      !strstr(ca, "-----BEGIN CERTIFICATE-----") || !strstr(ca, "-----END CERTIFICATE-----") ||
      !Storage.ensureDirectoryExists(kDir) || !Storage.writeFile("/.crosspoint/standby/ca.pem.part", String(ca)) ||
      !Storage.replaceFile("/.crosspoint/standby/ca.pem.part", "/.crosspoint/standby/ca.pem") || !s.save()) {
    logSerial.println("STANDBY_CONFIG:FAILED");
    return;
  }
  logSerial.println("STANDBY_CONFIG:OK");
}
}  // namespace smartstandby

void SmartStandbyFace::onEnter() {
  settings_.load();
  std::string cached;
  if (Storage.readFileToString("STANDBY", "/.crosspoint/standby/snapshot.json", 8192, cached))
    smartstandby::parse(cached.data(), cached.size(), snapshot_);
  if (snapshot_.poemCount) {
    poem_ = 0;
    displayedPoem_ = snapshot_.poems[0];
    hasPoem_ = true;
  }
  nextRequest_ = millis();
  nextPoem_ = millis() + uint32_t(settings_.poemMinutes) * 60000;
  shownTime_ = time(nullptr);
}
void SmartStandbyFace::prepareSleep(uint16_t modules) {
  sleep_ = true;
  offline_ = true;
  settings_.offline = modules & (smartstandby::Weather | smartstandby::Forecast | smartstandby::Air |
                                 smartstandby::Rain | smartstandby::Poem | smartstandby::Battery);
  std::string cached;
  if (Storage.readFileToString("SLP", "/.crosspoint/standby/snapshot.json", 8192, cached))
    smartstandby::parse(cached.data(), cached.size(), snapshot_);
  shownTime_ = time(nullptr);
  onPageNext();
  // Prefer a cached large tile. If upgrading while offline, use the embedded
  // large-font excerpt rather than shrinking arbitrary text to the old UI font.
  for (int i = 0; i < snapshot_.poemCount; ++i) {
    const auto& candidate = snapshot_.poems[(poem_ + i) % snapshot_.poemCount];
    char path[80];
    smartstandby::sleepRasterPath(candidate.rasterId, path, sizeof(path));
    if (candidate.rasterId[0] && smartstandby::sleepTileValid(path)) {
      displayedPoem_ = candidate;
      break;
    }
  }
}
void SmartStandbyFace::onExit() {
  cancel_.store(true);
  // Worker owns TLS and Wi-Fi cleanup; wait for its cancellation before this
  // bounded Activity storage is destroyed, never delete it inside SDK locks.
  while (running_.load()) delay(10);
}
void SmartStandbyFace::onPageNext() {
  if (snapshot_.poemCount) {
    const unsigned start = esp_random() % snapshot_.poemCount;
    for (unsigned i = 0; i < snapshot_.poemCount; ++i) {
      const unsigned candidate = (start + i) % snapshot_.poemCount;
      const auto& next = snapshot_.poems[candidate];
      if (snapshot_.poemCount > 1 && hasPoem_ && !strcmp(next.title, displayedPoem_.title) &&
          !strcmp(next.author, displayedPoem_.author) && next.lines == displayedPoem_.lines)
        continue;
      poem_ = candidate;
      displayedPoem_ = next;
      hasPoem_ = true;
      break;
    }
  }
  lastClock_ = UINT32_MAX;
}
void SmartStandbyFace::worker(void* ptr) {
  auto* self = static_cast<SmartStandbyFace*>(ptr);
  self->fetch();
  self->done_.store(true);
  self->running_.store(false);
  vTaskDelete(nullptr);
}
void SmartStandbyFace::startRequest() {
  if (running_.load() || authError_ || !settings_.token[0]) return;
  cancel_.store(false);
  done_.store(false);
  running_.store(true);
  // SecureHttpClient has a 2 KiB read buffer beneath the TLS/lwIP call chain.
  // 6 KiB overflowed during real HTTPS receive on S3; match the established
  // ReaderAI 16 KiB network stack, allocated only for this bounded request.
  if (xTaskCreate(worker, "StandbyFetch", 16384, this, 1, nullptr) != pdPASS) {
    running_.store(false);
    nextRequest_ = millis() + 3600000;
    LOG_ERR("STANDBY", "OOM allocating network task");
  }
}
void SmartStandbyFace::fetch() {
  HalPowerManager::Lock power;
  status_ = 0;
  valid_ = false;
  const auto mode = WiFi.getMode();
  // Standby is the foreground owner of a station connection, including one
  // inherited from Wi-Fi settings. Preserve an AP/shared AP+STA service.
  const bool owned = mode == WIFI_MODE_NULL || mode == WIFI_MODE_STA;
  const uint32_t start = millis();
  if (owned && WiFi.status() != WL_CONNECTED) {
    WIFI_STORE.loadFromFile();
    auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
    if (!credential) credential = WIFI_STORE.getCredentialAt(0);
    if (credential) {
      WiFi.mode(WIFI_STA);
      WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
      while (!cancel_.load() && WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(50);
    }
  }
  if (WiFi.status() == WL_CONNECTED && !cancel_.load()) {
    std::string ca;
    if (Storage.readFileToString("STANDBY", "/.crosspoint/standby/ca.pem", 16384, ca)) {
      freeink::SecureHttpClient client;
      client.setCACert(ca.c_str());
      client.setTimeout(15000);
      client.setFollowRedirects(0);
      client.setUserAgent("CrossMux-Standby/1");
      if (client.begin(settings_.endpoint)) {
        client.addHeader("Authorization", std::string("Bearer ") + settings_.token);
        std::string body;
        body.reserve(8192);  // one bounded response, discarded after parsing/cache write
        status_ = client.sendRequest(
            "GET", nullptr, 0,
            [&body](const uint8_t* p, size_t n) {
              if (body.size() + n > 8192) return false;
              body.append(reinterpret_cast<const char*>(p), n);
              return true;
            },
            [this, start] { return cancel_.load() || millis() - start >= 30000; });
        if (status_ == 200 && client.responseComplete() && !cancel_.load() &&
            smartstandby::parse(body.data(), body.size(), pending_)) {
          smartstandby::cacheRasters(body.data(), body.size());
          valid_ = true;
          const char* part = "/.crosspoint/standby/snapshot.json.part";
          if (strcmp(pending_.id, snapshot_.id) != 0 && Storage.ensureDirectoryExists(smartstandby::kDir) &&
              Storage.writeFile(part, String(body.c_str())))
            Storage.replaceFile(part, "/.crosspoint/standby/snapshot.json");
          // Optional assets cannot turn a valid weather/quota snapshot into a
          // network failure. All work stays inside this existing 30s window.
          std::string().swap(body);
          smartstandby::cacheSleepTiles(client, settings_, pending_, cancel_, start);
        }
      }
      client.end();
    }
  }
  if (owned) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
  LOG_INF("STANDBY", "fetch status=%d valid=%d stack=%u heap=%u", status_, valid_,
          unsigned(uxTaskGetStackHighWaterMark(nullptr)), unsigned(ESP.getFreeHeap()));
}
StandbyFace::TickResult SmartStandbyFace::tick() {
  const uint32_t now = millis();
  bool redraw = false;
  if (done_.exchange(false)) {
    if (valid_) {
      snapshot_ = pending_;
      if (!receivedPoems_ && snapshot_.poemCount) {
        displayedPoem_ = snapshot_.poems[0];
        hasPoem_ = true;
        receivedPoems_ = true;
      }
      smartstandby::pruneRasters(snapshot_, hasPoem_ ? displayedPoem_.rasterId : "");
      failures_ = 0;
      offline_ = false;
      nextRequest_ = now + 600000;
    } else if (status_ == 401 || status_ == 403) {
      authError_ = true;
      offline_ = true;
    } else {
      if (failures_ < 3) ++failures_;
      const bool wasOffline = offline_;
      offline_ = offline_ || failures_ >= 3;
      if (!wasOffline && offline_) nextPoem_ = now + 3600000;
      nextRequest_ = now + (offline_ ? 3600000 : (failures_ == 1 ? 30000 : 120000));
    }
    redraw = true;
  }
  if (!running_.load() && !authError_ && int32_t(now - nextRequest_) >= 0) {
    if (!settings_.token[0]) {
      offline_ = true;
      nextRequest_ = now + 3600000;
    } else
      startRequest();
  }
  const uint32_t unit = offline_ && !settings_.offlineMinute ? 3600 : 60;
  const uint32_t clock = uint32_t(time(nullptr) / unit);
  if (clock != lastClock_) {
    lastClock_ = clock;
    shownTime_ = time(nullptr);
    redraw = true;
  }
  if (settings_.poemMinutes && int32_t(now - nextPoem_) >= 0) {
    onPageNext();
    nextPoem_ = now + uint32_t(offline_ ? 60 : settings_.poemMinutes) * 60000;
    redraw = true;
  }
  return redraw ? TickResult::Redraw : TickResult::None;
}
StrId SmartStandbyFace::titleId() const { return StrId::STR_STANDBY_TITLE; }
uint32_t SmartStandbyFace::secondsUntilNextWake() const {
  const uint32_t now = millis();
  if (running_.load()) return 1;
  const uint32_t unit = offline_ && !settings_.offlineMinute ? 3600 : 60;
  uint32_t sec = unit - uint32_t(time(nullptr) % unit);
  if (!authError_ && settings_.token[0])
    sec = std::min<uint32_t>(sec,
                             std::max<uint32_t>(1, uint32_t(std::max(int32_t(0), int32_t(nextRequest_ - now))) / 1000));
  return sec;
}

namespace {
void center(GfxRenderer& r, const Rect& v, int font, int y, const char* s, bool black = true) {
  UITheme::drawCenteredText(r, v, font, y, s, black);
}
constexpr int clockFontId = 0x53544201, metaFontId = 0x53544202, weatherFontId = 0x53544203;
const EpdFont clockFont(&standby_clock), metaFont(&standby_meta), weatherFont(&standby_weather);
constexpr int sleepMetaId = 0x53544301, sleepDateId = 0x53544302, sleepWeekId = 0x53544303, sleepNumberId = 0x53544304,
              sleepPoemId = 0x53544305, sleepTitleId = 0x53544306;
const EpdFont sleepMetaFont(&sleep_meta), sleepDateFont(&sleep_date), sleepWeekFont(&sleep_week),
    sleepNumberFont(&sleep_number), sleepPoemFont(&sleep_poem), sleepTitleFont(&sleep_title);
void sleepFonts(GfxRenderer& r) {
  const int ids[] = {sleepMetaId, sleepDateId, sleepWeekId, sleepNumberId, sleepPoemId, sleepTitleId};
  const EpdFont* faces[] = {&sleepMetaFont,   &sleepDateFont, &sleepWeekFont,
                            &sleepNumberFont, &sleepPoemFont, &sleepTitleFont};
  for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
    if (!r.getFontMap().count(ids[i])) r.insertFont(ids[i], EpdFontFamily(faces[i]));
}
void fonts(GfxRenderer& r) {
  if (!r.getFontMap().count(clockFontId)) r.insertFont(clockFontId, EpdFontFamily(&clockFont));
  if (!r.getFontMap().count(metaFontId)) r.insertFont(metaFontId, EpdFontFamily(&metaFont));
  if (!r.getFontMap().count(weatherFontId)) r.insertFont(weatherFontId, EpdFontFamily(&weatherFont));
  r.setFallbackFont(metaFontId, CJK_UI_12_FONT_ID);
}
}  // namespace
void SmartStandbyFace::renderSleep(GfxRenderer& r, const Rect& v) {
  using namespace smartstandby;
  sleepFonts(r);
  // Proportional placement follows the viewport; fixed pixel fonts preserve
  // the approved 32px date/weekday and 28/84/40px content on the 3.97" panel.
  const int inset = std::max(16, v.width / 15), left = v.x + inset, width = v.width - inset * 2;
  const uint16_t modules = settings_.offline;
  const bool clockValid = TimeUtils::isClockValid();
  tm local{};
  localtime_r(&shownTime_, &local);
  char buf[128];
  const int headerY = v.y + v.height * 28 / 800;
  if (clockValid) {
    snprintf(buf, sizeof(buf), tr(STR_SLEEP_DATE_FMT), local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    r.drawText(sleepDateId, left, headerY, buf);
    const int dateWidth = r.getTextWidth(sleepDateId, buf);
    constexpr StrId weekdays[] = {StrId::STR_CAL_WEEKDAY_SUN, StrId::STR_CAL_WEEKDAY_MON, StrId::STR_CAL_WEEKDAY_TUE,
                                  StrId::STR_CAL_WEEKDAY_WED, StrId::STR_CAL_WEEKDAY_THU, StrId::STR_CAL_WEEKDAY_FRI,
                                  StrId::STR_CAL_WEEKDAY_SAT};
    // Same 32px size and baseline even though the two faces have different ascenders.
    const int weekY = headerY + r.getFontAscenderSize(sleepDateId) - r.getFontAscenderSize(sleepWeekId);
    r.drawText(sleepWeekId, left + dateWidth + 12, weekY, I18N.get(weekdays[local.tm_wday]));
  } else {
    r.drawText(sleepMetaId, left, headerY, tr(STR_NOT_SET));
  }
  if (modules & Battery) {
    snprintf(buf, sizeof(buf), "%u%%", powerManager.getBatteryPercentage());
    const int batteryY = headerY + r.getFontAscenderSize(sleepDateId) - r.getFontAscenderSize(sleepMetaId);
    r.drawText(sleepMetaId, left + width - r.getTextWidth(sleepMetaId, buf), batteryY, buf);
  }
  int y = v.y + v.height * 105 / 800;
  const time_t now = clockValid ? shownTime_ : 0;
  const bool weatherAvailable =
      snapshot_.weatherValid && (!now || (snapshot_.weatherAt <= now + 60 && now - snapshot_.weatherAt < 86400));
  if (weatherAvailable) {
    if (modules & Weather) {
      snprintf(buf, sizeof(buf), "%d°", snapshot_.temperature);
      const int numberWidth = r.getTextWidth(sleepNumberId, buf);
      const int detailWidth = std::max(0, width - numberWidth - 16);
      auto city = r.truncatedText(sleepMetaId, snapshot_.city, detailWidth);
      r.drawText(sleepMetaId, left, y, city.c_str());
      auto condition = r.truncatedText(sleepMetaId, snapshot_.description, detailWidth);
      r.drawText(sleepMetaId, left, y + 42, condition.c_str());
      r.drawText(sleepNumberId, left + width - numberWidth, y - 10, buf);
      if (snapshot_.humidity >= 0) {
        snprintf(buf, sizeof(buf), tr(STR_SMART_STANDBY_WEATHER_DETAILS), snapshot_.feels, snapshot_.humidity);
        auto detail = r.truncatedText(sleepMetaId, buf, width);
        r.drawText(sleepMetaId, left, y + 92, detail.c_str());
      }
      y += 134;
    }
    if (modules & Forecast) {
      char today[12], tomorrow[12], after[12];
      strftime(today, sizeof(today), "%Y-%m-%d", &local);
      tm day = local;
      ++day.tm_mday;
      mktime(&day);
      strftime(tomorrow, sizeof(tomorrow), "%Y-%m-%d", &day);
      ++day.tm_mday;
      mktime(&day);
      strftime(after, sizeof(after), "%Y-%m-%d", &day);
      for (int i = 0; i < snapshot_.dayCount; ++i) {
        const auto& forecast = snapshot_.days[i];
        if (now && strcmp(forecast.date, today) < 0) continue;
        const char* label = !strcmp(forecast.date, today)      ? tr(STR_SLEEP_TODAY)
                            : !strcmp(forecast.date, tomorrow) ? tr(STR_SLEEP_TOMORROW)
                            : !strcmp(forecast.date, after)    ? tr(STR_SLEEP_AFTER_TOMORROW)
                                                               : forecast.date + 5;
        r.drawText(sleepMetaId, left, y, label);
        const int labelWidth = std::max(64, r.getTextWidth(sleepMetaId, label));
        snprintf(buf, sizeof(buf), "%d°–%d°", forecast.low, forecast.high);
        const int rangeWidth = r.getTextWidth(sleepMetaId, buf);
        r.drawText(sleepMetaId, left + width - rangeWidth, y, buf);
        auto condition = r.truncatedText(sleepMetaId, forecast.description, width - labelWidth - rangeWidth - 24);
        r.drawText(sleepMetaId, left + labelWidth + 12, y, condition.c_str());
        y += 56;
      }
      y += 12;
    }
    bool extra = false;
    if ((modules & Air) && snapshot_.aqi >= 0 && (!now || now - snapshot_.weatherAt < 21600)) {
      snprintf(buf, sizeof(buf), "AQI %d", snapshot_.aqi);
      r.drawText(sleepMetaId, left, y, buf);
      extra = true;
    }
    if ((modules & Rain) && snapshot_.rain >= 0) {
      snprintf(buf, sizeof(buf), "%s %d%%", tr(STR_SLEEP_RAIN_SHORT), snapshot_.rain);
      r.drawText(sleepMetaId, left + width - r.getTextWidth(sleepMetaId, buf), y, buf);
      extra = true;
    }
    if (extra) y += 44;
  }
  if (!(modules & Poem)) return;
  const int top = std::max(y + 12, v.y + v.height * 490 / 800);
  const int bottom = v.y + v.height - 26;
  r.drawLine(left, top, left + width - 1, top);
  const int tileY = top + std::max(8, (bottom - top - 256) / 2);
  if (hasPoem_ && displayedPoem_.rasterId[0]) {
    char path[80];
    sleepRasterPath(displayedPoem_.rasterId, path, sizeof(path));
    auto file = Storage.open(path);
    if (file) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.is1Bit() && bitmap.getWidth() == 416 &&
          bitmap.getHeight() == 256 &&
          r.drawBitmap1Bit(bitmap, v.x + (v.width - std::min(width, 416)) / 2, tileY, width, bottom - tileY))
        return;
    }
  }
  center(r, v, sleepTitleId, tileY + 4, tr(STR_SLEEP_FALLBACK_TITLE));
  center(r, v, sleepMetaId, tileY + 52, tr(STR_SLEEP_FALLBACK_AUTHOR));
  center(r, v, sleepPoemId, tileY + 102, tr(STR_SLEEP_FALLBACK_LINE_ONE));
  center(r, v, sleepPoemId, tileY + 154, tr(STR_SLEEP_FALLBACK_LINE_TWO));
}
void SmartStandbyFace::render(GfxRenderer& r, const Rect& v) {
  if (sleep_) {
    renderSleep(r, v);
    return;
  }
  using namespace smartstandby;
  fonts(r);
  const uint16_t modules = offline_ ? settings_.offline : settings_.online;
  const int left = v.x + 32, width = v.width - 64;
  const bool clockValid = TimeUtils::isClockValid();
  const time_t now = clockValid ? time(nullptr) : 0;
  tm local{};
  localtime_r(&shownTime_, &local);
  char buf[128];
  int y = v.y + 26;
  r.drawText(metaFontId, left, y,
             sleep_                              ? tr(STR_SLEEPING)
             : authError_ || !settings_.token[0] ? tr(STR_SMART_STANDBY_CONFIG)
             : offline_                          ? tr(STR_SMART_STANDBY_OFFLINE)
                                                 : tr(STR_SMART_STANDBY_ONLINE));
  if (modules & Battery) {
    snprintf(buf, sizeof(buf), "%u%%", powerManager.getBatteryPercentage());
    r.drawText(metaFontId, left + width - r.getTextWidth(metaFontId, buf), y, buf);
  }
  y += 48;
  if (!sleep_) {
    if (clockValid) {
      const int cardWidth = std::min(154, (width - 30) / 2), cardHeight = 130;
      const int start = v.x + (v.width - cardWidth * 2 - 26) / 2;
      const int second = start + cardWidth + 26;
      const bool flip = settings_.clock == 0;
      if (flip) {
        r.fillRoundedRect(start, y, cardWidth, cardHeight, 8, Color::Black);
        r.fillRoundedRect(second, y, cardWidth, cardHeight, 8, Color::Black);
      }
      char hour[3], minute[3];
      snprintf(hour, sizeof(hour), "%02d", local.tm_hour);
      snprintf(minute, sizeof(minute), "%02d", local.tm_min);
      center(r, Rect(start, y, cardWidth, cardHeight), clockFontId, y + 4, hour, !flip);
      center(r, Rect(second, y, cardWidth, cardHeight), clockFontId, y + 4, minute, !flip);
      const int cx = start + cardWidth + 13;
      r.fillRoundedRect(cx - 2, y + 47, 5, 5, 2, Color::Black);
      r.fillRoundedRect(cx - 2, y + 79, 5, 5, 2, Color::Black);
      if (flip) {
        r.fillRect(start, y + cardHeight / 2, cardWidth, 1, false);
        r.fillRect(second, y + cardHeight / 2, cardWidth, 1, false);
      }
      y += cardHeight + 18;
      strftime(buf, sizeof(buf), "%Y / %m / %d", &local);
      center(r, v, metaFontId, y, buf);
      y += 25;
#ifdef ENABLE_CHINESE_VERSION
      if (modules & Lunar) {
        AlmanacDay day{};
        if (computeAlmanac(local, day)) {
          snprintf(buf, sizeof(buf), "%s%s", chinese_almanac::kLunarMonthNames[day.lunarMonth - 1],
                   chinese_almanac::kLunarDayNames[day.lunarDay - 1]);
          center(r, v, metaFontId, y, buf);
          y += 22;
        }
      }
#endif
    } else {
      center(r, v, UI_12_FONT_ID, y + 30, tr(STR_NOT_SET));
      y += 140;
    }
  }
  if ((modules & (sleep_ ? Weather | Forecast | Air | Rain : Weather)) && snapshot_.weatherValid &&
      (!now || now - snapshot_.weatherAt < 86400)) {
    r.drawLine(left, y + 8, left + width, y + 8);
    y += 25;
    if (modules & Weather) {
      snprintf(buf, sizeof(buf), "%s · %s", snapshot_.city, snapshot_.description);
      r.drawText(metaFontId, left, y + 4, buf);
      snprintf(buf, sizeof(buf), "%d°", snapshot_.temperature);
      r.drawText(weatherFontId, left + width - r.getTextWidth(weatherFontId, buf), y - 10, buf);
      y += 31;
      if (snapshot_.humidity >= 0) {
        snprintf(buf, sizeof(buf), tr(STR_SMART_STANDBY_WEATHER_DETAILS), snapshot_.feels, snapshot_.humidity);
        r.drawText(metaFontId, left, y, buf);
        y += 25;
      }
    }
    y += 12;
    if (offline_ || !now || now - snapshot_.weatherAt > 1800) {
      time_t stamp = snapshot_.weatherAt;
      tm cached{};
      localtime_r(&stamp, &cached);
      char date[24];
      strftime(date, sizeof(date), "%m/%d %H:%M", &cached);
      if (sleep_)
        snprintf(buf, sizeof(buf), "%s", tr(STR_SLEEP_CACHED_WEATHER));
      else if (now)
        snprintf(buf, sizeof(buf), tr(STR_SMART_STANDBY_CACHE), date);
      else
        snprintf(buf, sizeof(buf), "%s", tr(STR_SMART_STANDBY_CACHE_UNVERIFIED));
      center(r, v, metaFontId, y, buf);
      y += 28;
    }
    if ((modules & Forecast) && !quiet_) {
      char today[12];
      strftime(today, sizeof(today), "%Y-%m-%d", &local);
      int visible = 0;
      for (int i = 0; i < snapshot_.dayCount; ++i)
        if (!now || strcmp(snapshot_.days[i].date, today) >= 0) ++visible;
      int col = 0;
      for (int i = 0; i < snapshot_.dayCount; ++i) {
        const auto& d = snapshot_.days[i];
        if (now && strcmp(d.date, today) < 0) continue;
        Rect cell(left + width * col / std::max(1, visible), y, width / std::max(1, visible), 90);
        ++col;
        center(r, cell, metaFontId, y, d.date + 5);
        const auto description = r.truncatedText(metaFontId, d.description, cell.width - 12);
        center(r, cell, metaFontId, y + 24, description.c_str());
        snprintf(buf, sizeof(buf), "%d°-%d°", d.low, d.high);
        center(r, cell, metaFontId, y + 48, buf);
      }
      if (visible) y += 85;
    }
    if ((modules & Air) && snapshot_.aqi >= 0 && now && now - snapshot_.weatherAt < 21600) {
      snprintf(buf, sizeof(buf), "US AQI %d", snapshot_.aqi);
      center(r, v, metaFontId, y, buf);
      y += 28;
    }
    if ((modules & Rain) && snapshot_.rain >= 0) {
      snprintf(buf, sizeof(buf), "%s %d%%", tr(STR_SMART_STANDBY_RAIN), snapshot_.rain);
      center(r, v, metaFontId, y, buf);
      y += 28;
    }
  }
  if (!offline_ && (modules & Quota) && snapshot_.quotaFresh && now && snapshot_.quotaAt &&
      now - snapshot_.quotaAt <= 900) {
    r.drawLine(left, y, left + width, y);
    y += 18;
    r.drawText(metaFontId, left, y, tr(STR_SMART_STANDBY_QUOTA));
    y += 28;
    for (int i = 0; i < 2; ++i)
      if (snapshot_.remaining[i] >= 0) {
        r.drawText(metaFontId, left, y, i ? tr(STR_SMART_STANDBY_SEVEN) : tr(STR_SMART_STANDBY_FIVE));
        snprintf(buf, sizeof(buf), "%d%%", snapshot_.remaining[i]);
        r.drawText(metaFontId, left + width - r.getTextWidth(metaFontId, buf), y, buf);
        y += 24;
        r.fillRect(left, y, width, 1);
        r.fillRect(left, y, width * snapshot_.remaining[i] / 100, 5);
        y += 28;
      }
  }
  if (modules & Poem) {
    const auto* poem = hasPoem_ ? &displayedPoem_ : nullptr;
    const char* title = poem ? poem->title : "静夜思";
    const char* author = poem ? poem->author : "李白";
    constexpr const char* fallback[] = {"床前明月光，", "疑是地上霜。"};
    // The same two-line excerpt treatment as AirPage. Keep the block within
    // the viewport even when optional forecast modules occupy more room.
    if (y + 185 <= v.y + v.height - 24) {
      if (poem && poem->rasterId[0]) {
        char path[80];
        smartstandby::rasterPath(poem->rasterId, path, sizeof(path));
        HalStorage::StorageLock lock;
        auto file = Storage.open(path);
        if (file) {
          Bitmap bitmap(file, true);
          if (bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() == 416 && bitmap.getHeight() == 180) {
            const float scale = std::min(1.0f, float(width) / bitmap.getWidth());
            const int top =
                sleep_ ? y + std::max(24, (v.y + v.height - 24 - y - 180) / 2) : std::max(y + 20, v.y + v.height - 220);
            if (r.drawBitmap1Bit(bitmap, v.x + (v.width - int(bitmap.getWidth() * scale)) / 2, top, width,
                                 v.y + v.height - 24 - top))
              return;
          }
        }
      }
      y = sleep_ ? y + std::max(24, (v.y + v.height - 24 - y - 180) / 2) : std::max(y + 28, v.y + v.height - 220);
      const auto fittedTitle = r.truncatedText(UI_12_FONT_ID, title, width);
      center(r, v, UI_12_FONT_ID, y, fittedTitle.c_str());
      y += 43;
      const auto fittedAuthor = r.truncatedText(metaFontId, author, width);
      center(r, v, metaFontId, y, fittedAuthor.c_str());
      y += 35;
      for (int i = 0; i < 2; ++i) {
        const char* line = poem ? poem->lines[i].data() : fallback[i];
        const auto fitted = r.truncatedText(NOTOSERIF_12_FONT_ID, line, width);
        center(r, v, NOTOSERIF_12_FONT_ID, y, fitted.c_str());
        y += 39;
      }
    }
  }
}
