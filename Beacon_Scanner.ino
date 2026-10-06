// SPDX-License-Identifier: AGPL-3.0-only
#include "Beacon_Scanner.h"
// Adapted from ESPresense f89e5519709277dc80dcf58897058de91d3da200.
// SPDX-License-Identifier: AGPL-3.0-only
// See docs/ESPRESENSE_INTEGRATION.md and docs/ESPRESENSE_LICENSE.txt.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace bletrack {
constexpr size_t LIMIT = 45; // Persisted whitelist capacity.
constexpr size_t TRACKED_LIMIT = 32;
constexpr size_t HEAP_RESERVE = 32 * 1024;
constexpr uint32_t FORGET_MS = 150000;
constexpr float ABSORPTION = 2.7f;
constexpr char ALWAYS_TRACK_IBEACON_UUID[] = "699ebc80-e1f3-11e3-9a0f-0cf3ee3bc012";
template<size_t N> inline void copy(char (&out)[N], const std::string &in);
template<size_t N> inline void copy(char (&out)[N], const char *in);
std::string hex(const uint8_t *p, size_t n);
int nibble(char c);
bool unhex(const std::string &s, uint8_t *out, size_t n);
std::string normalize(const std::string &s, char separator);
bool topicId(const std::string &s, size_t maxLen = 159);
bool validUtf8(const std::string &s);
bool aliasValid(const std::string &s);
std::string macNormalized(const std::string &s);

// ESPresense's statistics with a fixed 20-reading circular history and explicit time for
// deterministic tests, checked allocations and a fixed-size sorting scratchpad.
class AdaptivePercentileRSSI {
    // Aligned timestamps followed by byte RSSI values, in one allocation.
    uint32_t *data = nullptr;
    uint16_t capacity = 0, head = 0, count = 0;

    unsigned char *samples();
    float sample(size_t i);
public:
    static constexpr uint16_t MAX_READINGS = 20;
    // Production can reserve network/OTA headroom; host tests exercise OOM directly.
    static inline bool (*allocationAllowed)(size_t) = nullptr;
    AdaptivePercentileRSSI() = default;
    ~AdaptivePercentileRSSI();
    AdaptivePercentileRSSI(const AdaptivePercentileRSSI&) = delete;
    AdaptivePercentileRSSI &operator=(const AdaptivePercentileRSSI&) = delete;
    void reset();
    uint16_t size() const;
    uint16_t allocated() const;
    static size_t storageBytes(uint16_t n);
    void expire(uint32_t now);
    bool addMeasurement(float rssi, uint32_t now);
    bool statistics(uint32_t now, float reference, float &rssi, float &variance, float &distanceVariance);
};

struct BleFingerprint {
    char id[160] = {}, mac[13] = {}, name[64] = {}, beacon[128] = {};
    char manufacturer[24] = "Unknown", type[24] = "Unknown";
    int16_t priority = 0, classification = 0;
    int16_t manufacturerPowerPriority = 0;
    int16_t beaconRef = -128, manufacturerRef = -128, serviceRef = -128;
    float temp = 0, humidity = 0;
    uint16_t mv = 0;
    uint8_t battery = 255;
    bool temperaturePresent = false, humidityPresent = false;
    bool addressIndependentId = false;
    uint32_t advertiseCount = 0, uptimeSeconds = 0;
    bool setId(const std::string &value, int p, bool stableIdentity = false);
    void classify(const char *value, int p, const std::string &identifier = "");
    void manufacturerPower(int value,int p);
    int reference() const;
};
uint16_t be16(const uint8_t *p);
uint16_t le16(const uint8_t *p);
std::string uuid(const uint8_t *p);
bool eddystoneURL(const uint8_t *p, size_t n, std::string &url);
void manufacturerData(BleFingerprint &f, const uint8_t *p, size_t n, bool hasTx, int tx);
bool alwaysTrack(const BleFingerprint &f);

void serviceData(BleFingerprint &f, const std::string &service, const uint8_t *p, size_t n, bool hasTx, int tx);
struct DeviceConfig {
    uint32_t token = 0;
    char match[160] = {}, alias[64] = {};
    uint8_t irk[16] = {};
    bool hasIrk = false;
};
struct Configuration {
    uint32_t version = 1, revision = 0;
    bool only = false;
    uint8_t count = 0;
    DeviceConfig entries[LIMIT];
};
// Avoid `c = Configuration()`: GCC can materialize an 11 KiB stack temporary.
void resetConfig(Configuration &c);
bool configValid(const Configuration &c);
constexpr size_t MAX_CONFIG_BYTES=10+LIMIT*(7+159+63+16);
size_t encodedSize(const Configuration &c);
bool encodeConfig(const Configuration &c,uint8_t *out,size_t capacity);
bool decodeConfig(const uint8_t *in,size_t length,Configuration &c);
uint32_t reportDelay(float movement);
} // namespace bletrack

#ifndef BEACON_CORE_ONLY
#include <ArduinoJson.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <memory>
#include <atomic>

namespace bletrack {
struct Snapshot {
  BleFingerprint fp;
  char effective[160] = {}, alias[64] = {};
  uint32_t first = 0, last = 0, samples = 0, sequence = 0, computedSequence = 0, token = 0;
  uint64_t nextReport = 0;
  float raw = 0, rssi = 0, variance = 0, distance = 0, distanceVariance = 0, lastDistance = 0;
  bool valid = false, pending = false, close = false;
  bool privateIdentityMissing = false;
};
struct Tracked {
  Snapshot view;
  AdaptivePercentileRSSI filter;
};
struct Runtime {
  Configuration config;
  Tracked devices[TRACKED_LIMIT];
  uint32_t generation = 0;
};

extern Runtime *runtime;
extern SemaphoreHandle_t stateMutex, configurationMutex, scanMutex;
extern NimBLEScan *scanner;
extern bool scanActive, oneShotActive, oneShotStarted;
extern std::atomic<bool> activeScanRequested;
extern uint64_t retryScan;
struct Lock {
  SemaphoreHandle_t mutex;
  bool held;
  explicit Lock(SemaphoreHandle_t m, TickType_t wait = pdMS_TO_TICKS(25));
  ~Lock();
  void release();
  explicit operator bool() const;
};

bool initialize();
// IRKs are entered in conventional AES big-endian hex order. NimBLE addresses
// are little-endian (getVal); only resolvable private addresses are candidates.
bool resolves(const uint8_t irk[16], const uint8_t address[6], uint8_t addressType);
int matchConfig(const BleFingerprint &f, const uint8_t address[6], uint8_t addressType);
std::string serviceKey(std::string u);
bool serviceAdvertisement(BleFingerprint &f, const std::string &u, bool hasTx, int tx);

void decode(BleFingerprint &f, const NimBLEAdvertisedDevice *d);
std::string effectiveId(const BleFingerprint &f, int match);
void receive(const NimBLEAdvertisedDevice *d);

void calculate(Tracked &t, uint32_t now);
bool hasTrackableIdentity(const Snapshot &v);
bool eligible(const Snapshot &v);
bool due(const Snapshot &v, uint64_t now);

bool publish(const Snapshot &v);
bool appendConfigurationState(JsonDocument &doc);
const char *editConfiguration(JsonVariantConst command);
} // namespace bletrack
void setupBLE(boolean active);
boolean doBLE();
void pauseBLEForOTA();
void requestActiveBLEScan();
#endif



