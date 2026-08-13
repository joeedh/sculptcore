/* Sculptcore's hand-written config.h for the vendored tinycc snapshot
   (upstream generates this with ./configure; see README.sculptcore.md).
   Only TCC_VERSION is required — tcc.h autodetects the native target and
   OS when no TCC_TARGET_* macro is predefined, which is exactly the
   JIT-only embedding this tree exists for. Everything else falls back to
   the in-tree defaults. */
#ifndef SCULPTCORE_TCC_CONFIG_H
#define SCULPTCORE_TCC_CONFIG_H

#define TCC_VERSION "0.9.28rc"

/* Embed the predefined macros (the vendored tccdefs_.h strings). Without
   this tcc tries to read include/tccdefs.h from an install dir at runtime,
   which an embedded JIT does not have. */
#define CONFIG_TCC_PREDEFS 1

#endif
