#pragma once
#include <atomic>

#include "ReaderAiClient.h"
namespace readerai {
struct Config {
  std::string endpoint = "https://www.dogzong.com/reader-ai/v1";
  std::string token;
  bool voiceConsent = false, reviewTranscript = false;
  bool load();
  bool save() const;
};
Reply deviceRequest(const Config& config, const Request& request, const std::atomic<bool>& cancel);
// Drops the kept-alive TLS session. Call when a question cycle is over: the
// session holds the wolfSSL context and sockets in the heap for as long as it
// stays open.
void closeSession();
}  // namespace readerai
