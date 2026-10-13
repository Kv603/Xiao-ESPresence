#pragma once
#include "LED_Core.h"
#include <mutex>

namespace leds {
// The event task acknowledges/reset requests; the loop performs MQTT calls.
class Subscriptions {
  struct Entry { std::string filter; bool wanted = true, acknowledged = false; int id = -1; uint32_t next = 0; bool delayed = false; };
  std::mutex mutex_;
  std::vector<Entry> entries_;
  bool connected_ = false;
  bool requesting_ = false;
  std::vector<int> earlyAcks_;
 public:
  void configure(const Config &config) {
    const auto desired = filters(config);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : entries_) {
      const bool wanted = std::find(desired.begin(), desired.end(), entry.filter) != desired.end();
      if (entry.wanted != wanted) entry.delayed = false;
      entry.wanted = wanted;
    }
    if (!connected_) entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry &e) { return !e.wanted; }), entries_.end());
    for (const auto &filter : desired) {
      auto found = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &entry) { return entry.filter == filter; });
      if (found == entries_.end()) entries_.push_back({filter,true,false,-1,0});
    }
  }
  void connection(bool connected) {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = connected;
    requesting_ = false; earlyAcks_.clear();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [](const Entry &e) { return !e.wanted; }), entries_.end());
    for (auto &e : entries_) { e.id = -1; e.acknowledged = false; e.next = 0; e.delayed = false; }
  }
  void acknowledge(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (auto &entry : entries_) if (entry.id >= 0 && entry.id == id) { entry.acknowledged = true; found = true; }
    if (!found && requesting_ && earlyAcks_.size() < MAX_ASSIGNMENTS + 4) earlyAcks_.push_back(id);
  }
  template<class Subscribe, class Unsubscribe>
  void service(uint32_t now, Subscribe subscribe, Unsubscribe unsubscribe) {
    // Copy one request under the lock, then call MQTT without holding it.
    // This avoids deadlock if an adapter delivers its ACK synchronously.
    std::vector<std::string> requests, removals;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!connected_) return;
      for (auto &e : entries_) {
        if (e.delayed && int32_t(now-e.next) < 0) continue;
        if (!e.wanted) removals.push_back(e.filter);
        else if (!e.acknowledged) requests.push_back(e.filter);
      }
    }
    for (const auto &filter : removals) {
      const int id = unsubscribe(filter);
      std::lock_guard<std::mutex> lock(mutex_);
      auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry &e) { return e.filter == filter; });
      if (entry == entries_.end()) continue;
      if (id >= 0) entries_.erase(entry);
      else { entry->next = now + 5000; entry->delayed = true; }
    }
    for (const auto &filter : requests) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) return;
        requesting_ = true; earlyAcks_.clear();
      }
      const int id = subscribe(filter);
      std::lock_guard<std::mutex> lock(mutex_);
      requesting_ = false;
      if (connected_) for (auto &e : entries_) if (e.filter == filter) {
        e.id = id; e.next = now + 5000; e.delayed = true;
        e.acknowledged = id >= 0 && std::find(earlyAcks_.begin(), earlyAcks_.end(), id) != earlyAcks_.end();
      }
      earlyAcks_.clear();
    }
  }
  size_t pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::count_if(entries_.begin(), entries_.end(), [](const Entry &e) { return !e.wanted || !e.acknowledged; });
  }
};
} // namespace leds
