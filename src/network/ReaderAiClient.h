#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace readerai {
constexpr size_t MaxReplyBytes = 16384;
constexpr size_t MaxTextBytes = 24000;
bool validEndpoint(const std::string& endpoint);
bool validJobId(const std::string& id);
bool validUtf8(const std::string& text);
struct Request {
  std::string method, path, body, file, key;
};
struct Reply {
  int status = 0;
  std::string body;
  bool complete = false;
};
struct Chunk {
  std::string text;
  uint32_t next = 0;
  bool done = false;
};
// Answer path. Quick omits the field entirely, so a question payload is
// byte-identical to the client that shipped before deep tasks existed and the
// server keeps its pre-deep behaviour; Deep asks for the agent path.
enum class Mode : uint8_t { Quick, Deep };
// Optional progress of a job (contract v1 §2.2). A server that reports no stage,
// or a stage this build does not know, degrades to None instead of failing.
enum class Stage : uint8_t { None, Queued, Searching, Thinking, Agent, Writing };
struct Status {
  std::string state;
  Stage stage = Stage::None;
  uint32_t elapsed = 0;  // Whole seconds the server has been working on the job.
};
// A replaceable transport permits contract testing without network/API charges.
class Client {
 public:
  using Transport = std::function<Reply(const Request&)>;
  explicit Client(Transport transport) : transport_(std::move(transport)) {}
  bool health(bool& asr, bool& qa);
  bool transcribe(const std::string& file, const std::string& key, std::string& id);
  bool question(const std::string& source, const std::string& text, const std::string& key, std::string& id,
                const std::string& parent = "", Mode mode = Mode::Quick);
  bool state(const std::string& id, std::string& state);
  bool status(const std::string& id, Status& status);
  bool chunk(const std::string& id, uint32_t cursor, Chunk& out);
  bool cancel(const std::string& id);
  const std::string& error() const { return error_; }

 private:
  Transport transport_;
  std::string error_;
  bool request(const Request& request, Reply& reply);
};
}  // namespace readerai
