#include "VoiceAssistantActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalAudioOutput.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "MappedInputManager.h"
#include "NetworkStartup.h"
#include "VoiceDebug.h"
#include "VoiceTrim.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/ReaderAiDevice.h"

namespace {
// Everything the assistant keeps on the card lives under one directory: the
// recording, its half-written .part and the resumable task.
constexpr const char* kDir = "/.crosspoint/voice-assistant";
// The recording the current question is built from. A new one is captured into
// Wav.part and published as Wav, and the trim rewrites Wav.part again: a failure
// at either step can only ever lose the .part, never the recording the uploader
// reads.
constexpr const char* Wav = "/.crosspoint/voice-assistant/question.wav";
constexpr const char* Part = "/.crosspoint/voice-assistant/question.wav.part";
// The one in-flight task: which question job is being waited on, which
// transcription it came from, and the idempotency keys that make Retry resume
// instead of asking again.
constexpr const char* kPending = "/.crosspoint/voice-assistant/pending.json";
constexpr const char* kCaPath = "/.crosspoint/reader-ai-ca.pem";
// The quick bound: a transcription and an ordinary answer both have to finish
// inside two minutes, or the wait is over.
constexpr uint32_t kPollTimeoutMs = 120000;
// Deep tasks run a whole agent loop on the server. The same wait was 120 s
// before deep tasks existed, and a deep answer that is still writing at five
// minutes is not coming.
constexpr uint32_t kDeepPollTimeoutMs = 300000;
constexpr uint32_t kPollIntervalMs = 400;
// The consent copy promises "up to 60 seconds", and the capture stops itself at
// that bound rather than trusting the user to press Confirm in time.
constexpr uint32_t kMaxRecordSeconds = 60;
constexpr uint32_t kMaxRecordSamples = readerai::kSampleRate * kMaxRecordSeconds;
// A rewrite costs a full copy of the recording plus a second file on the card,
// so the trim only pays when it removes at least this share of the bytes.
constexpr uint32_t kTrimMinGainPercent = 5;
// A pending record is a few hundred bytes; the bound only exists so a corrupt
// file dropped on the card cannot allocate the heap.
constexpr size_t kPendingBytes = 4096;
// How long a saved-credential connect is allowed to take. It runs in the
// background for the onEnter warm-up, and on the UI task for a screen the user
// explicitly opened.
constexpr uint32_t kWifiTimeoutMs = 8000;
// 8 KB was not enough: the question path streams a 24 KiB answer through
// ArduinoJson and std::string on this task, and the overflow surfaced as
// `assert failed: multi_heap_free multi_heap_poisoning.c:279 (head != NULL)` on
// the next allocation rather than as a stack fault. The headroom is logged after
// every run() so a regression is visible in the serial log.
constexpr uint32_t kTaskStackBytes = 16384;
constexpr size_t kStatusTextBytes = 48;
// Held this long, Confirm deletes the session under the cursor instead of
// opening it.
constexpr unsigned long kLongPressMs = 700;

void writeLe32(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value >> 16);
  dst[3] = static_cast<uint8_t>(value >> 24);
}

// 44-byte canonical WAV header for the 16 kHz mono 16-bit capture: the same
// shape the audio activity writes, so both files stay interchangeable.
void wavHeader(uint8_t* out, uint32_t pcmBytes) {
  memset(out, 0, 44);
  memcpy(out, "RIFF", 4);
  writeLe32(out + 4, pcmBytes + 36);
  memcpy(out + 8, "WAVEfmt ", 8);
  writeLe32(out + 16, 16);
  out[20] = 1;  // PCM
  out[22] = 1;  // mono
  writeLe32(out + 24, readerai::kSampleRate);
  writeLe32(out + 28, readerai::kSampleRate * 2);
  out[32] = 2;   // block align
  out[34] = 16;  // bits per sample
  memcpy(out + 36, "data", 4);
  writeLe32(out + 40, pcmBytes);
}

// Idempotency key for one request. 32 hex characters so it looks like the job
// ids it travels with, and random per question, so a retry of the SAME question
// carries the same key while a new question never reuses an old one.
std::string key() {
  char buffer[33];
  snprintf(buffer, sizeof(buffer), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  return std::string(buffer);
}

// Wait copy for the stage the server reports. A queued or unknown stage returns
// nullptr so the caller falls back to the copy that page already had: an
// unfamiliar stage must not blank the screen.
const char* stageText(readerai::Stage stage) {
  switch (stage) {
    case readerai::Stage::Searching:
      return tr(STR_AI_STAGE_SEARCHING);
    case readerai::Stage::Thinking:
      return tr(STR_AI_STAGE_THINKING);
    case readerai::Stage::Agent:
      return tr(STR_AI_STAGE_AGENT);
    case readerai::Stage::Writing:
      return tr(STR_AI_STAGE_WRITING);
    default:
      return nullptr;
  }
}

// The failures the device can name itself, before the server ever sees a
// request, mapped to the copy that tells the user which setting to open.
const char* errorText(const std::string& code) {
  if (code == "unauthorized") return tr(STR_AI_AUTH_ERROR);
  if (code == "ca") return tr(STR_AI_CA_ERROR);
  if (code == "clock") return tr(STR_AI_CLOCK_ERROR);
  if (code == "config") return tr(STR_AI_CONFIG_ERROR);
  if (code == "capability") return tr(STR_AI_CAPABILITY_ERROR);
  if (code == "timeout") return tr(STR_AI_TIMEOUT);
  if (code == "nopending") return tr(STR_AI_NO_PENDING);
  return tr(STR_AI_FAILED);
}

// Why the request would fail locally, or nullptr when it can be sent. The device
// transport answers all of these with an empty reply, so the reason has to be
// established before the call: "config", "network", "clock" and "ca" each point
// at a different fix.
const char* blockedReason(const readerai::Config& config) {
  if (!readerai::validEndpoint(config.endpoint) || config.token.empty()) return "config";
  if (WiFi.status() != WL_CONNECTED) return "network";
  if (time(nullptr) < 1700000000) return "clock";
  if (!Storage.exists(kCaPath)) return "ca";
  return nullptr;
}

// One source of truth for the row grid: preparePages() counts the pages from it
// and render() walks them with the same numbers, so the two cannot disagree.
int linesPerPage(const GfxRenderer& renderer, const Rect& body) {
  const int lineHeight = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  return std::max(1, body.height / lineHeight);
}

int pageCount(const GfxRenderer& renderer, const Rect& body, size_t lines) {
  const size_t perPage = static_cast<size_t>(linesPerPage(renderer, body));
  return static_cast<int>(std::max<size_t>(1, (lines + perPage - 1) / perPage));
}

// Draws a single message the same way the transcript is laid out, for the pages
// that show one paragraph instead of a turn.
void drawParagraph(GfxRenderer& renderer, const Rect& body, const char* message, int fontId) {
  if (message == nullptr || *message == '\0') return;
  const std::string text(message);
  std::vector<readerai::Line> lines;
  const auto measure = [&renderer, fontId](const std::string& run) {
    return renderer.getTextWidth(fontId, run.c_str());
  };
  if (!readerai::paginate(text, body.width, measure, lines)) {
    renderer.drawText(fontId, body.x, body.y, message, true);
    return;
  }
  const int lineHeight = renderer.getLineHeight(fontId);
  int y = body.y;
  for (const auto& line : lines) {
    if (y + lineHeight > body.y + body.height) break;
    renderer.drawText(fontId, body.x, y, text.substr(line.start, line.end - line.start).c_str(), true);
    y += lineHeight;
  }
}
}  // namespace

