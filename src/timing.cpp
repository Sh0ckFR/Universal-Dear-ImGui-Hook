// Speed of the game's clocks, set by the assistant with Config::aiAdvancedFeatures:
// 0 pauses the game while the assistant thinks, 0.25 slows it down. The
// platforms hook the clock functions the game reads (src/windows/clocks.cpp,
// src/linux/clocks.cpp) and turn each real reading into the game's here, so
// the game's time stays continuous across changes of speed.
//   time                 -> ok <speed>
//   time <speed>         -> ok <speed>   (0 pauses, 1 is real time, at most 8)
//   time advance <ms>    -> ok 0         (runs the game at speed 1 for ms, then pauses)
// While the game is paused, the input commands run it for their duration.
#include "internal.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace uih::detail::timing {
namespace {

constexpr double kMaxSpeed = 8.0;
constexpr unsigned long kMaxAdvanceMs = 10000;

// Per clock, its reading at the last change of speed, real and for the game.
struct Base {
    int64_t real = 0;
    int64_t game = 0;
    bool set = false;
};

std::mutex g_mutex;
Base g_bases[kClockCount];
double g_speed = 1.0;
// Changes of speed, for the input commands that run a paused game.
uint64_t g_epoch = 0;
// False until the speed first changes: every clock reads real time, at no cost.
std::atomic<bool> g_scaling{false};
thread_local int t_realTime = 0;

// Under g_mutex.
int64_t Scale(Base& base, int64_t real) {
    return base.game + static_cast<int64_t>(std::llround(static_cast<double>(real - base.real) * g_speed));
}

// Under g_mutex.
void Rebase(double speed) {
    for (int clock = 0; clock < kClockCount; ++clock) {
        int64_t real = 0;
        if (!RealClock(static_cast<Clock>(clock), real))
            continue;
        Base& base = g_bases[clock];
        int64_t game = base.set ? Scale(base, real) : real;
        base = {real, game, true};
    }
    g_speed = speed;
    ++g_epoch;
}

} // namespace

bool Scaling() { return g_scaling.load(std::memory_order_acquire) && !t_realTime; }

int64_t GameTime(Clock clock, int64_t real) {
    if (!Scaling())
        return real;
    std::lock_guard<std::mutex> lock(g_mutex);
    Base& base = g_bases[clock];
    // A clock first read since the change, such as timeGetTime() once winmm.dll loads.
    if (!base.set)
        base = {real, real, true};
    return Scale(base, real);
}

double Speed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_speed;
}

void SetSpeed(double speed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (speed == g_speed)
        return;
    Rebase(speed);
    g_scaling.store(true, std::memory_order_release);
}

void Reset() { SetSpeed(1.0); }

RealTime::RealTime() { ++t_realTime; }
RealTime::~RealTime() { --t_realTime; }

RunWhilePaused::RunWhilePaused() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_speed != 0.0)
        return;
    Rebase(1.0);
    epoch_ = g_epoch;
}

RunWhilePaused::~RunWhilePaused() {
    std::lock_guard<std::mutex> lock(g_mutex);
    // Unless the speed changed meanwhile, from the window of the overlay.
    if (epoch_ && g_epoch == epoch_)
        Rebase(0.0);
}

std::string Command(const std::string& rest) {
    char answer[48];
    if (!ClocksHooked())
        return "error the clocks of the game are not hooked";
    if (rest.rfind("advance", 0) == 0) {
        unsigned long ms = strtoul(rest.c_str() + 7, nullptr, 10);
        if (ms == 0 || ms > kMaxAdvanceMs)
            return "error give the milliseconds to run the game for, 1 to 10000";
        SetSpeed(1.0);
        RealTime real;
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until && !remote::StopRequested())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        SetSpeed(0.0);
        return "ok 0";
    }
    if (!rest.empty()) {
        char* end = nullptr;
        double speed = strtod(rest.c_str(), &end);
        // NaN fails both comparisons.
        if (end == rest.c_str() || !(speed >= 0.0) || !(speed <= kMaxSpeed))
            return "error give a speed from 0 (paused) to 8, 1 being real time";
        SetSpeed(speed);
    }
    snprintf(answer, sizeof(answer), "ok %g", Speed());
    return answer;
}

} // namespace uih::detail::timing
