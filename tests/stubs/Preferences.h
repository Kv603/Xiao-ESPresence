#pragma once
#include <cstring>
#include <vector>

class Preferences {
 public:
  static inline std::vector<unsigned char> saved;
  static inline bool failOpen = false, failWrite = false;
  bool begin(const char *, bool) { return !failOpen; }
  void end() {}
  bool isKey(const char *) { return !saved.empty(); }
  size_t getBytesLength(const char *) { return saved.size(); }
  size_t getBytes(const char *, void *buffer, size_t length) {
    if (length > saved.size()) return 0;
    std::memcpy(buffer, saved.data(), length); return length;
  }
  size_t putBytes(const char *, const void *buffer, size_t length) {
    if (failWrite) return 0;
    const auto *bytes = static_cast<const unsigned char *>(buffer);
    saved.assign(bytes, bytes + length); return length;
  }
};
