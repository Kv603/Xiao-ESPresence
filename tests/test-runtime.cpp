#define SCAN_BLE
#include "stubs/env.h"
#include "../Beacon_Scanner.ino"
#include <iostream>
#include <fstream>
#include <set>
using namespace bletrack;
static bool failArray=false;
static size_t lastArrayBytes=0,arrayCalls=0;
void *operator new[](size_t n,const std::nothrow_t&) noexcept {
    lastArrayBytes=n;++arrayCalls;if(failArray)return nullptr;
    try{return ::operator new[](n);}catch(const std::bad_alloc &){return nullptr;}
}
static void clearTracked(){for(auto &t:runtime->devices){t.view=Snapshot();t.filter.reset();}}
static size_t countTracked(){size_t n=0;for(auto &t:runtime->devices)n+=t.view.valid;return n;}

struct Edit {
    std::vector<std::pair<std::string,std::string>> parameters;
    struct Outcome { int code=0; };
    std::unique_ptr<Outcome> response{new Outcome};
};
static Edit form(const char *op,uint32_t token=0){
    Edit r;r.parameters.emplace_back("operation",std::string("ble_")+op);
    r.parameters.emplace_back("revision",std::to_string(runtime->config.revision));
    r.parameters.emplace_back("token",std::to_string(token));return r;
}
static void saveConfiguration(Edit *r){
    JsonDocument doc;doc["alias"]="";doc["match"]="";doc["only"]=false;
    for(const auto &p:r->parameters){
        if(p.first=="revision"||p.first=="token")doc[p.first]=uint32_t(std::stoul(p.second));
        else if(p.first=="only"||p.first=="remove_key")doc[p.first]=p.second=="1";
        else doc[p.first]=p.second;
    }
    const char *error=editConfiguration(doc.as<JsonVariantConst>());
    r->response->code=!error?302:std::string(error)=="stale revision"?409:std::string(error).find("NVS")!=std::string::npos?500:400;
}

