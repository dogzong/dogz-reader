#include "ReaderAiClient.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace readerai {
bool validEndpoint(const std::string& s) {
  if (s.find('\0') != std::string::npos || s.size() > 256 || s.compare(0, 8, "https://") != 0 ||
      s.find_first_of("\r\n\t ?#@\\") != std::string::npos)
    return false;
  const auto slash = s.find('/', 8);
  return slash != std::string::npos && slash > 8 && s.back() != '/';
}
bool validJobId(const std::string& s) {
  return s.size() == 32 &&
         std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<unsigned char>(s[i]);
    size_t n =
        c < 0x80 ? 1 : (c >= 0xc2 && c <= 0xdf ? 2 : (c >= 0xe0 && c <= 0xef ? 3 : (c >= 0xf0 && c <= 0xf4 ? 4 : 0)));
    if (!n || i + n > s.size() || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')) return false;
    for (size_t j = 1; j < n; ++j)
      if ((static_cast<unsigned char>(s[i + j]) & 0xc0) != 0x80) return false;
    if (n == 3 && ((c == 0xe0 && static_cast<unsigned char>(s[i + 1]) < 0xa0) ||
                   (c == 0xed && static_cast<unsigned char>(s[i + 1]) >= 0xa0)))
      return false;
    if (n == 4 && ((c == 0xf0 && static_cast<unsigned char>(s[i + 1]) < 0x90) ||
                   (c == 0xf4 && static_cast<unsigned char>(s[i + 1]) >= 0x90)))
      return false;
    i += n;
  }
  return true;
}
static uint32_t characters(const std::string& s) {
  uint32_t count = 0;
  for (unsigned char c : s)
    if ((c & 0xc0) != 0x80) ++count;
  return count;
}
// Server stage name -> enum. Unknown names map to None on purpose: a newer
// server adding a stage must not turn a running job into a protocol error.
static Stage stageOf(const std::string& name) {
  if (name == "searching") return Stage::Searching;
  if (name == "thinking") return Stage::Thinking;
  if (name == "agent") return Stage::Agent;
  if (name == "writing") return Stage::Writing;
  if (name == "queued") return Stage::Queued;
  return Stage::None;
}
bool Client::request(const Request& r, Reply& reply) {
  error_.clear();
  reply = transport_(r);
  if (reply.status == 401 || reply.status == 403)
    error_ = "unauthorized";
  else if (reply.status == 429)
    error_ = "quota";
  else if (!reply.complete)
    error_ = "network";
  else if (reply.body.size() > MaxReplyBytes || reply.status < 200 || reply.status >= 300)
    error_ = "server";
  return error_.empty();
}
bool Client::health(bool& asr, bool& qa) {
  Reply r;
  if (!request({"GET", "/health", "", "", ""}, r)) return false;
  JsonDocument d;
  if (deserializeJson(d, r.body) || d["protocol"].as<int>() != 1 || !d["asr"].is<bool>() || !d["qa"].is<bool>()) {
    error_ = "protocol";
    return false;
  }
  asr = d["asr"].as<bool>();
  qa = d["qa"].as<bool>();
  return true;
}
bool Client::transcribe(const std::string& file, const std::string& key, std::string& id) {
  Reply r;
  if (key.empty() || !request({"POST", "/transcriptions", "", file, key}, r)) return false;
  JsonDocument d;
  if (deserializeJson(d, r.body) || !d["job_id"].is<const char*>()) {
    error_ = "protocol";
    return false;
  }
  auto next = d["job_id"].as<std::string>();
  if (!validJobId(next)) {
    error_ = "protocol";
    return false;
  }
  id = std::move(next);
  return true;
}
bool Client::question(const std::string& source, const std::string& text, const std::string& key, std::string& id,
                      const std::string& parent, Mode mode) {
  if (!parent.empty() && !validJobId(parent)) {
    error_ = "question";
    return false;
  }
  if (!validJobId(source) || text.empty() || text.size() > 6000 || !validUtf8(text) || characters(text) > 2000 ||
      key.empty()) {
    error_ = "question";
    return false;
  }
  JsonDocument d;
  d["transcription_job_id"] = source;
  d["text"] = text;
  if (!parent.empty()) d["parent_job_id"] = parent;
  // Only deep adds a field: a quick payload keeps the exact bytes this client
  // sent before the deep task existed, so the server's default path is reached
  // unchanged.
  if (mode == Mode::Deep) d["mode"] = "deep";
  std::string payload;
  serializeJson(d, payload);
  Reply r;
  if (!request({"POST", "/questions", payload, "", key}, r)) return false;
  d.clear();
  if (deserializeJson(d, r.body) || !d["job_id"].is<const char*>()) {
    error_ = "protocol";
    return false;
  }
  auto next = d["job_id"].as<std::string>();
  if (!validJobId(next)) {
    error_ = "protocol";
    return false;
  }
  id = std::move(next);
  return true;
}
bool Client::status(const std::string& id, Status& out) {
  if (!validJobId(id)) {
    error_ = "protocol";
    return false;
  }
  Reply r;
  if (!request({"GET", "/jobs/" + id, "", "", ""}, r)) return false;
  JsonDocument d;
  if (deserializeJson(d, r.body) || !d["state"].is<const char*>()) {
    error_ = "protocol";
    return false;
  }
  auto value = d["state"].as<std::string>();
  if (value != "queued" && value != "running" && value != "completed" && value != "failed" && value != "cancelled") {
    error_ = "protocol";
    return false;
  }
  Status next;
  next.state = std::move(value);
  // stage and elapsed_s are optional additions: a server without them, or with
  // a stage name this build does not know, still yields a usable status.
  if (d["stage"].is<const char*>()) next.stage = stageOf(d["stage"].as<std::string>());
  if (d["elapsed_s"].is<uint32_t>()) next.elapsed = d["elapsed_s"].as<uint32_t>();
  out = std::move(next);
  return true;
}
bool Client::state(const std::string& id, std::string& out) {
  Status job;
  if (!status(id, job)) return false;
  out = std::move(job.state);
  return true;
}
bool Client::chunk(const std::string& id, uint32_t cursor, Chunk& out) {
  if (!validJobId(id) || cursor > 8000) {
    error_ = "protocol";
    return false;
  }
  Reply r;
  if (!request({"GET", "/jobs/" + id + "/result?cursor=" + std::to_string(cursor), "", "", ""}, r)) return false;
  JsonDocument d;
  if (deserializeJson(d, r.body) || !d["text"].is<const char*>() || !d["next_cursor"].is<uint32_t>() ||
      !d["done"].is<bool>()) {
    error_ = "protocol";
    return false;
  }
  Chunk result{d["text"].as<std::string>(), d["next_cursor"].as<uint32_t>(), d["done"].as<bool>()};
  if (!validUtf8(result.text) || result.text.size() > 2048 || result.next != cursor + characters(result.text) ||
      result.next > 8000 || (!result.done && result.next == cursor)) {
    error_ = "protocol";
    return false;
  }
  out = std::move(result);
  return true;
}
bool Client::cancel(const std::string& id) {
  if (!validJobId(id)) return false;
  Reply r;
  return request({"DELETE", "/jobs/" + id, "", "", ""}, r);
}
}  // namespace readerai