// The USB test hook's single definition (see VoiceDebug.h): main.cpp and this
// activity have to look at the same object, which a function-local static in an
// inline accessor cannot guarantee once LTO is on.
namespace readerai {
DebugAsk debugAskRequest;
}  // namespace readerai

void VoiceAssistantActivity::onEnter() {
  Activity::onEnter();
  HalAudioOutput::shutdown();
  config_.load();
  state_.store(State::Home, std::memory_order_release);
  pagesReady_ = false;
  wifiFailed_.store(false, std::memory_order_release);
  thinking_.store(false, std::memory_order_release);
  seconds_.store(0, std::memory_order_release);
  lastSeconds_ = UINT32_MAX;
  lastElapsed_ = UINT32_MAX;
  lastStage_ = readerai::Stage::None;
  // Warm the radio up in the background while the user reads the menu: the first
  // question needs it, the connect takes seconds, and Work::Wifi deliberately
  // leaves the page state alone so nothing on screen changes.
  if (WiFi.status() != WL_CONNECTED) {
    // NetworkStartup may reclaim fonts under RenderLock. Do this on the UI
    // thread, before starting a worker that onExit() may have to join.
    wifiOwned_ = true;
    if (NetworkStartup::setMode(renderer, WIFI_STA)) launch(Work::Wifi);
  }
  requestUpdate();
}

void VoiceAssistantActivity::onExit() {
  // The worker dereferences `this`, so it has to be gone before the activity is
  // destroyed. cancel_ is checked by the poll loop and by the request transport,
  // so a run in the middle of a deep answer still exits promptly.
  cancel_.store(true, std::memory_order_release);
  workQueued_ = false;
  // Never destroy a live worker's context. Transport timeouts and cancellation
  // bound I/O; publishUi() uses try-locks so it cannot deadlock on our render lock.
  while (active_.load(std::memory_order_acquire)) delay(20);
  // The kept-alive TLS session holds the wolfSSL context and its sockets.
  readerai::closeSession();
  if (wifiOwned_) {
    WiFi.disconnect(false);
    wifiOwned_ = false;
  }
  HalAudioOutput::shutdown();
  Activity::onExit();
}

// USB test hook: runs one round from an injected transcript, so the real worker,
// turn append and layout are exercised without a microphone. False means the
// caller has to park the request and try again.
bool VoiceAssistantActivity::startInjectedAsk(const std::string& transcriptId, const std::string& text) {
  if (active_.load(std::memory_order_acquire)) return false;
  if (!readerai::validJobId(transcriptId) || text.empty()) return false;
  // A new round of the SAME session: turns_ is kept, so the answer appends to
  // the file the previous round wrote and parent_job_id walks the chain.
  sourceId_ = transcriptId;
  question_ = text;
  parentId_ = readerai::ConversationStore::lastJob(conversation());
  // The submission state has to be empty, or run() skips the POST and polls the
  // previous round's job id instead of asking this question.
  answerId_.clear();
  answerKey_.clear();
  sourceKey_.clear();
  answer_.clear();
  error_.clear();
  thinking_.store(false, std::memory_order_release);
  const int parent = parentId_.empty() ? 0 : 1;
  LOG_INF("AI", "injected ask turns=%u parent=%d text=%u", static_cast<unsigned>(turns_.size()), parent,
          static_cast<unsigned>(text.size()));
  launch(Work::Ask);
  return true;
}