namespace bletrack {
// Identifier helpers, advertisement decoding and persisted configuration.
template<size_t N> inline void copy(char (&out)[N], const std::string &in) {
    const size_t n = std::min(N - 1, in.size());
    memcpy(out, in.data(), n); out[n] = 0;
}

template<size_t N> inline void copy(char (&out)[N], const char *in) {
    const size_t n = strnlen(in,N-1); memmove(out,in,n); out[n]=0;
}

std::string hex(const uint8_t *p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool unhex(const std::string &s, uint8_t *out, size_t n) {
    if (s.size() != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) {
        int a = nibble(s[2*i]), b = nibble(s[2*i+1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a * 16 + b);
    }
    return true;
}

std::string normalize(const std::string &s, char separator) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') out += char(c);
        else if (c >= 'A' && c <= 'Z') out += char(c + 32);
        else if (!out.empty() && out.back() != separator) out += separator;
    }
    while (!out.empty() && out.back() == separator) out.pop_back();
    return out;
}

bool topicId(const std::string &s, size_t maxLen) {
    if (s.empty() || s.size() > maxLen) return false;
    for (unsigned char c : s) if (c < 33 || c > 126 || c == '/' || c == '+' || c == '#') return false;
    return true;
}

bool validUtf8(const std::string &s) {
    for(size_t i=0;i<s.size();) {
        const uint8_t c=uint8_t(s[i++]);
        if(c<0x80) { if(!c) return false; continue; }
        unsigned extra; uint32_t value, minimum;
        if(c>=0xc2 && c<=0xdf) { extra=1; value=c&31; minimum=0x80; }
        else if(c>=0xe0 && c<=0xef) { extra=2; value=c&15; minimum=0x800; }
        else if(c>=0xf0 && c<=0xf4) { extra=3; value=c&7; minimum=0x10000; }
        else return false;
        if(i+extra>s.size()) return false;
        while(extra--) { const uint8_t b=uint8_t(s[i++]); if((b&0xc0)!=0x80) return false; value=(value<<6)|(b&63); }
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff)) return false;
    }
    return true;
}

bool aliasValid(const std::string &s) {
    if (s.size() > 63) return false;
    for (unsigned char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

std::string macNormalized(const std::string &s) {
    std::string result;
    if (s.size() != 12 && s.size() != 17) return result;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.size() == 17 && i % 3 == 2) { if (s[i] != ':') return ""; continue; }
        const int h = nibble(s[i]); if (h < 0) return "";
        result += "0123456789abcdef"[h];
    }
    return result;
}

uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) * 256 + p[1]; }

uint16_t le16(const uint8_t *p) { return uint16_t(p[1]) * 256 + p[0]; }

std::string uuid(const uint8_t *p) {
    return hex(p,4)+"-"+hex(p+4,2)+"-"+hex(p+6,2)+"-"+hex(p+8,2)+"-"+hex(p+10,6);
}

bool eddystoneURL(const uint8_t *p, size_t n, std::string &url) {
    static const char *const schemes[] = {"http://www.", "https://www.", "http://", "https://"};
    static const char *const expansions[] = {".com/", ".org/", ".edu/", ".net/", ".info/", ".biz/", ".gov/", ".com", ".org", ".edu", ".net", ".info", ".biz", ".gov"};
    url.clear(); if (n < 4 || n > 20 || p[0] != 0x10 || p[2] > 3) return false;
    for (size_t i = 3; i < n; ++i) if (p[i] > 13 && (p[i] < 0x21 || p[i] > 0x7e)) return false;
    url = schemes[p[2]];
    for (size_t i = 3; i < n; ++i) { if (p[i] < 14) url += expansions[p[i]]; else url += char(p[i]); }
    return true;
}

void manufacturerData(BleFingerprint &f, const uint8_t *p, size_t n, bool hasTx, int tx) {
    if (n < 2) return;
    const uint16_t company = le16(p);
    char unknown[16]; snprintf(unknown, sizeof unknown, "0x%04X", company);
    const char *maker = company == 0x004c ? "Apple" : company == 0x05a7 ? "Sonos" :
        company == 0x0087 ? "Garmin" : company == 0x0006 ? "Microsoft" : company == 0x0075 ? "Samsung" : unknown;
    // Only the block supporting the strongest format should label that format's maker.
    const int previous = f.classification;
    if (n == 25 && p[2] == 2 && p[3] == 0x15) {
        const std::string u = uuid(p+4);
        char suffix[24]; snprintf(suffix, sizeof suffix, "-%u-%u", be16(p+20), be16(p+22));
        f.setId("iBeacon:" + u + suffix, int8_t(p[24]) == 3 && u != ALWAYS_TRACK_IBEACON_UUID ? -10 : 180, true);
        f.beaconRef = int8_t(p[24]); f.classify("iBeacon", 180, u);
    } else if (n == 26 && p[2] == 0xbe && p[3] == 0xac) {
        const std::string u = uuid(p+4); char suffix[24];
        snprintf(suffix, sizeof suffix, "-%u-%u", be16(p+20), be16(p+22));
        f.setId("altBeacon:" + u + suffix, 175, true); f.beaconRef = int8_t(p[24]); f.classify("AltBeacon", 175, hex(p+4,20));
    } else if (company == 0x004c && n >= 4) {
        char value[64]; snprintf(value, sizeof value, "apple:%02x%02x:%u", p[2],p[3],unsigned(n));
        std::string id = value; if (hasTx) id += std::to_string(-tx);
        f.manufacturerPower(-65,150);
        if (p[2] == 0x10 && size_t(p[3]) + 4 <= n) { f.setId(id,150); f.classify("Apple Nearby",150); }
        else if (p[2] == 0x12 && n == 29 && size_t(p[3]) + 4 <= n) { f.setId("apple:findmy",32); f.classify("Apple Find My",150); }
        else { f.setId(id,-5); f.classify("Apple advertisement",30); }
    } else {
        const char *prefix = nullptr; int rank = 30; const char *type = "Unknown";
        if (company == 0x05a7) { prefix="sonos:"; rank=105; type="Sonos"; }
        else if (company == 0x0087) { prefix="garmin:"; rank=107; type="Garmin"; }
        else if (company == 0x4d4b) { prefix="iTrack:"; rank=127; type="iTrack"; }
        else if (company == 0x0157) { prefix="mifit:"; rank=115; type="Mi-fit"; }
        else if (company == 0x0075) { prefix="samsung:"; type="Samsung advertisement"; }
        if (prefix) { f.setId(std::string(prefix)+f.mac,rank); f.classify(type,rank); }
        else if (company == 6 && n == 29) {
            char id[32]; snprintf(id,sizeof id,"msft:cdp:%02x%02x",p[3],p[5]); f.setId(id,40); f.classify("Microsoft CDP",40);
        } else if (company) {
            char id[40]; snprintf(id,sizeof id,"md:%04x:%u",company,unsigned(n));
            f.setId(std::string(id)+(hasTx ? std::to_string(-tx) : ""),20);
        }
        if (hasTx) f.manufacturerPower(-65+tx,prefix ? rank : 20);
    }
    if (f.classification >= previous && (f.classification != previous || strcmp(f.manufacturer,"Unknown") == 0)) copy(f.manufacturer,maker);
}

