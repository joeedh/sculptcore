#include "subprocess_win.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace sculptcore::debug_app {

std::wstring quoteArg(const std::wstring &a)
{
  // Empty / contains-special → quote; mirror CommandLineToArgvW escaping.
  bool needs = a.empty();
  for (wchar_t c : a) {
    if (c == L' ' || c == L'\t' || c == L'"') {
      needs = true;
      break;
    }
  }
  if (!needs) {
    return a;
  }
  std::wstring out = L"\"";
  for (size_t i = 0; i < a.size(); i++) {
    int slashes = 0;
    while (i < a.size() && a[i] == L'\\') {
      slashes++;
      i++;
    }
    if (i == a.size()) {
      out.append(size_t(slashes) * 2, L'\\'); // double trailing slashes before "
      break;
    } else if (a[i] == L'"') {
      out.append(size_t(slashes) * 2 + 1, L'\\'); // escape the slashes + the quote
      out.push_back(L'"');
    } else {
      out.append(size_t(slashes), L'\\');
      out.push_back(a[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

Subprocess::~Subprocess()
{
  kill();
}

bool Subprocess::start(const std::wstring &exe, const std::vector<std::wstring> &args)
{
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    return false;
  }
  // The parent's read end must NOT be inherited by the child.
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

  std::wstring cmd = quoteArg(exe);
  for (const auto &a : args) {
    cmd += L' ';
    cmd += quoteArg(a);
  }
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi{};
  // lpApplicationName=nullptr → the OS resolves argv[0] from the (quoted)
  // command line, doing a PATH search + ".exe" append. That lets us launch both
  // a full path (remesh_cli.exe) and a bare program on PATH (node) uniformly.
  BOOL ok = CreateProcessW(nullptr,
                           buf.data(),
                           nullptr,
                           nullptr,
                           /*bInheritHandles=*/TRUE,
                           CREATE_NO_WINDOW,
                           nullptr,
                           nullptr,
                           &si,
                           &pi);
  // Parent never writes the child's stdout; close our copy so the reader sees
  // EOF when the child exits (else ReadFile blocks forever).
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    return false;
  }
  CloseHandle(pi.hThread);

  proc_ = pi.hProcess;
  readPipe_ = rd;
  running_.store(true);
  exitCode_.store(-1);
  reader_ = std::thread(&Subprocess::readerLoop, this);
  return true;
}

void Subprocess::readerLoop()
{
  HANDLE rd = static_cast<HANDLE>(readPipe_);
  char chunk[4096];
  std::string partial;
  for (;;) {
    DWORD got = 0;
    BOOL ok = ReadFile(rd, chunk, sizeof(chunk), &got, nullptr);
    if (!ok || got == 0) {
      break; // EOF / broken pipe → child closed stdout
    }
    partial.append(chunk, chunk + got);
    size_t nl;
    while ((nl = partial.find('\n')) != std::string::npos) {
      std::string line = partial.substr(0, nl);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      partial.erase(0, nl + 1);
      std::lock_guard<std::mutex> lk(mtx_);
      lines_.push_back(std::move(line));
    }
  }
  if (!partial.empty()) {
    if (partial.back() == '\r') {
      partial.pop_back();
    }
    std::lock_guard<std::mutex> lk(mtx_);
    lines_.push_back(std::move(partial));
  }

  HANDLE proc = static_cast<HANDLE>(proc_);
  if (proc) {
    WaitForSingleObject(proc, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(proc, &code)) {
      exitCode_.store(int(code));
    }
  }
  running_.store(false);
}

void Subprocess::drain(std::vector<std::string> &out)
{
  std::lock_guard<std::mutex> lk(mtx_);
  while (!lines_.empty()) {
    out.push_back(std::move(lines_.front()));
    lines_.pop_front();
  }
}

void Subprocess::joinReader()
{
  if (reader_.joinable()) {
    reader_.join();
  }
}

void Subprocess::kill()
{
  HANDLE proc = static_cast<HANDLE>(proc_);
  if (proc && running_.load()) {
    TerminateProcess(proc, 1); // unblocks the reader's ReadFile via pipe close
  }
  joinReader();
  if (readPipe_) {
    CloseHandle(static_cast<HANDLE>(readPipe_));
    readPipe_ = nullptr;
  }
  if (proc_) {
    CloseHandle(static_cast<HANDLE>(proc_));
    proc_ = nullptr;
  }
}

} // namespace sculptcore::debug_app
