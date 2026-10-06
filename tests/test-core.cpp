#define BEACON_CORE_ONLY
#include "../Beacon_Scanner.ino"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>
static bool failAllocation=false;
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return failAllocation ? nullptr : std::malloc(n); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
using namespace bletrack;
int main() {
    BleFingerprint f; copy(f.mac,"aabbccddeeff"); f.setId(f.mac,55);
    uint8_t beacon[25]={0x4c,0,2,0x15};
    for(int i=0;i<16;++i) beacon[4+i]=i;
    beacon[20]=0x12; beacon[21]=0x34; beacon[22]=0xab; beacon[23]=0xcd; beacon[24]=197;
    manufacturerData(f,beacon,sizeof beacon,false,0);
    assert(std::string(f.type)=="iBeacon" && std::string(f.manufacturer)=="Apple");
    assert(std::string(f.id)=="iBeacon:00010203-0405-0607-0809-0a0b0c0d0e0f-4660-43981");
    assert(f.reference()==-59);
    uint8_t generic[]={0x75,0,0,0}; manufacturerData(f,generic,4,true,-20);
    assert(std::string(f.type)=="iBeacon" && std::string(f.manufacturer)=="Apple" && f.reference()==-59);
    for(size_t n=0;n<25;++n) { BleFingerprint shortF; manufacturerData(shortF,beacon,n,false,0); assert(std::string(shortF.type)!="iBeacon"); }
    BleFingerprint unknown; uint8_t un[]={0xef,0xbe,0,0}; manufacturerData(unknown,un,4,false,0);
    assert(std::string(unknown.type)=="Unknown" && std::string(unknown.manufacturer)=="0xBEEF");
    BleFingerprint nearby; uint8_t near[]={0x4c,0,0x10,2,1,2}; manufacturerData(nearby,near,6,false,0); assert(std::string(nearby.type)=="Apple Nearby");
    near[3]=20; BleFingerprint badNear; manufacturerData(badNear,near,6,false,0); assert(std::string(badNear.type)!="Apple Nearby");
    BleFingerprint find; uint8_t findData[29]={0x4c,0,0x12,25}; manufacturerData(find,findData,29,false,0); assert(std::string(find.type)=="Apple Find My");
    BleFingerprint alt; uint8_t altData[26]={0x18,1,0xbe,0xac}; memcpy(altData+4,beacon+4,21); manufacturerData(alt,altData,26,false,0); assert(std::string(alt.type)=="AltBeacon" && alt.reference()==-59);
    std::string url;
    for(int s=0;s<4;++s) for(int e=0;e<14;++e) { uint8_t d[]={0x10,0xf0,uint8_t(s),'a',uint8_t(e)}; assert(eddystoneURL(d,5,url)); }
    uint8_t badURL[]={0x10,0xf0,4,'a'}; assert(!eddystoneURL(badURL,4,url) && url.empty());
    uint8_t uid[20]={0,0xec}; for(int i=2;i<18;++i) uid[i]=i;
    BleFingerprint eddy; serviceData(eddy,"feaa",uid,20,false,0); assert(std::string(eddy.id)=="eddy:02030405060708090a0b-0c0d0e0f1011" && eddy.reference()==-61);
    uint8_t tlm[]={0x20,0,0x0b,0xb8,0xff,0x80,0,0,0,7,0,0,0,100};
    serviceData(eddy,"feaa",tlm,14,false,0); assert(eddy.temp==-.5f && eddy.mv==3000 && eddy.advertiseCount==7 && eddy.uptimeSeconds==10);
    tlm[4]=0x80; tlm[5]=0; serviceData(eddy,"feaa",tlm,14,false,0); assert(!eddy.temperaturePresent);
    float r,v,dv;
    AdaptivePercentileRSSI history; assert(!history.statistics(0,-59,r,v,dv));
    failAllocation=true; assert(!history.addMeasurement(-59,0)); failAllocation=false;
    for(unsigned i=0;i<20;++i) assert(history.addMeasurement(-59,i*100));
    assert(history.statistics(2000,-59,r,v,dv) && r==-59 && v==0 && dv==0);
    assert(std::fabs(std::pow(10.0f,(-59-r)/27)-1)<.0001f);
    assert(history.allocated()==20 && history.size()==20);
    failAllocation=true; assert(history.addMeasurement(-59,2100)); failAllocation=false;
    assert(history.statistics(2100,-59,r,v,dv) && r==-59);
    assert(history.size()==20); // A full history overwrites without allocating.
    history.reset();
    for(unsigned i=0;i<=60;++i) assert(history.addMeasurement(-60,i*1000));
    assert(history.size()==16 && history.allocated()==20);
    assert(history.statistics(75000,-60,r,v,dv) && history.size()==1);
    assert(!history.statistics(75001,-60,r,v,dv));
    history.reset();
    for(unsigned i=0;i<9;++i) history.addMeasurement(-60,i*10);
    history.addMeasurement(-120,100); assert(history.statistics(100,-60,r,v,dv) && r==-60 && v>0);
    assert(!history.statistics(15101,-60,r,v,dv));
    history.reset(); history.addMeasurement(-70,UINT32_MAX-100); history.addMeasurement(-70,20);
    assert(history.statistics(100,-70,r,v,dv) && r==-70); assert(!history.statistics(16000,-70,r,v,dv));
    history.reset(); for(unsigned i=0;i<30000;++i) history.addMeasurement(-50,i);
    assert(history.allocated()==20 && history.size()==20);
    assert(reportDelay(.5f)>=500 && reportDelay(1000000)<=2500);
    assert(normalize(" Living Room! ",'_')=="living_room");
    assert(macNormalized("AA:bb:CC:dd:EE:ff")=="aabbccddeeff"); assert(macNormalized("ab:cd:ef").empty());
    Configuration cfg; cfg.count=1; cfg.entries[0].token=1; copy(cfg.entries[0].match,"iBeacon:abc"); assert(configValid(cfg));
    cfg.entries[0].match[0]=0; assert(!configValid(cfg)); cfg.entries[0].hasIrk=true; assert(configValid(cfg));
    cfg.count=2; cfg.entries[1]=cfg.entries[0]; cfg.entries[1].token=2; assert(!configValid(cfg));
    cfg.entries[1].irk[0]=1; assert(configValid(cfg)); copy(cfg.entries[0].alias,"a"); copy(cfg.entries[1].alias,"a"); assert(!configValid(cfg));
    assert(!aliasValid("bad/topic") && !topicId("a/#"));
    assert(validUtf8("caf\xc3\xa9") && !validUtf8("\xc0\x80") && !validUtf8("\xed\xa0\x80") && !validUtf8("\xf0\x9f"));
    Configuration original;original.count=2;original.only=true;original.revision=27;
    original.entries[0].token=1;copy(original.entries[0].match,"aabbccddeeff");copy(original.entries[0].alias,"tag");
    original.entries[1].token=2;original.entries[1].hasIrk=true;original.entries[1].irk[0]=0x12;
    std::vector<uint8_t> encoded(encodedSize(original));assert(encodeConfig(original,encoded.data(),encoded.size()));
    Configuration decoded;assert(decodeConfig(encoded.data(),encoded.size(),decoded));assert(decoded.only&&decoded.revision==27&&decoded.entries[1].irk[0]==0x12&&std::string(decoded.entries[0].alias)=="tag");
    for(size_t n=0;n<encoded.size();++n)assert(!decodeConfig(encoded.data(),n,decoded));
    encoded[9]=46;assert(!decodeConfig(encoded.data(),encoded.size(),decoded));
    Configuration empty;assert(encodedSize(empty)==10);
    // Device capacity changes must not invalidate existing full whitelists.
    Configuration full;full.count=45;
    for(unsigned i=0;i<full.count;++i){full.entries[i].token=i+1;copy(full.entries[i].match,std::to_string(i));}
    std::vector<uint8_t> fullBytes(encodedSize(full));
    assert(encodeConfig(full,fullBytes.data(),fullBytes.size()));
    assert(decodeConfig(fullBytes.data(),fullBytes.size(),decoded)&&decoded.count==45);
    // Deterministic malformed/unknown advertisement corpus exercises every length.
    uint32_t random=12345;
    for(size_t n=0;n<=255;++n) for(unsigned round=0;round<12;++round) {
        std::vector<uint8_t> bytes(n);
        for(auto &b:bytes){random=random*1664525+1013904223;b=uint8_t(random>>24);}
        BleFingerprint parsed;manufacturerData(parsed,bytes.data(),bytes.size(),true,-10);
        serviceData(parsed,"feaa",bytes.data(),bytes.size(),false,0);
        assert(memchr(parsed.id,0,sizeof parsed.id)&&memchr(parsed.beacon,0,sizeof parsed.beacon));
    }
    std::cout<<"Core tests passed: fingerprints, malformed frames, Eddystone, filter, rollover, allocation failure, configuration.\n";
    std::cout<<"sizeof fingerprint="<<sizeof(BleFingerprint)<<" configuration="<<sizeof(Configuration)<<" filter="<<sizeof(AdaptivePercentileRSSI)<<"\n";
}
