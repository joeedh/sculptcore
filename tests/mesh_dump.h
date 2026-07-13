#pragma once

#include "mesh/mesh.h"
#include "mesh/mesh_types.h"

#include "litestl/math/vector.h"
#include "litestl/util/vector.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace sculptcore::mesh::dump {

inline std::string b64encode(const void *data, size_t n)
{
  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t i = 0;
  while (i + 3 <= n) {
    uint32_t v = (uint32_t(p[i]) << 16) | (uint32_t(p[i + 1]) << 8) | uint32_t(p[i + 2]);
    out.push_back(tbl[(v >> 18) & 0x3f]);
    out.push_back(tbl[(v >> 12) & 0x3f]);
    out.push_back(tbl[(v >> 6) & 0x3f]);
    out.push_back(tbl[v & 0x3f]);
    i += 3;
  }
  if (i < n) {
    uint32_t v = uint32_t(p[i]) << 16;
    if (i + 1 < n)
      v |= uint32_t(p[i + 1]) << 8;
    out.push_back(tbl[(v >> 18) & 0x3f]);
    out.push_back(tbl[(v >> 12) & 0x3f]);
    if (i + 1 < n) {
      out.push_back(tbl[(v >> 6) & 0x3f]);
    } else {
      out.push_back('=');
    }
    out.push_back('=');
  }
  return out;
}

template <typename Attr, typename T>
inline void writeAttr(std::string &out, const char *name, Attr &attr, int cap, bool last)
{
  size_t bytes = size_t(cap) * sizeof(T);
  uint8_t *buf = static_cast<uint8_t *>(std::malloc(bytes));
  for (int i = 0; i < cap; i++) {
    T v = attr[i];
    std::memcpy(buf + size_t(i) * sizeof(T), &v, sizeof(T));
  }
  out += '"';
  out += name;
  out += "\":\"";
  out += b64encode(buf, bytes);
  out += '"';
  if (!last)
    out += ',';
  std::free(buf);
}

inline void
writeAlive(std::string &out, litestl::util::BoolVector<> &freemap, int cap, bool last)
{
  uint8_t *buf = static_cast<uint8_t *>(std::malloc(size_t(cap)));
  for (int i = 0; i < cap; i++) {
    buf[i] = freemap[i] ? 0 : 1;
  }
  out += "\"alive\":\"";
  out += b64encode(buf, size_t(cap));
  out += '"';
  if (!last)
    out += ',';
  std::free(buf);
}

