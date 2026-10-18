#!/usr/bin/env python3
"""Execute production activity methods with host UI/task collaborators.

This complements protocol/storage tests: a successful injected question does
not prove the panel was refreshed, input was usable, or destruction was safe.
The bodies below come from the firmware source, not a second implementation.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/activities/apps/voice-assistant/VoiceAssistantActivity.cpp').read_text()
header = (root / 'src/activities/apps/voice-assistant/VoiceAssistantActivity.h').read_text()


def method(signature):
    start = source.index(signature)
    # Top-level method closing braces are unindented in this source.
    end = source.index('\n}', start) + 2
    return source[start:end]


methods = '\n'.join(method(signature) for signature in (
    'void VoiceAssistantActivity::onEnter()',
    'void VoiceAssistantActivity::onExit()',
    'void VoiceAssistantActivity::task(',
    'void VoiceAssistantActivity::launch(',
    'void VoiceAssistantActivity::fail(',
    'void VoiceAssistantActivity::loop()',
    'void VoiceAssistantActivity::render(',
    'void VoiceAssistantActivity::enforceLines(',
))
declaration = header[header.index('class VoiceAssistantActivity'):].replace(' private:', ' public:')
ids = sorted(set(re.findall(r'\bSTR_\w+', methods)))

support = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>
using TaskHandle_t = void*;
thread_local TaskHandle_t taskId = reinterpret_cast<void*>(1);
TaskHandle_t xTaskGetCurrentTaskHandle() { return taskId; }
void delay(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
unsigned uxTaskGetStackHighWaterMark(void*) { return 8192; }
void vTaskDelete(void*) {}
constexpr int pdPASS = 1, WL_CONNECTED = 1, WIFI_STA = 1;
int xTaskCreate(void(*)(void*), const char*, unsigned, void*, int, void*) { return pdPASS; }
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
struct Rect { int x, y, width, height; };
struct GfxRenderer {
  enum class Orientation { Portrait };
  int clears = 0, refreshes = 0;
  void setOrientation(Orientation) {}
  void clearScreen() { ++clears; }
  void displayBuffer(int) { ++refreshes; }
  int getScreenWidth() const { return 480; }
  int getLineHeight(int) const { return 20; }
  int getTextWidth(int, const char* s) const { return std::string(s).size(); }
  void drawText(int, int, int, const char*, bool) {}
  void drawCenteredText(int, int, const char*, bool) {}
};
struct HalDisplay { static constexpr int FAST_REFRESH = 0; };
struct HalAudioOutput { static void shutdown() {} };
struct Wifi { int status() { return 0; } void disconnect(bool) {} } WiFi;
struct Metrics { int topPadding = 5, headerHeight = 20; };
struct Theme {
  Metrics metrics;
  const Metrics& getMetrics() const { return metrics; }
  template<class... T> void drawList(T&&...) {}
  template<class... T> void drawButtonHints(T&&...) {}
};
Theme GUI;
struct UITheme { static Theme& getInstance() { return GUI; } };
struct MappedInputManager {
  enum class Button { Confirm, Back, Up, Down, NavPrevious, NavNext };
  int pressed = -1;
  bool wasPressed(Button b) { return pressed == static_cast<int>(b); }
  bool wasReleased(Button) { return false; }
  bool wasLongPressed(Button, unsigned long) { return false; }
  struct Labels { const char *btn1, *btn2, *btn3, *btn4; };
  Labels mapLabels(const char* a, const char* b, const char* c, const char* d) { return {a,b,c,d}; }
};
struct KeyboardResult { std::string text; };
struct ActivityResult { bool isCancelled = false; std::variant<KeyboardResult> data; };
struct WifiSelectionActivity {};
enum class MainTab { None, VoiceAssistant };
enum class MainTabContentEdge { First, Last };
namespace MainTabs {
int contentEdgeIndex(MainTabContentEdge edge, int count) { return edge == MainTabContentEdge::First ? 0 : count-1; }
}
struct Activity {
  GfxRenderer& renderer; MappedInputManager& mappedInput; bool finished = false;
  Activity(const char*, GfxRenderer& r, MappedInputManager& i): renderer(r), mappedInput(i) {}
  virtual void onEnter() {} virtual void onExit() {} virtual void loop() {}
  virtual bool ownsAudio() const { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual void render(class RenderLock&&) {}
  virtual MainTab mainTab() const { return MainTab::None; }
  virtual void selectMainTabContentEdge(MainTabContentEdge) {}
  bool showMainTabContentSelection() const { return true; }
  MappedInputManager::Labels mainTabButtonLabels(const char* back, const char* confirm, bool) const {
    return mappedInput.mapLabels(back,confirm,"up","down");
  }
  Rect pageContentRect() const { return {10,30,460,700}; }
  void drawPageHeader(Rect, const char*) {} void requestUpdate(bool=false) {}
  void finish() { finished = true; }
  template<class T, class F> bool startActivityForResultWith(F) { return true; }
};
std::mutex renderMutex;
struct RenderLock {
  enum class Mode { Blocking, Try };
  std::unique_lock<std::mutex> lock;
  explicit RenderLock(Mode mode=Mode::Blocking): lock(renderMutex,std::defer_lock) {
    if (mode == Mode::Try) lock.try_lock(); else lock.lock();
  }
  explicit RenderLock(Activity&): RenderLock() {}
  bool ownsLock() const { return lock.owns_lock(); }
};
namespace NetworkStartup { bool setMode(GfxRenderer&, int) { return true; } }
namespace readerai {
enum class Stage { None, Searching, Thinking, Agent, Writing };
struct Config {
  bool voiceConsent = false, reviewTranscript = false; std::string token, endpoint;
  bool load() { return true; } bool save() { return true; }
};
struct Turn { std::string job, question, answer; };
struct Conversation { std::string root; unsigned updated; std::vector<Turn> turns; };
struct HistoryEntry { std::string file, title; };
struct Line { size_t start = 0, end = 0; bool question = false; };
struct Client {};
struct ConversationStore {
  static void remove(const std::string&) {}
  static std::string lastJob(const Conversation&) { return ""; }
};
struct Debug {
  std::atomic<bool> pending{false}; std::atomic<unsigned> trimTestSamples{0};
  std::string transcript, text; std::vector<std::string> queue;
} debugAskRequest;
void closeSession() {}
}
constexpr int UI_10_FONT_ID = 1, SMALL_FONT_ID = 2;
constexpr unsigned kTaskStackBytes = 16384, kLongPressMs = 700, kStatusTextBytes = 48;
const char* tr(int) { return "text"; }
const char* stageText(readerai::Stage) { return nullptr; }
const char* errorText(const std::string&) { return "error"; }
int linesPerPage(const GfxRenderer&, const Rect&) { return 20; }
int pageCount(const GfxRenderer&, const Rect&, size_t lines) { return std::max<size_t>(1,(lines+19)/20); }
void drawParagraph(GfxRenderer&, const Rect&, const char*, int) {}
'''

helpers = r'''
std::function<void(VoiceAssistantActivity&)> action;
void VoiceAssistantActivity::run() { if (action) action(*this); }
bool VoiceAssistantActivity::startInjectedAsk(const std::string&,const std::string&) { return false; }
void VoiceAssistantActivity::preparePages() { pagesReady_ = true; }
unsigned VoiceAssistantActivity::trimRecording(unsigned) { return 0; }
readerai::Conversation VoiceAssistantActivity::conversation() const { return {}; }
void VoiceAssistantActivity::startNew() { state_ = State::Recording; launch(Work::Record); }
void VoiceAssistantActivity::loadHistory() { state_ = State::History; }
bool VoiceAssistantActivity::loadPending() { return false; }
bool VoiceAssistantActivity::openConversation(const readerai::HistoryEntry&) { return true; }
void VoiceAssistantActivity::openSettingsField(bool) {}
void VoiceAssistantActivity::connectAndLaunch(Work w) { launch(w); }

void finishWorker(VoiceAssistantActivity& a) {
  taskId = reinterpret_cast<void*>(2);
  VoiceAssistantActivity::task(&a);
  taskId = reinterpret_cast<void*>(1);
}
int main() {
  using S = VoiceAssistantActivity::State;
  using W = VoiceAssistantActivity::Work;
  GfxRenderer r; MappedInputManager input;
  VoiceAssistantActivity a(r,input);
  a.onEnter();
  assert(a.mainTab() == MainTab::VoiceAssistant);
  a.selectMainTabContentEdge(MainTabContentEdge::Last); assert(a.selected_ == 5);
  a.selectMainTabContentEdge(MainTabContentEdge::First); assert(a.selected_ == 0);
  assert(a.active_ && a.work_ == W::Wifi);
  // A radio warm-up must not swallow menu navigation or Back.
  input.pressed = static_cast<int>(MappedInputManager::Button::Down);
  a.loop(); assert(a.selected_ == 1);
  input.pressed = static_cast<int>(MappedInputManager::Button::Back);
  a.loop(); assert(a.finished);
  input.pressed = -1;
  finishWorker(a);
  // Every visible state goes through clear + panel refresh.
  for (S s: {S::Home,S::Settings,S::History,S::Answer,S::Transcript,S::Recording,S::Busy,S::Error}) {
    a.state_ = s; int clears=r.clears, refreshes=r.refreshes;
    assert(a.mainTab() == (s == S::Home ? MainTab::VoiceAssistant : MainTab::None));
    { RenderLock lock; a.render(std::move(lock)); }
    assert(r.clears == clears+1 && r.refreshes == refreshes+1);
  }
  // Asking while warm-up runs queues the operation without changing the worker.
  a.launch(W::Wifi); a.launch(W::Record);
  assert(a.work_ == W::Wifi && a.workQueued_);
  finishWorker(a); a.state_ = S::Recording; a.loop();
  assert(a.active_ && a.work_ == W::Record && !a.workQueued_);
  input.pressed = static_cast<int>(MappedInputManager::Button::Confirm);
  a.loop(); assert(a.stop_);
  input.pressed = static_cast<int>(MappedInputManager::Button::Back);
  a.loop(); assert(a.cancel_);
  finishWorker(a); assert(a.state_ == S::Home && !a.thinking_);
  // Retrying a saved job must also queue behind Wi-Fi without replacing it.
  a.launch(W::Wifi); a.state_ = S::Error; a.answerId_ = "saved-job";
  input.pressed = static_cast<int>(MappedInputManager::Button::Confirm);
  a.loop(); assert(a.work_ == W::Wifi && a.workQueued_ && a.queuedWork_ == W::Question);
  finishWorker(a); a.loop(); assert(a.active_ && a.work_ == W::Question);
  finishWorker(a); a.answerId_.clear();
  // Cancellation during a network wait also leaves a usable menu.
  input.pressed = static_cast<int>(MappedInputManager::Button::Back);
  a.launch(W::Question); a.state_ = S::Busy;
  a.loop(); assert(a.cancel_); finishWorker(a); assert(a.state_ == S::Home);
  // Worker publication cannot change rendered vectors while the UI holds them.
  a.cancel_ = false; std::atomic<bool> entered{false}, published{false};
  std::thread worker;
  {
    RenderLock lock;
    worker = std::thread([&] {
      entered = true;
      published = a.publishUi([&] { a.turns_.push_back({"job","question","answer"}); });
    });
    while (!entered) std::this_thread::yield();
    delay(30); assert(!published && a.turns_.empty());
  }
  worker.join(); assert(published && a.turns_.size() == 1);
  // A short conversation must retain its first turn, including at the cap.
  a.turns_.resize(3); a.lines_.resize(6); a.lineTurns_ = {0,0,1,1,2,2};
  a.enforceLines(1024); assert(a.lines_.size() == 6 && a.lineTurns_.front() == 0);
  a.enforceLines(6); assert(a.lines_.size() == 6 && a.lineTurns_.front() == 0);
  a.enforceLines(4); assert(a.lines_.size() == 4 && a.lineTurns_.front() == 1);
  // onExit is called WITH RenderLock. A worker waiting to publish must cancel,
  // not deadlock, and a >5s worker must not outlive its activity as it used to.
  a.cancel_ = false; a.active_ = true;
  action = [&](VoiceAssistantActivity& self) {
    delay(5100);
    assert(!self.publishUi([&] { assert(false); }));
  };
  worker = std::thread([&] { finishWorker(a); });
  auto start = std::chrono::steady_clock::now();
  { RenderLock lock; a.onExit(); }
  auto elapsed = std::chrono::steady_clock::now()-start;
  assert(elapsed >= std::chrono::milliseconds(5000));
  assert(!a.active_ && a.workerTask_ == nullptr);
  worker.join();
  puts("PASS: real activity rendering, warm-up input, queued work, recording cancel, publication and safe exit");
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'activity.cpp'
    cpp.write_text(support + '\nenum { ' + ','.join(ids) + ' };\n' + declaration + '\n' + methods + '\n' + helpers)
    binary = Path(directory) / 'activity'
    subprocess.run(['c++', '-std=c++17', '-pthread', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=15)
