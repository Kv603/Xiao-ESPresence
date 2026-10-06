#pragma once
#include <ArduinoJson.h>
#include <array>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include <functional>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using boolean=bool;
static uint32_t clockMs=0;
inline uint32_t millis(){return clockMs;}
using TickType_t=uint32_t;
struct TestMutex { bool locked=false; };
static std::function<void()> beforeMutexTake;
using SemaphoreHandle_t=TestMutex*;
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return new TestMutex;}
inline int xSemaphoreTake(SemaphoreHandle_t m,TickType_t){if(beforeMutexTake)beforeMutexTake();if(m->locked)return 0;m->locked=true;return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t m){assert(m->locked);m->locked=false;}
inline void vTaskDelay(uint32_t ms){clockMs+=ms;}
inline int64_t esp_timer_get_time(){return int64_t(clockMs)*1000;}
inline uint32_t esp_random(){static uint32_t r=0x89abcdef;return ++r;}
inline void esp_fill_random(void *p,size_t n){for(size_t i=0;i<n;++i)static_cast<uint8_t*>(p)[i]=uint8_t(esp_random());}
#define log_e(...) ((void)0)
#define log_w(...) ((void)0)
#define log_i(...) ((void)0)
#define log_d(...) ((void)0)
static bool ProcessingOTA=false;
static unsigned Publish_BLE_Attempts=0;
static unsigned Total_BLE_Records_Last_Pub=0;
static unsigned Skipped_BLE_Records_Last_Pub=0;
static long LastPublishTimeBLE=0,TimeNow=0;
static char Room[64]="Living Room";
static struct {size_t heap=250000;size_t getFreeHeap()const{return heap;}} ESP;
inline std::string hostName(){return "node-aabbcc";}
class String {
    std::string s;
public:
    String()=default; String(const char *v):s(v?v:""){} String(const std::string &v):s(v){}
    const char *c_str()const{return s.c_str();}size_t length()const{return s.size();}
    bool operator==(const char *v)const{return s==v;} bool operator!=(const char *v)const{return s!=v;}
    bool operator==(const String &v)const{return s==v.s;}
};
class Preferences {
public:
    static inline std::vector<uint8_t> saved;
    static inline bool failOpen=false,failWrite=false;
    bool begin(const char*,bool){return !failOpen;}
    size_t getBytesLength(const char*){return saved.size();}
    size_t getBytes(const char*,void *p,size_t n){if(n>saved.size())return 0;memcpy(p,saved.data(),n);return n;}
    bool isKey(const char*){return !saved.empty();}
    size_t putBytes(const char*,const void *p,size_t n){if(failWrite)return 0;saved.assign((const uint8_t*)p,(const uint8_t*)p+n);return n;}
    void end(){}
};
struct Mqtt {
    bool online=true,accept=true;
    std::function<void()> onPublish;
    std::string topic,payload; int calls=0;
    bool connected()const{return online;}
    uint16_t publish(const char *t,uint8_t qos,bool retain,const char *p,size_t n){assert(qos==0&&!retain);++calls;if(!online||!accept)return 0;topic=t;payload.assign(p,n);auto callback=onPublish;if(callback)callback();return 1;}
} mqttClient;
#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1
#define BLE_ADDR_PUBLIC_ID 2
#define BLE_ADDR_RANDOM_ID 3
class NimBLEAddress {
    std::array<uint8_t,6> bytes{};
public:
    uint8_t type=BLE_ADDR_PUBLIC;
    NimBLEAddress()=default;
    explicit NimBLEAddress(std::array<uint8_t,6> b):bytes(b){}
    const uint8_t *getVal()const{return bytes.data();}
    bool isRpa()const{return type==BLE_ADDR_RANDOM&&(bytes[5]&0xc0)==0x40;}
    bool isNrpa()const{return type==BLE_ADDR_RANDOM&&(bytes[5]&0xc0)==0;}
    std::string toString()const{char s[18];snprintf(s,sizeof s,"%02x:%02x:%02x:%02x:%02x:%02x",bytes[5],bytes[4],bytes[3],bytes[2],bytes[1],bytes[0]);return s;}
};
class NimBLEUUID {
    std::string s;
public:
    NimBLEUUID(std::string v):s(v){} std::string toString()const{return s.size()==4 ? "0x"+s : s;}
};
class NimBLEAdvertisedDevice {
public:
    NimBLEAddress address{std::array<uint8_t,6>{0xff,0xee,0xdd,0xcc,0xbb,0xaa}};
    uint8_t addressType=BLE_ADDR_PUBLIC;
    int rssi=-60,tx=-6;
    bool hasTx=false;
    std::string name;
    std::vector<uint8_t> payload;
    std::vector<std::string> services,manufacturer;
    std::vector<std::pair<std::string,std::string>> data;
    NimBLEAddress getAddress()const{auto a=address;a.type=addressType;return a;} uint8_t getAddressType()const{return addressType;}
    bool haveName()const{return !name.empty();}std::string getName()const{return name;}
    bool haveTXPower()const{return hasTx;}int8_t getTXPower()const{return tx;}int getRSSI()const{return rssi;}
    const std::vector<uint8_t>&getPayload()const{return payload;}
    uint8_t getServiceUUIDCount()const{return services.size();}NimBLEUUID getServiceUUID(uint8_t i)const{return NimBLEUUID(services.at(i));}
    uint8_t getServiceDataCount()const{return data.size();}NimBLEUUID getServiceDataUUID(uint8_t i)const{return NimBLEUUID(data.at(i).first);}
    std::string getServiceData(uint8_t i)const{return data.at(i).second;}
    uint8_t getManufacturerDataCount()const{return manufacturer.size();}std::string getManufacturerData(uint8_t i=0)const{return manufacturer.at(i);}
};
class NimBLEScanCallbacks {public:virtual ~NimBLEScanCallbacks()=default;virtual void onResult(const NimBLEAdvertisedDevice*){}};
class NimBLEScan {
public:
    bool scanning=false,active=true,duplicate=true,accept=true;uint8_t maxResults=255;int starts=0;NimBLEScanCallbacks *callbacks=nullptr;
    uint32_t duration=0;bool stopAccept=true;
    bool isScanning()const{return scanning;}bool stop(){if(!stopAccept)return false;scanning=false;return true;}
    void setScanCallbacks(NimBLEScanCallbacks *c,bool duplicates){callbacks=c;assert(duplicates);}
    void setDuplicateFilter(bool d){duplicate=d;}void setMaxResults(uint8_t n){maxResults=n;}
    void setActiveScan(bool a){active=a;}void setInterval(int){}void setWindow(int){}
    bool start(uint32_t ms,bool){duration=ms;++starts;scanning=accept;return scanning;}
};
class NimBLEDevice {public:static inline NimBLEScan scan;static bool init(const std::string&){return true;}static NimBLEScan *getScan(){return &scan;}};
// AES answers generated independently using Node's crypto AES-128-ECB, for
// conventional BLE ah() test key ec0234a357c8ad05341010a60a397d9b.
struct mbedtls_aes_context {uint8_t key[16];};
#define MBEDTLS_AES_ENCRYPT 1
inline void mbedtls_aes_init(mbedtls_aes_context *c){memset(c,0,sizeof *c);}
inline void mbedtls_aes_free(mbedtls_aes_context*){}
inline int mbedtls_aes_setkey_enc(mbedtls_aes_context *c,const uint8_t *k,int n){assert(n==128);memcpy(c->key,k,16);return 0;}
inline int mbedtls_aes_crypt_ecb(mbedtls_aes_context *c,int,const uint8_t *in,uint8_t *out){
    static const uint8_t key[]={0xec,2,0x34,0xa3,0x57,0xc8,0xad,5,0x34,0x10,0x10,0xa6,0x0a,0x39,0x7d,0x9b};
    memset(out,0,16);if(memcmp(c->key,key,16)!=0)return 0;
    for(int i=0;i<13;++i)assert(in[i]==0);
    if(in[13]==0x70&&in[14]==0x81&&in[15]==0x94){out[13]=0x0d;out[14]=0xfb;out[15]=0xaa;}
    if(in[13]==0x41&&in[14]==0x23&&in[15]==0x45){out[13]=0xc4;out[14]=0xd0;out[15]=0xdf;}
    return 0;
}
enum {HTTP_GET=1,HTTP_POST=2};