bool alwaysTrack(const BleFingerprint &f) {
    return strcmp(f.type, "iBeacon") == 0 && strcmp(f.beacon, ALWAYS_TRACK_IBEACON_UUID) == 0;
}

void serviceData(BleFingerprint &f, const std::string &service, const uint8_t *p, size_t n, bool hasTx, int tx) {
    if (hasTx) f.serviceRef = -65 + tx;
    if (service == "fd6f") { f.setId("exp:"+std::to_string(n),120); f.beaconRef=-77; f.classify("Exposure notification",120); }
    else if (service == "fd5a") { f.setId("smarttag:"+std::to_string(n),121); f.classify("Samsung SmartTag",121); }
    else if (service == "181a" && (n == 13 || n == 15)) {
        if (n == 15) { f.temp=int16_t(le16(p+6))/100.0f; f.humidity=le16(p+8)/100.0f; f.mv=le16(p+10); f.battery=p[12]; }
        else { f.temp=int16_t(be16(p+6))/10.0f; f.humidity=p[8]; f.mv=be16(p+10); f.battery=p[9]; }
        f.temperaturePresent=f.humidityPresent=true; f.setId(std::string("miTherm:")+f.mac,110); f.classify("Mi thermometer",110);
    } else if (service == "feaa" && n) {
        if (p[0] == 0 && n == 20) {
            std::string value=hex(p+2,10)+"-"+hex(p+12,6);
            f.setId("eddy:"+value,170,true); f.beaconRef=int8_t(p[1])-41; f.classify("Eddystone UID",170,value);
        } else if (p[0] == 0x10) {
            std::string value; if (eddystoneURL(p,n,value)) { f.beaconRef=int8_t(p[1])-41; f.classify("Eddystone URL",170,value); }
        } else if (p[0] == 0x20 && n == 14 && p[1] == 0) {
            f.mv=be16(p+2); f.temperaturePresent=be16(p+4)!=0x8000;
            if (f.temperaturePresent) f.temp=int16_t(be16(p+4))/256.0f;
            f.advertiseCount=(uint32_t(be16(p+6))<<16)|be16(p+8);
            f.uptimeSeconds=((uint32_t(be16(p+10))<<16)|be16(p+12))/10;
            f.classify("Eddystone TLM",160);
        }
    }
}

void resetConfig(Configuration &c) {
    c.version=1; c.revision=0; c.only=false; c.count=0;
    for(auto &e:c.entries) {
        e.token=0; e.hasIrk=false;
        memset(e.match,0,sizeof e.match); memset(e.alias,0,sizeof e.alias); memset(e.irk,0,sizeof e.irk);
    }
}

bool configValid(const Configuration &c) {
    if (c.version != 1 || c.count > LIMIT) return false;
    for (size_t i=0;i<c.count;++i) {
        const auto &a=c.entries[i];
        if (!a.token || !memchr(a.match,0,sizeof a.match) || !memchr(a.alias,0,sizeof a.alias)) return false;
        if ((!a.match[0] && !a.hasIrk) || (a.match[0] && !topicId(a.match)) || !aliasValid(a.alias)) return false;
        for (size_t j=0;j<i;++j) {
            const auto &b=c.entries[j];
            if (a.token==b.token || (a.match[0] && strcmp(a.match,b.match)==0) ||
                (a.alias[0] && strcmp(a.alias,b.alias)==0) || (a.hasIrk && b.hasIrk && memcmp(a.irk,b.irk,16)==0)) return false;
        }
    }
    return true;
}

size_t encodedSize(const Configuration &c) {
    size_t n=10;
    for(size_t i=0;i<c.count;++i) n+=7+strlen(c.entries[i].match)+strlen(c.entries[i].alias)+(c.entries[i].hasIrk ? 16 : 0);
    return n;
}

bool encodeConfig(const Configuration &c,uint8_t *out,size_t capacity) {
    if(!configValid(c) || capacity<encodedSize(c)) return false;
    size_t pos=0;
    auto number=[&](uint32_t value) { for(unsigned i=0;i<4;++i) out[pos++]=uint8_t(value>>(8*i)); };
    memcpy(out,"BLE1",4); pos=4; number(c.revision); out[pos++]=c.only ? 1 : 0; out[pos++]=c.count;
    for(size_t i=0;i<c.count;++i) {
        const auto &e=c.entries[i]; size_t m=strlen(e.match), a=strlen(e.alias);
        number(e.token); out[pos++]=uint8_t(m); out[pos++]=uint8_t(a); out[pos++]=e.hasIrk ? 1 : 0;
        memcpy(out+pos,e.match,m); pos+=m; memcpy(out+pos,e.alias,a); pos+=a;
        if(e.hasIrk) { memcpy(out+pos,e.irk,16); pos+=16; }
    }
    return true;
}

bool decodeConfig(const uint8_t *in,size_t length,Configuration &c) {
    if(length<10 || length>MAX_CONFIG_BYTES || memcmp(in,"BLE1",4)!=0 || in[8]>1 || in[9]>LIMIT) return false;
    resetConfig(c); size_t pos=4;
    auto number=[&]() { uint32_t value=0; for(unsigned i=0;i<4;++i) value|=uint32_t(in[pos++])<<(8*i); return value; };
    c.revision=number(); c.only=in[pos++]!=0; c.count=in[pos++];
    for(size_t i=0;i<c.count;++i) {
        if(length-pos<7) return false;
        auto &e=c.entries[i]; e.token=number(); const size_t m=in[pos++], a=in[pos++]; const uint8_t flags=in[pos++];
        if(m>=sizeof e.match || a>=sizeof e.alias || flags>1 || length-pos<m+a+(flags ? 16 : 0)) return false;
        if(memchr(in+pos,0,m+a)) return false;
        memcpy(e.match,in+pos,m); pos+=m; memcpy(e.alias,in+pos,a); pos+=a;
        e.hasIrk=flags!=0; if(e.hasIrk) { memcpy(e.irk,in+pos,16); pos+=16; }
    }
    return pos==length && configValid(c);
}

