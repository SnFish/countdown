#include "timer.hpp"
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char* name) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); std::exit(1); }
}

int main() {
    using namespace countdown;
    Timer timer(5);
    timer.toggle(100);
    check(timer.seconds(100) == 5 && timer.seconds(1099) == 5, "ceiling keeps the initial second");
    check(timer.seconds(1100) == 4, "a whole second elapses");
    timer.toggle(1350);
    check(timer.remainingMs(100000) == 3750, "pause freezes subsecond precision");
    timer.toggle(100000);
    check(timer.remainingMs(100100) == 3650, "resume preserves subsecond precision");
    timer.adjust(2, 100100);
    check(timer.remainingMs(100100) == 5650 && timer.state() == State::Running, "adjust without stopping");
    check(timer.update(110000) && timer.state() == State::Finished, "missed ticks and sleep still expire");
    check(!timer.update(120000), "completion fires once");
    timer.toggle(120000);
    check(timer.seconds(120000) == 5 && timer.state() == State::Running, "restart after completion");
    timer.reset(0);
    check(timer.preset() == 1 && timer.state() == State::Ready, "zero duration is clamped");
    timer.adjust(-999, 0);
    check(timer.seconds(0) == 1, "adjust cannot become negative");
    timer.reset(Timer::maxSeconds + 1);
    timer.adjust(1000000, 0);
    check(timer.seconds(0) == Timer::maxSeconds, "upper bound");
    timer.reset(300);
    timer.toggle(0);
    for (int i = 1; i < 100; ++i) {
        auto now = static_cast<std::uint64_t>(i) * 937;
        check(timer.remainingMs(now) == 300000 - now, "irregular sampling does not drift");
    }
    timer.reset(300);
    timer.setRange(60, 600, 0);
    timer.adjust(-1000, 0);
    check(timer.seconds(0) == 60, "custom minimum bounds manual adjustment");
    timer.adjust(1000, 0);
    check(timer.seconds(0) == 600, "custom maximum bounds manual adjustment");
    timer.reset(60);
    timer.toggle(0);
    check(timer.seconds(59000) == 1, "natural countdown continues below the adjustment minimum");
    check(timer.update(60000), "natural countdown reaches zero with a positive minimum");
    timer.reset(600);
    timer.toggle(100000);
    timer.setRange(10, 30, 101000);
    check(timer.state() == State::Running && timer.seconds(101000) == 30, "new range clamps a running timer");
    timer.reset(2);
    check(timer.seconds(0) == 10, "reset respects custom minimum");
    std::puts("PASS: timer precision, pause/resume, adjustment, expiry, restart, bounds, drift, custom ranges");
}
