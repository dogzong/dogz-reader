#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "VoiceConversation.h"
namespace readerai {
// What the history list needs from a session file without parsing its turns: the
// root job id, the first question (row title), the turn count and the last
// append time. Reading a whole 64 KB session per row would not fit the C3 heap
// budget, so the list uses a JSON filter for exactly these four fields.
struct HistoryEntry {
  std::string file;   // name inside the history directory
  std::string root;   // session id: the first turn's question job id
  std::string title;  // first question, truncated on a UTF-8 boundary
  uint32_t turns = 0;
  uint32_t updated = 0;
};
// One CONVERSATION per file: /.crosspoint/voice-assistant/history/<root>.json
// (root = the job id of the session's first question). The file holds every
// turn, so a 3-turn conversation is one entry in the list, not three.
//
// Legacy files keep their old shape (<answer job id>.json with a single
// question/answer/job). They are not deleted or rewritten: load() reads them as
// a one-turn conversation named after the file, and the first follow-up writes a
// new session file. No legacy file is ever opened for append, so a bad legacy
// file can only fail its own entry.
class ConversationStore {
 public:
  // Newest question job id of `conversation`, i.e. the parent_job_id a follow-up
  // must send so the server can walk the chain back to the session root.
  static std::string lastJob(const Conversation& conversation);
  // Writes `conversation` to its own file, enforcing kMaxTurns and kMaxDocBytes
  // in place (the caller's copy is trimmed to what was written, so screen and
  // disk agree). Returns false only when the file could not be published.
  static bool save(Conversation& conversation);
  // Reads the session filed under `file` (see HistoryEntry::file). `root` is the
  // job id the file is named after, used when the record carries no "root" field
  // (every legacy record). Returns false for an unreadable, oversized or
  // malformed file, or one whose root is not a job id.
  static bool load(const std::string& file, const std::string& root, Conversation& conversation);
  // Lists the sessions on the card, newest filename first. Only file names of
  // the form <32 hex>.json are considered; a record that cannot be read is left
  // out instead of failing the page.
  static std::vector<HistoryEntry> list();
  static void remove(const std::string& file);
};
}  // namespace readerai
