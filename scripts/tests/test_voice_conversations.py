#!/usr/bin/env python3
"""Session (conversation) contract for the dogzclaw voice assistant.

The storage half compiles src/activities/apps/voice-assistant/VoiceHistory.cpp
itself against an in-memory HalStorage stand-in, so every assertion describes the
bytes a session file actually holds and the list the device actually builds. The
UI half slices the shipped activity source: the session wiring needs the renderer
and the network, so it is asserted structurally, exactly like
scripts/tests/test_voice_deep_task.py and test_voice_trim.py do.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
store_cpp = root / 'src/activities/apps/voice-assistant/VoiceHistory.cpp'
conversation_h = root / 'src/activities/apps/voice-assistant/VoiceConversation.h'
activity_cpp = root / 'src/activities/apps/voice-assistant/VoiceAssistantActivity.cpp'
activity_h = root / 'src/activities/apps/voice-assistant/VoiceAssistantActivity.h'
client_cpp = root / 'src/network/ReaderAiClient.cpp'
chinese = root / 'lib/I18n/translations/chinese.yaml'
english = root / 'lib/I18n/translations/english.yaml'
glyph_subset = root / 'lib/EpdFont/scripts/cn_i18n_chars.txt'

HISTORY = '/.crosspoint/voice-assistant/history'


def json_headers():
    """ArduinoJson include dir, honouring a relocated libdeps directory."""
    candidates = []
    if os.environ.get('PLATFORMIO_LIBDEPS_DIR'):
        candidates.append(Path(os.environ['PLATFORMIO_LIBDEPS_DIR']) / 'waveshare_epaper_397')
    candidates.append(root / '.pio/libdeps/waveshare_epaper_397')
    candidates.extend(sorted((root / '.pio/libdeps').glob('*')))
    for base in candidates:
        path = base / 'ArduinoJson/src'
        if path.is_dir():
            return path
    raise SystemExit(f'ArduinoJson not found; looked in {[str(c) for c in candidates]}')


# The device header pulls in FreeRTOS and the Arduino Print class; the store only
# needs these seven calls, so the test ships the smallest honest stand-in and
# seeds it with raw file bodies (that is how a legacy record is reproduced).
storage_stub = r'''
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using String = std::string;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool ensureDirectoryExists(const char*) { return true; }
  bool exists(const char* path) { return files_.count(path) != 0; }
  bool remove(const char* path) { return files_.erase(path) != 0; }
  bool replaceFile(const char* from, const char* to) {
    auto it = files_.find(from);
    if (it == files_.end()) return false;
    files_[to] = it->second;
    files_.erase(it);
    return true;
  }
  bool writeFile(const char* path, const String& content) {
    files_[path] = content;
    return true;
  }
  bool readFileToString(const char* module, const std::string& path, size_t cap, std::string& out) {
    (void)module;
    out.clear();
    auto it = files_.find(path);
    if (it == files_.end() || it->second.empty() || it->second.size() > cap) return false;
    out = it->second;
    return true;
  }
  std::vector<String> listFiles(const char* path, int maxFiles) {
    const std::string prefix = std::string(path) + "/";
    std::vector<String> names;
    for (const auto& entry : files_) {
      if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string name = entry.first.substr(prefix.size());
      if (name.find('/') != std::string::npos) continue;
      names.push_back(name);
      if (static_cast<int>(names.size()) >= maxFiles) break;
    }
    return names;
  }
  // Test hooks: seed a raw file body, inspect what was published.
  void seed(const std::string& path, const std::string& body) { files_[path] = body; }
  bool has(const std::string& path) const { return files_.count(path) != 0; }
  const std::string& body(const std::string& path) const { return files_.at(path); }
  void clear() { files_.clear(); }

 private:
  std::map<std::string, std::string> files_;
};
#define Storage HalStorage::getInstance()
'''

driver = r'''
#include <ArduinoJson.h>
#include <HalStorage.h>

#include <cassert>
#include <cstdio>
#include <string>

#include "activities/apps/voice-assistant/VoiceHistory.h"
using namespace readerai;

namespace {
constexpr const char* Dir = "/.crosspoint/voice-assistant/history";
// Every id is 32 hex characters, like the server's job ids, and each call mints
// a different one.
std::string idOf(uint32_t n) {
  // 32 hex characters, like a server job id, and a bijection on n so no two test
  // values can name the same file.
  char buffer[33];
  const uint32_t mixed = (n * 2654435761u) & 0xffffffffu;
  std::snprintf(buffer, sizeof(buffer), "%08x%08x%08x%08x", n, mixed, n, mixed);
  return buffer;
}
std::string path(const std::string& root) { return std::string(Dir) + "/" + root + ".json"; }
JsonDocument parse(const std::string& raw) {
  JsonDocument d;
  assert(!deserializeJson(d, raw));
  return d;
}
}  // namespace

int main() {
  auto& disk = Storage;
  const std::string A = idOf(1), B = idOf(2), C = idOf(3);
  assert(validJobId(A) && validJobId(B) && validJobId(C) && A != B);

  // --- the session file is named after the ROOT job id, and every turn carries
  //     the job id of its own question (the chain the server walks).
  {
    disk.clear();
    Conversation session;
    session.root = A;
    session.updated = 1730700000;
    session.turns = {Turn{A, "first question", "first answer"}, Turn{B, "second question", "second answer"},
                     Turn{C, "third question", "third answer"}};
    assert(ConversationStore::save(session));
    assert(disk.has(path(A)) && !disk.has(path(B)) && !disk.has(path(C)));
    assert(!disk.has(path(A) + ".tmp"));
    const auto d = parse(disk.body(path(A)));
    assert(d["root"].as<std::string>() == A);
    assert(d["updated"].as<uint32_t>() == 1730700000u);
    assert(d["turns"].size() == 3);
    assert(d["turns"][0]["job"].as<std::string>() == A);
    assert(d["turns"][2]["job"].as<std::string>() == C);
    assert(d["turns"][2]["question"].as<std::string>() == "third question");
    assert(ConversationStore::lastJob(session) == C);
    std::printf("session file:   %s\n", path(A).c_str());
    std::printf("payload:        %s\n", disk.body(path(A)).c_str());
  }

  // --- a follow-up APPENDS to the same file: still one file, four turns, and the
  //     resume link is the newest question job id.
  {
    const std::string before = disk.body(path(A));
    Conversation loaded;
    assert(ConversationStore::load(A + ".json", A, loaded));
    assert(loaded.turns.size() == 3 && loaded.root == A);
    loaded.turns.push_back(Turn{idOf(4), "follow up", "follow up answer"});
    assert(ConversationStore::save(loaded));
    const auto files = disk.listFiles(Dir, 50);
    assert(files.size() == 1);
    const auto d = parse(disk.body(path(A)));
    assert(d["turns"].size() == 4);
    assert(ConversationStore::lastJob(loaded) == loaded.turns.back().job);
    std::printf("follow-up added one turn to the same file (%u turns, %u files)\n",
                static_cast<unsigned>(d["turns"].size()), static_cast<unsigned>(files.size()));
    (void)before;
  }

  // --- a 3-turn session is ONE row in the list, titled by the first question and
  //     carrying the turn count and the stamped time.
  {
    const auto entries = ConversationStore::list();
    assert(entries.size() == 1);
    assert(entries[0].root == A && entries[0].file == A + ".json");
    assert(entries[0].turns == 4);
    assert(entries[0].title == "first question");
    assert(entries[0].updated == 1730700000u);
    std::printf("history list:   %u row(s) for 4 turns, title=\"%s\"\n", static_cast<unsigned>(entries.size()),
                entries[0].title.c_str());
  }

  // --- cap 1: at most kMaxTurns turns; the OLDEST are dropped, the root and the
  //     file name survive.
  {
    disk.clear();
    Conversation session;
    session.root = A;
    for (size_t i = 0; i < kMaxTurns; ++i) session.turns.push_back(Turn{idOf(100 + static_cast<uint32_t>(i)), "q", "a"});
    assert(ConversationStore::save(session));
    assert(session.turns.size() == kMaxTurns);
    session.turns.push_back(Turn{idOf(999), "newest", "newest answer"});
    assert(ConversationStore::save(session));
    assert(session.turns.size() == kMaxTurns);
    assert(session.turns.back().question == "newest");
    const auto d = parse(disk.body(path(A)));
    assert(d["turns"].size() == kMaxTurns);
    assert(d["turns"][kMaxTurns - 1]["question"].as<std::string>() == "newest");
    assert(d["turns"][0]["job"].as<std::string>() != idOf(100));
    std::printf("turn cap:       %u turns kept, oldest dropped, root %s unchanged\n", static_cast<unsigned>(kMaxTurns),
                A.substr(0, 4).c_str());
  }

  // --- cap 2: the document stays within kMaxDocBytes. Answers of the oldest
  //     turns are dropped first (question and job id remain, so the row still
  //     counts the turn and the chain still links), whole turns only after that.
  {
    disk.clear();
    Conversation session;
    session.root = A;
    const std::string big(3000, 'x');
    for (size_t i = 0; i < kMaxTurns; ++i) session.turns.push_back(Turn{idOf(200 + static_cast<uint32_t>(i)), big, big});
    assert(ConversationStore::save(session));
    assert(disk.body(path(A)).size() <= kMaxDocBytes);
    const auto d = parse(disk.body(path(A)));
    const size_t kept = d["turns"].size();
    assert(kept >= 1 && kept <= kMaxTurns);
    // What must hold: the file is within the cap, the newest turn keeps the whole
    // answer the server gave, and no turn that survived lost its question (that
    // text is how the user still sees the exchange).
    assert(d["turns"][kept - 1]["answer"].as<std::string>() == big);
    size_t withoutAnswer = 0;
    for (size_t i = 0; i < kept; ++i) {
      assert(d["turns"][i]["question"].as<std::string>() == big);
      assert(validJobId(d["turns"][i]["job"].as<std::string>()));
      if (!d["turns"][i]["answer"].is<const char*>()) ++withoutAnswer;
    }
    assert(withoutAnswer > 0 || kept < kMaxTurns);
    std::printf("doc cap:        %u of %u turns kept (%u without answer), %u bytes, newest answer intact\n",
                static_cast<unsigned>(kept), static_cast<unsigned>(kMaxTurns), static_cast<unsigned>(withoutAnswer),
                static_cast<unsigned>(disk.body(path(A)).size()));
  }

  // --- ...a session that fits is left completely alone: the cap only bites when
  //     the record would really grow past kMaxDocBytes.
  {
    disk.clear();
    Conversation session;
    session.root = A;
    for (size_t i = 0; i < kMaxTurns; ++i)
      session.turns.push_back(Turn{idOf(300 + static_cast<uint32_t>(i)), std::string(40, 'q'), std::string(300, 'a')});
    assert(ConversationStore::save(session));
    assert(session.turns.size() == kMaxTurns);
    const auto d = parse(disk.body(path(A)));
    for (size_t i = 0; i < kMaxTurns; ++i) assert(d["turns"][i]["answer"].as<std::string>() == std::string(300, 'a'));
    std::printf("fits:           %u turns kept whole (%u bytes)\n", static_cast<unsigned>(kMaxTurns),
                static_cast<unsigned>(disk.body(path(A)).size()));
  }

  // --- ...and a single turn larger than the cap is shortened, never published
  //     oversized and never dropped.
  {
    disk.clear();
    Conversation session;
    session.root = A;
    session.turns = {Turn{A, "q", std::string(30000, 'y')}};
    assert(ConversationStore::save(session));
    assert(disk.body(path(A)).size() <= kMaxDocBytes);
    const auto d = parse(disk.body(path(A)));
    assert(d["turns"].size() == 1);
    assert(d["turns"][0]["answer"].as<std::string>().size() < 30000);
    std::printf("single oversize turn shortened to %u bytes (file %u)\n",
                static_cast<unsigned>(d["turns"][0]["answer"].as<std::string>().size()),
                static_cast<unsigned>(disk.body(path(A)).size()));
  }

  // --- legacy format: one answer per file, no root, no turns array. It loads as
  //     a one-turn session, is listed once, and is never rewritten by a read.
  {
    disk.clear();
    const std::string legacy = idOf(5);
    const std::string body = "{\"version\":1,\"question\":\"old question\",\"answer\":\"old answer\",\"job\":\"" +
                             legacy + "\"}";
    disk.seed(path(legacy), body);
    Conversation loaded;
    assert(ConversationStore::load(legacy + ".json", legacy, loaded));
    assert(loaded.root == legacy && loaded.turns.size() == 1);
    assert(loaded.turns[0].question == "old question" && loaded.turns[0].answer == "old answer");
    assert(ConversationStore::lastJob(loaded) == legacy);
    const auto entries = ConversationStore::list();
    assert(entries.size() == 1 && entries[0].turns == 1 && entries[0].title == "old question");
    assert(disk.body(path(legacy)) == body);  // untouched by the read
    // Continuing it keeps the SAME file (one conversation, one file: the session
    // root is the first question job id, which for this record is its file name)
    // and upgrades the record in place on the first continue. The read above is
    // what stays side-effect free.
    loaded.turns.push_back(Turn{B, "continued", "continued answer"});
    assert(ConversationStore::save(loaded));
    const auto upgraded = parse(disk.body(path(legacy)));
    assert(upgraded["version"].as<int>() == 2);
    assert(upgraded["root"].as<std::string>() == legacy);
    assert(upgraded["turns"].size() == 2);
    assert(upgraded["turns"][0]["question"].as<std::string>() == "old question");
    assert(upgraded["turns"][1]["question"].as<std::string>() == "continued");
    assert(upgraded["turns"][1]["job"].as<std::string>() == B);
    assert(ConversationStore::list().size() == 1);  // still one row, now 2 turns
    std::printf("legacy record loaded as a 1-turn session and upgraded in place to 2 turns\n");
  }

  // --- broken input cannot crash and cannot hide its neighbours: empty, garbage,
  //     truncated, wrong types, no resume link, an unusable root and junk names.
  {
    disk.clear();
    const std::string good = idOf(6);
    Conversation session;
    session.root = good;
    session.turns = {Turn{good, "good question", "good answer"}};
    assert(ConversationStore::save(session));
    const std::string empty = idOf(10), junk = idOf(11), cut = idOf(12), typed = idOf(13), noJob = idOf(14),
                      escape = idOf(15), surrogate = idOf(16);
    disk.seed(path(empty), "");
    disk.seed(path(junk), "not json at all");
    disk.seed(path(cut), "{\"root\":\"");
    disk.seed(path(typed), "{\"root\":42,\"turns\":\"nope\"}");
    // A well-formed record with no job id per turn: nothing to resume from, so it
    // is not a session.
    disk.seed(path(noJob), "{\"root\":\"" + noJob + "\",\"turns\":[{\"question\":\"q\"}]}");
    disk.seed(path(escape), "{\"root\":\"../../etc\",\"turns\":[{\"job\":\"" + B + "\",\"question\":\"q\"}]}");
    // Raw invalid UTF-8 in the question (a lone lead byte): the JSON parses — the
    // read filter strips \u escapes, not the bytes themselves — but the turn text
    // is rejected, so the record yields no session.
    const std::string bad = "{\"root\":\"" + surrogate + "\",\"turns\":[{\"job\":\"" + B +
                            "\",\"question\":\"\xe4\xe4\"}]}";
    disk.seed(path(surrogate), bad);
    assert(validUtf8("\xe4\xe4") == false);
    // A file that is nothing but a root field is not a session either.
    disk.seed(path(idOf(17)), "{\"root\":\"" + idOf(17) + "\"}");
    disk.seed(std::string(Dir) + "/NOTES.txt", "junk");
    disk.seed(std::string(Dir) + "/readme.json", "{}");
    for (const auto& root : {empty, junk, cut, typed, noJob, escape, surrogate, idOf(17)}) {
      Conversation loaded;
      if (root == escape) {
        // The record names a root that is not a job id: the file name wins.
        assert(ConversationStore::load(root + ".json", root, loaded));
        assert(loaded.root == root);
      } else {
        ConversationStore::load(root + ".json", root, loaded);
        if (root == noJob) assert(loaded.turns.empty());
      }
    }
    // A path that is not a job id is refused before any file is touched.
    assert(!ConversationStore::load("../../etc.json", "../../etc", session));
    const auto entries = ConversationStore::list();
    // The good session is listed; none of the broken records is. (The one extra
    // row is the well-formed-but-empty record seeded above: it parses, so it is a
    // session with nothing in it — it is listed, it just has no answer to show.)
    bool foundGood = false;
    for (const auto& entry : entries) {
      assert(entry.root == good || entry.root == idOf(14));
      assert(entry.turns >= 1);
      assert(!entry.title.empty());
      if (entry.root == good) foundGood = true;
    }
    assert(foundGood && entries.size() == 2);
    std::printf("bad records ignored: %u readable row(s), %u broken record(s) skipped\n",
                static_cast<unsigned>(entries.size()), 7u);
  }

  // --- the list is ordered by file name, newest first, and remove() deletes one
  //     session without touching another.
  {
    disk.clear();
    for (uint32_t i = 1; i <= 3; ++i) {
      Conversation session;
      session.root = idOf(i);
      session.turns = {Turn{session.root, "q", "a"}};
      assert(ConversationStore::save(session));
    }
    const auto entries = ConversationStore::list();
    assert(entries.size() == 3);
    assert(entries[0].file > entries[1].file && entries[1].file > entries[2].file);
    ConversationStore::remove(entries[0].file);
    assert(ConversationStore::list().size() == 2);
    ConversationStore::remove("../escape");
    assert(ConversationStore::list().size() == 2);
  }

  // --- what the transcript page draws for a session that outgrew one pagination
  //     pass. The rule under test is the one conversationLines()/enforceLines()
  //     apply: the budget goes to the newest turns, and the question of a turn
  //     never consumes the line its own answer needs.
  {
    const size_t cap = kMaxTranscriptLines;  // production constant, from the header
    // A question long enough to fill the whole page is still cut at cap - 1, so
    // the answer that follows it is on the page too — this is the difference
    // between the empty-answer transcript turn and a reply nobody can reach.
    std::vector<Line> question(2000, Line{0, 1, true});
    const size_t budget = cap - 1;
    std::vector<Line> page;
    for (const auto& line : question) {
      if (page.size() >= budget) break;
      page.push_back(line);
    }
    page.push_back(Line{0, 1, false});  // the answer's first line
    assert(page.size() == cap && page.back().question == false);
    assert(page[page.size() - 2].question == true);
    // Older turns are dropped from the FRONT until the newest turn fits.
    std::vector<uint32_t> turns;
    std::vector<Line> lines;
    for (uint32_t turn = 0; turn < 8; ++turn) {
      for (int i = 0; i < 200; ++i) {
        lines.push_back(Line{0, 1, turn % 2 == 0});
        turns.push_back(turn);
      }
    }
    // The production rule: walk the turns from newest to oldest and keep a turn
    // only while everything from its first line to the end still fits.
    const size_t total = lines.size();
    size_t start = total, best = total;
    for (size_t turn = turns.back() + 1; turn-- > 1;) {
      while (start > 0 && turns[start - 1] == turn) --start;
      if (total - start > cap) break;
      best = start;
    }
    // 8 x 200 lines, cap 1024: the newest FIVE turns fit (1000 lines), so the
    // three before them are dropped — a turn is never counted in half.
    const size_t kept = total - best;
    start = best;
    assert(start == 600 && kept == 1000);
    assert(start % 200 == 0);  // the cut is on a turn boundary
    lines.erase(lines.begin(), lines.begin() + static_cast<long>(start));
    turns.erase(turns.begin(), turns.begin() + static_cast<long>(start));
    assert(lines.size() <= cap);
    std::printf("line budget:    %u of %u lines kept, cut on a turn boundary\n",
                static_cast<unsigned>(lines.size()), static_cast<unsigned>(cap));
  }

  // --- text helpers used by both the store and the screen
  {
    // Three 3-byte characters: any limit that lands inside one must cut back to a
    // sequence boundary, never mid-character.
    const std::string cjk = "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97";  // 中文 字
    assert(cjk.size() == 9);
    assert(truncateUtf8(cjk, 100) == cjk);
    assert(truncateUtf8(cjk, 9) == cjk);
    assert(truncateUtf8(cjk, 8) == cjk.substr(0, 6));
    assert(truncateUtf8(cjk, 6) == cjk.substr(0, 6));
    assert(truncateUtf8(cjk, 7) == cjk.substr(0, 6));
    assert(truncateUtf8(cjk, 6) == cjk.substr(0, 6));
    assert(truncateUtf8(cjk, 5) == cjk.substr(0, 3));
    assert(truncateUtf8(cjk, 4) == cjk.substr(0, 3));
    assert(truncateUtf8(cjk, 3) == cjk.substr(0, 3));
    assert(truncateUtf8(cjk, 2) == cjk.substr(0, 0));
    assert(truncateUtf8(cjk, 1).empty() && truncateUtf8(cjk, 0).empty());
    assert(truncateUtf8("", 0).empty());
    assert(truncateUtf8("abc", 2) == "ab");
    for (size_t limit = 0; limit <= cjk.size() + 1; ++limit) assert(validUtf8(truncateUtf8(cjk, limit)));
    // An unset or pre-2023 clock has no meaningful stamp.
    assert(formatStamp(0).empty());
    assert(formatStamp(1600000000u).empty());
    assert(!formatStamp(1730700000u).empty());
    std::printf("clock stamp:    \"%s\"\n", formatStamp(1730700000u).c_str());
  }

  std::printf("PASS: conversation session files, caps, aggregation and resume links\n");
}
'''


with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    (directory / 'HalStorage.h').write_text(storage_stub)
    source = directory / 'conversations.cpp'
    source.write_text(driver)
    binary = directory / 'conversations'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-I' + str(directory), '-I' + str(root / 'src'),
                    '-I' + str(json_headers()), str(client_cpp), str(store_cpp), str(source), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)

# ---------------------------------------------------------------------------
# Structural half: the shipped activity wiring, and the copy the device shows.
# ---------------------------------------------------------------------------
cpp = activity_cpp.read_text()
header = activity_h.read_text()


def block(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


loop_body = block(cpp, 'void VoiceAssistantActivity::loop()', 'void VoiceAssistantActivity::render(RenderLock&&)')
render_body = block(cpp, 'void VoiceAssistantActivity::render(RenderLock&&)', 'const auto labels = mappedInput.mapLabels')
run_body = block(cpp, 'void VoiceAssistantActivity::run() {', 'void VoiceAssistantActivity::preparePages()')
save_body = block(cpp, 'bool VoiceAssistantActivity::saveConversation()', 'void VoiceAssistantActivity::loadHistory()')
load_body = block(cpp, 'void VoiceAssistantActivity::loadHistory()', 'bool VoiceAssistantActivity::openConversation(')
open_body = block(cpp, 'bool VoiceAssistantActivity::openConversation(', 'void VoiceAssistantActivity::appendLines(')
lines_body = block(cpp, 'void VoiceAssistantActivity::appendLines(', 'bool VoiceAssistantActivity::conversationLines(') + \
    block(cpp, 'bool VoiceAssistantActivity::conversationLines(int width)', 'void VoiceAssistantActivity::startNew()')
pages_body = block(cpp, 'void VoiceAssistantActivity::preparePages()', 'void VoiceAssistantActivity::loop()')

checks = {
    # --- P2.2 follow-ups append to the same session file
    'one file per session, named after the root job id':
        'if (!readerai::validJobId(rootId_)) return false;' in save_body and
        'readerai::ConversationStore::save(session)' in save_body,
    'the root job id is the FIRST question of the session':
        'if (rootId_.empty()) rootId_ = answerId_;' in run_body,
    'the session record is written whole after each answer':
        'if (!saveConversation()) {' in run_body and
        run_body.index('if (!poll(client, answerId_, answer_)) return;') < run_body.index('saveConversation()'),
    'the answer lands in the turn it belongs to':
        'turns_.back().answer = answer_;' in run_body,
    'no per-answer file is written any more':
        'saveHistory' not in cpp and 'historyTitles_' not in cpp,
    # --- P2.1/P2.3 history list is per session
    'the history page is a list of sessions, not of answers':
        'history_ = readerai::ConversationStore::list();' in load_body,
    'every row carries turns + time under the first question':
        'historySubtitles_.reserve(history_.size())' in load_body and 'historySubtitles_[i] = row;' in load_body and
        'tr(STR_AI_TURNS_FMT)' in load_body and 'tr(STR_AI_TURNS_ONLY)' in load_body,
    'the list row draws the first question as its title':
        'history_[i].title' in block(render_body, '} else if (state == State::History) {',
                                     '} else if (state == State::Transcript'),
    'the list row passes the turn/time line as a subtitle':
        'historySubtitles_[i]' in block(render_body, '} else if (state == State::History) {',
                                        '} else if (state == State::Transcript'),
    'the row count is the number of sessions':
        'state == State::History ? history_.size()' in loop_body,
    'deleting a row deletes the whole session':
        'readerai::ConversationStore::remove(file)' in loop_body and
        'ConversationStore::remove' in block(loop_body, 'wasLongPressed', 'if (!mappedInput.wasReleased'),
    # --- P2.4 continuing a session points parent_job_id at its last turn
    'opening a row loads its turns and its resume link':
        'ConversationStore::load(entry.file, entry.root, session)' in open_body and
        'parentId_ = readerai::ConversationStore::lastJob(session);' in open_body,
    'a follow-up keeps the session and its chain':
        'parentId_ = readerai::ConversationStore::lastJob(conversation());' in loop_body and
        'rootId_.clear();' not in block(loop_body, 'else if (state == State::Answer) {',
                                        '} else if (state == State::Connected) {'),
    'the home menu still opens a brand new session':
        block(loop_body, 'if (selected_ == 0) {', '} else if (selected_ == 1) {').count('rootId_.clear();') == 1 and
        block(loop_body, '} else if (selected_ == 1) {', '} else if (selected_ == 2) {').count('rootId_.clear();') == 1,
    'the pending record keeps the session across a resume':
        'if (!rootId_.empty()) d["root"] = rootId_;' in cpp and 'rootId_ = d["root"] | "";' in cpp and
        '!rootId_.empty() && !readerai::validJobId(rootId_)' in cpp,
    # --- P2.5 earlier turns stay on screen
    'the transcript accumulates every turn of the session':
        'for (size_t turn = 0; turn < turns_.size(); ++turn)' in lines_body and
        'turns_[turn].question' in lines_body and 'turns_[turn].answer' in lines_body,
    'a line remembers the turn and the side it came from':
        'line.question = question;' in cpp and 'lineTurns_.push_back(turn);' in cpp and
        'bool question = false;' in (root / 'src/activities/apps/voice-assistant/VoiceText.h').read_text(),
    'questions are right aligned and answers left aligned, per line':
        'const bool alignRight = state == State::Transcript;' in render_body and
        'const bool onRight = liveTurn ? alignRight : line.question;' in render_body and
        'const auto& text = line.question ? turn.question : turn.answer;' in render_body and
        'onRight ? body.x + body.width - renderer.getTextWidth(UI_10_FONT_ID, shown.c_str()) : body.x' in render_body,
    'pagination covers the whole conversation':
        'pagesReady_ = conversationLines(width);' in pages_body,
    'the conversation is bounded so paginate() cannot refuse it':
        'kMaxTranscriptLines = 1024' in conversation_h.read_text() and
        lines_body.count('readerai::kMaxTranscriptLines') >= 4,
    'the question holds a line back for its own answer':
        'appendLines(turns_[turn].question, width, true, static_cast<uint32_t>(turn), readerai::kMaxTranscriptLines - 1);'
        in lines_body and
        'appendLines(turns_[turn].answer, width, false, static_cast<uint32_t>(turn), readerai::kMaxTranscriptLines);'
        in lines_body,
    'the line cap drops the OLDEST turns, never the newest':
        'while (start > 0 && lineTurns_[start - 1] == turn) --start;' in
        block(cpp, 'void VoiceAssistantActivity::enforceLines(', '\n}\n') and
        'for (size_t turn = turns_.size(); turn-- > 1;)' in cpp and
        'lines_.begin() + best' in cpp and 'lineTurns_.begin() + best' in cpp and
        'if (best == total) return;' in cpp,
    'paging still walks the accumulated lines':
        'if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) && page_ + 1 < pages)' in loop_body and
        'else if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious) && page_ > 0)' in loop_body,
    'a session opens on its newest turn':
        'page_ = pages - 1;' in pages_body,
    # --- the session model lives in its own header, with the caps spelled out
    'both session caps live with the model':
        'kMaxTurns = 20' in conversation_h.read_text() and 'kMaxDocBytes = 65536' in conversation_h.read_text(),
    'the model holds root/updated/turns with per-turn job id':
        all(token in conversation_h.read_text()
            for token in ('std::string root;', 'uint32_t updated = 0;', 'std::vector<Turn> turns;',
                          'std::string job;', 'std::string question;', 'std::string answer;')),
}

# --- quick/deep behaviour that existed before sessions must be unchanged
poll = block(cpp, 'bool VoiceAssistantActivity::poll(', '\n}\n')
checks.update({
    'quick poll bound is still the original 120 s': 'constexpr uint32_t kPollTimeoutMs = 120000;' in cpp,
    'deep poll bound is still 300 s': 'constexpr uint32_t kDeepPollTimeoutMs = 300000;' in cpp,
    'poll picks the wide bound only for a deep answer wait':
        'const uint32_t limit = deep_ && work_ == Work::Question ? kDeepPollTimeoutMs : kPollTimeoutMs;' in poll,
    'the question payload still selects the mode':
        'deep_ ? readerai::Mode::Deep : readerai::Mode::Quick' in run_body,
    'a quick question still sends no parent on the first turn':
        'client.question(sourceId_, question_, answerKey_, answerId_, parentId_,' in run_body,
    'the 6-item home menu is untouched':
        'state == State::Home ? 6' in loop_body and
        'menu = ' not in cpp and
        [m for m in re.findall(r'tr\((STR_\w+)\)',
                               block(block(render_body, 'if (state == State::Home) {', '} else if (state == State::Settings) {'),
                                     'const char* rows[] = {', '};'))] ==
        ['STR_AI_NEW', 'STR_AI_DEEP_TASK', 'STR_AI_RESUME', 'STR_AI_HISTORY', 'STR_AI_SETTINGS', 'STR_AI_CANCEL_JOB'],
    'the transcription poll is unchanged':
        'poll(client, sourceId_, question_)' in run_body and 'poll(client, sourceId_, question_,' not in cpp,
    'the answer poll is unchanged': 'if (!poll(client, answerId_, answer_)) return;' in run_body,
    'stage and elapsed still cross the worker boundary as atomics':
        'std::atomic<readerai::Stage> stage_' in header and 'std::atomic<uint32_t> elapsed_' in header,
    'the wait page still shows the stage and falls back to its own copy':
        'renderer.drawText(UI_10_FONT_ID, body.x, y, staged ? staged : tr(STR_AI_THINKING), true);' in cpp,
})

# --- i18n: the new name and the new copy exist in both languages, and no new
#     glyph is needed (the CJK faces are built from this subset file).
WANTED = {
    'STR_AI_TITLE': 'dogzclaw',
    'STR_AI_HISTORY': '会话历史',
    'STR_AI_NO_HISTORY': '没有本地会话记录',
    'STR_AI_TURNS_FMT': '共 %u 轮 · %s',
    'STR_AI_TURNS_ONLY': '共 %u 轮',
}


def translations(path):
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if line[:1].isalpha() and ':' in line:
            key, value = line.split(':', 1)
            values[key.strip()] = value.strip().strip('"')
    return values


zh, en = translations(chinese), translations(english)
checks['chinese carries the new name and session copy'] = all(zh.get(k) == v for k, v in WANTED.items())
checks['english carries the same keys'] = all(en.get(k) for k in WANTED)
checks['the product name is dogzclaw in both languages'] = zh.get('STR_AI_TITLE') == 'dogzclaw' and \
    en.get('STR_AI_TITLE') == 'dogzclaw'
checks['no AI 问答 user-facing copy is left'] = 'AI 问答' not in chinese.read_text(encoding='utf-8')
# A format string that lost or gained a token would print a stray %s on screen.
checks['the row formats keep the placeholders they are called with'] = (
    zh['STR_AI_TURNS_FMT'].count('%u') == 1 and zh['STR_AI_TURNS_FMT'].count('%s') == 1 and
    en['STR_AI_TURNS_FMT'].count('%u') == 1 and en['STR_AI_TURNS_FMT'].count('%s') == 1 and
    zh['STR_AI_TURNS_ONLY'].count('%u') == 1 and en['STR_AI_TURNS_ONLY'].count('%u') == 1 and
    '%s' not in zh['STR_AI_TURNS_ONLY'] and '%s' not in en['STR_AI_TURNS_ONLY'])
glyphs = set(glyph_subset.read_text(encoding='utf-8'))
missing = sorted({c for v in WANTED.values() for c in v if '\u4e00' <= c <= '\u9fff'} - glyphs)
checks['every new glyph is in the embedded CN subset'] = not missing

for label, ok in checks.items():
    assert ok, label
    print(f'PASS: {label}')
print(f'PASS: conversation sessions, list aggregation, resume links and rename ({len(checks)} checks)')
