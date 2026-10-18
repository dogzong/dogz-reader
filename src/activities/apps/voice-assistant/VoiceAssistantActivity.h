#pragma once
#include <atomic>

#include "VoiceConversation.h"
#include "VoiceHistory.h"
#include "VoiceText.h"
#include "activities/Activity.h"
#include "network/ReaderAiDevice.h"

class VoiceAssistantActivity final : public Activity {
 public:
  VoiceAssistantActivity(GfxRenderer& renderer, MappedInputManager& input)
      : Activity("VoiceAssistant", renderer, input) {}
  void onEnter() override;
  MainTab mainTab() const override {
    return state_.load(std::memory_order_acquire) == State::Home ? MainTab::VoiceAssistant : MainTab::None;
  }
  void selectMainTabContentEdge(MainTabContentEdge edge) override {
    RenderLock lock(*this);
    selected_ = MainTabs::contentEdgeIndex(edge, 6);
  }
  bool ownsAudio() const override { return true; }
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return active_.load(std::memory_order_acquire); }

 private:
  enum class State : uint8_t {
    Home,
    Consent,
    Recorded,
    Recording,
    Busy,
    Transcript,
    Answer,
    History,
    Settings,
    Connected,
    Error
  };
  enum class Work : uint8_t { Record, Transcribe, Question, Cancel, Health, Wifi, Ask };
  static void task(void* context);
  void run();
  // Worker publications use the render mutex, but must yield to cancellation:
  // ActivityManager holds that mutex while waiting for onExit().
  template <typename F>
  bool publishUi(F&& update) {
    while (!cancel_.load(std::memory_order_acquire)) {
      RenderLock lock(RenderLock::Mode::Try);
      if (lock.ownsLock()) {
        if (cancel_.load(std::memory_order_acquire)) return false;
        update();
        return true;
      }
      delay(10);
    }
    return false;
  }
  // USB test hook: runs one round from an injected transcript instead of a
  // recording, so the real worker, turn append and layout can be exercised
  // without a microphone. See VoiceDebug.h.
  bool startInjectedAsk(const std::string& transcriptId, const std::string& text);
  void launch(Work work);
  void connectAndLaunch(Work work);
  bool connectSavedWifi(uint32_t timeoutMs);
  void openSettingsField(bool token);
  void startNew();
  void preparePages();
  // Rewrites Wav without its leading/trailing silence. Returns 0 (and leaves Wav
  // untouched) when trimming would not clearly pay off.
  uint32_t trimRecording(uint32_t bytes);
  bool savePending();
  bool loadPending();
  // The live session as the store sees it (rootId_ + turns_). Read-only: the
  // caps are applied to a copy so the screen never loses a turn before the file
  // has actually accepted it.
  readerai::Conversation conversation() const;
  // Publishes the current conversation under its root job id: one file per
  // session, appended in place. Only ever called from the worker task.
  bool saveConversation();
  // Loads the session list and moves to the history page.
  void loadHistory();
  // Seeds turns_/rootId_ from a listed session; the caller sets parentId_ from
  // its last turn so a follow-up continues the server-side chain.
  bool openConversation(const readerai::HistoryEntry& entry);
  // Builds the accumulated transcript: every previous turn plus the turn in
  // progress, each line branded with the alignment it is drawn with.
  bool conversationLines(int width);
  // Appends one field of one turn to lines_/lineTurns_, stopping at `keep` lines.
  void appendLines(const std::string& text, int width, bool question, uint32_t turn, size_t keep);
  // Re-bases the line arrays on the newest turns when the cap was spent.
  void enforceLines(size_t keep);
  // Polls until the job finishes, using the deep-task bound only for the answer
  // wait of a deep task (see the definition).
  bool poll(readerai::Client& client, const std::string& id, std::string& text);
  void fail(const char* code);
  readerai::Config config_;
  std::atomic<State> state_{State::Home};
  std::atomic<bool> active_{false}, cancel_{false}, stop_{false};
  // Set when the saved-credential connect fails, so loop() can fall back to the
  // network picker without blocking the UI on the worker.
  std::atomic<bool> wifiFailed_{false};
  // True while the question is with the server: the transcript page then shows
  // "深度求索中…" instead of offering to send it again.
  std::atomic<bool> thinking_{false};
  std::atomic<uint32_t> seconds_{0};
  // Stage and elapsed seconds of the job being polled, published for the wait
  // page. An enum instead of the stage text keeps the UI task from ever reading
  // a std::string while the worker is assigning it.
  std::atomic<readerai::Stage> stage_{readerai::Stage::None};
  std::atomic<uint32_t> elapsed_{0};
  std::atomic<Work> work_{Work::Record};
  std::atomic<TaskHandle_t> workerTask_{nullptr};
  // UI-owned: a requested operation waits for Wi-Fi warm-up to finish rather
  // than changing the operation a running worker is already executing.
  Work queuedWork_ = Work::Record;
  bool workQueued_ = false;
  // Deep task selected from the menu; drives the submitted mode and the poll
  // bound, and is saved with the pending record so a resume keeps both.
  bool deep_ = false;
  std::string question_, answer_, sourceId_, answerId_, sourceKey_, answerKey_, error_, parentId_;
  // The conversation on screen. rootId_ is the session file name (the first
  // turn's question job id); parentId_ is the previous turn's job id, i.e. the
  // parent_job_id the next question is sent with — for a session opened from the
  // history list that is exactly its last turn, which is what lets the server
  // walk the chain back to the session root.
  std::string rootId_;
  std::vector<readerai::Turn> turns_;
  std::vector<readerai::HistoryEntry> history_;
  std::vector<std::string> historySubtitles_;
  std::vector<readerai::Line> lines_;
  // Turn behind each entry of lines_ (parallel array, filled by the same pass).
  std::vector<uint32_t> lineTurns_;
  // Composed in render() through tr(STR_AI_TURNS_FMT): the list row callback
  // receives a c_str(), so the text has to outlive the draw call.
  std::string stamp_;
  size_t selected_ = 0, page_ = 0;
  uint32_t lastSeconds_ = UINT32_MAX, lastElapsed_ = UINT32_MAX;
  // Last progress painted, so the wait page refreshes on change only.
  readerai::Stage lastStage_ = readerai::Stage::None;
  bool pagesReady_ = false, wifiOwned_ = false, confirmBarrier_ = false;
};
