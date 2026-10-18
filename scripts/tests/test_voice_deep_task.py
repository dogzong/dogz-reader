#!/usr/bin/env python3
"""Deep-task contract: request mode, optional stage/elapsed, menu and timeouts.

The protocol half compiles src/network/ReaderAiClient.cpp itself and drives it
through a fake transport, so the assertions describe the bytes on the wire. The
UI half slices the shipped activity source: the menu wiring and the poll bounds
have no host-side equivalent (they need the renderer and the network), so they
are asserted structurally, exactly like scripts/tests/test_voice_trim.py does.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
client_cpp = root / 'src/network/ReaderAiClient.cpp'
activity_cpp = root / 'src/activities/apps/voice-assistant/VoiceAssistantActivity.cpp'
activity_h = root / 'src/activities/apps/voice-assistant/VoiceAssistantActivity.h'
chinese = root / 'lib/I18n/translations/chinese.yaml'
english = root / 'lib/I18n/translations/english.yaml'
glyph_subset = root / 'lib/EpdFont/scripts/cn_i18n_chars.txt'


def json_headers():
    """ArduinoJson include dir, honouring a relocated libdeps directory.

    PLATFORMIO_LIBDEPS_DIR moves libdeps out of .pio/libdeps (the clean-build
    workflow points it at a temp dir), so the hardcoded default is only one of
    the places the dependency can live.
    """
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


driver = r'''
#include <ArduinoJson.h>

#include <cassert>
#include <cstdio>
#include <string>

#include "network/ReaderAiClient.h"
using namespace readerai;

int main() {
  const std::string id(32, 'a');
  const std::string parent(32, 'b');
  Request last;
  Reply reply{202, "{\"job_id\":\"" + id + "\"}", true};
  Client client([&](const Request& r) {
    last = r;
    return reply;
  });
  std::string job;

  // --- deep adds exactly one field: mode "deep"
  assert(client.question(id, "\xe9\x97\xae\xe9\xa2\x98", "k1", job, "", Mode::Deep));
  assert(last.method == "POST" && last.path == "/questions" && last.key == "k1");
  {
    JsonDocument body;
    assert(!deserializeJson(body, last.body));
    assert(body["transcription_job_id"].as<std::string>() == id);
    assert(body["text"].as<std::string>() == "\xe9\x97\xae\xe9\xa2\x98");
    assert(body["mode"].is<const char*>() && body["mode"].as<std::string>() == "deep");
    assert(body.size() == 3);
    std::printf("deep payload:   %s\n", last.body.c_str());
  }

  // --- default and explicit quick are byte-identical to the pre-deep payload
  const std::string quick = "{\"transcription_job_id\":\"" + id + "\",\"text\":\"\xe9\x97\xae\xe9\xa2\x98\"}";
  assert(client.question(id, "\xe9\x97\xae\xe9\xa2\x98", "k2", job));
  assert(last.body == quick);
  std::printf("quick payload:  %s\n", last.body.c_str());
  {
    JsonDocument body;
    assert(!deserializeJson(body, last.body));
    assert(!body["mode"].is<const char*>() && body.size() == 2);
  }
  assert(client.question(id, "\xe9\x97\xae\xe9\xa2\x98", "k3", job, "", Mode::Quick));
  assert(last.body == quick);
  // --- a follow-up keeps the parent and, when deep, the mode
  assert(client.question(id, "\xe8\xbf\xbd\xe9\x97\xae", "k4", job, parent));
  {
    JsonDocument body;
    assert(!deserializeJson(body, last.body));
    assert(body["parent_job_id"].as<std::string>() == parent);
    assert(!body["mode"].is<const char*>() && body.size() == 3);
  }
  assert(client.question(id, "\xe8\xbf\xbd\xe9\x97\xae", "k5", job, parent, Mode::Deep));
  {
    JsonDocument body;
    assert(!deserializeJson(body, last.body));
    assert(body["parent_job_id"].as<std::string>() == parent);
    assert(body["mode"].as<std::string>() == "deep" && body.size() == 4);
  }
  // --- deep is still validated like every other question
  assert(!client.question(id, "\xe9\x97\xae", "k6", job, "../../other", Mode::Deep));
  assert(!client.question("../other", "\xe9\x97\xae", "k7", job, "", Mode::Deep));

  // --- full progress response
  Status st;
  reply = {200, "{\"state\":\"running\",\"stage\":\"searching\",\"elapsed_s\":14}", true};
  assert(client.status(id, st));
  assert(st.state == "running" && st.stage == Stage::Searching && st.elapsed == 14);
  const std::pair<const char*, Stage> stages[] = {{"queued", Stage::Queued},   {"searching", Stage::Searching},
                                                  {"thinking", Stage::Thinking}, {"agent", Stage::Agent},
                                                  {"writing", Stage::Writing}};
  for (const auto& entry : stages) {
    reply = {200, std::string("{\"state\":\"running\",\"stage\":\"") + entry.first + "\",\"elapsed_s\":3}", true};
    assert(client.status(id, st) && st.stage == entry.second && st.elapsed == 3);
    std::printf("stage %-9s -> enum %d\n", entry.first, static_cast<int>(entry.second));
  }
  reply = {200, "{\"state\":\"completed\",\"stage\":\"writing\",\"elapsed_s\":301}", true};
  assert(client.status(id, st) && st.state == "completed" && st.stage == Stage::Writing && st.elapsed == 301);

  // --- absent optional fields: a successful poll that degrades to no stage
  reply = {200, "{\"state\":\"running\"}", true};
  assert(client.status(id, st));
  assert(st.state == "running" && st.stage == Stage::None && st.elapsed == 0);
  std::string plain;
  assert(client.state(id, plain) && plain == "running");

  // --- unknown or mistyped optional fields never fail the poll
  const char* degraded[] = {"{\"state\":\"running\",\"stage\":\"teleporting\",\"elapsed_s\":\"14\"}",
                            "{\"state\":\"running\",\"stage\":7,\"elapsed_s\":-5}",
                            "{\"state\":\"running\",\"stage\":null,\"elapsed_s\":null}",
                            "{\"state\":\"running\",\"stage\":\"\",\"elapsed_s\":14.5}"};
  for (const char* body : degraded) {
    reply = {200, body, true};
    assert(client.status(id, st));
    assert(st.state == "running" && st.stage == Stage::None && st.elapsed == 0);
  }
  // --- either optional field can arrive on its own
  reply = {200, "{\"state\":\"running\",\"stage\":\"agent\"}", true};
  assert(client.status(id, st) && st.stage == Stage::Agent && st.elapsed == 0);
  reply = {200, "{\"state\":\"running\",\"elapsed_s\":900}", true};
  assert(client.status(id, st) && st.stage == Stage::None && st.elapsed == 900);

  // --- the required state field keeps its strictness
  reply = {200, "{\"stage\":\"thinking\",\"elapsed_s\":9}", true};
  assert(!client.status(id, st) && client.error() == "protocol");
  reply = {200, "{\"state\":\"unexpected\",\"stage\":\"thinking\"}", true};
  assert(!client.status(id, st) && client.error() == "protocol");
  reply = {200, "{\"state\":\"running\",\"stage\":\"thinking\"}", false};
  assert(!client.status(id, st) && client.error() == "network");
  reply = {200, "{\"state\":\"running\",\"stage\":\"thinking\"}", true};
  assert(!client.status("../other", st) && client.error() == "protocol");
  // A rejected response must not half-write the caller's status.
  assert(st.state == "running" && st.stage == Stage::None && st.elapsed == 900);
  std::printf("degraded responses: %d accepted without a stage\n", static_cast<int>(sizeof(degraded) / sizeof(*degraded)));
  std::printf("PASS: question mode serialization and optional stage/elapsed parsing\n");
}
'''

with tempfile.TemporaryDirectory() as directory:
    source = Path(directory) / 'deep_task.cpp'
    source.write_text(driver)
    binary = Path(directory) / 'deep-task'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-I' + str(root / 'src'),
                    '-I' + str(json_headers()), str(client_cpp), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

cpp = activity_cpp.read_text()
header = activity_h.read_text()


def block(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


# --- 6-item home menu, with the deep entry second
loop_body = block(cpp, 'void VoiceAssistantActivity::loop()', 'void VoiceAssistantActivity::render(RenderLock&&)')
render_body = block(cpp, 'void VoiceAssistantActivity::render(RenderLock&&)',
                    'const auto labels = mainTabButtonLabels')
home_render = block(render_body, 'if (state == State::Home) {', '} else if (state == State::Settings) {')
home_rows = block(home_render, 'const char* rows[] = {', '};')
menu = re.findall(r'tr\((STR_\w+)\)', home_rows)
checks = {
    'home menu draws six rows': bool(re.search(r'GUI\.drawList\(\s*renderer, body, 6, selected_', home_render)),
    'home rows array holds six labels': len(menu) == 6,
    'the deep task is the second entry': menu[:2] == ['STR_AI_NEW', 'STR_AI_DEEP_TASK'],
    'home selection count is six':
        'state == State::Home ? 6' in block(loop_body, 'int count = state', 'if (count) {'),
    'menu selection still wraps around':
        '(selected_ + count - 1) % count' in loop_body and '(selected_ + 1) % count' in loop_body,
}
home_loop = block(loop_body, 'if (state == State::Home) {', '} else if (state == State::Consent) {')
deep_entry = block(home_loop, '} else if (selected_ == 1) {', '} else if (selected_ == 2) {')
quick_entry = block(home_loop, 'if (selected_ == 0) {', '} else if (selected_ == 1) {')
resume_entry = block(home_loop, '} else if (selected_ == 2) {', '} else if (selected_ == 3)')
checks.update({
    'the deep entry selects deep mode and starts the normal flow':
        'deep_ = true;' in deep_entry and 'startNew();' in deep_entry,
    'the plain voice entry clears deep mode':
        'deep_ = false;' in quick_entry and 'startNew();' in quick_entry,
    'resume takes the mode from the pending record':
        'deep_ = false;' in resume_entry and 'loadPending()' in resume_entry,
    'the cancel entry moved with the new row':
        '} else if (!sourceId_.empty() || !answerId_.empty())' in home_loop,
})

# --- timeout bounds: quick unchanged, deep wider
poll = block(cpp, 'bool VoiceAssistantActivity::poll(', '\n}\n')
checks.update({
    'quick poll bound is the original 120 s': 'constexpr uint32_t kPollTimeoutMs = 120000;' in cpp,
    'deep poll bound is 300 s': 'constexpr uint32_t kDeepPollTimeoutMs = 300000;' in cpp,
    'the two bounds differ':
        re.search(r'kDeepPollTimeoutMs = (\d+);', cpp).group(1) != re.search(r'kPollTimeoutMs = (\d+);', cpp).group(1),
    'poll picks the wide bound only for an answer wait of a deep task':
        'const uint32_t limit = deep_ && work_ == Work::Question ? kDeepPollTimeoutMs : kPollTimeoutMs;' in poll,
    'poll no longer hardcodes 120000': '120000' not in poll,
    'the transcription poll stays on the quick bound':
        'poll(client, sourceId_, question_)' in cpp and 'poll(client, sourceId_, question_,' not in cpp,
    'the answer poll keeps its original call site': 'if (!poll(client, answerId_, answer_)) return;' in cpp,
    'a deep question is submitted as mode deep and quick stays default':
        'deep_ ? readerai::Mode::Deep : readerai::Mode::Quick' in cpp,
})

# --- stage rendering
stage_text = block(cpp, 'const char* stageText(readerai::Stage stage) {', '\n}\n')
checks.update({
    'every displayable stage has copy': all(
        f'tr({key})' in stage_text
        for key in ('STR_AI_STAGE_SEARCHING', 'STR_AI_STAGE_THINKING', 'STR_AI_STAGE_AGENT', 'STR_AI_STAGE_WRITING')),
    'queued and unknown stages fall back to the generic copy': 'default:' in stage_text and 'return nullptr;' in stage_text,
    'stage text only replaces the wait copy on the question wait page':
        'if (const char* staged = stageText(stage_.load(std::memory_order_acquire))) message = staged;' in cpp and
        'state == State::Busy && work_ == Work::Question' in cpp,
    'the elapsed stamp is printed only when the server reported one':
        'if (elapsed) {' in cpp and 'tr(STR_AI_ELAPSED_FMT)' in cpp,
    'the wait page still falls back to the existing working copy':
        'tr(STR_AI_TRANSCRIBING) : tr(STR_AI_WORKING)' in cpp,
    # The ordinary question is polled while the transcript is on screen, so that
    # page carries the stage too; its own copy stays the fallback.
    'the transcript wait line shows the stage and falls back to its own copy':
        'renderer.drawText(UI_10_FONT_ID, body.x, y, staged ? staged : tr(STR_AI_THINKING), true);' in cpp,
    'the transcript wait line carries the elapsed seconds once a stage is shown':
        'if (staged && elapsed) {' in cpp and
        'body.x + body.width - renderer.getTextWidth(SMALL_FONT_ID, stamp)' in cpp,
    'the poll publishes stage and elapsed for the wait page':
        'stage_.store(job.stage, std::memory_order_release);' in poll and
        'elapsed_.store(job.elapsed, std::memory_order_release);' in poll,
    'a new cycle clears the previous stage':
        'stage_.store(readerai::Stage::None, std::memory_order_release);' in block(
            cpp, 'void VoiceAssistantActivity::launch(Work work) {', 'pagesReady_ = false;'),
    'the wait page repaints when the reported progress moves':
        'lastElapsed_ != elapsed || lastStage_ != stage' in
        block(loop_body, 'if (active_.load(std::memory_order_acquire) &&', 'return;\n  }'),
})

# --- pending.json keeps a deep task resumable without touching quick records
checks.update({
    'deep is saved only for a deep task': 'if (deep_) d["deep"] = true;' in cpp,
    'deep defaults to false when the record has no field': 'deep_ = d["deep"] | false;' in cpp,
    'an unusable record clears deep': block(cpp, 'bool VoiceAssistantActivity::loadPending()', '\n}\n').count(
        'deep_ = false;') == 1,
    'the deep flag survives on the header': 'bool deep_ = false;' in header,
    'stage crosses the worker boundary as an enum, not a string':
        'std::atomic<readerai::Stage> stage_' in header and 'std::atomic<uint32_t> elapsed_' in header,
})

# --- i18n: both languages carry the new copy, and no new glyph is required
WANTED = {
    'STR_AI_DEEP_TASK': '深度任务',
    'STR_AI_STAGE_SEARCHING': '正在检索…',
    'STR_AI_STAGE_THINKING': '正在思考…',
    'STR_AI_STAGE_AGENT': '深度任务执行中…',
    'STR_AI_STAGE_WRITING': '正在整理…',
    'STR_AI_ELAPSED_FMT': '已等待 %u 秒',
}


def translations(path):
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if line[:1].isalpha() and ':' in line:
            key, value = line.split(':', 1)
            values[key.strip()] = value.strip().strip('"')
    return values


zh, en = translations(chinese), translations(english)
checks['chinese carries every new string'] = all(zh.get(k) == v for k, v in WANTED.items())
checks['english carries every new string'] = all(en.get(k) for k in WANTED)
checks['english deep-task label is the plain menu entry'] = en.get('STR_AI_DEEP_TASK') == 'Deep task'
# The Chinese faces are subset fonts built from the characters already used in
# chinese.yaml; a new glyph would render as a blank box unless the committed
# subset and its bitmaps are rebuilt.
glyphs = set(glyph_subset.read_text(encoding='utf-8'))
missing = sorted({c for v in WANTED.values() for c in v if '\u4e00' <= c <= '\u9fff'} - glyphs)
checks['every new glyph is already in the embedded CN subset'] = not missing

for label, ok in checks.items():
    assert ok, label
    print(f'PASS: {label}')
print(f'PASS: deep task protocol, menu, stage display and timeouts ({len(checks)} checks)')
