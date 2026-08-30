#include "asset_quads.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace sculptcore::remesh::cli {

namespace {

std::string manifestPath(const std::string &dir)
{
  return dir + "/" + kQuadCountsFile;
}

/* Parse one manifest line into stem + count. Returns false for comments,
 * blanks, and malformed lines. */
bool parseLine(const std::string &line, std::string &stem, int &count)
{
  size_t i = line.find_first_not_of(" \t\r\n");
  if (i == std::string::npos || line[i] == '#') {
    return false;
  }
  size_t j = line.find_first_of(" \t", i);
  if (j == std::string::npos) {
    return false;
  }
  stem = line.substr(i, j - i);
  count = std::atoi(line.c_str() + j);
  return count > 0;
}

std::vector<std::string> readLines(const std::string &path)
{
  std::vector<std::string> lines;
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return lines;
  }
  std::string cur;
  char buf[1024];
  while (std::fgets(buf, sizeof(buf), f)) {
    cur += buf;
    if (!cur.empty() && cur.back() == '\n') {
      while (!cur.empty() && (cur.back() == '\n' || cur.back() == '\r')) {
        cur.pop_back();
      }
      lines.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) {
    lines.push_back(cur);
  }
  std::fclose(f);
  return lines;
}

} // namespace

std::string assetStem(const std::string &path)
{
  size_t slash = path.find_last_of("/\\");
  std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  return dot == std::string::npos ? base : base.substr(0, dot);
}

int lookupAssetQuadCount(const std::string &dir, const std::string &stem)
{
  for (const std::string &line : readLines(manifestPath(dir))) {
    std::string s;
    int count = 0;
    if (parseLine(line, s, count) && s == stem) {
      return count;
    }
  }
  return 0;
}

bool saveAssetQuadCount(const std::string &dir, const std::string &stem, int count)
{
  std::string path = manifestPath(dir);
  std::vector<std::string> lines = readLines(path);

  std::string entry = stem + " " + std::to_string(count);
  bool replaced = false;
  for (std::string &line : lines) {
    std::string s;
    int c = 0;
    if (parseLine(line, s, c) && s == stem) {
      line = entry;
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    if (lines.empty()) {
      lines.push_back("# Per-asset target quad counts (<stem> <count>);");
      lines.push_back("# maintained by the remesh debug app's save button.");
    }
    lines.push_back(entry);
  }

  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  for (const std::string &line : lines) {
    std::fprintf(f, "%s\n", line.c_str());
  }
  std::fclose(f);
  return true;
}

} // namespace sculptcore::remesh::cli