void VoiceAssistantActivity::task(void* context) {
  auto* self = static_cast<VoiceAssistantActivity*>(context);
  self->workerTask_.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
  self->run();
  if (self->cancel_.load(std::memory_order_acquire)) {
    readerai::closeSession();
    self->thinking_.store(false, std::memory_order_release);
    self->state_.store(State::Home, std::memory_order_release);
  }
  LOG_INF("AI", "ReaderAI stack headroom: %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  self->requestUpdate();
  // Publish completion last: onExit may destroy self as soon as it is false.
  self->workerTask_.store(nullptr, std::memory_order_release);
  self->active_.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void VoiceAssistantActivity::launch(Work work) {
  if (active_.load(std::memory_order_acquire)) {
    if (work_.load(std::memory_order_acquire) == Work::Wifi) {
      queuedWork_ = work;
      workQueued_ = true;
    }
    return;
  }
  work_.store(work, std::memory_order_release);
  cancel_.store(false, std::memory_order_release);
  stop_.store(false, std::memory_order_release);
  // A new cycle must not inherit the previous job's progress: the wait page
  // reads these two directly, and a stale "Writing up…" is worse than no copy.
  stage_.store(readerai::Stage::None, std::memory_order_release);
  elapsed_.store(0, std::memory_order_release);
  active_.store(true, std::memory_order_release);
  if (xTaskCreate(&VoiceAssistantActivity::task, "ReaderAI", kTaskStackBytes, this, 1, nullptr) != pdPASS) {
    active_.store(false, std::memory_order_release);
    fail("task");
    return;
  }
  requestUpdate();
}

bool VoiceAssistantActivity::connectSavedWifi(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  std::string ssid, password;
  {
    if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
    const std::string last = WIFI_STORE.getLastConnectedSsid();
    if (!last.empty()) {
      const auto credential = WIFI_STORE.findCredential(last);
      if (credential) {
        ssid = credential->ssid;
        password = credential->password;
      }
    }
  }
  if (ssid.empty()) return false;  // no saved network: the picker is the only way
  WiFi.persistent(false);
  // Fresh slate: disconnect(false) would keep the cached AP, and the next
  // begin() could then latch credentials that are not the ones just loaded.
  WiFi.disconnect(true, true);
  delay(100);
  if (password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
  wifiOwned_ = true;
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (cancel_.load(std::memory_order_acquire) || millis() - started >= timeoutMs) return false;
    delay(100);
  }
  return true;
}

// Connects first and then hands the worker the real work, so a screen the user
// explicitly opened does not need a second tap once the radio is up.
void VoiceAssistantActivity::connectAndLaunch(Work work) {
  // Connection retries stay on the worker; UI never waits eight seconds.
  launch(work);
}

void VoiceAssistantActivity::fail(const char* code) {
  const auto update = [&] {
    error_ = code;
    thinking_.store(false, std::memory_order_release);
    state_.store(State::Error, std::memory_order_release);
    requestUpdate();
  };
  if (xTaskGetCurrentTaskHandle() == workerTask_.load(std::memory_order_acquire)) {
    publishUi(update);
  } else {
    update();  // UI caller already owns RenderLock (or no worker has started).
  }
}

bool VoiceAssistantActivity::savePending() {
  JsonDocument d;
  if (!answerId_.empty()) d["answer_job"] = answerId_;
  if (!sourceId_.empty()) d["source_job"] = sourceId_;
  if (!parentId_.empty()) d["parent_job"] = parentId_;
  if (!sourceKey_.empty()) d["source_key"] = sourceKey_;
  if (!answerKey_.empty()) d["answer_key"] = answerKey_;
  if (!question_.empty()) d["question"] = question_;
  if (deep_) d["deep"] = true;
  if (!rootId_.empty()) d["root"] = rootId_;
  std::string body;
  serializeJson(d, body);
  if (d.overflowed() || body.empty() || body.size() >= kPendingBytes) return false;
  if (!Storage.ensureDirectoryExists(kDir)) return false;
  return Storage.writeFile(kPending, String(body.c_str()));
}

bool VoiceAssistantActivity::loadPending() {
  // The task on screen is about to be replaced by the record's: the page cache
  // has to be invalidated BEFORE turns_ is touched, exactly as in run().
  pagesReady_ = false;
  std::string raw;
  JsonDocument d;
  bool usable = Storage.readFileToString("AI", kPending, kPendingBytes, raw) && !deserializeJson(d, raw);
  if (usable) {
    rootId_ = d["root"] | "";
    sourceId_ = d["source_job"] | "";
    answerId_ = d["answer_job"] | "";
    parentId_ = d["parent_job"] | "";
    sourceKey_ = d["source_key"] | "";
    answerKey_ = d["answer_key"] | "";
    question_ = d["question"] | "";
    deep_ = d["deep"] | false;
    // A record that names a session has to name a real job id, and one with
    // neither a transcription nor an answer job has nothing left to resume.
    if ((!rootId_.empty() && !readerai::validJobId(rootId_)) || (sourceId_.empty() && answerId_.empty()))
      usable = false;
  }
  if (!usable) {
    // Nothing was restored, so the mode goes back to quick with the record: a
    // half-read deep flag must not widen the next poll.
    deep_ = false;
    fail("nopending");
    return false;
  }
  error_.clear();
  // The finished turns live in the session file; the turn in flight does not
  // (it is only written once its answer arrives), so it is rebuilt from the
  // record and re-linked when the answer lands.
  turns_.clear();
  if (!rootId_.empty()) {
    readerai::Conversation stored;
    if (readerai::ConversationStore::load(rootId_ + ".json", rootId_, stored)) turns_ = stored.turns;
  }
  if (!question_.empty() && (turns_.empty() || turns_.back().question != question_)) {
    turns_.push_back(readerai::Turn{answerId_, question_, std::string()});
  }
  lines_.clear();
  lineTurns_.clear();
  page_ = 0;
  const Work resumeWork = answerId_.empty() ? Work::Transcribe : Work::Question;
  thinking_.store(!answerId_.empty(), std::memory_order_release);
  state_.store(State::Busy, std::memory_order_release);
  launch(resumeWork);
  return true;
}

readerai::Conversation VoiceAssistantActivity::conversation() const {
  readerai::Conversation session;
  session.root = rootId_;
  // The clock is only used for the row stamp: the store writes no field when it
  // is still unset, and formatStamp() draws nothing for it either.
  session.updated = static_cast<uint32_t>(time(nullptr));
  session.turns = turns_;
  return session;
}

bool VoiceAssistantActivity::saveConversation() {
  if (!readerai::validJobId(rootId_)) return false;
  // The caps are applied to a copy: the screen keeps every turn it has until the
  // file has actually accepted the record, and the worker never reallocates the
  // turns_ buffer the render task is walking.
  readerai::Conversation session = conversation();
  if (!readerai::ConversationStore::save(session)) return false;
  LOG_INF("AI", "conversation saved: turns=%u", static_cast<unsigned>(session.turns.size()));
  return true;
}

void VoiceAssistantActivity::loadHistory() {
  history_ = readerai::ConversationStore::list();
  historySubtitles_.clear();
  // One row string per session, composed once: drawList hands the callback a
  // c_str(), so the text has to outlive the draw call.
  historySubtitles_.reserve(history_.size());
  historySubtitles_.resize(history_.size());
  for (size_t i = 0; i < history_.size(); ++i) {
    const std::string stamp = readerai::formatStamp(history_[i].updated);
    char row[kStatusTextBytes];
    if (stamp.empty()) {
      snprintf(row, sizeof(row), tr(STR_AI_TURNS_ONLY), static_cast<unsigned>(history_[i].turns));
    } else {
      snprintf(row, sizeof(row), tr(STR_AI_TURNS_FMT), static_cast<unsigned>(history_[i].turns), stamp.c_str());
    }
    historySubtitles_[i] = row;
  }
  selected_ = 0;
  state_.store(State::History, std::memory_order_release);
  requestUpdate();
}

void VoiceAssistantActivity::run() {
  // Every request goes through the device transport, which keeps one TLS session
  // for the whole cycle and aborts the moment cancel_ is set.
  readerai::Client client(
      [this](const readerai::Request& request) { return readerai::deviceRequest(config_, request, cancel_); });

  if (work_ == Work::Record) {
    // Capture runs here, not on the UI task: the codec, the card and the trim
    // all block for seconds at a time.
    if (!Storage.ready() || !Storage.ensureDirectoryExists(kDir)) {
      fail("storage");
      return;
    }
    uint32_t written = 0;
    bool failed = false;
    bool audioFailed = false;
    {
      HalFile file;
      if (!Storage.openFileForWrite("audio", Part, file)) {
        fail("storage");
        return;
      }
      uint8_t header[44];
      wavHeader(header, 0);
      if (file.write(header, sizeof(header)) != sizeof(header)) {
        failed = true;
      } else if (!HalAudioOutput::beginCapture(readerai::kSampleRate)) {
        failed = true;
        audioFailed = true;
      } else {
        seconds_.store(0, std::memory_order_release);
        int16_t pcm[256];
        while (!stop_.load(std::memory_order_acquire) && !cancel_.load(std::memory_order_acquire) &&
               written < kMaxRecordSamples * 2) {
          const int count = HalAudioOutput::readCapture(pcm, 256, 100);
          if (count < 0) {
            failed = true;
            break;
          }
          if (count == 0) continue;
          const size_t bytes = static_cast<size_t>(count) * sizeof(int16_t);
          if (file.write(reinterpret_cast<const uint8_t*>(pcm), bytes) != bytes) {
            failed = true;
            break;
          }
          written += static_cast<uint32_t>(bytes);
          seconds_.store(written / 2 / readerai::kSampleRate, std::memory_order_release);
        }
        HalAudioOutput::endCapture();
        if (!failed && written > 0) {
          uint8_t final[44];
          wavHeader(final, written);
          failed = !file.seek(0) || file.write(final, sizeof(final)) != sizeof(final);
          file.flush();
        } else {
          failed = true;
        }
      }
      // The capture file is closed by its destructor here, before anything
      // renames or deletes the path it was writing.
    }
    if (cancel_.load(std::memory_order_acquire)) {
      // Back means "forget this recording": nothing was published, so there is
      // nothing to resume and the menu is where the user left it.
      Storage.remove(Part);
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
      return;
    }
    if (failed) {
      Storage.remove(Part);
      fail(audioFailed ? "audio" : "storage");
      return;
    }
    if (!Storage.replaceFile(Part, Wav)) {
      Storage.remove(Part);
      fail("storage");
      return;
    }
    // Trim only after the recording is published: the .part is the trim's own
    // scratch space, and a failure there leaves the untrimmed recording in place
    // for the upload to use.
    const uint32_t trimmed = trimRecording(written + 44);
    if (trimmed) written = trimmed;
    (void)written;
    sourceId_.clear();
    answerId_.clear();
    answerKey_.clear();
    answer_.clear();
    question_.clear();
    sourceKey_ = key();
    if (!savePending()) {
      fail("storage");
      return;
    }
    // Consent is already persisted: stopping capture automatically uploads.
    // Transcript review, when enabled, pauses only after ASR has completed.
    state_.store(State::Busy, std::memory_order_release);
    requestUpdate();
    work_ = Work::Transcribe;
  }

  if (work_ == Work::Cancel) {
    // Best effort on the server, unconditional locally: the user has said the
    // saved task is not to be resumed, and an offline device must still be able
    // to drop it.
    if (!answerId_.empty()) {
      client.cancel(answerId_);
    } else if (!sourceId_.empty()) {
      client.cancel(sourceId_);
    }
    answerId_.clear();
    sourceId_.clear();
    answerKey_.clear();
    sourceKey_.clear();
    question_.clear();
    answer_.clear();
    Storage.remove(Wav);
    Storage.remove(Part);
    Storage.remove(kPending);
    readerai::closeSession();
    state_.store(State::Home, std::memory_order_release);
    requestUpdate();
    return;
  }

  if (work_ == Work::Wifi) {
    // Warm-up failure is silent: offline history/settings remain usable.
    connectSavedWifi(kWifiTimeoutMs);
    return;
  }

  if (WiFi.status() != WL_CONNECTED && !connectSavedWifi(kWifiTimeoutMs)) {
    if (!cancel_.load(std::memory_order_acquire)) {
      wifiFailed_.store(true, std::memory_order_release);
      fail("network");
    }
    return;
  }

  const char* blocked = blockedReason(config_);
  if (blocked != nullptr) {
    fail(blocked);
    return;
  }

  if (work_ == Work::Health) {
    bool asr = false, qa = false;
    if (!client.health(asr, qa)) {
      fail(client.error().c_str());
      return;
    }
    if (!asr || !qa) {
      fail("capability");
      return;
    }
    state_.store(State::Connected, std::memory_order_release);
    requestUpdate();
    return;
  }

  if (work_ == Work::Ask) {
    // One round from an injected transcript: no recording was made, but
    // everything from here on is the ordinary spoken path.
    Storage.remove(Wav);
    // The page cache is invalidated BEFORE turns_ grows: the UI task indexes
    // turns_ through lines_/lineTurns_, and push_back may free the buffer it is
    // reading. Invalidating after the append is what corrupted the heap.
    if (!publishUi([&] {
          pagesReady_ = false;
          turns_.push_back(readerai::Turn{sourceId_, question_, std::string()});
          state_.store(State::Transcript, std::memory_order_release);
          requestUpdate();
        }))
      return;
    thinking_.store(true, std::memory_order_release);
    work_ = Work::Question;
  }

  if (work_ == Work::Transcribe) {
    if (!client.transcribe(Wav, sourceKey_, sourceId_)) {
      fail(client.error().c_str());
      return;
    }
    if (!savePending()) {
      fail("storage");
      return;
    }
    if (!poll(client, sourceId_, question_)) return;
    if (question_.empty() || !readerai::validUtf8(question_)) {
      fail("protocol");
      return;
    }
    // Same rule as the injected path: invalidate the page cache before the
    // vector can reallocate and free the buffer the render task is reading.
    if (!publishUi([&] {
          pagesReady_ = false;
          turns_.push_back(readerai::Turn{sourceId_, question_, std::string()});
          state_.store(State::Transcript, std::memory_order_release);
          requestUpdate();
        }))
      return;
    if (config_.reviewTranscript) {
      // The user reads the transcript first; Confirm sends it as the question.
      thinking_.store(false, std::memory_order_release);
      return;
    }
    thinking_.store(true, std::memory_order_release);
    work_ = Work::Question;
  }

  if (work_ == Work::Question) {
    if (answerId_.empty()) {
      if (answerKey_.empty()) answerKey_ = key();
      // A quick question omits the mode field entirely, so the payload stays
      // byte-identical to the client that shipped before deep tasks existed.
      if (!client.question(sourceId_, question_, answerKey_, answerId_, parentId_,
                           deep_ ? readerai::Mode::Deep : readerai::Mode::Quick)) {
        fail(client.error().c_str());
        return;
      }
      // The first question of a session names the file the session is stored in.
      if (rootId_.empty()) rootId_ = answerId_;
      if (!savePending()) {
        fail("storage");
        return;
      }
    }
    thinking_.store(true, std::memory_order_release);
    requestUpdate();
    if (!poll(client, answerId_, answer_)) return;
    if (answer_.empty()) {
      fail("protocol");
      return;
    }
    // Invalidate first, then write: the layout has to be rebuilt anyway, because
    // the answer is new text on the page.
    if (!publishUi([&] {
          pagesReady_ = false;
          if (!turns_.empty()) {
            turns_.back().job = answerId_;
            turns_.back().answer = answer_;
          }
        }))
      return;
    if (!saveConversation()) {
      fail("storage");
      return;
    }
    // The task is complete: the session file is the record now, and a stale
    // pending record would offer to resume a job that has already answered.
    Storage.remove(kPending);
    readerai::closeSession();
    thinking_.store(false, std::memory_order_release);
    state_.store(State::Answer, std::memory_order_release);
    requestUpdate();
    return;
  }

  fail("protocol");
}

bool VoiceAssistantActivity::openConversation(const readerai::HistoryEntry& entry) {
  readerai::Conversation session;
  if (!readerai::ConversationStore::load(entry.file, entry.root, session)) return false;
  // A follow-up continues THIS session: the file keeps its name and the chain
  // continues from its newest question, which is what parent_job_id carries.
  parentId_ = readerai::ConversationStore::lastJob(session);
  // Invalidate before replacing turns_, or the next paint indexes the new vector
  // through lines_/lineTurns_ offsets that describe the old one.
  pagesReady_ = false;
  turns_ = session.turns;
  lines_.clear();
  lineTurns_.clear();
  question_.clear();
  answer_.clear();
  sourceId_.clear();
  answerId_.clear();
  sourceKey_.clear();
  answerKey_.clear();
  error_.clear();
  rootId_ = session.root;
  deep_ = false;
  page_ = 0;
  thinking_.store(false, std::memory_order_release);
  state_.store(State::Answer, std::memory_order_release);
  requestUpdate();
  return true;
}

void VoiceAssistantActivity::appendLines(const std::string& text, int width, bool question, uint32_t turn,
                                         size_t keep) {
  if (lines_.size() >= keep || text.empty()) return;
  // Warm the font cache with ONE whole-string measurement before paginate().
  // paginate() asks for a glyph at a time, and a cold font opens the .cpfont
  // file for every glyph it has not seen yet: a 2159-byte answer took 20423 ms
  // to lay out on a cold cache and 218 ms once the whole string had been
  // measured once. prewarmCache() was measured at 0 ms (it does not walk the
  // string) and a per-glyph width cache made the pass slower, not faster.
  renderer.getTextWidth(UI_10_FONT_ID, text.c_str());
  std::vector<readerai::Line> part;
  const auto measure = [this](const std::string& run) { return renderer.getTextWidth(UI_10_FONT_ID, run.c_str()); };
  if (!readerai::paginate(text, width, measure, part)) {
    LOG_ERR("AI", "paginate refused turn=%u bytes=%u", static_cast<unsigned>(turn), static_cast<unsigned>(text.size()));
    return;
  }
  for (auto& line : part) {
    if (lines_.size() >= keep) break;
    // The alignment travels with the line: one page mixes the two.
    line.question = question;
    lines_.push_back(line);
    lineTurns_.push_back(turn);
  }
}

// Re-bases the line arrays on the newest turns when the line cap was spent, so
// the page shows whole turns and never half of one.
void VoiceAssistantActivity::enforceLines(size_t keep) {
  const size_t total = lines_.size();
  if (total <= keep) return;
  size_t start = total, best = total;
  for (size_t turn = turns_.size(); turn-- > 1;) {
    while (start > 0 && lineTurns_[start - 1] == turn) --start;
    if (total - start > keep) break;
    best = start;
  }
  if (best == total) return;
  lines_.erase(lines_.begin(), lines_.begin() + best);
  lineTurns_.erase(lineTurns_.begin(), lineTurns_.begin() + best);
}

bool VoiceAssistantActivity::conversationLines(int width) {
  const uint32_t started = millis();
  bool ok = true;
  lines_.clear();
  lineTurns_.clear();
  // paginate() refuses to describe more than 1024 lines, so both arrays are
  // reserved for the cap the session model already spells out.
  lines_.reserve(readerai::kMaxTranscriptLines);
  lineTurns_.reserve(readerai::kMaxTranscriptLines);
  for (size_t turn = 0; turn < turns_.size(); ++turn) {
    // Logged per turn: a stall here has to say WHICH turn and part it stalled on,
    // because the answer that took 20 s to measure was always the newest one.
    LOG_INF("AI", "layout turn=%u/%u lines=%u q=%u a=%u", static_cast<unsigned>(turn + 1),
            static_cast<unsigned>(turns_.size()), static_cast<unsigned>(lines_.size()),
            static_cast<unsigned>(turns_[turn].question.size()), static_cast<unsigned>(turns_[turn].answer.size()));
    if (!readerai::validUtf8(turns_[turn].question) || !readerai::validUtf8(turns_[turn].answer)) {
      LOG_ERR("AI", "layout turn=%u rejected: invalid utf8", static_cast<unsigned>(turn + 1));
      ok = false;
      continue;
    }
    // The question holds a line back so the answer that follows it is reachable
    // on the same screen: a transcript turn whose question filled the page is a
    // reply nobody can scroll to.
    appendLines(turns_[turn].question, width, true, static_cast<uint32_t>(turn), readerai::kMaxTranscriptLines - 1);
    appendLines(turns_[turn].answer, width, false, static_cast<uint32_t>(turn), readerai::kMaxTranscriptLines);
  }
  if (turns_.size() > 1) enforceLines(readerai::kMaxTranscriptLines);
  LOG_INF("AI", "conversation: turns=%u lines=%u layout=%ums", static_cast<unsigned>(turns_.size()),
          static_cast<unsigned>(lines_.size()), static_cast<unsigned>(millis() - started));
  return ok;
}

void VoiceAssistantActivity::startNew() {
  // Per-question state only. rootId_ is the session identity and the caller
  // decides it: a menu entry clears it (new session, new file), a follow-up
  // keeps it so the answer appends to the same one.
  parentId_.clear();
  turns_.clear();
  lines_.clear();
  lineTurns_.clear();
  question_.clear();
  answer_.clear();
  sourceId_.clear();
  answerId_.clear();
  sourceKey_.clear();
  answerKey_.clear();
  error_.clear();
  page_ = 0;
  // Invalidate before the arrays above are refilled, or the next paint indexes
  // the empty turns_ through the previous offsets.
  pagesReady_ = false;
  thinking_.store(false, std::memory_order_release);
  seconds_.store(0, std::memory_order_release);
  lastSeconds_ = UINT32_MAX;
  if (!config_.voiceConsent) {
    state_.store(State::Consent, std::memory_order_release);
    requestUpdate();
    return;
  }
  state_.store(State::Recording, std::memory_order_release);
  launch(Work::Record);
}

uint32_t VoiceAssistantActivity::trimRecording(uint32_t bytes) {
  if (bytes <= 44) return 0;
  const uint32_t total = (bytes - 44) / 2;
  // Rounded up: the scan writes the window a sample belongs to, so a recording
  // that is not a whole number of windows needs the last, partial one. The old
  // truncated size left that window without an entry and the scan wrote two
  // bytes past the array -- straight into the next heap block's header, which
  // only surfaced later as `assert failed: multi_heap_free ... (head != NULL)`.
  const uint32_t count = readerai::windowCount(total);
  if (count == 0) return 0;
  auto peaks = makeUniqueNoThrow<uint16_t[]>(count);
  if (!peaks) {
    LOG_ERR("AI", "OOM: %u voice trim windows", static_cast<unsigned>(count));
    return 0;
  }
  const size_t blockBytes = readerai::kWindow * 2;
  auto buf = makeUniqueNoThrow<uint8_t[]>(blockBytes);
  if (!buf) {
    LOG_ERR("AI", "OOM: voice trim buffer");
    return 0;
  }
  bool failed = false;
  // Pass 1: stream the PCM once and keep one peak per 10 ms window. The
  // whole-file buffer this replaces would not fit the C3 heap.
  {
    HalFile src;
    if (!Storage.openFileForRead("storage", Wav, src) || !src.seek(44)) failed = true;
    for (uint32_t window = 0; !failed && window < count; ++window) {
      const uint32_t from = window * readerai::kWindow;
      const uint32_t samples = std::min(readerai::kWindow, total - from);
      const size_t want = static_cast<size_t>(samples) * 2;
      if (want == 0) break;
      if (src.read(buf.get(), want) != static_cast<int>(want)) {
        failed = true;
        break;
      }
      peaks[window] = readerai::windowPeak(buf.get(), want);
    }
  }
  if (failed) return 0;
  // The decision itself is pure and lives in VoiceTrim.h; this only streams.
  const readerai::Trim keep = readerai::trimRange(peaks.get(), count, total);
  if (keep.end <= keep.start) return 0;
  const uint32_t keptBytes = (keep.end - keep.start) * 2;
  // A rewrite costs a full copy of the file plus a second file on the card, so it
  // only pays when it really removes something.
  if (bytes <= 44 + keptBytes || bytes - (44 + keptBytes) < bytes / 100 * kTrimMinGainPercent) return 0;
  // Pass 2: write the kept range into the .part, header included, then publish.
  {
    HalFile src, dst;
    if (!Storage.openFileForRead("storage", Wav, src) || !Storage.openFileForWrite("storage", Part, dst) ||
        !src.seek(44 + static_cast<size_t>(keep.start) * 2)) {
      Storage.remove(Part);
      return 0;
    }
    wavHeader(buf.get(), keptBytes);
    if (dst.write(buf.get(), 44) != 44) failed = true;
    uint32_t remaining = keptBytes;
    while (!failed && remaining > 0) {
      const size_t want = std::min(blockBytes, static_cast<size_t>(remaining));
      if (src.read(buf.get(), want) != static_cast<int>(want) || dst.write(buf.get(), want) != want) {
        failed = true;
        break;
      }
      remaining -= static_cast<uint32_t>(want);
    }
    // Both files are closed by their destructors before the rename below: a
    // replace with the source still open would publish a partial file.
  }
  if (failed || !Storage.replaceFile(Part, Wav)) {
    Storage.remove(Part);
    return 0;
  }
  LOG_INF("AI", "voice trim: %u -> %u bytes", static_cast<unsigned>(bytes), static_cast<unsigned>(44 + keptBytes));
  return 44 + keptBytes;
}

bool VoiceAssistantActivity::poll(readerai::Client& client, const std::string& id, std::string& text) {
  // Only the answer wait of a deep task gets the wide bound: a deep answer is an
  // agent run, while every quick answer and every transcription has to finish
  // inside the original two minutes.
  const uint32_t limit = deep_ && work_ == Work::Question ? kDeepPollTimeoutMs : kPollTimeoutMs;
  // A new wait must not inherit the previous job's stage, or the page shows
  // "Searching" for a question that is only being transcribed.
  stage_.store(readerai::Stage::None, std::memory_order_release);
  elapsed_.store(0, std::memory_order_release);
  const uint32_t started = millis();
  while (true) {
    if (cancel_.load(std::memory_order_acquire)) return false;
    readerai::Status job;
    if (!client.status(id, job)) {
      fail(client.error().c_str());
      return false;
    }
    // Published for the wait page. Only the values cross the task boundary: the
    // UI task never reads a std::string the worker is assigning.
    stage_.store(job.stage, std::memory_order_release);
    elapsed_.store(job.elapsed, std::memory_order_release);
    if (job.state == "completed") break;
    if (job.state != "running" && job.state != "queued") {
      fail("protocol");
      return false;
    }
    if (millis() - started >= limit) {
      fail("timeout");
      return false;
    }
    delay(kPollIntervalMs);
  }
  text.clear();
  uint32_t cursor = 0;
  while (true) {
    if (cancel_.load(std::memory_order_acquire)) return false;
    readerai::Chunk chunk;
    if (!client.chunk(id, cursor, chunk)) {
      fail(client.error().c_str());
      return false;
    }
    text.append(chunk.text);
    if (text.size() > readerai::MaxTextBytes) {
      fail("protocol");
      return false;
    }
    cursor = chunk.next;
    if (chunk.done) break;
    if (millis() - started >= limit) {
      fail("timeout");
      return false;
    }
    delay(kPollIntervalMs);
  }
  // The stage belongs to the job, not to the text: leaving it set would keep the
  // wait copy on a page that already has its answer.
  stage_.store(readerai::Stage::None, std::memory_order_release);
  return true;
}

void VoiceAssistantActivity::openSettingsField(bool token) {
  const std::string initial = token ? config_.token : config_.endpoint;
  const size_t maxLength = token ? 256 : 128;
  if (!startActivityForResultWith<KeyboardEntryActivity>(
          [this, token](const ActivityResult& result) {
            requestUpdate();
            if (result.isCancelled) return;
            const std::string text = std::get<KeyboardResult>(result.data).text;
            if (text.empty()) return;
            if (token) {
              config_.token = text;
            } else {
              config_.endpoint = text;
            }
            if (!config_.save()) fail("config");
          },
          tr(STR_AI_SETTINGS), initial, maxLength, token ? InputType::Password : InputType::Url)) {
    fail("storage");
  }
}

void VoiceAssistantActivity::preparePages() {
  // Measure in the orientation render() draws in. Landscape would hand back the
  // 800-pixel width and every line would wrap where nothing is ever painted.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  const int width = std::max(1, pageContentRect().width - 32);
  pagesReady_ = conversationLines(width);
  const Rect body = pageContentRect();
  const size_t pages = static_cast<size_t>(pageCount(renderer, body, lines_.size()));
  // A session opens on its newest turn: the answer to the question just asked is
  // at the bottom, not the top.
  page_ = pages - 1;
  LOG_INF("AI", "conversation pages: turns=%u lines=%u page=%u/%u ok=%d", static_cast<unsigned>(turns_.size()),
          static_cast<unsigned>(lines_.size()), static_cast<unsigned>(page_ + 1), static_cast<unsigned>(pages),
          pagesReady_ ? 1 : 0);
}

void VoiceAssistantActivity::loop() {
  RenderLock uiLock(*this);
  if (!active_.load(std::memory_order_acquire) && workQueued_) {
    const Work next = queuedWork_;
    workQueued_ = false;
    launch(next);
  }
  const State state = state_.load(std::memory_order_acquire);
  {
    // Layout and the debug channel both run under the render lock: preparePages()
    // measures with the renderer, and startInjectedAsk() replaces the turn in
    // flight, which render() walks through lines_/lineTurns_.
    if (!pagesReady_) preparePages();
    // --- USB test hooks (see VoiceDebug.h): they consume nothing production
    // sets, and they run only while the worker is idle.
    if (readerai::debugAskRequest.pending.load(std::memory_order_acquire)) {
      // Cleared only on success: onEnter's Wi-Fi warm-up owns the worker for the
      // first seconds, and a dropped request would have to be typed again.
      if (startInjectedAsk(readerai::debugAskRequest.transcript, readerai::debugAskRequest.text)) {
        readerai::debugAskRequest.pending.store(false, std::memory_order_release);
      }
    }
    const uint32_t trimSamples = readerai::debugAskRequest.trimTestSamples.load(std::memory_order_acquire);
    if (trimSamples != 0 && !active_.load(std::memory_order_acquire)) {
      readerai::debugAskRequest.trimTestSamples.store(0, std::memory_order_release);
      const uint32_t kept = trimRecording(trimSamples * 2 + 44);
      LOG_INF("AI", "trim test samples=%u kept=%u", static_cast<unsigned>(trimSamples), static_cast<unsigned>(kept));
    }
    if (!readerai::debugAskRequest.pending.load(std::memory_order_acquire) &&
        !active_.load(std::memory_order_acquire) && !readerai::debugAskRequest.queue.empty()) {
      // One question per idle tick, inside the SAME activity instance: that is
      // the only way turns_ accumulates, because re-entering the app drops it.
      if (startInjectedAsk(readerai::debugAskRequest.transcript, readerai::debugAskRequest.queue.front())) {
        readerai::debugAskRequest.queue.erase(readerai::debugAskRequest.queue.begin());
      }
    }
  }
  if (active_.load(std::memory_order_acquire) && (work_.load(std::memory_order_acquire) != Work::Wifi || workQueued_)) {
    // While the worker waits on the server, the only repaint worth doing is the
    // one the server's progress asks for.
    const uint32_t elapsed = elapsed_.load(std::memory_order_acquire);
    const readerai::Stage stage = stage_.load(std::memory_order_acquire);
    const uint32_t seconds = seconds_.load(std::memory_order_acquire);
    if (lastElapsed_ != elapsed || lastStage_ != stage || lastSeconds_ != seconds) {
      lastElapsed_ = elapsed;
      lastStage_ = stage;
      lastSeconds_ = seconds;
      requestUpdate();
    }
    if (state == State::Recording) {
      // Confirm ends the capture; the worker publishes the recording.
      if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
        workQueued_ = false;
        cancel_.store(true, std::memory_order_release);
      } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        stop_.store(true, std::memory_order_release);
      }
      return;
    }
    // Back stops waiting without touching the saved task: the job keeps running
    // on the server and Resume picks it up from the pending record.
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      workQueued_ = false;
      cancel_.store(true, std::memory_order_release);
    }
    return;
  }
  if (wifiFailed_.load(std::memory_order_acquire)) {
    wifiFailed_.store(false, std::memory_order_release);
    // The saved credential did not come up: let the user pick a network instead
    // of leaving every question to fail on the radio.
    if (!startActivityForResultWith<WifiSelectionActivity>([this](const ActivityResult& result) {
          LOG_INF("AI", "network picker %s", result.isCancelled ? "cancelled" : "done");
          requestUpdate();
        })) {
      fail("network");
    }
    return;
  }
  if (confirmBarrier_) {
    // A page that acts on the RELEASE of Confirm (the session list) must not act
    // on the release of the press that opened it: that release is consumed here,
    // so one gesture never turns into two actions.
    if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
    confirmBarrier_ = false;
    return;
  }

  const Rect body = pageContentRect();
  const int pages = pageCount(renderer, body, lines_.size());
  // The two pages that keep a cursor: the home menu and the session list. The
  // settings rows are a fixed list as well, everything else has no selection.
  int count = state == State::Home ? 6 : state == State::History ? history_.size() : 0;
  if (state == State::Settings) count = 4;
  if (count && selected_ >= static_cast<size_t>(count)) {
    selected_ = 0;
    requestUpdate();
  }
  bool moved = false;
  if (count) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::NavPrevious)) {
      selected_ = (selected_ + count - 1) % count;
      moved = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
               mappedInput.wasPressed(MappedInputManager::Button::NavNext)) {
      selected_ = (selected_ + 1) % count;
      moved = true;
    }
  }
  if (moved) {
    requestUpdate(true);
    return;
  }

  if (state == State::Home) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selected_ == 0) {
        deep_ = false;
        rootId_.clear();
        startNew();
      } else if (selected_ == 1) {
        deep_ = true;
        rootId_.clear();
        startNew();
      } else if (selected_ == 2) {
        deep_ = false;
        loadPending();
      } else if (selected_ == 3) {
        loadHistory();
        // A row is opened by the release of Confirm; the press that opened the
        // list must not also open its first row.
        if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) confirmBarrier_ = true;
      } else if (selected_ == 4) {
        selected_ = 0;
        state_.store(State::Settings, std::memory_order_release);
        requestUpdate();
      } else if (!sourceId_.empty() || !answerId_.empty()) {
        state_.store(State::Busy, std::memory_order_release);
        launch(Work::Cancel);
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
  } else if (state == State::Consent) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      config_.voiceConsent = true;
      if (!config_.save()) {
        fail("config");
        return;
      }
      state_.store(State::Recording, std::memory_order_release);
      launch(Work::Record);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Recorded) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Upload for recognition. The pending record already holds the keys, so a
      // timeout here can be retried without uploading the audio twice.
      state_.store(State::Busy, std::memory_order_release);
      thinking_.store(true, std::memory_order_release);
      launch(Work::Transcribe);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Recording) {
    // Reached only when no worker is capturing (the active_ branch above handles
    // the normal case): Confirm retries the capture and Back leaves the page, so
    // a task that never started cannot strand the screen.
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      launch(Work::Record);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Busy) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      thinking_.store(false, std::memory_order_release);
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Transcript) {
    if (thinking_.load(std::memory_order_acquire)) {
      // The question is already with the server; this page is a wait page now.
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Review is on: the user has read the transcript, send it as the question.
      state_.store(State::Busy, std::memory_order_release);
      launch(Work::Question);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Answer) {
    if (mappedInput.wasPressed(MappedInputManager::Button::NavNext) && page_ + 1 < pages) {
      ++page_;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::NavPrevious) && page_ > 0) {
      --page_;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Follow up in the same session: the answer just read is the newest turn,
      // so it becomes the parent the next question links to and the reply appends
      // to the same file. rootId_ is deliberately NOT cleared.
      parentId_ = readerai::ConversationStore::lastJob(conversation());
      question_.clear();
      answer_.clear();
      sourceId_.clear();
      answerId_.clear();
      sourceKey_.clear();
      answerKey_.clear();
      error_.clear();
      deep_ = false;
      thinking_.store(false, std::memory_order_release);
      state_.store(State::Recording, std::memory_order_release);
      launch(Work::Record);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::History) {
    if (!history_.empty() && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, kLongPressMs)) {
      // One row is one session: deleting the row deletes every turn of it.
      const std::string file = history_[selected_].file;
      readerai::ConversationStore::remove(file);
      loadHistory();
      // The gesture that deleted the row must not also open the row that took its
      // place: while Confirm is still held down nothing else is accepted.
      if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!history_.empty() && !openConversation(history_[selected_])) fail("storage");
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Settings) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selected_ == 0) {
        openSettingsField(false);
      } else if (selected_ == 1) {
        openSettingsField(true);
      } else if (selected_ == 2) {
        config_.reviewTranscript = !config_.reviewTranscript;
        if (!config_.save()) fail("config");
        requestUpdate();
      } else {
        state_.store(State::Busy, std::memory_order_release);
        connectAndLaunch(Work::Health);
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      selected_ = 0;
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Connected) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      selected_ = 3;
      state_.store(State::Settings, std::memory_order_release);
      requestUpdate();
    }
  } else if (state == State::Error) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Retry resumes the saved task: the job ids and the keys are unchanged, so
      // the server sees the same request rather than a duplicate.
      if (!answerId_.empty() || !sourceId_.empty()) {
        const Work retryWork = answerId_.empty() ? Work::Transcribe : Work::Question;
        thinking_.store(!answerId_.empty(), std::memory_order_release);
        state_.store(State::Busy, std::memory_order_release);
        launch(retryWork);
      } else {
        state_.store(State::Home, std::memory_order_release);
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state_.store(State::Home, std::memory_order_release);
      requestUpdate();
    }
  }
}

