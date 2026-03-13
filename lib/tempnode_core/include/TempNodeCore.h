#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tempnode {

int compareVersionStrings(const char* a, const char* b);
bool parseHexDigest(const char* hex, uint8_t* out, size_t outLen);
bool parseHistoryTimestampMs(const char* line, uint64_t& outTs);

} // namespace tempnode
