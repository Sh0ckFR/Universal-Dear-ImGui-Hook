// The speed of the game's clocks, set with the time command, that the Windows
// and the Linux test hosts check the same way. gameMs reads the clock as the
// game's code does, in milliseconds. Include after remote_advanced.h.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

// Milliseconds of game time that pass while `ms` real ones do.
inline int64_t GameMsDuring(int64_t (*gameMs)(), int ms) {
    int64_t start = gameMs();
    Pause(ms);
    return gameMs() - start;
}

// Empty when the commands worked, the reason otherwise.
template <typename Client>
std::string CheckRemoteTime(Client& client, int64_t (*gameMs)()) {
    std::string initial = client.Request("time");
    int64_t normal = GameMsDuring(gameMs, 200);
    std::string paused = client.Request("time 0");
    int64_t whilePaused = GameMsDuring(gameMs, 300);
    // The game runs while the assistant waits on it, then pauses again.
    int64_t beforeWait = gameMs();
    std::string waited = client.Request("wait 200 100");
    int64_t duringWait = gameMs() - beforeWait;
    std::string stillPaused = client.Request("time");
    int64_t beforeAdvance = gameMs();
    std::string advanced = client.Request("time advance 150");
    int64_t duringAdvance = gameMs() - beforeAdvance;
    int64_t afterAdvance = GameMsDuring(gameMs, 200);
    std::string half = client.Request("time 0.5");
    int64_t whileHalf = GameMsDuring(gameMs, 400);
    std::string tooFast = client.Request("time 9");
    std::string notANumber = client.Request("time nan");
    std::string zeroAdvance = client.Request("time advance 0");
    std::string back = client.Request("time 1");
    int64_t restored = GameMsDuring(gameMs, 200);
    printf("time: %s, %lld ms of game time in 200 ms\ntime 0: %s, %lld ms in 300 ms\nwait while paused: %s, %lld ms, then time: %s\n"
           "time advance 150: %s, %lld ms, then %lld ms in 200 ms\ntime 0.5: %s, %lld ms in 400 ms\ntime 9: %s\ntime nan: %s\n"
           "time advance 0: %s\ntime 1: %s, %lld ms in 200 ms\n",
           initial.c_str(), static_cast<long long>(normal), paused.c_str(), static_cast<long long>(whilePaused), waited.c_str(),
           static_cast<long long>(duringWait), stillPaused.c_str(), advanced.c_str(), static_cast<long long>(duringAdvance),
           static_cast<long long>(afterAdvance), half.c_str(), static_cast<long long>(whileHalf), tooFast.c_str(), notANumber.c_str(),
           zeroAdvance.c_str(), back.c_str(), static_cast<long long>(restored));
    // The sanitizer runtimes stand for clock_gettime() before the library: nothing to check.
    if (initial.rfind("error the clocks of the game are not hooked", 0) == 0)
        return "";
    if (initial != "ok 1" || normal < 150 || normal > 400)
        return "the game's clock does not run in real time at first";
    if (paused != "ok 0" || whilePaused < 0 || whilePaused > 5)
        return "the game's clock does not stop at speed 0";
    if (waited.rfind("ok ", 0) != 0 || duringWait < 150 || stillPaused != "ok 0")
        return "the game does not run while the assistant waits on it, or does not pause again";
    if (advanced != "ok 0" || duringAdvance < 120 || duringAdvance > 400 || afterAdvance > 5)
        return "time advance does not run the game for its milliseconds, then pause it";
    if (half != "ok 0.5" || whileHalf < 150 || whileHalf > 300)
        return "the game's clock does not run at half speed";
    if (tooFast.rfind("error give a speed", 0) != 0 || notANumber.rfind("error give a speed", 0) != 0 ||
        zeroAdvance.rfind("error give the milliseconds", 0) != 0)
        return "the time command takes a wrong speed";
    if (back != "ok 1" || restored < 150 || restored > 400)
        return "the game's clock does not run in real time again";
    return "";
}
