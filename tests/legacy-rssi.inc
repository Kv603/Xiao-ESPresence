// SPDX-License-Identifier: AGPL-3.0-only
class LegacyRSSI {
    struct Reading { float rssi; uint32_t time; };
    Reading *data = nullptr;
    uint16_t capacity = 0, head = 0, count = 0;
    uint32_t lastRate = 0;
public:
    static constexpr uint16_t MAX_READINGS = 200;
    // Production can reserve network/OTA headroom; host tests exercise OOM directly.
    static inline bool (*allocationAllowed)(size_t) = nullptr;
    LegacyRSSI() = default;
    ~LegacyRSSI() { delete[] data; }
    LegacyRSSI(const LegacyRSSI&) = delete;
    LegacyRSSI &operator=(const LegacyRSSI&) = delete;
    void reset() { delete[] data; data = nullptr; capacity = head = count = 0; lastRate = 0; }
    uint16_t size() const { return count; }
    uint16_t allocated() const { return capacity; }
    void expire(uint32_t now) {
        while (count && uint32_t(now - data[(head + capacity - count) % capacity].time) > 15000) --count;
    }
    bool resize(uint16_t n) {
        n = std::max<uint16_t>(10, std::min<uint16_t>(MAX_READINGS, n));
        if(allocationAllowed && !allocationAllowed(size_t(n)*sizeof(Reading))) return false;
        Reading *next = new (std::nothrow) Reading[n];
        if (!next) return false;
        const uint16_t kept = std::min(count, n);
        for (uint16_t i = 0; i < kept; ++i) next[i] = data[(head + capacity - kept + i) % capacity];
        delete[] data; data = next; capacity = n; count = kept;
        head = kept % n; // A full shrink must wrap before the next write.
        return true;
    }
    bool addMeasurement(float rssi, uint32_t now) {
        if (!std::isfinite(rssi) || rssi <= -128 || rssi > 20) return false;
        if (!capacity && !resize(20)) return false;
        expire(now);
        data[head] = {rssi, now}; head = (head + 1) % capacity;
        if (count < capacity) ++count;
        if (uint32_t(now - lastRate) > 10000) {
            const uint32_t elapsed = now - data[(head + capacity - count) % capacity].time;
            if (elapsed) {
                const float ideal = std::ceil(count * 15000.0f / elapsed) * 1.5f;
                const uint16_t n = uint16_t(std::max(10.0f, std::min(200.0f, ideal)));
                if (std::abs(int(n) - int(capacity)) > capacity / 4) resize(n);
            }
            lastRate = now;
        }
        return true;
    }
    bool statistics(uint32_t now, float reference, float &rssi, float &variance, float &distanceVariance) {
        expire(now); if (!count) return false;
        float values[MAX_READINGS];
        float sum = 0, square = 0, ds = 0, dss = 0;
        for (uint16_t i = 0; i < count; ++i) {
            const float v = data[(head + capacity - count + i) % capacity].rssi;
            values[i] = v; sum += v; square += v * v;
            const float d = std::pow(10.0f, (reference - v) / (10.0f * ABSORPTION)); ds += d; dss += d*d;
        }
        std::sort(values, values + count);
        auto percentile = [&](float p) {
            float pos = p * (count - 1); size_t lo = size_t(pos);
            return lo + 1 < count ? values[lo] + (values[lo+1] - values[lo]) * (pos-lo) : values[lo];
        };
        float q1 = percentile(.25f), q3 = percentile(.75f), span = 1.5f * (q3-q1), filtered = 0;
        unsigned survivors = 0;
        for (uint16_t i = 0; i < count; ++i) if (values[i] >= q1-span && values[i] <= q3+span) { filtered += values[i]; ++survivors; }
        rssi = survivors ? filtered / survivors : percentile(.5f);
        variance = std::max(0.0f, square/count - (sum/count)*(sum/count));
        distanceVariance = std::max(0.0f, dss/count - (ds/count)*(ds/count));
        return true;
    }
};

