#pragma once

// Named-pipe command server for the remesh debug app. An accept thread reads
// one command line per client connection and hands it to the MAIN thread (via a
// queue drained each frame), waits for the reply, writes it back framed with a
// "\n.\n" terminator line, and disconnects. The command handler runs on the
// main thread, so it can freely touch Scene / the app state; the accept thread
// never does. This is the agent-facing control channel (see tools/remesh_dbg.mjs).

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace sculptcore::debug_app {

class PipeServer {
public:
  using Handler = std::function<std::string(const std::string &line)>;

  PipeServer() = default;
  ~PipeServer();
  PipeServer(const PipeServer &) = delete;
  PipeServer &operator=(const PipeServer &) = delete;

  /* Start the accept thread on `pipeName` (e.g.
   * L"\\\\.\\pipe\\sculpt-remesh-debug"). `handler` is invoked on the main
   * thread for each command line and returns the reply body (the "\n.\n"
   * terminator is appended by the server). */
  bool start(const std::wstring &pipeName, Handler handler);
  void stop();

  /* Execute all queued commands on the calling (main) thread and fulfill each
   * reply promise so the accept thread can respond. Call once per frame. */
  void drainMainThreadQueue();

  bool running() const
  {
    return running_.load();
  }

private:
  struct Command {
    std::string line;
    std::promise<std::string> reply;
  };
  void acceptLoop();

  std::wstring pipeName_;
  Handler handler_;
  std::thread accept_;
  std::mutex mtx_;
  std::deque<Command> queue_;
  std::atomic<bool> running_{false};
};

} // namespace sculptcore::debug_app
