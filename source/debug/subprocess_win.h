#pragma once

// Minimal Windows subprocess with line-buffered stdout capture. The remesh
// debug app uses it to run `remesh_cli` and `node meshy_gen.mjs` out-of-process
// (so the remesher can be rebuilt without restarting the app) and stream their
// PROGRESS/RESULT/... protocol lines back. A background reader thread only
// enqueues lines; the main thread drains them, so all Scene/UI mutation stays
// single-threaded.

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sculptcore::debug_app {

class Subprocess {
public:
  Subprocess() = default;
  ~Subprocess();
  Subprocess(const Subprocess &) = delete;
  Subprocess &operator=(const Subprocess &) = delete;

  /* Build a quoted command line from argv[0]=exe + args and launch it. Each
   * element is wrapped per the Windows CommandLineToArgvW rules. Returns false
   * (and running() stays false) if the child could not be created. */
  bool start(const std::wstring &exe, const std::vector<std::wstring> &args);

  /* True from a successful start() until the reader thread observes child exit. */
  bool running() const { return running_.load(); }

  /* Pop every newline-terminated stdout/stderr line captured since last call
   * (the trailing newline is stripped; \r is trimmed). */
  void drain(std::vector<std::string> &out);

  /* Valid once running() is false; -1 beforehand. */
  int exitCode() const { return exitCode_.load(); }

  /* TerminateProcess + join the reader. Safe to call on the main thread. */
  void kill();

private:
  void readerLoop();
  void joinReader();

  void *proc_ = nullptr;     // HANDLE
  void *readPipe_ = nullptr; // HANDLE (parent read end; owned by reader)
  std::thread reader_;
  std::mutex mtx_;
  std::deque<std::string> lines_;
  std::atomic<bool> running_{false};
  std::atomic<int> exitCode_{-1};
};

/* Quote one argument for a Windows command line (CommandLineToArgvW rules):
 * wrap in double quotes, backslash-escape embedded quotes and the run of
 * backslashes preceding them. Exposed for the pipe/UI command builders. */
std::wstring quoteArg(const std::wstring &a);

} // namespace sculptcore::debug_app
