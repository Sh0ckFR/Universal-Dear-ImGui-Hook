"""Checks that CMake and the Visual Studio solution build the same sources, as the CI does.

Python 3.8+, standard library only. Run it from anywhere:

    python tests/check_sources.py

The library is built by two build systems: CMakeLists.txt, and the projects
of vs/. A source added to one and not the other builds on one side and fails
to link on the other; a merge can also drop a line from one list. It reports:

- a source of src/ that CMakeLists.txt does not build;
- a source of src/, outside src/linux/, that vs/uih.vcxproj does not build;
- a Windows source of example/ that vs/Universal-ImGui-Hook.vcxproj does not
  build, and a source of the Windows uih_host that vs/uih_host.vcxproj does
  not build, or the other way round;
- a file named by CMakeLists.txt, tests/CMakeLists.txt or a vs/ project that
  does not exist.

Exits with 1 when it found a problem.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = re.compile(r"\.(c|cc|cpp)$")
# Built only on Linux, by CMake.
LINUX_EXAMPLES = {"example/preload.cpp"}
# Files a vs/ project names; their paths are relative to vs/.
VS_ITEM = re.compile(r'<(?:ClCompile|ClInclude|ResourceCompile|None|Image)\s+Include="([^"]+)"')
# A path of the repository in a CMakeLists.txt, outside ${...} expressions.
CMAKE_PATH = re.compile(r"(?<![\w/${}.-])((?:[\w.-]+/)*[\w.-]+\.(?:c|cc|cpp|h|rc|def|map|png))(?![\w/])")


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as file:
        return file.read()


def tracked(pattern):
    out = subprocess.run(["git", "ls-files", "-z", pattern], cwd=ROOT, check=True,
                         stdout=subprocess.PIPE).stdout.decode("utf-8")
    return sorted(f for f in out.split("\0") if f and SOURCE.search(f))


def vs_items(project):
    """The files a vs/ project names, relative to the root, with / separators."""
    items = []
    for name in VS_ITEM.findall(read(project)):
        if "$(" in name:
            continue
        path = os.path.normpath(os.path.join("vs", name.replace("\\", "/")))
        items.append(path.replace(os.sep, "/"))
    return items


def cmake_paths(cmake_file):
    """The files a CMakeLists.txt names, relative to the root, comments removed."""
    base = os.path.dirname(cmake_file)
    text = "\n".join(line.split("#", 1)[0] for line in read(cmake_file).split("\n"))
    paths = []
    for name in CMAKE_PATH.findall(text):
        if "${" in name or "<" in name:
            continue
        paths.append(os.path.normpath(os.path.join(base, name)).replace(os.sep, "/"))
    return paths


def target_sources(cmake_file, call):
    """The sources of the first `call(` of cmake_file, such as 'add_executable(uih_host'."""
    text = read(cmake_file)
    start = text.index(call) + len(call)
    body = text[start:text.index(")", start)]
    base = os.path.dirname(cmake_file)
    return sorted(os.path.normpath(os.path.join(base, word)).replace(os.sep, "/")
                  for word in body.split() if SOURCE.search(word))


def main():
    problems = []

    cmake = set(cmake_paths("CMakeLists.txt"))
    for source in tracked("src/"):
        if source not in cmake:
            problems.append(f"CMakeLists.txt does not build {source}")

    vs_library = set(vs_items("vs/uih.vcxproj"))
    for source in tracked("src/"):
        if not source.startswith("src/linux/") and source not in vs_library:
            problems.append(f"vs/uih.vcxproj does not build {source}")

    vs_dll = set(vs_items("vs/Universal-ImGui-Hook.vcxproj"))
    for source in tracked("example/"):
        if source not in LINUX_EXAMPLES and source not in vs_dll:
            problems.append(f"vs/Universal-ImGui-Hook.vcxproj does not build {source}")

    cmake_host = set(target_sources("tests/CMakeLists.txt", "add_executable(uih_host"))
    vs_host = {f for f in vs_items("vs/uih_host.vcxproj") if SOURCE.search(f)}
    for source in sorted(cmake_host - vs_host):
        problems.append(f"vs/uih_host.vcxproj does not build {source}, which tests/CMakeLists.txt builds into uih_host")
    for source in sorted(vs_host - cmake_host):
        problems.append(f"tests/CMakeLists.txt does not build {source} into uih_host, which vs/uih_host.vcxproj builds")

    named = [("CMakeLists.txt", p) for p in cmake] + [("tests/CMakeLists.txt", p) for p in cmake_paths("tests/CMakeLists.txt")]
    for project in sorted(f for f in os.listdir(os.path.join(ROOT, "vs")) if f.endswith(".vcxproj")):
        named += [("vs/" + project, p) for p in vs_items("vs/" + project)]
    for owner, path in named:
        if not os.path.exists(os.path.join(ROOT, path)):
            problems.append(f"{owner} names {path}, which does not exist")

    for problem in problems:
        print(problem)
    if problems:
        print(f"{len(problems)} problem(s): add the file to both build systems, or remove it from both")
        return 1
    print("CMake and the Visual Studio solution build the same sources")
    return 0


if __name__ == "__main__":
    sys.exit(main())
