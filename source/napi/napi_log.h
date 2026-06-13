#pragma once
// Native log sink: route a message somewhere visible from engine code.
//
// sc_napi_log / sc_napi_logf live in the mesh lib (source/mesh/napi_log.cc) —
// the lowest lib that calls them — and forward to an installable sink. The default sink is stderr; the N-API
// addon installs a sink that calls the renderer's `console.log`
// (source/napi/napi_runtime.cc), which shows in DevTools regardless of how
// Electron plumbs child-process stdout. No node_api.h dependency here, so any
// engine TU can call sc_napi_log in any build.

#ifdef __cplusplus
extern "C" {
#endif

// Sink signature. Receives a single, already-formatted message (no newline).
typedef void (*sc_napi_log_fn)(const char *msg);

// Install the active sink (the addon does this at module init). Passing nullptr
// restores the default stderr sink. Not thread-safe; call once on the JS thread.
void sc_napi_set_sink(sc_napi_log_fn sink);

// Log a single string (a newline is appended by the stderr default; the console
// sink relies on console.log's own newline). nullptr is treated as "".
void sc_napi_log(const char *msg);

// printf-style convenience wrapper over sc_napi_log.
void sc_napi_logf(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#ifdef __cplusplus
}  // extern "C"
#endif
