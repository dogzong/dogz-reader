#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "VoiceText.h"
#include "network/ReaderAiClient.h"
namespace readerai {
// A conversation is the parent_job_id chain rooted at its first question job:
// the root job id names the session file, and every follow-up stores the job id
// of the question it asked, so the chain can be walked again later.
struct Turn {
  std::string job;  // job id of this turn's question (chain link + resume point)
  std::string question;
  std::string answer;
};
// Hard session bounds, fixed rather than settings so a device cannot grow an
// unbounded history file. Enforcement lives in ConversationStore::save():
//   1. the newest turns beyond kMaxTurns are dropped;
//   2. if the serialized document would still exceed kMaxDocBytes, the answer
//      text of the oldest turns is dropped (question and job id survive, so the
//      turn count and the resume chain stay intact) until it fits.
constexpr size_t kMaxTurns = 20;
constexpr size_t kMaxDocBytes = 65536;
// Third cap, for the SCREEN rather than the file: paginate() refuses to describe
// more than 1024 lines. A session that renders longer than this keeps its newest
// turns and drops the oldest ones from the page (the file keeps them).
constexpr size_t kMaxTranscriptLines = 1024;
struct Conversation {
  std::string root;   // session file name: the root question job id
  uint32_t updated = 0;  // unix seconds of the last append, 0 when time is unset
  std::vector<Turn> turns;
};
// Truncates to at most `limit` bytes without splitting a UTF-8 sequence. The
// remaining bytes of an unfinished sequence are dropped, so the result is a
// valid prefix of a valid string.
inline std::string truncateUtf8(const std::string& text, size_t limit) {
  if (text.size() <= limit) return text;
  // Forward scan: `end` is only ever advanced past a WHOLE sequence, so walking
  // back from a byte offset (which can land inside a character) is never needed.
  size_t end = 0;
  while (end < text.size()) {
    const auto lead = static_cast<unsigned char>(text[end]);
    const size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
    if (end + length > limit) break;
    end += length;
  }
  return text.substr(0, end);
}
// Local wall clock as "MM-DD HH:MM". An unset or pre-2023 clock has no
// meaningful stamp, so the caller gets an empty string and draws no time.
inline std::string formatStamp(uint32_t updated) {
  if (updated < 1700000000u) return std::string();
  const time_t when = static_cast<time_t>(updated);
  struct tm parts;
  if (!localtime_r(&when, &parts)) return std::string();
  char stamp[24];
  snprintf(stamp, sizeof(stamp), "%02d-%02d %02d:%02d", parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
           parts.tm_min);
  return stamp;
}
}  // namespace readerai