inline void dumpMesh(Mesh &m, std::string &out)
{
  using math::float3;
  using math::int2;
  using math::int4;

  int vc = int(m.v.capacity());
  int ec = int(m.e.capacity());
  int cc = int(m.c.capacity());
  int lc = int(m.l.capacity());
  int fc = int(m.f.capacity());

  out += '{';

  char hdr[256];
  snprintf(hdr,
           sizeof(hdr),
           "\"caps\":{\"v\":%d,\"e\":%d,\"c\":%d,\"l\":%d,\"f\":%d},"
           "\"counts\":{\"v\":%d,\"e\":%d,\"c\":%d,\"l\":%d,\"f\":%d},",
           vc,
           ec,
           cc,
           lc,
           fc,
           m.v.count,
           m.e.count,
           m.c.count,
           m.l.count,
           m.f.count);
  out += hdr;

  /* v */
  out += "\"v\":{";
  writeAttr<decltype(m.v.co), float3>(out, "co", m.v.co, vc, false);
  writeAttr<decltype(m.v.no), float3>(out, "no", m.v.no, vc, false);
  writeAttr<decltype(m.v.e), int>(out, "e", m.v.e, vc, false);
  writeAlive(out, m.v.freemap, vc, true);
  out += "},";

  /* e */
  out += "\"e\":{";
  writeAttr<decltype(m.e.vs), int2>(out, "vs", m.e.vs, ec, false);
  writeAttr<decltype(m.e.c), int>(out, "c", m.e.c, ec, false);
  writeAlive(out, m.e.freemap, ec, true);
  out += "},";

  /* c */
  out += "\"c\":{";
  writeAttr<decltype(m.c.v), int>(out, "v", m.c.v, cc, false);
  writeAttr<decltype(m.c.e), int>(out, "e", m.c.e, cc, false);
  writeAttr<decltype(m.c.l), int>(out, "l", m.c.l, cc, false);
  writeAttr<decltype(m.c.next), int>(out, "next", m.c.next, cc, false);
  writeAttr<decltype(m.c.prev), int>(out, "prev", m.c.prev, cc, false);
  writeAttr<decltype(m.c.radial_next), int>(
      out, "radial_next", m.c.radial_next, cc, false);
  writeAttr<decltype(m.c.radial_prev), int>(
      out, "radial_prev", m.c.radial_prev, cc, false);
  writeAlive(out, m.c.freemap, cc, true);
  out += "},";

  /* l */
  out += "\"l\":{";
  writeAttr<decltype(m.l.c), int>(out, "c", m.l.c, lc, false);
  writeAttr<decltype(m.l.f), int>(out, "f", m.l.f, lc, false);
  writeAttr<decltype(m.l.next), int>(out, "next", m.l.next, lc, false);
  writeAttr<decltype(m.l.size), int>(out, "size", m.l.size, lc, false);
  writeAlive(out, m.l.freemap, lc, true);
  out += "},";

  /* f */
  out += "\"f\":{";
  writeAttr<decltype(m.f.l), int>(out, "l", m.f.l, fc, false);
  writeAttr<decltype(m.f.no), float3>(out, "no", m.f.no, fc, false);
  writeAttr<decltype(m.f.list_count), short>(
      out, "list_count", m.f.list_count, fc, false);
  writeAlive(out, m.f.freemap, fc, true);
  out += '}';

  out += '}';
}

struct Highlight {
  char kind = 0; /* 'v','e','f','c' or 0 */
  litestl::util::Vector<int> ids;
};

class MeshLog {
public:
  MeshLog(const char *dir, const char *tag)
  {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s.meshlog.json", dir, tag);
    fp_ = fopen(path, "wb");
    if (!fp_) {
      fprintf(stderr, "MeshLog: failed to open %s\n", path);
      return;
    }
    fputc('{', fp_);
    fprintf(fp_, "\"version\":1,\"tag\":\"%s\",", tag);
  }

  ~MeshLog()
  {
    finalize();
  }

  void initial(Mesh &m)
  {
    if (!fp_ || started_)
      return;
    fputs("\"initial\":", fp_);
    std::string buf;
    dumpMesh(m, buf);
    fwrite(buf.data(), 1, buf.size(), fp_);
    fputs(",\"steps\":[", fp_);
    started_ = true;
  }

  void step(const char *op, const Highlight &hl, Mesh &m, const char *note = nullptr)
  {
    if (!fp_)
      return;
    if (!started_)
      initial(m);
    if (steps_ > 0)
      fputc(',', fp_);
    fputc('{', fp_);
    fprintf(fp_, "\"op\":\"%s\"", op);
    if (hl.kind) {
      fprintf(fp_, ",\"highlight\":{\"kind\":\"%c\",\"ids\":[", hl.kind);
      for (size_t i = 0; i < hl.ids.size(); i++) {
        if (i)
          fputc(',', fp_);
        fprintf(fp_, "%d", hl.ids[i]);
      }
      fputs("]}", fp_);
    }
    if (note)
      fprintf(fp_, ",\"note\":\"%s\"", note);
    fputs(",\"snapshot\":", fp_);
    std::string buf;
    dumpMesh(m, buf);
    fwrite(buf.data(), 1, buf.size(), fp_);
    fputc('}', fp_);
    steps_++;
  }

  void finalize()
  {
    if (!fp_)
      return;
    if (!started_) {
      fputs("\"steps\":[", fp_);
    }
    fputs("]}", fp_);
    fclose(fp_);
    fp_ = nullptr;
  }

  static const char *envDir()
  {
    return std::getenv("SCULPTCORE_MESH_LOG");
  }

private:
  FILE *fp_ = nullptr;
  bool started_ = false;
  int steps_ = 0;
};

} // namespace sculptcore::mesh::dump
