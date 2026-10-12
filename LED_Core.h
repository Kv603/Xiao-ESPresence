#pragma once

// Portable LED configuration, matching, parsing and rendering model.
#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#ifndef LED_STRIP_PIN
#define LED_STRIP_PIN 2
#endif
#ifndef LED_STRIP_COUNT
#define LED_STRIP_COUNT 51
#endif
#ifndef LED_SKIP_FIRST
#define LED_SKIP_FIRST 1
#endif
#ifndef LED_GROUP_WIDTH
#define LED_GROUP_WIDTH 3
#endif
#ifndef LED_GROUP_GAP
#define LED_GROUP_GAP 1
#endif

namespace leds {
static_assert(LED_STRIP_COUNT >= 1 && LED_STRIP_COUNT <= 51, "LED_STRIP_COUNT must be 1-51");
static_assert(LED_SKIP_FIRST == 0 || LED_SKIP_FIRST == 1, "LED_SKIP_FIRST must be 0 or 1");
static_assert(LED_GROUP_WIDTH > 0, "LED_GROUP_WIDTH must be positive");
static_assert(LED_GROUP_GAP >= 0, "LED_GROUP_GAP must be nonnegative");
constexpr size_t PIXELS = LED_STRIP_COUNT;
constexpr size_t STRIDE = size_t(LED_GROUP_WIDTH) + size_t(LED_GROUP_GAP);
static_assert(STRIDE > 0 && size_t(LED_GROUP_WIDTH) <= SIZE_MAX - size_t(LED_GROUP_GAP), "LED group stride is too large");
constexpr size_t GROUPS = PIXELS == LED_SKIP_FIRST ? 0 : 1 + (PIXELS - LED_SKIP_FIRST - 1) / STRIDE;
constexpr size_t MAX_ASSIGNMENTS = 64, MAX_SOURCES = 64, MAX_EVENTS = 32;
constexpr size_t MAX_TOPIC = 160, MAX_PAYLOAD = 64;
constexpr uint32_t FADE_MS = 60000;
constexpr size_t groupStart(size_t index) { return LED_SKIP_FIRST + index * STRIDE; }

struct Config {
  uint8_t brightness = 255;
  std::array<std::vector<std::string>, GROUPS> topics;
};

inline bool validUtf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i++]);
    if (!c) return false;
    if (c < 128) continue;
    unsigned n; uint32_t code, minimum;
    if (c >= 0xc2 && c <= 0xdf) { n = 1; code = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { n = 2; code = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 3; code = c & 7; minimum = 0x10000; }
    else return false;
    if (n > s.size() - i) return false;
    while (n--) {
      const uint8_t next = uint8_t(s[i++]);
      if ((next & 0xc0) != 0x80) return false;
      code = (code << 6) | (next & 63);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}
inline bool validFilter(const std::string &s) {
  if (s.empty() || s.size() > MAX_TOPIC || !validUtf8(s) || s.compare(0, 7, "$share/") == 0) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+' && ((i && s[i-1] != '/') || (i+1 < s.size() && s[i+1] != '/'))) return false;
    if (s[i] == '#' && ((i && s[i-1] != '/') || i+1 != s.size())) return false;
  }
  return true;
}
// Both arguments are validated by their respective ingress paths.
inline bool matches(const std::string &filter, const std::string &topic) {
  if (filter.empty() || topic.empty()) return false;
  if (topic[0] == '$' && (filter[0] == '#' || filter[0] == '+')) return false;
  size_t f = 0, t = 0;
  for (;;) {
    const size_t fe = filter.find('/', f), te = topic.find('/', t);
    const size_t fn = (fe == std::string::npos ? filter.size() : fe) - f;
    const size_t tn = (te == std::string::npos ? topic.size() : te) - t;
    if (fn == 1 && filter[f] == '#') return true;
    if (!(fn == 1 && filter[f] == '+') && (fn != tn || filter.compare(f, fn, topic, t, tn) != 0)) return false;
    if (te == std::string::npos) {
      return fe == std::string::npos || filter.compare(fe, std::string::npos, "/#") == 0;
    }
    if (fe == std::string::npos) return false;
    f = fe+1; t = te+1;
  }
}
inline uint64_t matchingGroups(const Config &config, const std::string &topic) {
  uint64_t mask = 0;
  for (size_t g = 0; g != GROUPS; ++g)
    for (const auto &filter : config.topics[g])
      if (matches(filter, topic)) { mask |= uint64_t(1) << g; break; }
  return mask;
}
inline std::vector<std::string> filters(const Config &config) {
  std::vector<std::string> out;
  for (const auto &group : config.topics)
    for (const auto &filter : group)
      if (std::find(out.begin(), out.end(), filter) == out.end()) out.push_back(filter);
  return out;
}
inline bool validConfig(const Config &config) {
  size_t count = 0;
  for (const auto &group : config.topics) {
    count += group.size();
    for (size_t i = 0; i < group.size(); ++i)
      if (!validFilter(group[i]) || std::find(group.begin(), group.begin()+i, group[i]) != group.begin()+i) return false;
  }
  return count <= MAX_ASSIGNMENTS;
}
// This function is transactional even when used independently of AppSettings.
inline bool patchConfig(JsonVariantConst command, Config &config) {
  if (!command.is<JsonObjectConst>()) return false;
  Config next = config;
  const auto object = command.as<JsonObjectConst>();
  if (!object["brightness"].isUnbound()) {
    if (!object["brightness"].is<unsigned>() || object["brightness"].as<unsigned>() > 255) return false;
    next.brightness = object["brightness"].as<unsigned>();
  }
  if (!object["groups"].isUnbound()) {
    if (!object["groups"].is<JsonArrayConst>()) return false;
    uint64_t seen = 0;
    for (JsonVariantConst item : object["groups"].as<JsonArrayConst>()) {
      if (!item.is<JsonObjectConst>() || !item["group"].is<unsigned>() || !item["topics"].is<JsonArrayConst>()) return false;
      const unsigned number = item["group"].as<unsigned>();
      if (!number || number > GROUPS || (seen & (uint64_t(1) << (number-1)))) return false;
      seen |= uint64_t(1) << (number-1);
      auto &topics = next.topics[number-1]; topics.clear();
      for (JsonVariantConst value : item["topics"].as<JsonArrayConst>()) {
        if (!value.is<const char *>()) return false;
        topics.push_back(value.as<std::string>());
        if (topics.size() > MAX_ASSIGNMENTS) return false;
      }
    }
  }
  if (!validConfig(next)) return false;
  config = std::move(next);
  return true;
}
inline void configJson(JsonObject object, const Config &config) {
  object["brightness"] = config.brightness;
  auto groups = object["groups"].to<JsonArray>();
  for (size_t g = 0; g != GROUPS; ++g) {
    if (config.topics[g].empty()) continue;
    auto item = groups.add<JsonObject>(); item["group"] = g+1;
    auto topics = item["topics"].to<JsonArray>();
    for (const auto &filter : config.topics[g]) topics.add(filter);
  }
}

