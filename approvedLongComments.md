# Approved long comments

Permanent **non-doc** comments longer than the 3-line limit (see the "Code
Comments" section of `CLAUDE.md`) that have been explicitly approved. Entries
here are exempt from the per-file length budget — do not flag or shorten them
during a comment audit, and do not count them toward the "one >3-line comment
per ~500 lines" allowance.

Doc comments are **not** listed here: they are exempt from the length limit by
rule (they should still be reasonably concise). Only non-doc comments need an
entry.

An approved long comment must use a **C-style** `/* … */` block (the only
non-doc exception to the C++ `//` rule), **without** a leading `*` on the lines
between the opening and closing tags. For example:

```cpp
/* First line of the rationale, no leading asterisks on the
   continuation lines, closing tag on its own or trailing a line. */
```

Format, one per line:

```
{path}:{function}:{one-line summary of the comment}
```

`path` is relative to the sculptcore root; `function` is the enclosing function
(or `(file)` for a file-level comment).

## Entries

source/meshlog/meshlog_base.h:LogChunkTopo::undo:why undo drives the spatial tree's face ownership itself (raw alloc/release bypass make_face/kill_face)
source/meshlog/meshlog_base.h:LogChunkTopo::undo:pre-pass drops face/vert ownership while post-step connectivity is still valid, before restore
source/meshlog/meshlog_base.h:LogChunkTopo::redo:pre-pass drops face/vert ownership so an index-reusing recreate can't resurrect a double-owned vert
source/meshlog/meshlog_base.h:MeshLog::endStep:refresh created-vert positions from the final mesh after later dabs brush-deformed them without a connectivity touch
source/meshlog/meshlog_base.h:MeshLog::pushTopoChunk:why the outgoing topo chunk is finalized per-dab, not at endStep
source/meshlog/meshlog_base.h:MeshLog::undo:why chunks undo in reverse creation order; topo chunk and element store no longer overlap on dyntopo-moved verts
source/meshlog/meshlog_base.h:MeshLog::redo:why chunks redo in forward creation order, each fully applied before the next
