#include "nxc_sdk.hpp"
#include <cassert>
#include <climits>
#include <string>
int main() {
    nxc::Args a;
    assert(!a.add("data", std::string(4200, 'x').c_str()));
    assert(a.count() == 0);
    assert(a.invalid());
    nxc::Args b;
    assert(b.add("len", "12oops"));
    assert(b.getInt("len", -9) == -9);
    assert(b.invalid());
    nxc::Args c;
    assert(c.add("v", "9223372036854775808"));
    assert(c.getInt("v", -3) == -3);
    assert(c.invalid());
    nxc::Args d;
    assert(d.add("v", "-9223372036854775808"));
    assert(d.getInt("v", 0) == LLONG_MIN);
    assert(!d.invalid());
    assert(!d.add("v", "1"));
    nxc::Args e;
    for (int i=0; i<nxc::kMaxArgs; ++i) assert(e.add(std::to_string(i).c_str(), "x"));
    assert(!e.add("extra", "x"));
    assert(e.invalid());
    puts("SDK strict validation: PASS");
}