int main(){
    assert(initialize());setupBLE(false);
    assert(!scanner->active&&!scanner->duplicate&&scanner->maxResults==0);
    doBLE();assert(scanner->isScanning());
    requestActiveBLEScan();assert(!scanner->active);doBLE();
    assert(scanner->active&&scanner->duration==5000&&oneShotStarted);
    int activeStarts=scanner->starts;requestActiveBLEScan();doBLE();
    assert(scanner->starts==activeStarts&&!activeScanRequested.load());
    scanner->stop();doBLE(); // Simulate NimBLE's finite-scan completion.
    assert(scanner->isScanning()&&!scanner->active&&scanner->duration==0);
    scanner->stopAccept=false;requestActiveBLEScan();doBLE();
    assert(activeScanRequested.load()&&!scanner->active);
    scanner->stopAccept=true;scanner->accept=false;doBLE();
    assert(oneShotActive&&!oneShotStarted&&!scanner->isScanning());
    activeStarts=scanner->starts;doBLE();assert(scanner->starts==activeStarts);
    clockMs+=3001;scanner->accept=true;doBLE();assert(oneShotStarted);
    ProcessingOTA=true;doBLE();assert(!scanner->isScanning());
    ProcessingOTA=false;doBLE();assert(!scanner->active&&scanner->duration==0);
    ProcessingOTA=true;requestActiveBLEScan();doBLE();
    assert(activeScanRequested.load()&&!scanner->isScanning());
    ProcessingOTA=false;doBLE();assert(scanner->active&&scanner->duration==5000);
    scanner->stop();doBLE();assert(!scanner->active);
    {NimBLEAdvertisedDevice service;service.services={"feed"};service.hasTx=true;service.tx=-10;BleFingerprint f;decode(f,&service);assert(std::string(f.type)=="Tile"&&f.reference()==-69);}
    {NimBLEAdvertisedDevice service;uint8_t uid[20]={0,0xec};service.data.emplace_back("feaa",std::string((char*)uid,20));BleFingerprint f;decode(f,&service);assert(std::string(f.type)=="Eddystone UID"&&f.reference()==-61);}
    ProcessingOTA=true;doBLE();assert(!scanner->isScanning());ProcessingOTA=false;clockMs+=3100;doBLE();assert(scanner->isScanning());
    scanner->stop();scanner->accept=false;clockMs+=7000;doBLE();int starts=scanner->starts;doBLE();assert(scanner->starts==starts);scanner->accept=true;clockMs+=3001;doBLE();assert(scanner->isScanning());
    NimBLEAdvertisedDevice d;d.name="<Beacon & name>";
    uint8_t ibeacon[25]={0x4c,0,2,0x15};ibeacon[24]=197;d.manufacturer.emplace_back((char*)ibeacon,25);
    receive(&d);assert(countTracked()==1);doBLE();
    assert(mqttClient.topic=="espresense/devices/iBeacon:00000000-0000-0000-0000-000000000000-0-0/living_room");
    JsonDocument json;assert(deserializeJson(json,mqttClient.payload)==DeserializationError::Ok);assert(json["rssi@1m"].as<int>()==-59);assert(json["distance"].as<float>()>1);assert(json["id"].is<const char*>());
    int before=mqttClient.calls;clockMs+=100;receive(&d);doBLE();assert(mqttClient.calls==before);
    clockMs+=6000;receive(&d);mqttClient.accept=false;doBLE();assert(runtime->devices[0].view.pending);mqttClient.accept=true;doBLE();assert(!runtime->devices[0].view.pending);
    mqttClient.online=false;clockMs+=6000;receive(&d);before=mqttClient.calls;doBLE();assert(mqttClient.calls==before);mqttClient.online=true;doBLE();assert(mqttClient.calls>before);
    mqttClient.online=false;receive(&d);clockMs+=16000;before=mqttClient.calls;mqttClient.online=true;doBLE();assert(mqttClient.calls==before);
    clockMs+=6000;receive(&d);mqttClient.onPublish=[&]{assert(!stateMutex->locked);receive(&d);};doBLE();mqttClient.onPublish=nullptr;assert(runtime->devices[0].view.pending);
    d.name="";receive(&d);assert(std::string(runtime->devices[0].view.fp.name)=="<Beacon & name>");
    clearTracked();runtime->config.only=true;receive(&d);assert(countTracked()==0);
    {
        NimBLEAdvertisedDevice required;
        // Exact advertisement reported in the NimBLE discovery/erase log.
        uint8_t observed[25];
        assert(unhex("4c000215699ebc80e1f311e39a0f0cf3ee3bc0120001cd55c1",observed,25));
        required.manufacturer.emplace_back((char*)observed,25);
        required.address=NimBLEAddress({0x55,0xcd,0,0xee,0xf3,0x0c});
        ESP.heap=43092;required.rssi=-36; receive(&required); assert(runtime->devices[0].filter.allocated()==20); clearTracked(); ESP.heap=HEAP_RESERVE;
        size_t allocations=arrayCalls;
        receive(&required);assert(countTracked()==1&&arrayCalls==allocations);
        auto &lowHeap=runtime->devices[0];
        assert(lowHeap.filter.allocated()==0&&lowHeap.view.rssi==-36&&lowHeap.view.close);
        assert(runtime->devices[0].view.fp.reference()==-63);
        before=mqttClient.calls;doBLE();assert(mqttClient.calls==before+1);
        assert(mqttClient.topic=="espresense/devices/iBeacon:699ebc80-e1f3-11e3-9a0f-0cf3ee3bc012-1-52565/living_room");
        clockMs+=6000;required.rssi=-80;receive(&required);doBLE();
        assert(mqttClient.calls==before+2&&lowHeap.view.rssi==-80&&!lowHeap.view.close);
        assert(std::fabs(lowHeap.view.distance-std::pow(10.0f,17.0f/27))<0.001f);
        assert(lowHeap.view.variance==0&&lowHeap.view.distanceVariance==0);
        auto acceptedSamples=lowHeap.view.samples;
        required.rssi=127;receive(&required);assert(lowHeap.view.samples==acceptedSamples);
        required.rssi=-128;receive(&required);assert(lowHeap.view.samples==acceptedSamples);
        // Recovery automatically resumes history filtering; actual OOM also falls back.
        ESP.heap=250000;required.rssi=-60;failArray=true;receive(&required);
        assert(lowHeap.view.rssi==-60&&lowHeap.filter.allocated()==0);failArray=false;
        clockMs+=6000;receive(&required);doBLE();
        assert(lowHeap.filter.allocated()>0&&lowHeap.view.rssi==-60&&mqttClient.calls==before+3);
        clearTracked();ESP.heap=HEAP_RESERVE;runtime->config.only=false;receive(&d);
        assert(countTracked()==0);runtime->config.only=true;ESP.heap=250000;
        required.manufacturer.clear();clearTracked();
        uint8_t packet[25]={0x4c,0,2,0x15};
        assert(unhex("699EBC80E1F311E39A0F0CF3EE3BC012",packet+4,16));
        packet[21]=12;packet[23]=34;packet[24]=3;
        required.manufacturer.emplace_back((char*)packet,25);
        required.addressType=BLE_ADDR_RANDOM;
        required.address=NimBLEAddress({1,2,3,4,5,0x40});required.rssi=-110;
        receive(&required);assert(countTracked()==1);
        auto &tracked=runtime->devices[0];calculate(tracked,clockMs);
        assert(alwaysTrack(tracked.view.fp)&&tracked.view.distance>16&&eligible(tracked.view));
        before=mqttClient.calls;doBLE();assert(mqttClient.calls==before+1);
        assert(mqttClient.topic=="espresense/devices/iBeacon:699ebc80-e1f3-11e3-9a0f-0cf3ee3bc012-12-34/living_room");
        // Similar UUIDs and other beacon formats do not receive the exemption.
        clearTracked();packet[19]^=1;required.manufacturer[0]=std::string((char*)packet,25);
        receive(&required);assert(countTracked()==0);packet[19]^=1;
        packet[2]=0xbe;packet[3]=0xac;
        required.manufacturer[0]=std::string((char*)packet,25)+char(0);
        receive(&required);assert(countTracked()==0);packet[2]=2;packet[3]=0x15;
        required.manufacturer[0]=std::string((char*)packet,25);
        receive(&required);runtime->config.only=false;
        // More than a table of ordinary devices cannot evict the required beacon.
        NimBLEAdvertisedDevice ordinary=d;
        for(unsigned i=0;i<TRACKED_LIMIT+5;++i){ordinary.address=NimBLEAddress({uint8_t(i),0,0,0,0,0});clockMs++;receive(&ordinary);}
        assert(countTracked()==TRACKED_LIMIT&&alwaysTrack(runtime->devices[0].view.fp));
        // A table full of required beacons rejects ordinary traffic, admits new required ones.
        clearTracked();
        for(unsigned i=0;i<TRACKED_LIMIT;++i){required.address=NimBLEAddress({uint8_t(i),2,3,4,5,0x40});packet[23]=uint8_t(i);required.manufacturer[0]=std::string((char*)packet,25);clockMs++;receive(&required);}
        receive(&ordinary);for(auto &t:runtime->devices)assert(alwaysTrack(t.view.fp));
        required.address=NimBLEAddress({99,2,3,4,5,0x40});packet[23]=99;required.manufacturer[0]=std::string((char*)packet,25);clockMs++;receive(&required);
        assert(std::string(runtime->devices[0].view.fp.mac)=="400504030263");
        clockMs+=FORGET_MS+1;doBLE();assert(countTracked()==0);
    }
    runtime->config.only=false;
    auto add=form("save");add.parameters.emplace_back("match","AA:BB:CC:DD:EE:FF");add.parameters.emplace_back("alias","my_beacon");saveConfiguration(&add);assert(add.response->code==302&&runtime->config.count==1);assert(std::string(runtime->config.entries[0].match)=="aabbccddeeff");
    uint32_t token=runtime->config.entries[0].token;receive(&d);clockMs+=6000;doBLE();assert(mqttClient.topic=="espresense/devices/my_beacon/living_room");
    auto stale=form("mode");--runtime->config.revision;saveConfiguration(&stale);assert(stale.response->code==409);++runtime->config.revision;
    auto duplicate=form("save");duplicate.parameters.emplace_back("match","aabbccddeeff");saveConfiguration(&duplicate);assert(duplicate.response->code==400&&runtime->config.count==1);
    auto fail=form("delete",token);Preferences::failWrite=true;saveConfiguration(&fail);assert(fail.response->code==500&&runtime->config.count==1);Preferences::failWrite=false;
    auto update=form("save",token);update.parameters.emplace_back("match","aabbccddeeff");update.parameters.emplace_back("alias","private_beacon");update.parameters.emplace_back("irk","ec0234a357c8ad05341010a60a397d9b");saveConfiguration(&update);assert(update.response->code==302);
    assert(runtime->config.entries[0].hasIrk);
    auto blank=form("save",token);blank.parameters.emplace_back("match","aabbccddeeff");blank.parameters.emplace_back("alias","private_beacon");blank.parameters.emplace_back("irk","");saveConfiguration(&blank);assert(blank.response->code==302&&runtime->config.entries[0].hasIrk);
    auto &c=runtime->config.entries[0];
    uint8_t address[]={0xaa,0xfb,0x0d,0x94,0x81,0x70};assert(resolves(c.irk,address,BLE_ADDR_RANDOM));assert(!resolves(c.irk,address,BLE_ADDR_PUBLIC));address[0]^=1;assert(!resolves(c.irk,address,BLE_ADDR_RANDOM));address[0]^=1;
    runtime->config.only=true;NimBLEAdvertisedDevice privateDevice;privateDevice.address=NimBLEAddress({0xaa,0xfb,0x0d,0x94,0x81,0x70});privateDevice.addressType=BLE_ADDR_RANDOM;
    receive(&privateDevice);assert(countTracked()==1);assert(std::string(runtime->devices[0].view.effective)=="private_beacon");
    auto samples=runtime->devices[0].view.samples;
    privateDevice.address=NimBLEAddress({0xdf,0xd0,0xc4,0x45,0x23,0x41});clockMs+=20;receive(&privateDevice);
    assert(countTracked()==1&&runtime->devices[0].view.samples==samples+1&&std::string(runtime->devices[0].view.effective)=="private_beacon");
    privateDevice.address=NimBLEAddress({0xaa,0xfb,0x0d,0x94,0x81,0x70});
    // Wrong key fails; correct key takes precedence over an exact MAC match.
    runtime->config.count=2;runtime->config.entries[1].token=42;copy(runtime->config.entries[1].match,"7081940dfbaa");copy(runtime->config.entries[1].alias,"mac_alias");
    BleFingerprint fp;decode(fp,&privateDevice);assert(matchConfig(fp,address,BLE_ADDR_RANDOM)==0);
    runtime->config.count=1;runtime->config.entries[1]=DeviceConfig();
    // A saved configuration survives re-initialization; the CSRF token changes.
    delete runtime;runtime=nullptr;assert(initialize());assert(runtime->config.entries[0].hasIrk&&runtime->config.count==1);
    auto remove=form("save",token);remove.parameters.emplace_back("match","aabbccddeeff");remove.parameters.emplace_back("alias","private_beacon");remove.parameters.emplace_back("remove_key","1");saveConfiguration(&remove);assert(remove.response->code==302&&!runtime->config.entries[0].hasIrk);
    auto del=form("delete",token);saveConfiguration(&del);assert(del.response->code==302&&runtime->config.count==0);
    runtime->config.only=false;clearTracked();
    for(unsigned i=0;i<50;++i){d.address=NimBLEAddress({uint8_t(i),0,0,0,0,0});clockMs++;receive(&d);}
    assert(countTracked()==32);
    d.rssi=-110;d.address=NimBLEAddress({100,0,0,0,0,0});clockMs++;receive(&d);
    bool distant=false;for(auto &t:runtime->devices)if(t.view.fp.mac==std::string("000000000064")){calculate(t,clockMs);assert(t.view.distance>16&&!eligible(t.view));distant=true;}assert(distant);
    clockMs+=FORGET_MS+1;doBLE();assert(countTracked()==0);
    {stateMutex->locked=true;NimBLEAdvertisedDevice device;receive(&device);stateMutex->locked=false;assert(countTracked()==0);}
    {
        resetConfig(runtime->config);clearTracked();
        for(uint8_t top:{uint8_t(0),uint8_t(0x40)}) {
            NimBLEAdvertisedDevice p;p.address=NimBLEAddress({1,2,3,4,5,top});p.addressType=BLE_ADDR_RANDOM;
            auto suppressed=[&]{
                clearTracked();receive(&p);assert(countTracked()==1);
                auto &v=runtime->devices[0].view;calculate(runtime->devices[0],clockMs);
                int calls=mqttClient.calls;assert(!eligible(v)&&!publish(v)&&mqttClient.calls==calls);
            };
            suppressed();p.name="Shared model name";suppressed();
            p.services={"feed"};suppressed();p.services.clear();
            p.manufacturer={std::string("\x4c\x00\x10\x00",4)};suppressed();p.manufacturer.clear();
            p.manufacturer={std::string(reinterpret_cast<char*>(ibeacon),24)};suppressed();
            p.manufacturer={std::string(reinterpret_cast<char*>(ibeacon),25)};
            clearTracked();receive(&p);assert(eligible(runtime->devices[0].view));
            std::string stable=runtime->devices[0].view.effective;
            p.manufacturer.clear();p.name.clear();receive(&p);assert(eligible(runtime->devices[0].view));
            p.address=NimBLEAddress({2,2,3,4,5,top});receive(&p);
            assert(!eligible(runtime->devices[1].view));
            p.manufacturer={std::string(reinterpret_cast<char*>(ibeacon),25)};receive(&p);
            assert(eligible(runtime->devices[1].view)&&runtime->devices[1].view.effective==stable);
            uint8_t alt[26]={0x34,0x12,0xbe,0xac};alt[24]=197;
            clearTracked();p.manufacturer={std::string(reinterpret_cast<char*>(alt),26)};receive(&p);assert(eligible(runtime->devices[0].view));
            uint8_t uid[20]={0,0xec};p.manufacturer.clear();p.data={{"feaa",std::string(reinterpret_cast<char*>(uid),20)}};
            clearTracked();receive(&p);assert(eligible(runtime->devices[0].view));
            p.data.clear();p.services={"feed"};p.name="";
            runtime->config.count=1;auto &entry=runtime->config.entries[0];entry.token=1;
            copy(entry.match,macNormalized(p.address.toString()));copy(entry.alias,"alias_only");
            suppressed();entry.hasIrk=true;memset(entry.irk,0,sizeof entry.irk);suppressed();
            resetConfig(runtime->config);
            // Public and static-random addresses retain their prior eligibility.
            p.addressType=BLE_ADDR_PUBLIC;clearTracked();receive(&p);assert(eligible(runtime->devices[0].view));
            p.addressType=BLE_ADDR_RANDOM;p.address=NimBLEAddress({1,2,3,4,5,0xc0});
            clearTracked();receive(&p);assert(eligible(runtime->devices[0].view));
        }
        resetConfig(runtime->config);clearTracked();runtime->config.count=1;
        auto &entry=runtime->config.entries[0];entry.token=5;entry.hasIrk=true;
        assert(unhex("ec0234a357c8ad05341010a60a397d9b",entry.irk,16));
        NimBLEAdvertisedDevice p;p.addressType=BLE_ADDR_RANDOM;p.address=NimBLEAddress({0xaa,0xfb,0x0d,0x94,0x81,0x70});
        receive(&p);assert(eligible(runtime->devices[0].view));int calls=mqttClient.calls;
        assert(publish(runtime->devices[0].view)&&mqttClient.calls==calls+1);
        p.address=NimBLEAddress({0xdf,0xd0,0xc4,0x45,0x23,0x41});receive(&p);
        assert(countTracked()==1&&eligible(runtime->devices[0].view));
        resetConfig(runtime->config);clearTracked();
    }
    {
        resetConfig(runtime->config);clearTracked();
        NimBLEAdvertisedDevice otaDevice;
        for(unsigned i=0;i<TRACKED_LIMIT;++i){otaDevice.address=NimBLEAddress({uint8_t(i),0,0,0,0,0});receive(&otaDevice);}
        assert(countTracked()==32);for(auto &t:runtime->devices)assert(t.filter.allocated()==20);
        doBLE();assert(scanner->isScanning());
        ProcessingOTA=true;pauseBLEForOTA();assert(ProcessingOTA); // No doBLE tick is needed.
        assert(!scanner->isScanning()&&countTracked()==0);
        for(auto &t:runtime->devices)assert(t.filter.allocated()==0);
        size_t allocations=arrayCalls;receive(&otaDevice);assert(countTracked()==0&&arrayCalls==allocations);
        requestActiveBLEScan();setupBLE(true);doBLE();assert(!scanner->isScanning());
        pauseBLEForOTA(); // Repeated start/progress callbacks are harmless.
        doBLE();assert(ProcessingOTA&&!scanner->isScanning());
        ProcessingOTA=false;
        doBLE();assert(scanner->isScanning()&&scanner->active);
        receive(&otaDevice);assert(countTracked()==1&&runtime->devices[0].filter.allocated()==20);
        scanner->stopAccept=false;ProcessingOTA=true;pauseBLEForOTA();assert(countTracked()==0);
        receive(&otaDevice);assert(countTracked()==0);
        scanner->stopAccept=true;doBLE();assert(!scanner->isScanning());
        ProcessingOTA=false;
        beforeMutexTake=[&]{ProcessingOTA=true;};receive(&otaDevice);beforeMutexTake=nullptr;
        assert(countTracked()==0); // Callback waiting on the state lock observes OTA.
        ProcessingOTA=false;
        beforeMutexTake=[&]{ProcessingOTA=true;};doBLE();beforeMutexTake=nullptr;
        assert(!scanner->isScanning()); // Scan-start race also observes OTA under lock.
        ProcessingOTA=false;doBLE();assert(scanner->isScanning()&&!scanner->active);
        clearTracked();
    }
    {
        resetConfig(runtime->config);clearTracked();
        runtime->config.count=LIMIT;
        for(size_t i=0;i<LIMIT;++i){auto &c=runtime->config.entries[i];c.token=uint32_t(i+1);copy(c.match,(std::string("never_")+std::to_string(i)+"_")+std::string(140,'x'));copy(c.alias,std::string("entry_")+std::to_string(i));}
        NimBLEAdvertisedDevice required;
        uint8_t bytes[25];assert(unhex("4c000215699ebc80e1f311e39a0f0cf3ee3bc0120001cd55c1",bytes,25));
        required.manufacturer.emplace_back((char*)bytes,25);required.rssi=-110;
        for(bool only:{false,true}){
            clearTracked();runtime->config.only=only;receive(&required);assert(countTracked()==1);
            calculate(runtime->devices[0],clockMs);assert(runtime->devices[0].view.distance>16&&eligible(runtime->devices[0].view));
        }
        auto &entry=runtime->config.entries[0];copy(entry.match,runtime->devices[0].view.fp.id);copy(entry.alias,"fixed_alias");
        entry.hasIrk=true;assert(unhex("ec0234a357c8ad05341010a60a397d9b",entry.irk,16));
        required.addressType=BLE_ADDR_RANDOM;required.address=NimBLEAddress({0xaa,0xfb,0x0d,0x94,0x81,0x70});
        JsonDocument edit;edit["operation"]="ble_mode";edit["revision"]=runtime->config.revision;edit["only"]=true;
        assert(!editConfiguration(edit.as<JsonVariantConst>()));
        ProcessingOTA=true;pauseBLEForOTA();ProcessingOTA=false;
        delete runtime;runtime=nullptr;assert(initialize()&&runtime->config.count==45&&runtime->config.only);
        ESP.heap=HEAP_RESERVE;receive(&required);assert(countTracked()==1);
        assert(std::string(runtime->devices[0].view.effective)=="fixed_alias"&&eligible(runtime->devices[0].view));
        assert(publish(runtime->devices[0].view));ESP.heap=250000;
        assert(mqttClient.topic=="espresense/devices/fixed_alias/living_room");
        JsonDocument state;assert(appendConfigurationState(state));
        assert(state["entries"].size()==45&&state["always_tracked"][0]["read_only"].as<bool>());
        assert(state["always_tracked"][0]["ibeacon_uuid"].as<std::string>()==ALWAYS_TRACK_IBEACON_UUID);
        assert(state["entries"][0]["irk_set"].as<bool>()&&state["entries"][0]["irk"].isNull());
        std::cout<<"Always-tracked full-whitelist/mode/IRK/alias/reload/OTA/low-memory matrix passed.\n";
    }
    {delete runtime;runtime=nullptr;Preferences::failOpen=true;assert(initialize()&&runtime->config.only);Preferences::failOpen=false;}
    std::cout<<"Runtime/settings tests passed: scanning, OTA, MQTT, whitelist, aliases, IRK, persistence, expiry and capacity.\n";
    std::cout<<"sizeof Runtime="<<sizeof(Runtime)<<" Snapshot="<<sizeof(Snapshot)<<"\n";
}