uint32_t reportDelay(float movement) {
    // Equivalent to upstream log2(round(pow(2,movement/0.5))) with bounded input.
    const float steps = std::min(10.0f, std::max(0.0f,movement/.5f));
    int rounded = int(std::log2(std::round(std::pow(2.0f,steps))));
    return 5000 / std::max(2,std::min(10,10-rounded));
}

// RSSI history and fingerprint methods.
unsigned char *AdaptivePercentileRSSI::samples() { return reinterpret_cast<unsigned char *>(data+capacity); }

float AdaptivePercentileRSSI::sample(size_t i) { const unsigned char v=samples()[i]; return v<128 ? float(v) : float(int(v)-256); }

AdaptivePercentileRSSI::~AdaptivePercentileRSSI() { delete[] data; }

void AdaptivePercentileRSSI::reset() { delete[] data; data = nullptr; capacity = head = count = 0; }

uint16_t AdaptivePercentileRSSI::size() const { return count; }

uint16_t AdaptivePercentileRSSI::allocated() const { return capacity; }

size_t AdaptivePercentileRSSI::storageBytes(uint16_t n) { return ((size_t(n)*5+3)/4)*4; }

void AdaptivePercentileRSSI::expire(uint32_t now) {
        while (count && uint32_t(now - data[(head + capacity - count) % capacity]) > 15000) --count;
    }

bool AdaptivePercentileRSSI::addMeasurement(float rssi, uint32_t now) {
        if (!std::isfinite(rssi) || rssi <= -128 || rssi > 20 || std::trunc(rssi)!=rssi) return false;
        if (!capacity) {
            const size_t bytes=storageBytes(MAX_READINGS);
            if(allocationAllowed && !allocationAllowed(bytes)) return false;
            data=new (std::nothrow) uint32_t[bytes/4];
            if(!data) return false;
            capacity=MAX_READINGS;
        }
        expire(now);
        data[head]=now; samples()[head]=static_cast<unsigned char>(int(rssi)); head = (head + 1) % capacity;
        if (count < capacity) ++count;
        return true;
    }

