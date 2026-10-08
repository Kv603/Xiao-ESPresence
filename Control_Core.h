#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace control {
constexpr size_t MAX_COMMAND = 8192;
// MQTT callbacks own this object. Completed messages are copied to a queue.
class Assembly {
  std::string body_, topic_;
  size_t total_ = 0;
 public:
  void reset() { body_.clear(); topic_.clear(); total_ = 0; }
  bool add(const char *topic, size_t topicLength, const char *data, size_t length,
           size_t offset, size_t total, bool retained) {
    if (offset == 0) {
      reset();
      if (retained || !topic || !topicLength || topicLength > 160 || !total || total > MAX_COMMAND) return false;
      topic_.assign(topic, topicLength); total_ = total;
    }
    if (!total_ || total != total_ || offset != body_.size() || length > total_ - offset || !data ||
        (topicLength && (!topic || topic_.compare(0, topic_.size(), topic, topicLength) != 0)) || retained) {
      reset(); return false;
    }
    body_.append(data, length);
    return body_.size() == total_;
  }
  const std::string &body() const { return body_; }
  const std::string &topic() const { return topic_; }
};
inline bool requestId(const std::string &s) {
  if (s.empty() || s.size() > 64) return false;
  for (unsigned char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  return true;
}
inline bool firmwareUrl(const std::string &s) {
  if (s.size() > 512) return false;
  size_t start = s.compare(0, 7, "http://") == 0 ? 7 : s.compare(0, 8, "https://") == 0 ? 8 : 0;
  if (!start || s.size() <= start || s[start] == '/') return false;
  for (unsigned char c : s) if (c <= 32 || c >= 127 || c == '@' || c == '#' || c == '\\') return false;
  return true;
}
}
