#include "../Source/Ge1100Engine.h"
#include <chrono>
#include <cstdio>
int main() {
    std::array<double,12> p; double acc = 0; const int N = 50;
    auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < N; ++k) { for (int i = 0; i < 12; ++i) p[(size_t) i] = 0.5 + 0.4 * std::sin (k * 0.7 + i); auto c = ge::buildContinuous (p); acc += c.D; }
    auto t1 = std::chrono::steady_clock::now();
    for (int k = 0; k < N; ++k) { for (int i = 0; i < 12; ++i) p[(size_t) i] = 0.5 + 0.4 * std::sin (k * 0.7 + i); auto c = ge::buildContinuous (p); auto m = ge::modalFoh (c, 1.0 / 192000); acc += m.d; }
    auto t2 = std::chrono::steady_clock::now();
    std::printf ("build %.3f ms, build+modal %.3f ms per update (%g)\n", std::chrono::duration<double, std::milli> (t1 - t0).count() / N, std::chrono::duration<double, std::milli> (t2 - t1).count() / N, acc);
}
