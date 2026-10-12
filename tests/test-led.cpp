#include <ArduinoJson.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
static uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
#include "../LED_Controller.h"
#include "../LED_Subscriptions.h"
using namespace leds;
static Value parsed(const std::string &payload) { Value v; assert(parseValue(payload,v)); return v; }
static Event message(const std::string &topic, const std::string &payload, uint32_t time = 0, uint64_t groups = 1) {
  return {topic,parsed(payload),groups,time};
}
static Config patched(const char *json, Config initial = {}) {
  JsonDocument doc; assert(!deserializeJson(doc,json,DeserializationOption::NestingLimit(4)));
  assert(patchConfig(doc.as<JsonVariantConst>(),initial)); return initial;
}
static void send(Controller &c, const char *topic, const char *payload, uint32_t time = 0, bool retained = false) {
  c.ingest(topic, std::strlen(topic), payload, std::strlen(payload), 0, std::strlen(payload), retained, time);
}
int main() {
  static_assert(GROUPS == 13 && groupStart(0) == 1 && groupStart(12) == 49);
  for (const char *filter : {"sensor/lobby/#","sensor/+/red","#","+","/","a//b","$SYS/#"}) assert(validFilter(filter));
  for (const char *filter : {"","a#","a/+b","a/#/b","a/##","a/b+","$share/team/a"}) assert(!validFilter(filter));
  assert(!validFilter(std::string(161,'a')) && !validFilter(std::string("a\0b",3)));
  assert(validFilter("sensor/\xc3\xa9/#") && !validFilter("\xc0\x80"));
  assert(!validFilter("\xed\xa0\x80") && !validFilter("\xf4\x90\x80\x80"));
  assert(matches("sensor/lobby/#","sensor/lobby"));
  assert(matches("sensor/lobby/#","sensor/lobby/mmwave"));
  assert(matches("sensor/lobby/#","sensor/lobby/nested/blue"));
  assert(!matches("sensor/lobby/#","sensor/lobbyish"));
  assert(!matches("sensor/lobby/#","sensor/Lobby/red"));
  assert(matches("a/+","a/") && !matches("a/+","a") && !matches("a/+","a/b/c"));
  assert(matches("a/+/c/#","a//c") && matches("#","a"));
  assert(!matches("#","$SYS/x") && !matches("+/x","$SYS/x") && matches("$SYS/#","$SYS/x"));
  assert(matches("a/","a/") && !matches("a/","a"));
  Config cfg = patched(R"({"brightness":128,"groups":[{"group":1,"topics":["sensor/lobby/#","sensor/+/red"]},{"group":2,"topics":["sensor/lobby/#"]}]})");
  assert(matchingGroups(cfg,"sensor/lobby/red") == 3 && filters(cfg).size() == 2);
  assert(matchingGroups(cfg,"sensor/lobby") == 3);
  for (const char *json : {
    R"({"brightness":256})",R"({"brightness":-1})",R"({"brightness":1.5})",R"({"brightness":null})",
    R"({"groups":null})",R"({"groups":[{"group":0,"topics":[]}]})",R"({"groups":[{"group":14,"topics":[]}]})",
    R"({"groups":[{"group":1,"topics":["a#"]}]})",R"({"groups":[{"group":1,"topics":["a","a"]}]})",
    R"({"groups":[{"group":1,"topics":[]},{"group":1,"topics":[]}]})",R"({"groups":[{"group":1,"topics":[null]}]})"
  }) {
    JsonDocument d; assert(!deserializeJson(d,json)); Config copy = cfg;
    assert(!patchConfig(d.as<JsonVariantConst>(),copy)); assert(copy.brightness == cfg.brightness && copy.topics == cfg.topics);
  }
  auto cleared = patched(R"({"groups":[{"group":1,"topics":[]}]})",cfg);
  assert(cleared.topics[0].empty() && cleared.topics[1] == cfg.topics[1] && cleared.brightness == 128);
  auto cap = cfg; cap.topics[0].clear(); cap.topics[1].clear();
  for (unsigned i=0;i<64;++i) cap.topics[0].push_back("t/"+std::to_string(i));
  assert(validConfig(cap)); cap.topics[1].push_back("extra"); assert(!validConfig(cap));
  assert(parsed(" 50.5 \n").numeric && parsed("1e2").percent == 100);
  assert(parsed("\" LiMe \"").color == 0x00ff00 && parsed("GhOsTwHiTe").color == 0xf8f8ff);
  assert(parsed("green").color == 0x008000 && parsed("ivory").color == 0xfffff0);
  assert(parsed("brickred").persistent && parsed("red").persistent && !parsed("firebrick").persistent);
  assert(parsed("on").percent == 75 && parsed("off").percent == 0);
  Value invalid;
  for (const char *s : {""," ","-1","101","NaN","Infinity","1e999","+1",".5","01","1.","1e","100x","true","null","[]","{}","mauve","\"100\"","\"red\"x"}) assert(!parseValue(s,invalid));
  assert(!parseValue(std::string(65,'a'),invalid) && !parseValue(std::string("red\0",4),invalid));
  Model m;
  assert(m.apply(message("sensor/lobby/green","100")));
  assert((m.colors(0)[0] == RGB{0,255,0}));
  assert(m.apply(message("sensor/lobby/red","100")));
  assert((m.colors(0)[0] == RGB{255,255,0}));
  assert(m.apply(message("sensor/lobby/blue","50")));
  assert((m.colors(30000)[0] == RGB{255,128,64}));
  assert((m.colors(60000)[0] == RGB{255,0,0}));
  m.apply(message("sensor/lobby/red","0",60000)); assert((m.colors(60000)[0] == RGB{}));
  m.clear(); m.apply(message("a","100")); m.apply(message("b","100",30000));
  assert((m.colors(30000)[0] == RGB{255,255,255}));
  assert((m.colors(60000)[0] == RGB{128,128,128}));
  assert((m.colors(90000)[0] == RGB{}));
  m.clear(); m.apply(message("a","100")); m.apply(message("a","50",30000));
  assert((m.colors(60000)[0] == RGB{64,64,64}));
  m.clear(); m.apply(message("a","red")); m.apply(message("b","blue"));
  assert((m.colors(0)[0] == RGB{191,0,191})); assert((m.colors(60000)[0] == RGB{191,0,0}));
  m.apply(message("b","off",60000)); assert((m.colors(60000)[0] == RGB{191,0,0}));
  m.clear(); m.apply(message("a","100")); m.apply(message("x/red","0"));
  assert((m.colors(30000)[0] == RGB{0,128,128}));
  m.apply(message("b","lime",30000)); assert((m.colors(30000)[0] == RGB{128,255,128}));
  m.clear(); m.apply(message("a","red")); m.apply(message("x/blue","100"));
  m.apply(message("a","off",1000)); assert((m.colors(1000)[0] == RGB{})); // ordinary clears overrides
  m.clear(); m.apply(message("x/red","green")); assert((m.colors(0)[0] == RGB{0,96,0}));
  m.clear(); m.apply(message("a","100",UINT32_MAX-999));
  assert((m.colors(29000)[0] == RGB{128,128,128})); assert((m.colors(59000)[0] == RGB{}));
  m.clear(); m.apply(message("a","red",0,(uint64_t(1)<<12)|1));
  auto frame = m.frame(0); assert(frame[0] == RGB{} && frame[4] == RGB{});
  assert((frame[1] == RGB{191,0,0} && frame[3] == frame[1] && frame[49] == frame[1] && frame[50] == frame[1]));
  m.clear(); for(unsigned i=0;i<64;++i) assert(m.apply(message("source/"+std::to_string(i),"red")));
  assert(!m.apply(message("overflow","red")) && m.sourceOverflow == 1);
  m.apply(message("source/0","off")); assert(m.apply(message("reclaimed","red")));
  m.clear(); for(unsigned i=0;i<64;++i) m.apply(message("source/"+std::to_string(i),"100"));
  assert(m.apply(message("reclaimed","red",60000)));
  Model recon;
  recon.apply(message("sensor/lobby","red",0,3));
  auto next = cfg; next.topics[0].clear(); recon.configure(cfg,next);
  assert(recon.colors(0)[0] == RGB{} && (recon.colors(0)[1] == RGB{191,0,0}));
  next.topics[2] = {"sensor/lobby/#"}; recon.configure(cfg,next); assert(recon.colors(0)[2] == RGB{});
  Controller controller; controller.begin(cfg);
  send(controller,"sensor/lobby/green","100"); send(controller,"sensor/lobby/red","100");
  controller.service(0,true); assert((controller.lastFrame()[1] == RGB{255,255,0} && controller.lastFrame()[5] == RGB{255,255,0}));
  assert(controller.lastBrightness() == 128);
  send(controller,"sensor/lobby/blue","100",0,true); send(controller,"sensor/lobby/blue","invalid");
  controller.service(0,true); assert((controller.lastFrame()[1] == RGB{255,255,0}));
  controller.ingest("sensor/lobby/blue",17,"1",1,0,3,false,0);
  controller.ingest(nullptr,0,"00",2,1,3,false,0); controller.service(0,true);
  assert((controller.lastFrame()[1] == RGB{255,255,255}));
  controller.ingest("sensor/lobby/blue",17,"0",1,0,3,false,0);
  controller.ingest(nullptr,0,"00",2,2,3,false,0); controller.service(0,true);
  assert(controller.lastFrame()[1].b == 255); // bad offset discards the fragment
  controller.pause(); controller.service(0,true); assert(controller.lastFrame()[1] == RGB{});
  send(controller,"sensor/lobby/red","100"); controller.resume(); controller.service(0,true); assert(controller.lastFrame()[1] == RGB{});
  for(unsigned i=0;i<33;++i) send(controller,"sensor/lobby/red",i==32?"0":"100");
  JsonDocument diagnostics; controller.state(diagnostics.to<JsonObject>()); assert(diagnostics["event_overflow"] == 1);
  controller.service(0,true); assert(controller.lastFrame()[1].r == 255);
  send(controller,"sensor/lobby/green","100"); send(controller,"sensor/lobby","off"); send(controller,"sensor/lobby/red","50");
  controller.service(0,true); assert((controller.lastFrame()[1] == RGB{128,0,0}));
  // A wildcard's parent and arbitrary suffixes are normal events, not rejected
  // or interpreted as color names. Concrete topics still mix independently.
  Controller woodshop;
  const Config woodshopConfig = patched(R"({"groups":[{"group":1,"topics":["motion/woodshopdoor/#"]}]})");
  woodshop.begin(woodshopConfig);
  send(woodshop,"motion/woodshopdoor","25"); woodshop.service(0,true);
  assert((woodshop.lastFrame()[1] == RGB{64,64,64}));
  send(woodshop,"motion/woodshopdoor/human","25"); woodshop.service(0,true);
  assert((woodshop.lastFrame()[1] == RGB{128,128,128}));
  send(woodshop,"motion/woodshopdoor/blue","100"); woodshop.service(0,true);
  assert((woodshop.lastFrame()[1] == RGB{128,128,255}));
  send(woodshop,"motion/woodshopdoor/human","50"); woodshop.service(0,true);
  assert((woodshop.lastFrame()[1] == RGB{191,191,191})); // normal event resumes mixing
  send(woodshop,"motion/woodshopdoor/human","off"); woodshop.service(0,true);
  assert((woodshop.lastFrame()[1] == RGB{64,64,64}));
  send(woodshop,"motion/woodshopdoor","off"); woodshop.service(0,true);
  assert(woodshop.lastFrame()[1] == RGB{});
  send(woodshop,"motion/woodshopdoor/human","lime"); woodshop.service(0,true);
  assert((woodshop.lastFrame()[1] == RGB{0,191,0})); // color comes from the payload
  woodshop.service(60000,true); assert(woodshop.lastFrame()[1] == RGB{});
  Subscriptions subscriptions; subscriptions.configure(cfg); assert(subscriptions.pending() == 2);
  subscriptions.connection(true); unsigned calls=0, removals=0; int id=1;
  auto subscribe = [&](const std::string &) { ++calls; return id++; };
  auto unsubscribe = [&](const std::string &) { ++removals; return id++; };
  subscriptions.service(0,subscribe,unsubscribe); assert(calls == 2);
  subscriptions.acknowledge(1); subscriptions.acknowledge(2); assert(subscriptions.pending() == 0);
  subscriptions.service(5000,subscribe,unsubscribe); assert(calls == 2);
  subscriptions.connection(false); subscriptions.service(6000,subscribe,unsubscribe); assert(calls == 2);
  subscriptions.connection(true); subscriptions.service(6000,subscribe,unsubscribe); assert(calls == 4);
  subscriptions.service(11000,subscribe,unsubscribe); assert(calls == 6); // missing ACK retries
  subscriptions.configure(Config{}); subscriptions.service(16000,subscribe,unsubscribe); assert(removals == 2 && subscriptions.pending() == 0);
  subscriptions.configure(cfg); subscriptions.service(20000,[](const std::string &) {return -1;},unsubscribe);
  assert(subscriptions.pending() == 2); subscriptions.service(25000,subscribe,unsubscribe); assert(calls == 8);
  subscriptions.connection(false); subscriptions.connection(true);
  subscriptions.service(26000,[&](const std::string &) { const int n=id++; subscriptions.acknowledge(n); return n; },unsubscribe);
  assert(subscriptions.pending() == 0); // ACK racing the subscribe return is preserved
  std::cout << "LED layout, wildcard filters, parsing, mixing, channel overrides, fades, FIFO and subscriptions passed.\n";
}
