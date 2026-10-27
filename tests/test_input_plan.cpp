#include "input_sequence.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
int main(){
 nxc::input::Plan p;
 auto key=[](const char* s, uint64_t* bits){ if(strcmp(s,"A"))return false; *bits=1;return true;};
 assert(nxc::input::parsePlan("A:120;WAIT:20;A",120,key,p)); assert(p.count==3&&p.totalMs==260);
 assert(!nxc::input::parsePlan("A:120;BAD:20",120,key,p));
 assert(!nxc::input::parsePlan("A:12oops",120,key,p));
 assert(!nxc::input::parsePlan("A:99999999999999999999999999",120,key,p));
 assert(!nxc::input::parsePlan("WAIT:10000;WAIT:10000;WAIT:10000;WAIT:10000;WAIT:10000;WAIT:10000;A:1",120,key,p));
 puts("input preflight parsing: PASS");
}
