#include "remesh_pipe_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sculptcore::debug_app {

PipeServer::~PipeServer()
{
  stop();
}

bool PipeServer::start(const std::wstring &pipeName, Handler handler)
{
  if (running_.load()) {
    return false;
  }
  pipeName_ = pipeName;
  handler_ = std::move(handler);
  running_.store(true);
  accept_ = std::thread(&PipeServer::acceptLoop, this);
  return true;
}

void PipeServer::acceptLoop()
{
  while (running_.load()) {
    HANDLE pipe = CreateNamedPipeW(
        pipeName_.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
        64 * 1024, 64 * 1024, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      // Transient failure; bail if we're shutting down, else retry.
      if (!running_.load()) {
        break;
      }
      Sleep(50);
      continue;
    }

    BOOL connected =
        ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

    // The shutdown self-connect (see stop()) trips this branch — bail out.
    if (!running_.load()) {
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      break;
    }
    if (!connected) {
      CloseHandle(pipe);
      continue;
    }

    // Read one command line (until '\n' or client EOF).
    std::string in;
    char chunk[4096];
    bool gotLine = false;
    while (running_.load()) {
      DWORD got = 0;
      BOOL ok = ReadFile(pipe, chunk, sizeof(chunk), &got, nullptr);
      if (!ok || got == 0) {
        break; // client closed without newline → treat whatever we have as the line
      }
      in.append(chunk, chunk + got);
      size_t nl = in.find('\n');
      if (nl != std::string::npos) {
        in.erase(nl);
        gotLine = true;
        break;
      }
    }
    if (!gotLine && !in.empty()) {
      gotLine = true; // EOF-terminated command
    }
    if (!in.empty() && in.back() == '\r') {
      in.pop_back();
    }

    std::string reply;
    if (gotLine) {
      // Marshal to the main thread and block on the result.
      std::future<std::string> fut;
      {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.emplace_back();
        queue_.back().line = std::move(in);
        fut = queue_.back().reply.get_future();
      }
      reply = fut.get(); // fulfilled by drainMainThreadQueue() (or stop()).
    }

    // Frame the reply: body, then a lone "." line, then blank — "\n.\n".
    std::string framed = reply;
    if (!framed.empty() && framed.back() != '\n') {
      framed.push_back('\n');
    }
    framed += ".\n";
    DWORD wrote = 0;
    WriteFile(pipe, framed.data(), (DWORD)framed.size(), &wrote, nullptr);
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}

void PipeServer::stop()
{
  if (!running_.exchange(false)) {
    return;
  }
  // Fulfill any pending promises so a blocked accept thread can unwind rather
  // than deadlock waiting on the main thread that's now tearing down.
  {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &cmd : queue_) {
      try {
        cmd.reply.set_value("server shutting down");
      } catch (...) {
      }
    }
    queue_.clear();
  }
  // Wake a blocked ConnectNamedPipe by self-connecting; the accept loop then
  // sees running_==false and exits.
  HANDLE h = CreateFileW(pipeName_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    CloseHandle(h);
  }
  if (accept_.joinable()) {
    accept_.join();
  }
}

void PipeServer::drainMainThreadQueue()
{
  for (;;) {
    Command cmd;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (queue_.empty()) {
        return;
      }
      cmd.line = std::move(queue_.front().line);
      cmd.reply = std::move(queue_.front().reply);
      queue_.pop_front();
    }
    std::string reply;
    try {
      reply = handler_ ? handler_(cmd.line) : std::string("no handler");
    } catch (const std::exception &e) {
      reply = std::string("ERROR ") + e.what();
    } catch (...) {
      reply = "ERROR unknown exception";
    }
    cmd.reply.set_value(std::move(reply));
  }
}

} // namespace sculptcore::debug_app
