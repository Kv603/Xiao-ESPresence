#include "../Control_Core.h"
#include <cassert>
#include <iostream>
int main() {
  control::Assembly a;
  assert(!a.add("t",1,"ab",2,0,4,false));
  assert(a.add(nullptr,0,"cd",2,2,4,false));
  assert(a.body()=="abcd"&&a.topic()=="t");
  assert(!a.add("t",1,"x",1,0,1,true));
  assert(!a.add("t",1,"x",1,0,8193,false));
  assert(!a.add("t",1,"x",1,0,0,false));
  assert(!a.add("t",1,"ab",2,0,4,false));
  assert(!a.add(nullptr,0,"cd",2,3,4,false));
  assert(!a.add(nullptr,0,"cd",2,2,4,false));
  assert(!a.add("t",1,"ab",2,0,4,false));
  assert(!a.add("u",1,"cd",2,2,4,false));
  assert(!a.add("t",1,"ab",2,0,4,false));
  assert(!a.add(nullptr,0,"cd",2,2,3,false));
  std::string big(8192,'x');assert(a.add("t",1,big.data(),big.size(),0,big.size(),false));
  assert(control::requestId("deploy-123_abc")&&!control::requestId("")&&!control::requestId(std::string(65,'a')));
  assert(!control::requestId("space here")&&!control::requestId("a/b"));
  assert(control::firmwareUrl("https://example.com/app.bin")&&control::firmwareUrl("http://192.168.1.2:8000/app.bin"));
  for(const char *s:{"ftp://a/b","http:///b","https://","http://user:pass@a/b","http://a/b#fragment","http://a/\nb"})assert(!control::firmwareUrl(s));
  std::cout<<"Control fragmentation, bounds, retained commands and URL validation passed.\n";
}
