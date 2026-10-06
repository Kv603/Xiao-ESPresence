#define BEACON_CORE_ONLY
#include "../Beacon_Scanner.ino"
#include <cassert>
#include <iostream>
namespace bletrack {
#include "legacy-rssi.inc"
}
int main(){
    using namespace bletrack;
    AdaptivePercentileRSSI compact;LegacyRSSI legacy;
    assert(AdaptivePercentileRSSI::MAX_READINGS==20);
    assert(AdaptivePercentileRSSI::storageBytes(20)==100);
    assert(!compact.addMeasurement(-50.5f,0));
    uint32_t now=UINT32_MAX-20000,random=91;
    // Under the <=1 callback/second assumption, fixed and adaptive histories
    // retain the same complete time window and produce identical statistics.
    for(unsigned i=0;i<12000;++i){
        random=random*1664525+1013904223;now+=1000+random%1200;
        const int rssi=int((random>>16)%148)-127;
        assert(compact.addMeasurement(float(rssi),now)==legacy.addMeasurement(float(rssi),now));
        float a=0,b=0,c=0,x=0,y=0,z=0;
        assert(compact.statistics(now,-65,a,b,c)==legacy.statistics(now,-65,x,y,z));
        assert(a==x&&b==y&&c==z&&compact.size()==legacy.size());
        assert(compact.allocated()==20&&compact.size()<=16);
        if(i%257==0){now+=16000;compact.expire(now);legacy.expire(now);assert(!compact.size()&&!legacy.size());}
    }
    std::cout<<"Fixed-history equivalence passed: 12000 observations at <=1 Hz, outliers, expiry and rollover.\n";
}