bool AdaptivePercentileRSSI::statistics(uint32_t now, float reference, float &rssi, float &variance, float &distanceVariance) {
        expire(now); if (!count) return false;
        float values[MAX_READINGS];
        float sum = 0, square = 0, ds = 0, dss = 0;
        for (uint16_t i = 0; i < count; ++i) {
            const float v = sample((head + capacity - count + i) % capacity);
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

bool BleFingerprint::setId(const std::string &value, int p, bool stableIdentity) {
        if ((priority < 0 && p < 0 && p >= priority) || (priority > 0 && p <= priority) || !topicId(value)) return false;
        copy(id, value); priority = p; addressIndependentId=stableIdentity; return true;
    }

void BleFingerprint::classify(const char *value, int p, const std::string &identifier) {
        if (p < classification) return;
        copy(type, value); classification = p;
        if (!identifier.empty()) copy(beacon, identifier);
    }

void BleFingerprint::manufacturerPower(int value,int p) { if(p>=manufacturerPowerPriority) { manufacturerRef=value; manufacturerPowerPriority=p; } }

int BleFingerprint::reference() const { return beaconRef != -128 ? beaconRef : manufacturerRef != -128 ? manufacturerRef : serviceRef != -128 ? serviceRef : -71; }
} // namespace bletrack

#ifndef BEACON_CORE_ONLY
// Scanner runtime, tracking and MQTT reporting.
namespace bletrack {
Runtime *runtime = nullptr;
SemaphoreHandle_t stateMutex = nullptr, configurationMutex = nullptr, scanMutex = nullptr;
NimBLEScan *scanner = nullptr;
bool scanActive = false;
std::atomic<bool> activeScanRequested{ false };
bool oneShotActive = false, oneShotStarted = false;
uint64_t retryScan = 0;
Lock::Lock(SemaphoreHandle_t m, TickType_t wait)
  : mutex(m), held(m && xSemaphoreTake(m, wait) == pdTRUE) {}
Lock::~Lock() { if (held) xSemaphoreGive(mutex); }
void Lock::release() { if (held) { xSemaphoreGive(mutex); held = false; } }
Lock::operator bool() const { return held; }

bool initialize() {
  if (runtime) return true;
  if (!stateMutex) stateMutex = xSemaphoreCreateMutex();
  if (!configurationMutex) configurationMutex = xSemaphoreCreateMutex();
  if (!scanMutex) scanMutex = xSemaphoreCreateMutex();
  if (!stateMutex || !configurationMutex || !scanMutex) return false;
  AdaptivePercentileRSSI::allocationAllowed = [](size_t bytes) {
    return ESP.getFreeHeap() > bytes + HEAP_RESERVE;
  };
  if (ESP.getFreeHeap() < sizeof(Runtime) + HEAP_RESERVE) return false;
  auto *next = new (std::nothrow) Runtime();
  if (!next) return false;
  Preferences prefs;
  // Opening read/write distinguishes a new namespace from a storage failure.
  // Existing blobs are only read here; defaults do not overwrite saved settings.
  if (prefs.begin("bletracking", false)) {
    if (prefs.isKey("config")) {
      const size_t length = prefs.getBytesLength("config");
      std::unique_ptr<uint8_t[]> bytes;
      if (length >= 10 && length <= MAX_CONFIG_BYTES && ESP.getFreeHeap() > length + HEAP_RESERVE) bytes.reset(new (std::nothrow) uint8_t[length]);
      if (!bytes || prefs.getBytes("config", bytes.get(), length) != length || !decodeConfig(bytes.get(), length, next->config)) {
        resetConfig(next->config);
        next->config.only = true;
        log_e("Invalid BLE configuration; only built-in iBeacons will be published until configuration is saved");
      }
    }
    prefs.end();
  } else {
    next->config.only = true;
    log_e("Cannot load BLE configuration; only built-in iBeacons will be published until configuration is saved");
  }
runtime = next;
  return true;
}

bool resolves(const uint8_t irk[16], const uint8_t address[6], uint8_t addressType) {
  if ((addressType != BLE_ADDR_RANDOM && addressType != BLE_ADDR_RANDOM_ID) || (address[5] & 0xc0) != 0x40) return false;
  uint8_t plain[16] = {}, cipher[16] = {};
  plain[13] = address[5];
  plain[14] = address[4];
  plain[15] = address[3];
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_enc(&aes, irk, 128) == 0 && mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plain, cipher) == 0;
  mbedtls_aes_free(&aes);
  return ok && cipher[13] == address[2] && cipher[14] == address[1] && cipher[15] == address[0];
}

int matchConfig(const BleFingerprint &f, const uint8_t address[6], uint8_t addressType) {
  const auto &c = runtime->config;
  for (size_t i = 0; i < c.count; ++i)
    if (c.entries[i].hasIrk && resolves(c.entries[i].irk, address, addressType)) return int(i);
  for (size_t i = 0; i < c.count; ++i)
    if (strcmp(c.entries[i].match, f.mac) == 0) return int(i);
  for (size_t i = 0; i < c.count; ++i)
    if (strcmp(c.entries[i].match, f.id) == 0) return int(i);
  return -1;
}

std::string serviceKey(std::string u) {
  if (u.compare(0, 2, "0x") == 0) u.erase(0, 2);
  if (u.size() == 36 && u.compare(0, 4, "0000") == 0 && u.compare(8, 28, "-0000-1000-8000-00805f9b34fb") == 0) u = u.substr(4, 4);
  return u;
}

bool serviceAdvertisement(BleFingerprint &f, const std::string &u, bool hasTx, int tx) {
  struct Signature {
    const char *uuid;
    const char *prefix;
    const char *type;
    int priority;
    int fallback;
  };
  static const Signature known[] = {
    { "feed", "tile:", "Tile", 135, -4 }, { "fe07", "sonos:", "Sonos", 105, -128 }, { "ffe0", "itag:", "iTag", 125, -10 }, { "0f3e", "trackr:", "TrackR", 130, -128 }, { "20130001-0719-4b6e-be5d-158ab92fa5a4", "tractive:", "Tractive", 142, -128 }, { "6acc5540-e631-4069-944d-b8ca7598ad50", "vanmoof:", "VanMoof", 145, -128 }, { "a75cc7fc-c956-488f-ac2a-2dbc08b63a04", "meater:", "Meater", 140, -128 }, { "1803", "nut:", "Nut", 128, -12 }, { "fe95", "flora:", "Mi Flora", 129, -10 }, { "febc", "dexa:", "Dexa", 146, -128 }
  };
  for (const auto &k : known)
    if (u == k.uuid) {
      if (f.setId(std::string(k.prefix) + f.mac, k.priority)) {
        int power = u == "feed" ? -4 : hasTx ? tx
                                             : k.fallback;
        if (power != -128) f.serviceRef = -65 + power;
      }
      f.classify(k.type, k.priority);
      return true;
    }
  return false;
}

void decode(BleFingerprint &f, const NimBLEAdvertisedDevice *d) {
  copy(f.mac, macNormalized(d->getAddress().toString()));
  if (!f.id[0]) {
    const auto address = d->getAddress();
    const auto a = address.getVal();
    const uint8_t t = d->getAddressType();
    int rank = (t == BLE_ADDR_PUBLIC || t == BLE_ADDR_PUBLIC_ID) ? 55 : (a[5] & 0xc0) == 0xc0 ? 5
                                                                                              : 1;
    f.setId(f.mac, rank);
  }
  if (d->haveName()) {
    std::string n = d->getName();
    // Legacy ESP32 advertisements have at most 31 bytes per packet; cap all externally sourced text anyway.
    if (!n.empty() && n.size() < sizeof f.name && validUtf8(n)) {
      copy(f.name, n);
      f.setId("name:" + normalize(n, '-'), 35);
    }
  }
  const bool hasTx = d->haveTXPower();
  const int tx = d->getTXPower();
  std::string ad = "ad:", sd = "sd:";
  bool knownService = false, adOverflow = false, sdOverflow = false;
  for (uint8_t i = 0; i < d->getServiceUUIDCount(); ++i) {
    std::string u = d->getServiceUUID(i).toString();
    if (ad.size() + u.size() < sizeof f.id) ad += u;
    else adOverflow = true;
    knownService = serviceAdvertisement(f, serviceKey(u), hasTx, tx) || knownService;
  }
  if (!knownService && !adOverflow && ad.size() > 3) {
    if (hasTx) {
      ad += std::to_string(-tx);
      f.serviceRef = -65 + tx;
    }
    f.setId(ad, 10);
  }
  for (uint8_t i = 0; i < d->getServiceDataCount(); ++i) {
    std::string u = d->getServiceDataUUID(i).toString(), data = d->getServiceData(i);
    if (data.size() > 255) continue;
    const auto key = serviceKey(u);
    serviceData(f, key, reinterpret_cast<const uint8_t *>(data.data()), data.size(), hasTx, tx);
    if (key != "fd6f" && key != "fd5a" && key != "181a") {
      if (sd.size() + u.size() < sizeof f.id) sd += u;
      else sdOverflow = true;
    }
  }
  if (!sdOverflow && sd.size() > 3) {
    if (hasTx) sd += std::to_string(-tx);
    f.setId(sd, 15);
  }
  for (uint8_t i = 0; i < d->getManufacturerDataCount(); ++i) {
    std::string data = d->getManufacturerData(i);
    if (data.size() <= 255) manufacturerData(f, reinterpret_cast<const uint8_t *>(data.data()), data.size(), hasTx, tx);
  }
}

std::string effectiveId(const BleFingerprint &f, int match) {
  if (match < 0) return f.id;
  const auto &c = runtime->config.entries[match];
  if (c.alias[0]) return c.alias;
  if (c.hasIrk) {
    if (c.match[0]) return c.match;
    char id[24];
    snprintf(id, sizeof id, "device:%08" PRIx32, c.token);
    return id;
  }
  return f.id;
}

void receive(const NimBLEAdvertisedDevice *d) {
  if (ProcessingOTA) return;
  if (!d) return;
  if (!runtime) {
    log_w("BLE receive skipped: tracking runtime unavailable");
    return;
  }
  if (d->getPayload().size() > 255) {
    log_d("BLE receive skipped: payload exceeds 255 bytes");
    return;
  }
  Lock lock(stateMutex);
  if (!lock) {
    log_d("BLE receive skipped: tracking state busy");
    return;
  }
  if (ProcessingOTA) return;  // OTA may have started while this callback waited.
  const uint32_t now = millis();
  BleFingerprint candidate;
  decode(candidate, d);
  const auto addressObject = d->getAddress();
  const auto address = addressObject.getVal();
  int match = matchConfig(candidate, address, d->getAddressType());
  const bool resolvedIrk = match >= 0 && runtime->config.entries[match].hasIrk && resolves(runtime->config.entries[match].irk, address, d->getAddressType());
  const uint32_t token = match >= 0 ? runtime->config.entries[match].token : 0;
  Tracked *entry = nullptr;
  for (auto &t : runtime->devices) {
    if (!t.view.valid) continue;
    if (uint32_t(now - t.view.last) > FORGET_MS) {
      t.view = Snapshot();
      t.filter.reset();
      continue;
    }
    // Explicit configuration binds rotating addresses. Unconfigured model-level
    // fingerprints are never merged: different physical devices can share them.
    if ((resolvedIrk && token == t.view.token) || strcmp(t.view.fp.mac, candidate.mac) == 0) {
      entry = &t;
      break;
    }
  }
  if (entry) {
    candidate = entry->view.fp;
    decode(candidate, d);
    match = matchConfig(candidate, address, d->getAddressType());
  }
  if (runtime->config.only && match < 0 && !alwaysTrack(candidate)) return;
  if (!entry) {
    for (auto &t : runtime->devices)
      if (!t.view.valid) {
        entry = &t;
        break;
      }
    if (!entry) {
      // Ordinary traffic must not evict the built-in beacon family.
      for (auto &t : runtime->devices) {
        if (alwaysTrack(t.view.fp)) continue;
        if (!entry || uint32_t(now - t.view.last) > uint32_t(now - entry->view.last)) entry = &t;
      }
      if (!entry && alwaysTrack(candidate)) {
        entry = &*std::max_element(std::begin(runtime->devices), std::end(runtime->devices), [now](const Tracked &a, const Tracked &b) {
          return uint32_t(now - a.view.last) < uint32_t(now - b.view.last);
        });
      }
      if (!entry) return;
    }
    entry->view = Snapshot();
    entry->filter.reset();
    entry->view.first = now;
  }
  const int rawRssi = d->getRSSI();
  const bool filtered = entry->filter.addMeasurement(rawRssi, now);
  // A required beacon can use the snapshot's latest reading when history
  // allocation is denied. Keep the network/OTA reserve and reject invalid RSSI.
  if (!filtered && (!alwaysTrack(candidate) || rawRssi <= -128 || rawRssi > 20)) {
    log_d("BLE tracking skipped: id=%s RSSI=%d freeHeap=%lu historyCapacity=%u (allocation requires 32 KiB reserve)",
          candidate.id, d->getRSSI(), (unsigned long)ESP.getFreeHeap(), unsigned(entry->filter.allocated()));
    return;
  }
  auto &v = entry->view;
  const std::string identity = effectiveId(candidate, match);
  if (strcmp(v.effective, identity.c_str()) != 0) {
    v.nextReport = 0;
    v.lastDistance = 0;
  }
  v.fp = candidate;
  copy(v.effective, identity);
  v.alias[0] = 0;
  if (match >= 0) copy(v.alias, runtime->config.entries[match].alias);
  v.token = match >= 0 ? runtime->config.entries[match].token : 0;
  v.privateIdentityMissing = (addressObject.isRpa() || addressObject.isNrpa()) && !candidate.addressIndependentId && !(match >= 0 && runtime->config.entries[match].hasIrk && resolves(runtime->config.entries[match].irk, address, d->getAddressType()));
  v.last = now;
  v.raw = d->getRSSI();
  ++v.samples;
  ++v.sequence;
  v.pending = true;
  v.valid = true;
  if (v.samples == 1 || !filtered) {
    v.rssi = v.raw;
    v.distance = std::pow(10.0f, (v.fp.reference() - v.rssi) / (10.0f * ABSORPTION));
  }
  if (!filtered) {
    v.variance = v.distanceVariance = 0;
    if (!v.close && v.rssi > -40) v.close = true;
    else if (v.close && v.rssi < -50) v.close = false;
    v.computedSequence = v.sequence;
    log_d("BLE required beacon using latest RSSI without history: id=%s freeHeap=%lu",
          v.effective, (unsigned long)ESP.getFreeHeap());
  }
  if (alwaysTrack(v.fp)) {
    log_d("BLE tracked: mac=%s id=%s samples=%lu RSSI=%.0f distance=%.2f MQTT=%s",
          v.fp.mac, v.effective, (unsigned long)v.samples, v.raw, v.distance,
          mqttClient.connected() ? "connected" : "disconnected");
  }
}

void calculate(Tracked &t, uint32_t now) {
  auto &v = t.view;
  if (t.filter.statistics(now, v.fp.reference(), v.rssi, v.variance, v.distanceVariance)) {
    v.distance = std::pow(10.0f, (v.fp.reference() - v.rssi) / (10.0f * ABSORPTION));
    if (!v.close && v.rssi > -40) v.close = true;
    else if (v.close && v.rssi < -50) v.close = false;
    v.computedSequence = v.sequence;
  }
}

bool hasTrackableIdentity(const Snapshot &v) {
  // Names, model/service signatures and MAC aliases cannot bind a rotating
  // address. Require a decoded beacon identity or an actually resolved IRK.
  return !v.privateIdentityMissing;
}

bool eligible(const Snapshot &v) {
  // Configured identities take the place of upstream known-IRK/alias precedence.
  return v.valid && v.pending && hasTrackableIdentity(v) && std::isfinite(v.distance) && (alwaysTrack(v.fp) || ((v.token || v.fp.priority > 1) && v.distance <= 16));
}

bool due(const Snapshot &v, uint64_t now) {
  if (!eligible(v) || uint32_t(millis() - v.last) > 15000) return false;
  if (now >= v.nextReport) return true;
  const float movement = std::fabs(v.distance - v.lastDistance);
  const uint32_t advance = reportDelay(movement);
  return movement >= .5f && v.nextReport - now <= advance;
}

bool publish(const Snapshot &v) {
  if (!hasTrackableIdentity(v)) {
    log_d("Lacking TrackableIdentity, skipping publish!");
    return false;
  }
  if (!mqttClient.connected()) return false;

  std::string room = normalize(Room, '_');
  if (room.empty()) room = normalize(hostName(), '_');
  char topic[256], payload[1024];
  int n = snprintf(topic, sizeof topic, "espresense/devices/%s/%s", v.effective, room.c_str());
  if (n < 0 || size_t(n) >= sizeof topic) return false;
  size_t used = 0;
  bool ok = true;
  auto append = [&](const char *text) {
    const size_t n = strlen(text);
    if (n >= sizeof(payload) - used) {
      ok = false;
      log_i("publishing %lu bytes overruns 1024 byte payload", (unsigned long)n);
      return;
    }
    memcpy(payload + used, text, n);
    used += n;
    payload[used] = 0;
  };
  auto quoted = [&](const char *text) {
    append("\"");
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
      char escaped[7] = {};
      if (*p == '"' || *p == '\\') {
        escaped[0] = '\\';
        escaped[1] = char(*p);
      } else if (*p < 32) snprintf(escaped, sizeof escaped, "\\u%04x", *p);
      else escaped[0] = char(*p);
      append(escaped);
    }
    append("\"");
  };
  auto number = [&](const char *key, float value) {
    if (!std::isfinite(value)) {
      ok = false;
      return;
    }
    char numeric[48];
    int n = snprintf(numeric, sizeof numeric, "%.2f", value);
    if (n < 0 || size_t(n) >= sizeof numeric) {
      ok = false;
      log_d("sprintf numeric overrun");
      return;
    }
    append(",\"");
    append(key);
    append("\":");
    append(numeric);
  };
  auto integer = [&](const char *key, int64_t value) {
    char numeric[24];
    snprintf(numeric, sizeof numeric, "%" PRId64, value);
    append(",\"");
    append(key);
    append("\":");
    append(numeric);
  };
  append("{\"mac\":");
  quoted(v.fp.mac);
  append(",\"id\":");
  quoted(v.effective);
  if (v.alias[0] || v.fp.name[0]) {
    append(",\"name\":");
    quoted(v.alias[0] ? v.alias : v.fp.name);
  }
  integer("rssi@1m", v.fp.reference());
  integer("rxAdj", 0);
  number("rssi", v.rssi);
  if (v.variance > 0) number("rssiVar", v.variance);
  number("distance", v.distance);
  number("var", v.distanceVariance);
  if (v.close) append(",\"close\":true");
  integer("int", v.samples ? uint32_t(v.last - v.first) / v.samples : 0);
  if (v.fp.mv) integer("mV", v.fp.mv);
  if (v.fp.battery != 255) integer("batt", v.fp.battery);
  if (v.fp.temperaturePresent) number("temp", v.fp.temp);
  if (v.fp.humidityPresent) number("rh", v.fp.humidity);
  append("}");
  if (!ok) {
    log_d("Not ok, cannot publish");
    return false;
  }
  Publish_BLE_Attempts++;
  boolean success = false;
  log_d("Publishing '%s'", payload);
  success = mqttClient.publish(topic, 0, false, payload, used) != 0;
  if (success) {
    log_d("Published BLE payload to '%s'", topic);
    LastPublishTimeBLE = TimeNow;
  }
  return (success);
}
class Callbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *device) override {
    receive(device);
  }
};
static Callbacks callbacks;

