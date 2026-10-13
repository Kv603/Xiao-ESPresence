#pragma once
#include "LED_Core.h"
#include <mutex>
#ifndef TRACKER_NATIVE_TEST
#include <LiteLED.h>
#endif

namespace leds {
// MQTT callbacks only assemble and enqueue. The application loop owns rendering.
class Controller {
  std::mutex mutex_;
  Config config_;
  Model model_;
  std::array<Event, MAX_EVENTS> queue_{};
  size_t head_ = 0, count_ = 0;
  std::string topic_, body_;
  size_t total_ = 0;
  uint64_t fragmentGroups_ = 0;
  bool initialized_ = false, paused_ = false, haveFrame_ = false;
  uint32_t lastRender_ = 0;
  uint8_t lastBrightness_ = 255;
  std::array<RGB, PIXELS> lastFrame_{};
  int driverError_ = 0;
  unsigned driverFailures_ = 0, eventOverflow_ = 0, invalidValues_ = 0;
#ifndef TRACKER_NATIVE_TEST
  LiteLED strip_{LED_STRIP_WS2812, false};
#endif
  void resetAssemblyLocked() { topic_.clear(); body_.clear(); total_ = 0; fragmentGroups_ = 0; }
  void drainLocked() {
    while (count_) {
      model_.apply(queue_[head_]); queue_[head_] = Event{};
      head_ = (head_+1) % MAX_EVENTS; --count_;
    }
  }
 public:
  void begin(const Config &config) {
    { std::lock_guard<std::mutex> lock(mutex_); config_ = config; }
#ifndef TRACKER_NATIVE_TEST
    driverError_ = strip_.begin(LED_STRIP_PIN, PIXELS, DMA_OFF, PRIORITY_HIGH, PSRAM_DISABLE);
    if (!driverError_) driverError_ = strip_.setOrder(ORDER_GRB);
    initialized_ = driverError_ == ESP_OK;
#else
    initialized_ = true;
#endif
    if (driverError_) { ++driverFailures_; log_w("LED initialization failed: %d", driverError_); }
    service(millis(), true);
  }
  void activate(const Config &config) {
    std::lock_guard<std::mutex> lock(mutex_);
    drainLocked();
    model_.configure(config_, config); config_ = config;
    resetAssemblyLocked();
  }
  void resetAssembly() { std::lock_guard<std::mutex> lock(mutex_); resetAssemblyLocked(); }
  void ingest(const char *topic, size_t topicLength, const char *data, size_t length,
              size_t offset, size_t total, bool retained, uint32_t received) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset == 0) {
      resetAssemblyLocked();
      if (paused_ || retained || !topic || !topicLength || topicLength > MAX_TOPIC || !total || total > MAX_PAYLOAD) return;
      topic_.assign(topic, topicLength);
      if (!validUtf8(topic_) || topic_.find_first_of("+#") != std::string::npos) { resetAssemblyLocked(); return; }
      fragmentGroups_ = matchingGroups(config_, topic_);
      if (!fragmentGroups_) { resetAssemblyLocked(); return; }
      total_ = total;
    }
    if (!total_ || paused_ || retained || total != total_ || offset != body_.size() ||
        offset > total_ || length > total_-offset || !data ||
        (topicLength && (!topic || topic_.compare(0, topic_.size(), topic, topicLength) != 0))) {
      resetAssemblyLocked(); return;
    }
    body_.append(data, length);
    if (body_.size() != total_) return;
    Value value;
    if (!parseValue(body_, value)) ++invalidValues_;
    else if (count_ == MAX_EVENTS) ++eventOverflow_;
    else { queue_[(head_+count_) % MAX_EVENTS] = {topic_, value, fragmentGroups_, received}; ++count_; }
    resetAssemblyLocked();
  }
  void pause() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      paused_ = true; model_.clear(); resetAssemblyLocked();
      for (auto &event : queue_) event = Event{};
      head_ = count_ = 0;
    }
    service(millis(), true);
  }
  void resume() { std::lock_guard<std::mutex> lock(mutex_); paused_ = false; }
  void service(uint32_t now, bool force = false) {
    std::array<RGB, PIXELS> frame;
    uint8_t brightness;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      drainLocked();
      if (!force && uint32_t(now-lastRender_) < 20) return;
      frame = model_.frame(now); brightness = config_.brightness;
    }
    lastRender_ = now;
    if (!initialized_ || (!force && haveFrame_ && frame == lastFrame_ && brightness == lastBrightness_)) return;
    int error = 0;
#ifndef TRACKER_NATIVE_TEST
    error = strip_.brightness(brightness);
    // Pass canonical pixels; LiteLED applies general brightness while encoding.
    for (size_t p = 0; !error && p < PIXELS; ++p)
      error = strip_.setPixel(p, rgb_t{frame[p].r,frame[p].g,frame[p].b});
    if (!error) error = strip_.show();
#endif
    if (error) {
      ++driverFailures_;
      if (driverError_ != error) log_w("LED frame failed: %d", error);
      driverError_ = error; return;
    }
    driverError_ = 0; lastFrame_ = frame; lastBrightness_ = brightness; haveFrame_ = true;
  }
  void state(JsonObject object) {
    std::lock_guard<std::mutex> lock(mutex_);
    object["pin"] = LED_STRIP_PIN; object["pixels"] = PIXELS; object["group_count"] = GROUPS;
    object["skip_first"] = LED_SKIP_FIRST; object["group_width"] = LED_GROUP_WIDTH; object["group_gap"] = LED_GROUP_GAP;
    object["driver_ready"] = initialized_; object["driver_error"] = driverError_;
    object["driver_failures"] = driverFailures_; object["event_overflow"] = eventOverflow_;
    object["source_overflow"] = model_.sourceOverflow; object["invalid_values"] = invalidValues_;
    object["queued_events"] = count_;
  }
#ifdef TRACKER_NATIVE_TEST
  const std::array<RGB, PIXELS> &lastFrame() const { return lastFrame_; }
  uint8_t lastBrightness() const { return lastBrightness_; }
#endif
};
} // namespace leds
static leds::Controller ledController;
inline void serviceLeds() { ledController.service(millis()); }
