// The objects of an Unreal Engine game, laid out in the memory of the test
// host as the engine lays them out, for the engine commands of src/unreal.cpp:
// tests/fake_unreal.cpp. The name table and the object array are globals of
// the executable, as in a game.
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
};

// Builds the objects once; false when `generation` is neither 4 nor 5.
bool StartFakeUnreal(int generation, FakeUnreal& fake);
