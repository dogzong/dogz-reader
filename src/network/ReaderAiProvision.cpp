#include <BoardConfig.h>
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include "ReaderAiDevice.h"
#include "ReaderAiProvision.h"
#include "WifiCredentialStore.h"
namespace readerai {
void provisionUsb(const String& json) {
  // Physical USB only; never print payloads. The bounded temporary document
  // avoids adding persistent RAM or embedding credentials in the firmware.
  if (json.length() > 20000) {
    logSerial.println("AI_CONFIG:INVALID");
    return;
  }
  JsonDocument d;
  if (deserializeJson(d, json)) {
    logSerial.println("AI_CONFIG:INVALID");
    return;
  }
  Config c;
  c.endpoint = d["endpoint"].as<std::string>();
  c.token = d["token"].as<std::string>();
  String ca = d["ca"].as<String>();
  auto epoch = d["epoch"].as<int64_t>();
  if (!validEndpoint(c.endpoint) || c.token.empty() || c.token.size() > 256 ||
      c.token.find_first_of("\r\n") != std::string::npos || ca.length() > 16384 ||
      !ca.startsWith("-----BEGIN CERTIFICATE-----") || ca.indexOf("-----END CERTIFICATE-----") < 0 ||
      epoch < 1700000000 || epoch > 4102444800LL) {
    logSerial.println("AI_CONFIG:INVALID");
    return;
  }
  if (!Storage.writeFile("/.crosspoint/reader-ai-ca.pem.part", ca) ||
      !Storage.replaceFile("/.crosspoint/reader-ai-ca.pem.part", "/.crosspoint/reader-ai-ca.pem") || !c.save() ||
      !halClock.setUtcTime(static_cast<time_t>(epoch))) {
    logSerial.println("AI_CONFIG:FAILED");
    return;
  }
  trustedtime::note();
  logSerial.println("AI_CONFIG:OK");
}
void provisionWifiUsb(const String& json) {
  if (json.length() > 512) {
    logSerial.println("AI_WIFI:INVALID");
    return;
  }
  JsonDocument d;
  if (deserializeJson(d, json) || !d["ssid"].is<std::string>() || !d["password"].is<std::string>()) {
    logSerial.println("AI_WIFI:INVALID");
    return;
  }
  const auto ssid = d["ssid"].as<std::string>();
  const auto password = d["password"].as<std::string>();
  if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != std::string::npos ||
      (!password.empty() && (password.size() < 8 || password.size() > 63)) ||
      password.find('\0') != std::string::npos) {
    logSerial.println("AI_WIFI:INVALID");
    return;
  }
  WIFI_STORE.loadFromFile();
  if (!WIFI_STORE.addCredential(ssid, password)) {
    logSerial.println("AI_WIFI:FAILED");
    return;
  }
  WIFI_STORE.setLastConnectedSsid(ssid);
  logSerial.println("AI_WIFI:OK");
}
void testUsbConnection() {
  HalPowerManager::Lock power;
  Config c;
  if (!c.load()) {
    logSerial.println("AI_HEALTH:CONFIG_MISSING");
    return;
  }
  WIFI_STORE.loadFromFile();
  bool owned = WiFi.status() != WL_CONNECTED;
  if (owned) {
    auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
    if (!cred) cred = WIFI_STORE.getCredentialAt(0);
    if (!cred) {
      logSerial.println("AI_HEALTH:WIFI_MISSING");
      return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    const auto deadline = millis() + 15000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
  }
  std::atomic<bool> cancel{false};
  Client client([&](const Request& r) { return deviceRequest(c, r, cancel); });
  bool asr = false, qa = false;
  bool ok = client.health(asr, qa);
  logSerial.printf("AI_HEALTH:%s ASR=%d QA=%d\n", ok ? "OK" : "FAILED", asr, qa);
  if (owned) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
}
}  // namespace readerai
#endif
