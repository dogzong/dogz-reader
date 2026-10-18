#include "VoiceHistory.h"

#include <ArduinoJson.h>
#include <HalStorage.h>

#include <algorithm>

namespace readerai {
namespace {
constexpr const char* Dir = "/.crosspoint/voice-assistant/history";
// Question text captured per turn. The server rejects anything above 6000 bytes,
// so this is only a sanity bound on what a hand-edited card can inject.
constexpr size_t kMaxQuestionBytes = 6000;
// Bounds a single answer so one huge reply cannot fill the session on its own:
// the newest turn is always kept, and a turn that does not fit is shortened
// rather than dropped.
constexpr size_t kMaxTurnAnswerBytes = 6144;
// Rows the list reads. A card with more conversations still keeps them; the
// oldest are simply not listed.
constexpr size_t kMaxEntries = 50;
// Row title length, cut on a UTF-8 boundary (same as the pre-session list).
constexpr size_t kTitleBytes = 90;
constexpr size_t kReadCap = kMaxDocBytes;
// Serializes `conversation` into `out`. `budget` bounds the text carried by the
// newest turn, so a caller that is over the document cap shortens the one turn
// it refuses to drop instead of failing. False means the finished record is
// larger than kMaxDocBytes, so the caller must drop or shorten more.
bool serialize(const Conversation& conversation, size_t budget, std::string& out) {
  const size_t last = conversation.turns.size() ? conversation.turns.size() - 1 : 0;
  JsonDocument d;
  d["version"] = 2;
  d["root"] = conversation.root;
  if (conversation.updated) d["updated"] = conversation.updated;
  JsonArray turns = d["turns"].to<JsonArray>();
  for (size_t i = 0; i < conversation.turns.size(); ++i) {
    const Turn& turn = conversation.turns[i];
    const size_t limit = i == last ? budget : kMaxDocBytes;
    JsonObject row = turns.add<JsonObject>();
    row["job"] = turn.job;
    row["question"] = truncateUtf8(turn.question, limit);
    // Older turns may already have had their answer dropped by a previous cap
    // hit; an empty answer is written as no field at all.
    if (!turn.answer.empty()) row["answer"] = truncateUtf8(turn.answer, limit);
  }
  out.clear();
  serializeJson(d, out);
  return out.size() <= kMaxDocBytes;
}
// The fields a session file may carry. It is also the filter a read uses, so one
// pass covers both the one-file-per-session shape and the legacy
// one-file-per-answer shape (question/answer/job at the top level, no turns).
// Reading only these keys is what keeps a 64 KB session inside the render heap.
void filterFields(JsonDocument& filter) {
  filter["root"] = true;
  filter["updated"] = true;
  JsonObject flat = filter.as<JsonObject>();
  flat["question"] = true;
  flat["answer"] = true;
  flat["job"] = true;
  JsonObject row = filter["turns"].to<JsonArray>().add<JsonObject>();
  row["job"] = true;
  row["question"] = true;
  row["answer"] = true;
}
void normalize(Conversation& conversation) {
  for (Turn& turn : conversation.turns) {
    turn.question = truncateUtf8(turn.question, kMaxQuestionBytes);
    turn.answer = truncateUtf8(turn.answer, kMaxTurnAnswerBytes);
  }
}
// Unreadable entries are skipped, not fatal: one corrupt record must not hide
// the rest of the history.
bool read(const char* file, JsonDocument& filter, JsonDocument& out) {
  std::string raw;
  const std::string path = std::string(Dir) + "/" + file;
  if (!Storage.readFileToString("AI", path, kReadCap, raw)) return false;
  return !deserializeJson(out, raw, DeserializationOption::Filter(filter));
}
}  // namespace
std::string ConversationStore::lastJob(const Conversation& conversation) {
  for (size_t i = conversation.turns.size(); i > 0; --i)
    if (validJobId(conversation.turns[i - 1].job)) return conversation.turns[i - 1].job;
  return std::string();
}
void ConversationStore::remove(const std::string& file) {
  if (file.empty() || file.find('/') != std::string::npos) return;
  Storage.remove((std::string(Dir) + "/" + file).c_str());
}
bool ConversationStore::save(Conversation& conversation) {
  if (!validJobId(conversation.root)) return false;
  normalize(conversation);
  // Cap 1: never keep more than kMaxTurns turns. The root stays the file name,
  // so dropping the oldest turn keeps the session addressable; the chain the
  // server walks simply starts later.
  if (conversation.turns.size() > kMaxTurns)
    conversation.turns.erase(conversation.turns.begin(), conversation.turns.end() - kMaxTurns);
  // Cap 2: the serialized record must stay within kMaxDocBytes. Each round
  // measures the document ArduinoJson would actually write, never an estimate:
  //   1. drop the answer text of the OLDER turns — their question stays visible
  //      and their job id stays in the resume chain, so the row still counts the
  //      turn and the chain still links;
  //   2. if that is not enough, drop whole turns from the oldest end (the root
  //      keeps naming the file, so the session stays addressable);
  //   3. the newest turn is never dropped. If it alone is too large — one very
  //      long answer — its text is halved until it fits, because the invariant is
  //      what the file may hold, not what the server returned.
  std::string text;
  const auto measured = [&](size_t budget) { return serialize(conversation, budget, text); };
  if (!measured(kMaxTurnAnswerBytes)) {
    if (conversation.turns.size() > 1) {
      for (size_t i = 0; i + 1 < conversation.turns.size(); ++i) conversation.turns[i].answer.clear();
      while (!measured(kMaxTurnAnswerBytes) && conversation.turns.size() > 1)
        conversation.turns.erase(conversation.turns.begin());
    }
    for (size_t budget = kMaxTurnAnswerBytes; budget > 96 && !measured(budget); budget /= 2) {
    }
    // Last resort: accept whatever the smallest budget produced rather than
    // refusing to record a turn the server already answered.
    if (text.size() > kMaxDocBytes) measured(96);
  }
  if (!Storage.ensureDirectoryExists(Dir)) return false;
  const std::string path = std::string(Dir) + "/" + conversation.root + ".json";
  const std::string temp = path + ".tmp";
  // Same publish path as the rest of the app: temp file, then replaceFile.
  return Storage.writeFile(temp.c_str(), text.c_str()) && Storage.replaceFile(temp.c_str(), path.c_str());
}
bool ConversationStore::load(const std::string& file, const std::string& root, Conversation& conversation) {
  conversation = Conversation();
  if (!validJobId(root)) return false;
  JsonDocument filter;
  filterFields(filter);
  JsonDocument d;
  if (!read(file.c_str(), filter, d)) return false;
  const JsonObjectConst body = d.as<JsonObjectConst>();
  auto stored = body["root"].as<std::string>();
  conversation.root = validJobId(stored) ? stored : root;
  conversation.updated = static_cast<uint32_t>(body["updated"].as<uint64_t>());
  const JsonArrayConst turns = body["turns"].as<JsonArrayConst>();
  const bool legacy = !body["turns"].is<JsonArrayConst>();
  for (JsonObjectConst row : turns) {
    Turn turn;
    turn.job = row["job"].as<std::string>();
    if (!validJobId(turn.job)) continue;
    turn.question = truncateUtf8(row["question"].as<std::string>(), kMaxQuestionBytes);
    turn.answer = truncateUtf8(row["answer"].as<std::string>(), kMaxTurnAnswerBytes);
    if (!validUtf8(turn.question) || !validUtf8(turn.answer)) continue;
    if (turn.question.empty() && turn.answer.empty()) continue;
    conversation.turns.push_back(std::move(turn));
  }
  // The pre-session shape is one answer per file: question/answer/job at the top
  // level and no session at all. It is read as a ONE-TURN session named after its
  // file (the answer job id is the file name, so it is a valid chain link), and
  // it is neither migrated nor deleted — the first follow-up simply writes a new
  // session file. Nothing here writes, so a legacy record can only be read.
  if (conversation.turns.empty() && legacy) {
    Turn turn;
    turn.job = conversation.root;
    turn.question = truncateUtf8(body["question"].as<std::string>(), kMaxQuestionBytes);
    turn.answer = truncateUtf8(body["answer"].as<std::string>(), kMaxTurnAnswerBytes);
    if (validUtf8(turn.question) && validUtf8(turn.answer) &&
        (!turn.question.empty() || !turn.answer.empty()))
      conversation.turns.push_back(std::move(turn));
  }
  // A record with nothing readable in it is not a session.
  return !conversation.turns.empty();
}
std::vector<HistoryEntry> ConversationStore::list() {
  std::vector<HistoryEntry> entries;
  auto files = Storage.listFiles(Dir, static_cast<int>(kMaxEntries));
  entries.reserve(files.size());
  for (const auto& file : files) {
    const std::string name(file.c_str());
    if (name.size() != 37 || name.compare(32, 5, ".json") != 0) continue;
    const std::string root = name.substr(0, 32);
    if (!validJobId(root)) continue;
    JsonDocument filter;
    filterFields(filter);
    JsonDocument d;
    if (!read(name.c_str(), filter, d)) continue;
    const JsonObjectConst body = d.as<JsonObjectConst>();
    // A record that names a root has to name a job id; one that names none (the
    // legacy shape) is identified by its file name. A corrupt root is refused
    // rather than repaired, so a bad file cannot silently claim another session.
    const JsonVariantConst stored = body["root"];
    if (stored.is<const char*>() && !validJobId(stored.as<std::string>())) continue;
    HistoryEntry entry;
    entry.file = name;
    entry.root = validJobId(stored.as<std::string>()) ? stored.as<std::string>() : root;
    entry.updated = static_cast<uint32_t>(body["updated"].as<uint64_t>());
    // A session counts its own turns; a legacy one-answer record IS one turn, and
    // its question is the row title too, so both shapes list the same way.
    const JsonArrayConst turns = body["turns"].as<JsonArrayConst>();
    // Only a record with NO turns array at all is the legacy one-answer shape. A
    // session whose turns array exists but is empty has no answer/job to fall
    // back to, and is left unlisted rather than shown as a phantom row.
    const bool legacy = !body["turns"].is<JsonArrayConst>();
    entry.turns = legacy ? 1 : static_cast<uint32_t>(turns.size());
    // The first question is the row title, so a record without one is not
    // listable: every row the user can see is a row they can open and read.
    // ArduinoJson turns an absent or mistyped value into the literal text
    // "null", so the type is checked before the text is trusted.
    const JsonVariantConst first = legacy ? body["question"] : turns[0]["question"];
    if (!entry.turns || !first.is<const char*>()) continue;
    const auto question = first.as<std::string>();
    if (question.empty() || !validUtf8(question)) continue;
    entry.title = truncateUtf8(question, kTitleBytes);
    entries.push_back(std::move(entry));
  }
  // Newest first. Job ids are random per session, so the name is the only order
  // available without opening every file; the stamped time is displayed instead.
  std::sort(entries.begin(), entries.end(),
            [](const HistoryEntry& a, const HistoryEntry& b) { return a.file > b.file; });
  return entries;
}
}  // namespace readerai
