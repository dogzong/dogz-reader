#include <BoardConfig.h>
#if FREEINK_DEVICE_WAVESHARE_EPAPER_397
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <ctime>

#include "HttpDownloader.h"
#include "ReaderAiDevice.h"
#include "WifiPowerSaveGuard.h"
namespace readerai {
namespace {
// One TLS session per question cycle. Rebuilding the client per request forced
// a fresh handshake (measured 5.2 s on this board) for each of the six requests
// a voice question needs. SecureHttpClient keeps the socket alive between
// requests when reuse is on, so the cycle pays for the handshake once.
struct Session {
  freeink::SecureHttpClient client;
  // setCACert() stores the pointer, not a copy, so the PEM has to outlive the
  // client. Keeping it here instead of per-request is what makes reuse safe.
  std::string ca;
  std::string endpoint;
  bool configured = false;
};
Session& session() {
  static Session s;
  return s;
}
}  // namespace

void closeSession() {
  Session& s = session();
  s.client.end();
  s.configured = false;
  s.ca.clear();
  s.endpoint.clear();
}

bool Config::load() {
  Preferences p;
  if (!p.begin("reader-ai", true)) return false;
  endpoint = p.getString("endpoint", endpoint.c_str()).c_str();
  token = p.getString("token", "").c_str();
  voiceConsent = p.getBool("consent", false);
  reviewTranscript = p.getBool("review", false);
  p.end();
  return validEndpoint(endpoint) && !token.empty();
}
bool Config::save() const {
  if (!validEndpoint(endpoint) || token.size() > 256 || token.find_first_of("\r\n") != std::string::npos) return false;
  Preferences p;
  if (!p.begin("reader-ai", false)) return false;
  bool ok = p.putString("endpoint", endpoint.c_str()) == endpoint.size();
  ok = p.putString("token", token.c_str()) == token.size() && ok;
  ok = p.putBool("consent", voiceConsent) == 1 && ok;
  ok = p.putBool("review", reviewTranscript) == 1 && ok;
  p.end();
  return ok;
}
Reply deviceRequest(const Config& c, const Request& r, const std::atomic<bool>& cancel) {
  const uint32_t requestStartMs = millis();
  Reply out;
  if (r.key.size() > 128 || r.key.find_first_of("\r\n") != std::string::npos) return out;
  if (!validEndpoint(c.endpoint) || c.token.empty() || c.token.size() > 256 ||
      c.token.find_first_of("\r\n") != std::string::npos || cancel.load() || WiFi.status() != WL_CONNECTED ||
      time(nullptr) < 1700000000 || !HttpDownloader::hasMemoryForTls())
    return out;
  std::string ca;
  if (!Storage.readFileToString("AI", "/.crosspoint/reader-ai-ca.pem", 16384, ca)) return out;
  WifiPowerSaveGuard power;
  Session& s = session();
  if (!s.configured || s.ca != ca || s.endpoint != c.endpoint) {
    s.client.end();
    s.ca = ca;
    s.endpoint = c.endpoint;
    s.client.setCACert(s.ca.c_str());  // No insecure or HTTP fallback; verify hostname too.
    s.client.setTimeout(15000);
    s.client.setReuse(true);  // keep-alive across the requests of one cycle
    s.client.setFollowRedirects(0);
    s.client.setUserAgent("CrossMux-ReaderAI/1");
    s.configured = true;
  }
  freeink::SecureHttpClient& http = s.client;
  if (!http.begin(c.endpoint + r.path)) return out;
  http.addHeader("Authorization", "Bearer " + c.token);
  http.addHeader("Accept", "application/json");
  if (!r.key.empty()) http.addHeader("Idempotency-Key", r.key);
  const uint8_t* payload = reinterpret_cast<const uint8_t*>(r.body.data());
  size_t length = r.body.size();
  HalMemory::PsramBuffer upload;
  if (!r.file.empty()) {
    HalFile wav;
    if (!Storage.openFileForRead("AI", r.file, wav)) return out;
    length = wav.size();
    if (length < 44 || length > 1920044) return out;
    // SecureHttpClient takes a contiguous payload. Only the S3 upload phase
    // allocates this bounded WAV buffer, exclusively in PSRAM, then frees it.
    upload = HalMemory::allocatePsram(length);
    if (!upload) return out;
    for (size_t pos = 0; pos < length;) {
      if (cancel.load()) return out;
      size_t count = std::min(size_t(2048), length - pos);
      if (wav.read(upload.get() + pos, count) != static_cast<int>(count)) return out;
      pos += count;
    }
    payload = upload.get();
    http.addHeader("Content-Type", "audio/wav");
  } else if (!r.body.empty())
    http.addHeader("Content-Type", "application/json");
  out.body.reserve(MaxReplyBytes);
  // Per-phase timing: a voice question was 32 s with almost all of it invisible
  // from either end of the wire. `prep` covers reading the CA and (for an
  // upload) pulling the WAV off the SD card into PSRAM; `send` covers the TLS
  // write plus the response, which is where the upload actually leaves.
  const uint32_t prepMs = millis() - requestStartMs;
  out.status = http.sendRequest(
      r.method.c_str(), payload, length,
      [&out](const uint8_t* data, size_t n) {
        if (out.body.size() + n > MaxReplyBytes) return false;
        out.body.append(reinterpret_cast<const char*>(data), n);
        return true;
      },
      [&cancel] { return cancel.load(); });
  out.complete = http.responseComplete() && !http.callbackAborted() && !http.aborted();
  LOG_INF("AI", "%s %s bytes=%u prep=%ums send=%ums status=%d", r.method.c_str(), r.path.c_str(),
          static_cast<unsigned>(length), static_cast<unsigned>(prepMs), static_cast<unsigned>(millis() - requestStartMs - prepMs),
          out.status);
  return out;
}
}  // namespace readerai
#endif