// Redacted BLE configuration response, including immutable tracking rules.
bool appendConfigurationState(JsonDocument &doc) {
  auto fixed = doc["always_tracked"].to<JsonArray>();
  auto item = fixed.add<JsonObject>(); item["ibeacon_uuid"] = ALWAYS_TRACK_IBEACON_UUID; item["read_only"] = true;
  if (runtime) {
    Lock lock(stateMutex); if (!lock) return false;
    const auto &config = runtime->config;
    doc["ble_revision"] = config.revision; doc["only"] = config.only; doc["capacity"] = LIMIT;
    auto entries = doc["entries"].to<JsonArray>();
    for (size_t i = 0; i < config.count; ++i) {
      auto entry = entries.add<JsonObject>(); const auto &c = config.entries[i];
      entry["token"] = c.token; entry["match"] = c.match; entry["alias"] = c.alias; entry["irk_set"] = c.hasIrk;
    }
  }
  return true;
}

// Revisioned configurable-whitelist updates.
const char *editConfiguration(JsonVariantConst command) {
  if (!runtime) return "BLE unavailable";
  if (!command["revision"].is<uint32_t>()) return "revision required";
  Lock editing(configurationMutex);
  if (!editing) return "configuration busy";
  if (ESP.getFreeHeap() < sizeof(Configuration) + MAX_CONFIG_BYTES + HEAP_RESERVE) return "insufficient heap";
  auto next = std::unique_ptr<Configuration>(new(std::nothrow) Configuration());
  if (!next) return "allocation failed";
  { Lock state(stateMutex); if (!state) return "BLE busy"; *next = runtime->config; }
  if (command["revision"].as<uint32_t>() != next->revision) return "stale revision";
  if (next->revision == UINT32_MAX) return "revision exhausted";
  const std::string operation = command["operation"] | "";
  uint32_t token = command["token"] | uint32_t(0);
  size_t index = next->count;
  for (size_t i = 0; i < next->count; ++i) if (next->entries[i].token == token) index = i;
  if (operation == "ble_mode") {
    if (!command["only"].is<bool>()) return "only must be boolean";
    next->only = command["only"].as<bool>();
  } else if (operation == "ble_delete") {
    if (!command["token"].is<uint32_t>() || index == next->count) return "entry not found";
    for (size_t i = index + 1; i < next->count; ++i) next->entries[i - 1] = next->entries[i];
    next->entries[--next->count] = DeviceConfig();
  } else if (operation == "ble_save") {
    if ((!command["token"].isNull() && !command["token"].is<uint32_t>()) ||
        (token && index == next->count) || index >= LIMIT) return "entry missing or whitelist full";
    if (!command["match"].is<const char *>() || !command["alias"].is<const char *>()) return "match and alias required";
    std::string identifier = command["match"].as<std::string>(), alias = command["alias"].as<std::string>();
    const std::string mac = macNormalized(identifier); if (!mac.empty()) identifier = mac;
    if ((!identifier.empty() && !topicId(identifier)) || !aliasValid(alias)) return "invalid identifier or alias";
    auto &entry = next->entries[index];
    if (!token) {
      entry = DeviceConfig(); bool duplicate;
      do { entry.token = esp_random(); duplicate = !entry.token;
        for (size_t i = 0; i < next->count; ++i) duplicate |= next->entries[i].token == entry.token;
      } while (duplicate);
      ++next->count;
    }
    copy(entry.match, identifier); copy(entry.alias, alias);
    if (!command["remove_key"].isNull() && !command["remove_key"].is<bool>()) return "remove_key must be boolean";
    const bool remove = command["remove_key"] | false;
    if (remove) { memset(entry.irk, 0, sizeof entry.irk); entry.hasIrk = false; }
    if (!command["irk"].isNull()) {
      if (!command["irk"].is<const char *>()) return "invalid IRK";
      const auto key = command["irk"].as<std::string>();
      if (!key.empty()) {
        if (remove || !unhex(key, entry.irk, 16)) return "IRK must be 32 hex characters";
        entry.hasIrk = true;
      }
    }
  } else return "unknown BLE operation";
  if (!configValid(*next)) return "identifiers, aliases and IRKs must be valid and unique";
  ++next->revision;
  const size_t length = encodedSize(*next);
  std::unique_ptr<uint8_t[]> bytes(new(std::nothrow) uint8_t[length]);
  if (!bytes || !encodeConfig(*next, bytes.get(), length)) return "encoding failed";
  Lock state(stateMutex, pdMS_TO_TICKS(250)); if (!state) return "BLE busy";
  Preferences prefs;
  if (!prefs.begin("bletracking", false)) return "NVS unavailable";
  const size_t saved = prefs.putBytes("config", bytes.get(), length); prefs.end();
  if (saved != length) return "NVS write failed; settings unchanged";
  runtime->config = *next; ++runtime->generation;
  for (auto &t : runtime->devices) { t.view = Snapshot(); t.filter.reset(); }
  return nullptr;
}
} // namespace bletrack
/* Advertisement-only ESPresense adapter; see docs/ESPRESENSE_INTEGRATION.md.
 * SPDX-License-Identifier: AGPL-3.0-only
 */
