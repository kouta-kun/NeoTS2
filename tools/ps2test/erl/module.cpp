// Relocatable module exercising what the lot editor would need across an ERL boundary.
#include <cstdio>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

// Called by the host after loading so the unwinder can see this module's frames.
extern "C" void __register_frame_info(const void*, void*);
extern "C" char erl_eh_frame_begin[];
static char ehObject[64];
extern "C" void erl_register_eh() { __register_frame_info(erl_eh_frame_begin, ehObject); }

extern "C" int erl_test_plain(int n) { return n * 2; }

extern "C" int erl_test_vector(int n) {
    std::vector<int> v;
    for (int i = 0; i < n; ++i) v.push_back(i * i);
    int sum = 0;
    for (int x : v) sum += x;
    return sum;
}

extern "C" int erl_test_catch(int n) {
    try { throw std::runtime_error("from erl"); }
    catch (const std::exception& e) { std::printf("[erl] caught: %s\n", e.what()); return n; }
    return -1;
}

extern "C" int erl_test_format(int n) {
    std::string s = std::format("n={}", n);
    std::printf("[erl] %s\n", s.c_str());
    return static_cast<int>(s.size());
}
