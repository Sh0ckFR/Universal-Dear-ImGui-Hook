# Contributing

Bug reports, fixes and features are welcome. Open an [issue](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/issues/new/choose) first for a large change, so we can agree on the approach before you write it. Security problems go to a private report: see [SECURITY.md](SECURITY.md).

## Principles

- **One file.** The library ships as a single DLL or `.so`: no configuration file or other file next to it. Settings are compiled in, from `example/config.cpp`.
- **No dependencies.** Dear ImGui, MinHook, Zydis and the Vulkan headers are vendored in `third_party/`, and the C and C++ runtimes are linked statically. The scripts of `remote/` use the Python standard library only.
- **The game comes first.** A fault in the overlay removes the overlay instead of crashing the game, and the backends leave the graphics state as the game set it.

## Rules

- A new source file goes into `CMakeLists.txt` **and** the matching project in `vs/`.
- A fix comes with a `uih_host` scenario that fails without it. [tests/README.md](tests/README.md) lists the scenarios.
- Run the CTest suite on Windows; the CI covers Linux.
- A change of behavior updates the README, and `python tests/check_docs.py` passes.
- A new source file goes in `CMakeLists.txt` and in the project of `vs/` that builds it: `python tests/check_sources.py` checks it.

## Getting started

[Building](README.md#building) lists the toolchains and presets. To build with the tests:

```sh
cmake --preset msvc-x64 -DUIH_BUILD_TESTS=ON && cmake --build --preset msvc-x64
ctest --test-dir build/msvc-x64 -C Release
```

Some tests press real keys and move the mouse: leave the machine alone while they run. `uih_host --preview <dll>` shows a menu without a game.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) maps where the code is and how it works.

## Pull requests

- The title says what the change does, as a sentence: *Hide presses and motion from Linux games while the menu is open*.
- One subject per pull request.
- Label it `bug`, `enhancement` or `documentation`: the labels sort the [release notes](.github/release.yml).

## Releases

Pushing a `v*` tag publishes a release once the whole CI has passed. The message of an annotated tag becomes the highlights at the top of the release notes, above the generated list of pull requests:

```sh
git tag -a v2.2.0 -m "v2.2.0" -m "- What changed for users, in a few lines"
git push origin v2.2.0
```
