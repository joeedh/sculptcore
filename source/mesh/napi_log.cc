// Native log sink implementation (napi/napi_log.h).
//
// Compiled into every build. sc_napi_log forwards to an installable sink; the
// default writes to stderr. The N-API addon installs a sink that routes to the
// renderer's DevTools console (source/napi/napi_runtime.cc, sc_napi_set_sink).

#include "napi/napi_log.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace {
void stdoutSink(const char *msg)
{
  std::fputs(msg, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}
sc_napi_log_fn g_sink = &stdoutSink;
} // namespace

extern "C" void sc_napi_set_sink(sc_napi_log_fn sink)
{
  g_sink = sink ? sink : &stdoutSink;
}

extern "C" void sc_napi_log(const char *msg)
{
  g_sink(msg ? msg : "");
}

extern "C" void sc_napi_logf(const char *fmt, ...)
{
  char stackbuf[1024];
  std::va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = std::vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return;
  }
  if (static_cast<size_t>(n) < sizeof(stackbuf)) {
    sc_napi_log(stackbuf);
  } else {
    std::vector<char> heap(static_cast<size_t>(n) + 1);
    std::vsnprintf(heap.data(), heap.size(), fmt, ap2);
    sc_napi_log(heap.data());
  }
  va_end(ap2);
}
