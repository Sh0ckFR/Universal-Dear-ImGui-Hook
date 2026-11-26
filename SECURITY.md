# Security policy

## Supported versions

Fixes go into `master` and the next [release](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/releases). Only the latest release is supported.

## Reporting a vulnerability

Report it privately through [GitHub's private vulnerability reporting](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/security/advisories/new) (*Security* tab, *Report a vulnerability*), not in a public issue.

Include the version or commit, the operating system, the settings of the library (`remoteControl`, `remoteMemory`, `aiAdvancedFeatures`), the steps to reproduce and what an attacker gains. Reports are handled on a best-effort basis, fixed in a new release, and credited in the advisory unless you prefer otherwise.

## What counts as a vulnerability

The library is injected into games on purpose, and its remote control gives a local program control of the game on purpose. These behaviors are by design, and documented in the [README](README.md#remote-control-by-an-ai-assistant):

- with `remoteControl` on, programs running on the same machine (on Linux, only those of the same user) can see the game and send it keys and clicks;
- with `remoteMemory` on, those programs can also read and write the game's memory, bypassing Yama's ptrace restrictions on Linux;
- Claude, through `remote/uih_chat.py`, acts on the game with the game's tools;
- anti-cheat systems detect the DLL.

These are vulnerabilities:

- reaching the pipe or socket from another machine, or on Linux from another user;
- the remote control answering while `remoteControl` is off, or the memory or advanced commands answering while their setting is off;
- the bridge or `remote/uih_mcp.py` giving Claude other tools than the game's, such as a shell or file access, including through text shown by the game;
- a pipe or socket command that crashes the game, or writes memory that `memory_write` was not asked to write;
- released libraries that do not match their `SHA256SUMS` or their build provenance attestation.
