#pragma once

/* Per-asset target-quad-count manifest: <assets-dir>/quad-counts.txt, one
 * "<stem> <count>" entry per line ('#' comments preserved). Read by the CLI /
 * debug app when no explicit --target-quads / --target is given; written by
 * the debug UI's "save quad count" button. Host-tool code (CLI + debug app
 * only) — never built into the WASM remesh lib. */

#include <string>

namespace sculptcore::remesh::cli {

inline const char *kQuadCountsFile = "quad-counts.txt";

/* The asset's file stem: basename without the final extension. */
std::string assetStem(const std::string &path);

/* Look up @p stem in <dir>/quad-counts.txt. Returns the recorded count (> 0)
 * on a hit, 0 on a miss or unreadable file. */
int lookupAssetQuadCount(const std::string &dir, const std::string &stem);

/* Insert or update @p stem's entry, preserving the other lines (comments
 * included). Creates the file if absent. Returns false on I/O failure. */
bool saveAssetQuadCount(const std::string &dir, const std::string &stem,
                        int count);

} // namespace sculptcore::remesh::cli