void VoiceAssistantActivity::render(RenderLock&&) {
  const State state = state_.load(std::memory_order_acquire);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  drawPageHeader(Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_AI_TITLE));
  const Rect body = pageContentRect();
  const uint32_t elapsed = elapsed_.load(std::memory_order_acquire);
  // The stage copy replaces the wait copy only on the question wait page: a
  // transcription has its own line and must not show the agent's stages.
  const bool questionWait = state == State::Busy && work_ == Work::Question;

  if (state == State::Home) {
    const char* rows[] = {tr(STR_AI_NEW),     tr(STR_AI_DEEP_TASK), tr(STR_AI_RESUME),
                          tr(STR_AI_HISTORY), tr(STR_AI_SETTINGS),  tr(STR_AI_CANCEL_JOB)};
    GUI.drawList(
        renderer, body, 6, selected_, [&rows](int index) { return std::string(rows[index]); }, nullptr, nullptr,
        nullptr, false, nullptr, showMainTabContentSelection());
  } else if (state == State::Settings) {
    auto rowTitle = [this](int index) -> std::string {
      if (index == 0) return std::string(tr(STR_AI_ENDPOINT));
      if (index == 1) return std::string(tr(STR_AI_TOKEN));
      if (index == 2) return std::string(config_.reviewTranscript ? tr(STR_AI_REVIEW_ON) : tr(STR_AI_REVIEW_OFF));
      return std::string(tr(STR_AI_TEST));
    };
    auto rowValue = [this](int index) -> std::string {
      if (index == 0) return config_.endpoint;
      if (index == 1) return config_.token.empty() ? std::string() : std::string("******");
      return std::string();
    };
    GUI.drawList(renderer, body, 4, selected_, rowTitle, nullptr, nullptr, rowValue);
    drawParagraph(renderer, Rect{body.x, body.y + body.height * 2 / 3, body.width, body.height / 3},
                  tr(STR_AI_SETUP_HINT), SMALL_FONT_ID);
  } else if (state == State::History) {
    if (history_.empty()) {
      drawParagraph(renderer, body, tr(STR_AI_NO_HISTORY), UI_10_FONT_ID);
    } else {
      auto rowTitle = [this](int i) -> std::string { return history_[i].title; };
      auto rowSubtitle = [this](int i) -> std::string { return historySubtitles_[i]; };
      GUI.drawList(renderer, body, static_cast<int>(history_.size()), selected_, rowTitle, rowSubtitle);
    }
  } else if (state == State::Transcript || state == State::Answer) {
    const size_t perPage = static_cast<size_t>(linesPerPage(renderer, body));
    const size_t pages = std::max<size_t>(1, (lines_.size() + perPage - 1) / perPage);
    if (page_ >= pages) page_ = pages - 1;
    const size_t first = page_ * perPage;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    // On the transcript page the turn in flight is the question that was just
    // asked, so it is drawn on the question side; on the answer page every line
    // keeps the side it was laid out with, and one page mixes both.
    const bool alignRight = state == State::Transcript;
    int y = body.y;
    if (!pagesReady_) {
      // The worker invalidated the layout before touching turns_ (see run()):
      // walking lines_ now would index a turns_ that may already be reallocated.
      renderer.drawText(UI_10_FONT_ID, body.x, y, tr(STR_AI_THINKING), true);
      y += lineHeight;
    } else {
      for (size_t i = first; i < lines_.size() && i < first + perPage; ++i) {
        // Defence in depth: the arrays are written by a different task than the
        // one drawing them, and an index that outlived its turn must skip a line
        // rather than read past the end of turns_.
        if (i >= lineTurns_.size() || lineTurns_[i] >= turns_.size()) continue;
        const readerai::Turn& turn = turns_[lineTurns_[i]];
        const readerai::Line& line = lines_[i];
        const auto& text = line.question ? turn.question : turn.answer;
        if (line.end <= line.start || line.end > text.size()) continue;
        const bool liveTurn = state == State::Transcript && lineTurns_[i] + 1 == turns_.size();
        const bool onRight = liveTurn ? alignRight : line.question;
        const std::string shown = text.substr(line.start, line.end - line.start);
        renderer.drawText(UI_10_FONT_ID,
                          onRight ? body.x + body.width - renderer.getTextWidth(UI_10_FONT_ID, shown.c_str()) : body.x,
                          y, shown.c_str(), true);
        y += lineHeight;
      }
    }
    if (thinking_.load(std::memory_order_acquire)) {
      // The question is with the server: this page is the wait page now, and it
      // shows the stage the worker published exactly like the busy page does.
      const char* staged = stageText(stage_.load(std::memory_order_acquire));
      renderer.drawText(UI_10_FONT_ID, body.x, y, staged ? staged : tr(STR_AI_THINKING), true);
      if (staged && elapsed) {
        char text[kStatusTextBytes];
        snprintf(text, sizeof(text), tr(STR_AI_ELAPSED_FMT), static_cast<unsigned>(elapsed));
        stamp_ = text;
        const char* stamp = stamp_.c_str();
        renderer.drawText(SMALL_FONT_ID, body.x + body.width - renderer.getTextWidth(SMALL_FONT_ID, stamp),
                          y + lineHeight + renderer.getLineHeight(SMALL_FONT_ID), stamp, true);
      }
    } else if (state == State::Transcript) {
      renderer.drawText(UI_10_FONT_ID, body.x, y, tr(STR_AI_SEND), true);
    }
    if (pages > 1) {
      char stamp[32];
      snprintf(stamp, sizeof(stamp), tr(STR_WEREAD_PAGE_FMT), static_cast<unsigned>(page_ + 1),
               static_cast<unsigned>(pages));
      renderer.drawText(SMALL_FONT_ID, body.x + body.width - renderer.getTextWidth(SMALL_FONT_ID, stamp),
                        body.y + body.height - renderer.getLineHeight(SMALL_FONT_ID), stamp, true);
    }
  } else if (state == State::Recorded) {
    drawParagraph(renderer, body, tr(STR_AI_RECORDED), UI_10_FONT_ID);
  } else if (state == State::Recording) {
    const uint32_t seconds = seconds_.load(std::memory_order_acquire);
    char counter[24];
    snprintf(counter, sizeof(counter), "%u", static_cast<unsigned>(seconds));
    renderer.drawCenteredText(UI_10_FONT_ID, body.y + body.height / 4, tr(STR_AUDIO_RECORDING), true);
    renderer.drawCenteredText(UI_10_FONT_ID, body.y + body.height / 4 + renderer.getLineHeight(UI_10_FONT_ID) * 2,
                              counter, true);
  } else if (state == State::Busy) {
    const char* message = work_ == Work::Transcribe ? tr(STR_AI_TRANSCRIBING) : tr(STR_AI_WORKING);
    if (questionWait) {
      if (const char* staged = stageText(stage_.load(std::memory_order_acquire))) message = staged;
    }
    drawParagraph(renderer, body, message, UI_10_FONT_ID);
    if (elapsed) {
      char stamp[32];
      snprintf(stamp, sizeof(stamp), tr(STR_AI_ELAPSED_FMT), static_cast<unsigned>(elapsed));
      renderer.drawCenteredText(SMALL_FONT_ID, body.y + body.height - renderer.getLineHeight(SMALL_FONT_ID) * 2, stamp,
                                true);
    }
  } else if (state == State::Consent) {
    drawParagraph(renderer, body, tr(STR_AI_CONSENT), UI_10_FONT_ID);
  } else if (state == State::Connected) {
    drawParagraph(renderer, body, tr(STR_AI_CONNECTED), UI_10_FONT_ID);
  } else if (state == State::Error) {
    drawParagraph(renderer, body, errorText(error_), UI_10_FONT_ID);
  }

  const char* primary = tr(STR_SELECT);
  if (state == State::Recording) {
    primary = tr(STR_AUDIO_STOP);
  } else if (state == State::Transcript) {
    primary = thinking_.load(std::memory_order_acquire) ? "" : tr(STR_AI_SEND);
  } else if (state == State::Answer) {
    primary = tr(STR_AI_FOLLOWUP);
  } else if (state == State::Error) {
    primary = tr(STR_AI_RETRY);
  } else if (state == State::Home || state == State::History || state == State::Settings) {
    primary = tr(STR_SELECT);
  }
  const auto labels = mainTabButtonLabels(tr(STR_BACK), primary, true);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  LOG_DBG("AI", "rendered state=%u page=%u", static_cast<unsigned>(state), static_cast<unsigned>(page_));
}