struct RGB {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const RGB &v) const { return r == v.r && g == v.g && b == v.b; }
  bool operator!=(const RGB &v) const { return !(*this == v); }
};
struct ColorName { const char *name; uint32_t rgb; };
// CSS named sRGB colors, plus the application aliases below.
inline bool colorCode(const std::string &name, uint32_t &code) {
  static constexpr ColorName table[] = {
    {"aliceblue",0xf0f8ff},{"antiquewhite",0xfaebd7},{"aqua",0x00ffff},{"aquamarine",0x7fffd4},
    {"azure",0xf0ffff},{"beige",0xf5f5dc},{"bisque",0xffe4c4},{"black",0x000000},
    {"blanchedalmond",0xffebcd},{"blue",0x0000ff},{"blueviolet",0x8a2be2},{"brown",0xa52a2a},
    {"burlywood",0xdeb887},{"cadetblue",0x5f9ea0},{"chartreuse",0x7fff00},{"chocolate",0xd2691e},
    {"coral",0xff7f50},{"cornflowerblue",0x6495ed},{"cornsilk",0xfff8dc},{"crimson",0xdc143c},
    {"cyan",0x00ffff},{"darkblue",0x00008b},{"darkcyan",0x008b8b},{"darkgoldenrod",0xb8860b},
    {"darkgray",0xa9a9a9},{"darkgreen",0x006400},{"darkgrey",0xa9a9a9},{"darkkhaki",0xbdb76b},
    {"darkmagenta",0x8b008b},{"darkolivegreen",0x556b2f},{"darkorange",0xff8c00},{"darkorchid",0x9932cc},
    {"darkred",0x8b0000},{"darksalmon",0xe9967a},{"darkseagreen",0x8fbc8f},{"darkslateblue",0x483d8b},
    {"darkslategray",0x2f4f4f},{"darkslategrey",0x2f4f4f},{"darkturquoise",0x00ced1},{"darkviolet",0x9400d3},
    {"deeppink",0xff1493},{"deepskyblue",0x00bfff},{"dimgray",0x696969},{"dimgrey",0x696969},
    {"dodgerblue",0x1e90ff},{"firebrick",0xb22222},{"floralwhite",0xfffaf0},{"forestgreen",0x228b22},
    {"fuchsia",0xff00ff},{"gainsboro",0xdcdcdc},{"ghostwhite",0xf8f8ff},{"gold",0xffd700},
    {"goldenrod",0xdaa520},{"gray",0x808080},{"green",0x008000},{"greenyellow",0xadff2f},
    {"grey",0x808080},{"honeydew",0xf0fff0},{"hotpink",0xff69b4},{"indianred",0xcd5c5c},
    {"indigo",0x4b0082},{"ivory",0xfffff0},{"khaki",0xf0e68c},{"lavender",0xe6e6fa},
    {"lavenderblush",0xfff0f5},{"lawngreen",0x7cfc00},{"lemonchiffon",0xfffacd},{"lightblue",0xadd8e6},
    {"lightcoral",0xf08080},{"lightcyan",0xe0ffff},{"lightgoldenrodyellow",0xfafad2},{"lightgray",0xd3d3d3},
    {"lightgreen",0x90ee90},{"lightgrey",0xd3d3d3},{"lightpink",0xffb6c1},{"lightsalmon",0xffa07a},
    {"lightseagreen",0x20b2aa},{"lightskyblue",0x87cefa},{"lightslategray",0x778899},{"lightslategrey",0x778899},
    {"lightsteelblue",0xb0c4de},{"lightyellow",0xffffe0},{"lime",0x00ff00},{"limegreen",0x32cd32},
    {"linen",0xfaf0e6},{"magenta",0xff00ff},{"maroon",0x800000},{"mediumaquamarine",0x66cdaa},
    {"mediumblue",0x0000cd},{"mediumorchid",0xba55d3},{"mediumpurple",0x9370db},{"mediumseagreen",0x3cb371},
    {"mediumslateblue",0x7b68ee},{"mediumspringgreen",0x00fa9a},{"mediumturquoise",0x48d1cc},{"mediumvioletred",0xc71585},
    {"midnightblue",0x191970},{"mintcream",0xf5fffa},{"mistyrose",0xffe4e1},{"moccasin",0xffe4b5},
    {"navajowhite",0xffdead},{"navy",0x000080},{"oldlace",0xfdf5e6},{"olive",0x808000},
    {"olivedrab",0x6b8e23},{"orange",0xffa500},{"orangered",0xff4500},{"orchid",0xda70d6},
    {"palegoldenrod",0xeee8aa},{"palegreen",0x98fb98},{"paleturquoise",0xafeeee},{"palevioletred",0xdb7093},
    {"papayawhip",0xffefd5},{"peachpuff",0xffdab9},{"peru",0xcd853f},{"pink",0xffc0cb},
    {"plum",0xdda0dd},{"powderblue",0xb0e0e6},{"purple",0x800080},{"rebeccapurple",0x663399},
    {"red",0xff0000},{"rosybrown",0xbc8f8f},{"royalblue",0x4169e1},{"saddlebrown",0x8b4513},
    {"salmon",0xfa8072},{"sandybrown",0xf4a460},{"seagreen",0x2e8b57},{"seashell",0xfff5ee},
    {"sienna",0xa0522d},{"silver",0xc0c0c0},{"skyblue",0x87ceeb},{"slateblue",0x6a5acd},
    {"slategray",0x708090},{"slategrey",0x708090},{"snow",0xfffafa},{"springgreen",0x00ff7f},
    {"steelblue",0x4682b4},{"tan",0xd2b48c},{"teal",0x008080},{"thistle",0xd8bfd8},
    {"tomato",0xff6347},{"turquoise",0x40e0d0},{"violet",0xee82ee},{"wheat",0xf5deb3},
    {"white",0xffffff},{"whitesmoke",0xf5f5f5},{"yellow",0xffff00},{"yellowgreen",0x9acd32},
    {"on",0xffffff},{"off",0x000000},{"brickred",0xb22222}
  };
  for (const auto &entry : table) if (name == entry.name) { code = entry.rgb; return true; }
  return false;
}
struct Value {
  uint32_t color = 0;
  double percent = 0;
  bool numeric = false, persistent = false;
};
inline bool parseValue(const std::string &payload, Value &value) {
  if (payload.empty() || payload.size() > MAX_PAYLOAD || payload.find('\0') != std::string::npos) return false;
  const auto first = payload.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  std::string text = payload.substr(first, payload.find_last_not_of(" \t\r\n") - first + 1);
  JsonDocument doc;
  // ArduinoJson accepts some non-JSON numbers, so constrain the syntax first.
  if (text[0] == '"') {
    bool closed = false;
    for (size_t i = 1; i < text.size(); ++i) {
      if (text[i] == '\\') { if (++i == text.size()) return false; }
      else if (text[i] == '"') { if (i+1 != text.size()) return false; closed = true; break; }
    }
    if (!closed) return false;
    if (deserializeJson(doc, text) || !doc.is<const char *>()) return false;
    text = doc.as<std::string>();
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return false;
    text = text.substr(begin, text.find_last_not_of(" \t\r\n") - begin+1);
  } else if ((text[0] >= '0' && text[0] <= '9') || text[0] == '-') {
    size_t i = text[0] == '-' ? 1 : 0;
    if (i == text.size()) return false;
    if (text[i] == '0') ++i;
    else { if (text[i] < '1' || text[i] > '9') return false; while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i; }
    if (i < text.size() && text[i] == '.') {
      const size_t start = ++i; while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
      if (i == start) return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
      ++i; if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
      const size_t start = i; while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
      if (i == start) return false;
    }
    if (i != text.size() || deserializeJson(doc, text) || !doc.is<double>()) return false;
    const double percent = doc.as<double>();
    if (!std::isfinite(percent) || percent < 0 || percent > 100) return false;
    value = {0xffffff, percent, true, false}; return true;
  }
  for (char &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  uint32_t color;
  if (!colorCode(text, color)) return false;
  value = {color, color ? 75.0 : 0.0, false, text == "red" || text == "brickred"};
  return true;
}
inline int channel(const std::string &topic) {
  const auto slash = topic.rfind('/');
  if (slash == std::string::npos) return -1;
  const auto last = topic.substr(slash+1);
  return last == "red" ? 0 : last == "green" ? 1 : last == "blue" ? 2 : -1;
}
struct Event { std::string topic; Value value; uint64_t groups = 0; uint32_t received = 0; };
struct Level {
  double initial = 0;
  uint32_t received = 0;
  bool persistent = false, expired = true;
  double at(uint32_t now) {
    if (persistent) return initial;
    if (expired) return 0;
    const uint32_t elapsed = now - received;
    if (elapsed >= FADE_MS) { expired = true; return 0; }
    return initial * double(FADE_MS-elapsed) / FADE_MS;
  }
};
class Model {
  struct Source { std::string topic; uint32_t color = 0; uint64_t groups = 0; Level level; };
  struct Override { bool active = false; Level level; };
  std::array<Source, MAX_SOURCES> sources_{};
  std::array<std::array<Override, 3>, GROUPS> overrides_{};
 public:
  unsigned sourceOverflow = 0;
  void clear() { for (auto &s : sources_) s = Source{}; overrides_ = {}; }
  void configure(const Config &before, const Config &after) {
    for (auto &s : sources_) s.groups &= matchingGroups(after, s.topic);
    for (size_t g = 0; g != GROUPS; ++g) if (before.topics[g] != after.topics[g]) overrides_[g] = {};
  }
  bool apply(const Event &event) {
    if (!event.groups) return true;
    const int component = event.value.numeric ? channel(event.topic) : -1;
    if (component >= 0) {
      for (size_t g = 0; g != GROUPS; ++g) if (event.groups & (uint64_t(1) << g)) {
        auto &v = overrides_[g][component]; v.active = true;
        v.level = {std::round(255.0 * event.value.percent / 100.0), event.received, component == 0, event.value.percent == 0};
      }
      return true;
    }
    Source *selected = nullptr, *free = nullptr;
    for (auto &s : sources_) {
      if (s.topic == event.topic) { selected = &s; break; }
      if (!s.groups || s.level.at(event.received) == 0) free = &s;
    }
    if (!selected) selected = free;
    if (!selected) { ++sourceOverflow; return false; }
    selected->topic = event.topic; selected->color = event.value.color; selected->groups = event.groups;
    selected->level = {event.value.percent / 100.0, event.received, event.value.persistent, event.value.percent == 0};
    for (size_t g = 0; g != GROUPS; ++g) if (event.groups & (uint64_t(1) << g)) overrides_[g] = {};
    return true;
  }
  std::array<RGB, GROUPS> colors(uint32_t now) {
    std::array<std::array<double,3>, GROUPS> mixed{};
    for (auto &s : sources_) {
      const double scale = s.level.at(now);
      if (!scale || !s.groups) continue;
      for (size_t g = 0; g != GROUPS; ++g) if (s.groups & (uint64_t(1) << g)) {
        mixed[g][0] += ((s.color >> 16) & 255) * scale;
        mixed[g][1] += ((s.color >> 8) & 255) * scale;
        mixed[g][2] += (s.color & 255) * scale;
      }
    }
    std::array<RGB, GROUPS> output{};
    for (size_t g = 0; g != GROUPS; ++g) {
      std::array<uint8_t,3> rgb{};
      for (size_t c = 0; c < 3; ++c) {
        const double level = overrides_[g][c].active ? overrides_[g][c].level.at(now) : mixed[g][c];
        rgb[c] = uint8_t(std::round(std::min(255.0, level)));
      }
      output[g] = {rgb[0],rgb[1],rgb[2]};
    }
    return output;
  }
  std::array<RGB, PIXELS> frame(uint32_t now) {
    std::array<RGB, PIXELS> pixels{};
    const auto groups = colors(now);
    for (size_t g = 0; g != GROUPS; ++g)
      for (size_t p = groupStart(g); p < PIXELS && p-groupStart(g) < size_t(LED_GROUP_WIDTH); ++p) pixels[p] = groups[g];
    return pixels;
  }
};
} // namespace leds
