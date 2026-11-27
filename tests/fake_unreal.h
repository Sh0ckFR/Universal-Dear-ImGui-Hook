// The objects of an Unreal Engine game, laid out in the memory of the test
// host as the engine lays them out, for the engine commands of src/unreal.cpp:
// tests/fake_unreal.cpp. The name table and the object array are globals of
// the executable, as in a game, and the objects have a virtual table whose
// UObject::ProcessEvent calls their functions.
#pragma once

#include <cstdint>

struct FakeUnreal {
    // 5: Unreal Engine 5.8, with FNamePool, FField properties, the object of
    // FUObjectItem after its flags and a hash index after the name of
    // UObjectBase. 4: Unreal Engine 4.22, with TNameEntryArray, UProperty
    // objects and the object first in FUObjectItem.
    int generation = 0;
    // Game.GamePlayer: Alice, Bob, Carol; a Pawn; the default object of GamePlayer.
    uintptr_t alice = 0;
    uintptr_t bob = 0;
    uintptr_t carol = 0;
    uintptr_t pawn = 0;
    uintptr_t defaults = 0;
    // /Game/Blueprints/BP_Hero.BP_Hero_C, a blueprint class derived from GamePlayer: Dave.
    uintptr_t dave = 0;
    // The index of UObject::ProcessEvent in the virtual table: 0x44 (4.22) or 0x4f (5.8).
    int processEvent = 0;
    // The thread that started the fake: the thread of the host's window.
    uint64_t mainThread = 0;
};

// Builds the objects once; false when `generation` is neither 4 nor 5.
bool StartFakeUnreal(int generation, FakeUnreal& fake);
// The thread of the last call of ProcessEvent, 0 before the first, and the number of calls.
uint64_t FakeUnrealCallThread();
int FakeUnrealCalls();
