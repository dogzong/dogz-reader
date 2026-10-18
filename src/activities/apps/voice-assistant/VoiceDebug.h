#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace readerai {

// USB-only test hook. The voice assistant normally needs a spoken question, which
// makes the real worker/render path impossible to exercise from a serial console.
// `CMD:AI_ASK <transcription_job_id> <question>` parks a request here, and the
// activity picks it up on its next loop() tick and runs the ordinary question
// path: same worker task, same turn append, same layout, same rendering.
//
// This exists only to reproduce and verify the multi-turn crash; it changes no
// production behaviour, because nothing sets `pending` outside the serial
// command, and a request is consumed once and cleared.
struct DebugAsk {
  // Atomic so the read inside the activity's loop() cannot be hoisted out of
  // the surrounding main loop: loop() runs in a tight while(1) that never
  // writes this, so a plain bool let the compiler cache it in a register.
  std::atomic<bool> pending{false};
  std::string transcript;
  std::string text;
  // Remaining questions of a multi-round run. They are consumed one per idle tick
  // inside the SAME activity instance, which is the only way to accumulate turns:
  // re-entering the app replaces the activity and drops the session.
  std::vector<std::string> queue;
  // Non-zero asks the activity to run trimRecording() on the wav already on the
  // card and report what it kept. Exercises the scan that overran its peak array
  // without needing a microphone.
  std::atomic<uint32_t> trimTestSamples{0};
};

// One definition, in VoiceAssistantActivity.cpp. A function-local static in an
// inline accessor is not reliable here: this header is included by both main.cpp
// and the activity, and a build that does not merge the two copies would leave
// the writer and the reader looking at different objects.
extern DebugAsk debugAskRequest;

}  // namespace readerai
