#include "TempNodeCore.h"

#include <ctype.h>
#include <string.h>

namespace {

size_t parseVersionParts(const char* s, int* out, size_t maxParts) {
  size_t count = 0;
  const char* p = s;

  while (*p && count < maxParts) {
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) break;

    int v = 0;
    while (*p && isdigit((unsigned char)*p)) {
      v = (v * 10) + (*p - '0');
      p++;
    }
    out[count++] = v;
  }
  return count;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return (c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (c - 'A') + 10;
  return -1;
}

} // namespace

namespace tempnode {

int compareVersionStrings(const char* a, const char* b) {
  if (!a) a = "";
  if (!b) b = "";

  int pa[8] = {0};
  int pb[8] = {0};

  size_t ca = parseVersionParts(a, pa, 8);
  size_t cb = parseVersionParts(b, pb, 8);

  if (ca == 0 || cb == 0) {
    int c = strcmp(a, b);
    return (c < 0) ? -1 : ((c > 0) ? 1 : 0);
  }

  size_t n = (ca > cb) ? ca : cb;
  for (size_t i = 0; i < n; i++) {
    int va = (i < ca) ? pa[i] : 0;
    int vb = (i < cb) ? pb[i] : 0;
    if (va < vb) return -1;
    if (va > vb) return 1;
  }
  return 0;
}

bool parseHexDigest(const char* hex, uint8_t* out, size_t outLen) {
  if (!hex || !out || outLen == 0) return false;

  const size_t needed = outLen * 2;
  if (strlen(hex) != needed) return false;

  for (size_t i = 0; i < outLen; i++) {
    int hi = hexNibble(hex[i * 2]);
    int lo = hexNibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

bool parseHistoryTimestampMs(const char* line, uint64_t& outTs) {
  if (!line) return false;

  const char* key = "\"timestamp\":";
  const char* p = strstr(line, key);
  if (!p) return false;

  p += strlen(key);
  while (*p == ' ' || *p == '\t') p++;
  if (!isdigit((unsigned char)*p)) return false;

  uint64_t value = 0;
  while (isdigit((unsigned char)*p)) {
    value = (value * 10ULL) + (uint64_t)(*p - '0');
    p++;
  }

  outTs = value;
  return true;
}

} // namespace tempnode