#ifdef SCAN_BLE

void setupBLE(boolean active) {
  using namespace bletrack;
  if (!initialize()) return;
  Lock scanLock(scanMutex, portMAX_DELAY);
  if (!scanLock || ProcessingOTA) return;
  if (!NimBLEDevice::init("")) {
    log_e("NimBLE initialization failed");
    return;
  }
  scanner = NimBLEDevice::getScan();
  if (!scanner) return;
  if (scanner->isScanning()) scanner->stop();
  scanActive = active;
  oneShotActive = false;
  oneShotStarted = false;
  scanner->setScanCallbacks(&callbacks, true);
  scanner->setDuplicateFilter(false);
  scanner->setMaxResults(0);
  scanner->setActiveScan(active);
  scanner->setInterval(128);
  scanner->setWindow(128);
  retryScan = 0;
}

// Called synchronously by OTA entry points, before Update.begin allocates buffers.
// Scan transitions and callbacks cannot recreate histories after this returns.
void pauseBLEForOTA() {
  using namespace bletrack;
  if (!runtime) return;
  Lock scanLock(scanMutex, portMAX_DELAY);
  if (!scanLock) return;
  if (scanner && scanner->isScanning() && !scanner->stop()) log_e("Cannot stop BLE scan for OTA; will retry");
  scanActive = false;
  oneShotActive = oneShotStarted = false;
  retryScan = 0;
  Lock state(stateMutex, portMAX_DELAY);
  if (!state) return;
  for (auto &t : runtime->devices) {
    t.filter.reset();
    t.view = Snapshot();
  }
}

