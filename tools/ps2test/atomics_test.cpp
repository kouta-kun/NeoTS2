// Runtime check of exceptions, std::atomic, std::mutex, std::thread, std::async, std::filesystem.
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <debug.h>

static int failures = 0;
#define CHECK(c) do { bool ok_ = (c); std::printf("%s %s\n", ok_ ? "PASS" : "FAIL", #c); if (!ok_) ++failures; } while (0)

int main() {
    std::printf("== ps2 runtime test ==\n");

    try { throw std::runtime_error("boom"); } catch (const std::exception& e) { CHECK(std::string(e.what()) == "boom"); }

    std::atomic<int> a{0};
    a.fetch_add(5); a.fetch_sub(2);
    CHECK(a.load() == 3);
    int exp = 3; CHECK(a.compare_exchange_strong(exp, 9)); CHECK(a == 9);
    std::atomic_flag f = ATOMIC_FLAG_INIT; CHECK(!f.test_and_set()); CHECK(f.test_and_set());

    std::mutex m; { std::lock_guard<std::mutex> l(m); CHECK(true); }

    int shared = 0; std::mutex sm;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) ts.emplace_back([&] { for (int j = 0; j < 1000; ++j) { std::lock_guard<std::mutex> l(sm); ++shared; } });
    for (auto& t : ts) t.join();
    CHECK(shared == 4000);

    auto fut = std::async(std::launch::async, [] { return 42; });
    CHECK(fut.get() == 42);

    std::error_code ec;
    std::filesystem::path p = "host:/";
    CHECK(!p.empty());
    (void)std::filesystem::exists("mc0:/", ec);

    std::printf("== done: %d failure(s) ==\n", failures);
    return failures;
}
