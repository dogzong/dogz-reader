#include <ArduinoJson.h>

#include <cassert>
#include <iostream>

#include "activities/apps/voice-assistant/VoiceText.h"
#include "network/ReaderAiClient.h"
using namespace readerai;
int main() {
  const std::string id(32, 'a');
  assert(validEndpoint("https://example.com/reader-ai/v1"));
  assert(!validEndpoint("http://example.com/v1"));
  assert(!validEndpoint("https://token@example.com/v1"));
  assert(!validEndpoint("https://example.com/v1\r\nInjected: evil"));
  assert(validUtf8("中文🙂ASCII"));
  assert(!validUtf8(std::string("\xc0\xaf", 2)));
  assert(!validUtf8(std::string("\xed\xa0\x80", 3)));
  assert(!validUtf8(std::string("\xf4\x90\x80\x80", 4)));
  assert(!validUtf8(std::string("\0", 1)));
  Request last;
  Reply reply{202, "{\"job_id\":\"" + id + "\"}", true};
  int calls = 0;
  Client client([&](const Request& r) {
    last = r;
    ++calls;
    return reply;
  });
  bool asr = false, qa = false;
  reply = {200, "{\"protocol\":1,\"asr\":true,\"qa\":false}", true};
  assert(client.health(asr, qa));
  assert(asr && !qa && last.path == "/health");
  calls = 0;
  reply = {202, "{\"job_id\":\"" + id + "\"}", true};
  std::string job;
  assert(client.transcribe("question.wav", "stable-key", job));
  assert(job == id && last.path == "/transcriptions" && last.key == "stable-key");
  assert(calls == 1);  // Upload/recognition cannot itself issue a paid question.
  assert(client.question(id, "问题\"\n内容", "qa-key", job));
  assert(last.path == "/questions" && last.key == "qa-key");
  JsonDocument payload;
  assert(!deserializeJson(payload, last.body));
  assert(payload["text"].as<std::string>() == "问题\"\n内容");
  assert(payload["transcription_job_id"].as<std::string>() == id);
  assert(client.question(id, "追问", "follow-key", job, std::string(32, 'b')));
  payload.clear();
  assert(!deserializeJson(payload, last.body));
  assert(payload["parent_job_id"].as<std::string>() == std::string(32, 'b'));
  const int previousCalls = calls;
  assert(!client.question(id, "问", "invalid-parent", job, "../../other"));
  assert(calls == previousCalls);
  reply = {401, "{}", true};
  assert(!client.question(id, "问", "qa-key", job));
  assert(client.error() == "unauthorized");
  reply = {200, "{\"state\":\"running\"}", false};
  std::string status;
  assert(!client.state(id, status));
  assert(client.error() == "network");
  reply.complete = true;
  assert(client.state(id, status));
  assert(status == "running");
  reply.body = "{\"state\":\"unexpected\"}";
  assert(!client.state(id, status));
  Chunk chunk;
  reply = {200, "{\"text\":\"中文🙂\",\"next_cursor\":3,\"done\":false}", true};
  assert(client.chunk(id, 0, chunk));
  assert(chunk.next == 3 && validUtf8(chunk.text));
  reply.body = "{\"text\":\"\",\"next_cursor\":3,\"done\":false}";
  assert(!client.chunk(id, 3, chunk));
  reply.body = "{\"text\":\"x\",\"next_cursor\":2,\"done\":true}";
  assert(!client.chunk(id, 3, chunk));
  reply = {200, std::string(MaxReplyBytes + 1, 'x'), true};
  assert(!client.state(id, status));
  assert(!client.state("../other", status));
  std::vector<Line> lines;
  const std::string text = "中文🙂AB\n下一行结束";
  assert(paginate(text, 4, [](const std::string& c) { return c.size() == 1 ? 1 : 2; }, lines));
  std::string rebuilt;
  for (auto l : lines) {
    auto part = text.substr(l.start, l.end - l.start);
    assert(validUtf8(part));
    rebuilt += part;
  }
  assert(rebuilt == "中文🙂AB下一行结束");
  assert(!paginate(text, 0, [](const std::string&) { return 1; }, lines));
  assert(!paginate(std::string(24001, 'a'), 100, [](const std::string&) { return 1; }, lines));
  assert(paginate("", 4, [](const std::string&) { return 1; }, lines));
  assert(lines.size() == 1);
  // Regression: the renderer resolves one face for a whole run, so summing
  // single-glyph widths under-reports what drawText() paints and the tail of
  // every line gets clipped off screen. Each emitted line must still fit when
  // measured as a whole run.
  const std::string cjk = "好好好好好好好好";
  const auto runWidth = [](const std::string& run) {
    size_t glyphs = 0;
    for (size_t i = 0; i < run.size();) {
      const unsigned char c = static_cast<unsigned char>(run[i]);
      i += c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
      ++glyphs;
    }
    return static_cast<int>(glyphs) * (glyphs == 1 ? 4 : 6);  // a lone glyph measures narrow
  };
  lines.clear();
  assert(paginate(cjk, 12, runWidth, lines));
  assert(lines.size() > 1);
  for (auto l : lines) assert(runWidth(cjk.substr(l.start, l.end - l.start)) <= 12);
  std::cout
      << "PASS: authentication errors, explicit confirmation, idempotency keys, bounded results, UTF-8 pagination\n";
}