// Queue one five-second active scan. doBLE() owns all scanner transitions;
// callers may request this from another task without touching NimBLE state.
void requestActiveBLEScan() {
  log_i("Next scan will be Active BLE");
  bletrack::activeScanRequested.store(true);
}

boolean doBLE() {
  log_d("Starting doBLE...");
  using namespace bletrack;
  if (ProcessingOTA) {
    pauseBLEForOTA();
    return false;
  }
  if (!runtime) {
    log_d("doBLE gave up, no runtime!");
    return (false);
  }
  if (!scanner) setupBLE(false);
  if (!scanner) {
    log_d("doBLE gave up, no scanner!");
    return (false);
  }
  Lock scanLock(scanMutex);
  if (!scanLock || ProcessingOTA) return false;

  const uint64_t nowMs = uint64_t(esp_timer_get_time()) / 1000;
  if (oneShotStarted && !scanner->isScanning()) {
    oneShotStarted = false;
    oneShotActive = false;
    scanActive = false;
    retryScan = 0;
  }
  if (activeScanRequested.load()) {
    // Requests received during the same active scan coalesce. A failed stop
    // leaves the request queued, and an OTA pause leaves it untouched.
    if (oneShotActive || !scanner->isScanning() || scanner->stop()) {
      activeScanRequested.exchange(false);
      if (!oneShotActive) {
        oneShotActive = true;
        scanActive = true;
        retryScan = 0;
      }
    }
  }
  if (!scanner->isScanning() && nowMs >= retryScan) {
    scanner->setActiveScan(scanActive);
    if (!scanner->start(oneShotActive ? 5000 : 0, false)) log_w("BLE scan start failed; retrying");
    else if (oneShotActive) oneShotStarted = true;
    retryScan = nowMs + 3000;
  }
  scanLock.release();
  const uint32_t now = millis();
  Total_BLE_Records_Last_Pub = 0;
  Skipped_BLE_Records_Last_Pub = 0;
  for (size_t i = 0; i < TRACKED_LIMIT; ++i) {
    if (ProcessingOTA) return false;
    // Serialize settings activation with publication, without holding the state
    // lock over network operations. All writers take these locks in this order.
    Lock configLock(configurationMutex, 0);
    if (!configLock) return (false);
    Snapshot snapshot;
    bool send = false;

    {
      Lock state(stateMutex);
      if (!state) continue;
      auto &t = runtime->devices[i];
      if (!t.view.valid) continue;
      if (uint32_t(now - t.view.last) > FORGET_MS) {
        t.view = Snapshot();
        t.filter.reset();
        continue;
      }
      if (t.view.sequence != t.view.computedSequence) calculate(t, now);
      if (t.view.pending && mqttClient.connected()) {
        if (due(t.view, nowMs)) {
          snapshot = t.view;
          send = true;
          log_d("Sending due");
        }
      }
    }
    if (send && publish(snapshot)) {
      Total_BLE_Records_Last_Pub++;
      Lock state(stateMutex);
      if (!state) continue;
      auto &v = runtime->devices[i].view;
      if (v.valid && strcmp(v.effective, snapshot.effective) == 0 && strcmp(v.fp.mac, snapshot.fp.mac) == 0) {
        v.nextReport = nowMs + (5000 - nowMs % 5000);
        v.lastDistance = snapshot.distance;
        if (v.sequence == snapshot.sequence) v.pending = false;
      }
    }
    if (!send) Skipped_BLE_Records_Last_Pub++;
  }
  log_d("Completed doBLE,  published %lu to MQTT, skipped %lu", Total_BLE_Records_Last_Pub, Skipped_BLE_Records_Last_Pub);
  return (true);
}
#endif

#endif // BEACON_CORE_ONLY

#ifndef BEACON_CORE_ONLY
bool bleScannerIsRunning() {
  return bletrack::scanner && bletrack::scanner->isScanning();
}

bool bleConfigurationRevision(uint32_t &revision) {
  if (!bletrack::runtime) return false;
  revision = bletrack::runtime->config.revision;
  return true;
}

bool appendBLEConfigurationState(JsonDocument &doc) {
  return bletrack::appendConfigurationState(doc);
}

const char *editBLEConfiguration(JsonVariantConst command) {
  return bletrack::editConfiguration(command);
}

bool prepareForFirmwareUpdate() {
  pauseBLEForOTA();
  return !bleScannerIsRunning();
}

void resumeAfterFirmwareUpdate() {
  setupBLE(false);
}

bool decodeHexString(const std::string &value, uint8_t *out, size_t size) {
  return bletrack::unhex(value, out, size);
}
#endif
