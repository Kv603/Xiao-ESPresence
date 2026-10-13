#include <cassert>
#include <iostream>
#include "../LED_Core.h"
int main() {
  leds::Model model;
  leds::Value value;
  assert(leds::parseValue("red",value));
  for(size_t g=0;g!=leds::GROUPS;++g) model.apply({"t/"+std::to_string(g),value,uint64_t(1)<<g,0});
  auto frame = model.frame(0);
  for(size_t p=0;p<leds::PIXELS;++p) {
    bool lit = (p-LED_SKIP_FIRST)%leds::STRIDE < size_t(LED_GROUP_WIDTH);
    if constexpr (LED_SKIP_FIRST != 0) if (!p) lit = false;
    assert(frame[p].r == (lit ? 191 : 0) && !frame[p].g && !frame[p].b);
  }
  std::cout << "LED alternate layout passed: " << leds::PIXELS << " pixels, " << leds::GROUPS << " groups.\n";
}
