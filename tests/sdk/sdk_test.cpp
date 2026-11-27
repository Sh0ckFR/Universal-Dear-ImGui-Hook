// A trainer of the fake game of tests/fake_unreal.cpp, built against the SDK
// that `engine dump all` wrote of it: it reads and writes the properties of
// its objects through the accessors, and calls their functions through the
// wrappers, as a DLL injected into a real game does with the SDK of that game.
// uih_host --sdk-test loads it and calls uih_sdk_test() on a thread of its own.
#include "uih_sdk.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define UIH_SDK_TEST_EXPORT extern "C" __declspec(dllexport)
#else
#define UIH_SDK_TEST_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

std::string g_report;

bool Check(bool condition, const std::string& what) {
    g_report += (condition ? "ok    " : "FAIL  ") + what + "\n";
    return condition;
}

bool Near(double value, double expected) { return std::fabs(value - expected) < 1e-4; }

template <typename T>
T Raw(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(T));
    return value;
}

// The functions of the game run on its game thread: the thread of its window
// on Windows, through uesdk::GameThread; on Linux, where the SDK has none,
// the fake game takes the calls of this thread.
template <typename Work>
bool OnGameThread(Work work) {
#ifdef _WIN32
    return uesdk::GameThread::Run(work, 5000);
#else
    work();
    return true;
#endif
}

bool Run() {
    bool initialized = uesdk::WaitForInit(5000);
    if (!Check(initialized, "uesdk::WaitForInit(): " + uesdk::LastError()))
        return false;
    bool ok = true;
    SDK::AGamePlayer* alice = uesdk::FindObject<SDK::AGamePlayer>("GamePlayer_0");
    SDK::AGamePlayer* bob = uesdk::FindObject<SDK::AGamePlayer>("GamePlayer_1");
    SDK::ABP_Hero_C* dave = uesdk::FirstInstance<SDK::ABP_Hero_C>();
    if (!Check(alice && bob && dave, "FindObject<AGamePlayer>(\"GamePlayer_0\"), FirstInstance<ABP_Hero_C>()"))
        return false;
    ok = Check(alice->GetName() == "GamePlayer_0" && alice->GetFullName() == "GamePlayer /Script/Game.GamePlayer_0", "GetName(), GetFullName(): " +
               alice->GetFullName()) && ok;
    ok = Check(alice->IsA<SDK::APawn>() && !alice->IsA<SDK::ABP_Hero_C>() && dave->Cast<SDK::AGamePlayer>() == dave, "IsA<T>(), Cast<T>()") && ok;
    ok = Check(uesdk::Instances<SDK::AGamePlayer>().size() == 4 && uesdk::Instances<SDK::APawn>().size() == 5, "Instances<T>()") && ok;
    ok = Check(SDK::AGamePlayer::StaticClass() && SDK::AGamePlayer::StaticClass()->GetName() == "GamePlayer", "StaticClass()") && ok;

    // Properties: references to the memory of the object.
    ok = Check(alice->Health() == 100 && alice->Health_Max() == 200 && alice->Speed() == 4.5f, "Health(), Health_Max(), Speed()") && ok;
    alice->Health() = 70;
    ok = Check(Raw<int32_t>(alice, 0x100) == 70, "Health() = 70 writes the object") && ok;
    ok = Check(alice->Team() == SDK::ETeam::Green && alice->bAlive(), "Team() == ETeam::Green, bAlive()") && ok;
    ok = Check(alice->bHidden(), "bHidden(), a bit of a byte") && ok;
    alice->bHidden() = false;
    ok = Check(Raw<uint8_t>(alice, 0x58) == 0x01, "bHidden() = false clears its bit alone") && ok;
    ok = Check(alice->PlayerName().ToString() == "Alice" && alice->Target() == bob, "PlayerName(), Target()") && ok;
    ok = Check(alice->Location().X() == 1 && alice->Location().Z() == 3, "Location().X(), a struct by value") && ok;
    ok = Check(alice->Inventory().Count() == 3 && alice->Inventory()[2] == 4, "Inventory(), a TArray") && ok;
    ok = Check(alice->Tags().Count() == 2 && alice->Tags()[1].ToString() == "Player", "Tags(), a TArray<FName>") && ok;
    ok = Check(dave->Mana() == 50.0f, "Mana(), a property of a blueprint class") && ok;

    // Functions, through ProcessEvent on the game thread. The SDK of a game
    // whose ProcessEvent the library did not find has wrappers that fail with
    // an error; the fake game's is always found.
    if (uesdk::layout::kProcessEvent < 0) {
        bool called = alice->Heal(5);
        ok = Check(!called && !uesdk::LastError().empty(), "without ProcessEvent, Heal(5) fails: " + uesdk::LastError()) && ok;
        uesdk::Shutdown();
        return ok;
    }
    bool healed = false;
    bool onGameThread = false;
    int32_t total = 0;
    float ratio = 0;
    int32_t health = 0;
    int32_t boosted = 21;
    SDK::FVector location{};
    float heroRatio = 0;
    int32_t sum = 0;
    bool ran = OnGameThread([&] {
#ifdef _WIN32
        onGameThread = uesdk::GameThread::IsCurrent();
#else
        onGameThread = true;
#endif
        healed = alice->Heal(5);
        health = alice->GetStats(10, total, ratio);
        alice->Boost(boosted);
        location = alice->GetLocation();
        SDK::FVector to{};
        to.X() = 9;
        to.Y() = 8;
        to.Z() = 7;
        alice->SetLocation(to);
        alice->Rename(uesdk::FString(u"Zed"));
        heroRatio = dave->Get_Health_Ratio();
        sum = SDK::UKismetMathLibrary::Add_IntInt(20, 22);
    });
    ok = Check(ran && onGameThread, "GameThread::Run(): " + uesdk::LastError()) && ok;
    ok = Check(healed && alice->Health() == 75, "Heal(5) returns true, Health() is 75") && ok;
    ok = Check(health == 75 && total == 85 && Near(ratio, 0.75), "GetStats(10, total, ratio): out parameters") && ok;
    ok = Check(boosted == 42, "Boost(value): a parameter by reference") && ok;
    ok = Check(location.X() == 1 && location.Y() == 2 && location.Z() == 3, "GetLocation(), a struct returned") && ok;
    ok = Check(alice->Location().X() == 9 && alice->Location().Z() == 7, "SetLocation(to), a struct given") && ok;
    ok = Check(alice->PlayerName().ToString() == "Zed", "Rename(FString): " + alice->PlayerName().ToString()) && ok;
    ok = Check(Near(heroRatio, 0.45), "Get_Health_Ratio(), the blueprint function \"Get Health Ratio\"") && ok;
    ok = Check(sum == 42, "UKismetMathLibrary::Add_IntInt(20, 22), a static function") && ok;
    ok = Check(uesdk::LastError().empty(), "no error: " + uesdk::LastError()) && ok;
    uesdk::Shutdown();
    return ok;
}

} // namespace

UIH_SDK_TEST_EXPORT int uih_sdk_test(char* report, int size) {
    g_report.clear();
    bool ok = Run();
    snprintf(report, static_cast<size_t>(size), "%s", g_report.c_str());
    return ok ? 0 : 1;
}
