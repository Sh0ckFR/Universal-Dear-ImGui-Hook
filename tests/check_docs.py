"""Checks the Markdown files of the repository, as the CI does.

Python 3.8+, standard library only. Run it from anywhere:

    python tests/check_docs.py [FILE.md ...]

Without arguments, it checks every Markdown file tracked by git, except the
vendored ones in third_party/. It reports, outside fenced code blocks:

- relative links to files that do not exist, and #anchors that no heading of
  the target file produces, as GitHub builds them;
- table rows cut off from their table, such as a row after a blank line, which
  GitHub renders as a paragraph, and rows with another number of cells than
  the header;
- tabs and other control characters, which an escape such as \\t turned into
  when it was written by a tool that expanded it;
- code spans opened and never closed, and unbalanced <details> blocks.

It does not fetch external links. Exits with 1 when it found a problem.
"""
import os
import re
import subprocess
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
HEADING = re.compile(r"^ {0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
# [text](target "title") and ![alt](target), the target up to a space or ).
LINK = re.compile(r"\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HREF = re.compile(r"""<(?:a|img)\s[^>]*?(?:href|src)\s*=\s*["']([^"']+)["']""", re.IGNORECASE)
DELIMITER_ROW = re.compile(r"^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$")
CONTROL = re.compile(r"[\x00-\x09\x0b-\x1f\x7f]")   # tab included, newline excluded


def markdown_files():
    out = subprocess.run(["git", "ls-files", "-z", "*.md"], cwd=ROOT, check=True,
                         stdout=subprocess.PIPE).stdout.decode("utf-8")
    return [os.path.join(ROOT, f) for f in out.split("\0") if f and not f.startswith("third_party/")]


def prose_lines(text):
    """(line number, line) of the lines outside fenced code blocks; fenced lines become None."""
    fence = None
    for number, line in enumerate(text.split("\n"), 1):
        match = FENCE.match(line)
        if fence is None and match:
            fence = match.group(1)
            yield number, None
        elif fence is not None:
            if match and match.group(1)[0] == fence[0] and len(match.group(1)) >= len(fence) \
                    and not line.strip()[len(match.group(1)):].strip():
                fence = None
            yield number, None
        else:
            yield number, line


def slug(heading):
    """The anchor GitHub gives a heading, before the -1, -2 suffixes of duplicates."""
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", heading)   # links and images keep their text
    text = re.sub(r"<[^>]+>", "", text)                          # HTML tags
    text = text.lower()
    kept = []
    for char in text:
        category = unicodedata.category(char)
        if char in " -_" or category[0] in "LN" or category == "Mn":
            kept.append("-" if char == " " else char)
    return "".join(kept)


_anchors = {}


def anchors(path):
    if path not in _anchors:
        with open(path, encoding="utf-8") as file:
            text = file.read()
        found, seen = set(), {}
        for _, line in prose_lines(text):
            match = line is not None and HEADING.match(line)
            if match:
                base = slug(match.group(2))
                count = seen.get(base, 0)
                seen[base] = count + 1
                found.add(base if count == 0 else "%s-%d" % (base, count))
        # Explicit anchors: <a name="x"> or id="x".
        found.update(re.findall(r"""<[^>]+\b(?:name|id)\s*=\s*["']([^"']+)["']""", text))
        _anchors[path] = found
    return _anchors[path]


def cells(row):
    """Cells of a table row: GitHub splits on every | not escaped by a backslash, code spans included."""
    row = row.strip()
    if row.startswith("|"):
        row = row[1:]
    if row.endswith("|") and not row.endswith("\\|"):
        row = row[:-1]
    return len(re.split(r"(?<!\\)\|", row))


def unclosed_code_span(text):
    """True when a run of backticks opens a code span that no run of the same length closes."""
    # A backslash escapes nothing inside a code span, so `\\` is a whole span.
    runs = re.findall(r"`+", text)
    while runs:
        opener = runs.pop(0)
        if opener not in runs:
            return True
        runs = runs[runs.index(opener) + 1:]
    return False


def check(path, problems):
    def report(number, message):
        problems.append("%s:%d: %s" % (os.path.relpath(path, ROOT).replace(os.sep, "/"), number, message))

    with open(path, encoding="utf-8", newline="") as file:
        text = file.read().replace("\r\n", "\n")
    lines = list(prose_lines(text))

    # Links and anchors.
    for number, line in lines:
        if line is None:
            continue
        for target in LINK.findall(line) + HREF.findall(line):
            if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE):   # https:, mailto:...
                continue
            file_part, _, anchor = target.partition("#")
            file_part = file_part.split("?")[0]
            resolved = os.path.normpath(os.path.join(os.path.dirname(path), file_part)) if file_part else path
            if not resolved.startswith(ROOT):
                continue   # ../../releases and other pages of GitHub, outside the repository
            if not os.path.exists(resolved):
                report(number, "link to a missing file: " + target)
            elif anchor and resolved.endswith(".md") and anchor.lower() not in anchors(resolved):
                report(number, "link to a missing anchor: " + target)

    # Tables: blocks of consecutive lines that start with |.
    index = 0
    while index < len(lines):
        number, line = lines[index]
        if line is None or not line.lstrip().startswith("|"):
            index += 1
            continue
        block = []
        while index < len(lines) and lines[index][1] is not None and lines[index][1].lstrip().startswith("|"):
            block.append(lines[index])
            index += 1
        if len(block) < 2 or not DELIMITER_ROW.match(block[1][1]):
            report(number, "table row outside a table: a header and a | --- | row must come first, "
                           "with no blank line in the table")
            continue
        columns = cells(block[0][1])
        if cells(block[1][1]) != columns:
            report(block[1][0], "the | --- | row has %d cells, the header %d" % (cells(block[1][1]), columns))
        for row_number, row in block[2:]:
            if cells(row) != columns:
                report(row_number, "%d cells in a table of %d columns (escape a | in a cell as \\|)"
                       % (cells(row), columns))

    # Control characters, anywhere outside code blocks.
    for number, line in lines:
        if line is not None and CONTROL.search(line):
            report(number, "control character %r: write escapes such as \\t as text"
                   % CONTROL.search(line).group())

    # Code spans, per paragraph; each table row is a paragraph of its own.
    paragraph = []
    for number, line in lines + [(len(lines) + 1, "")]:
        is_row = line is not None and line.lstrip().startswith("|")
        if line is None or not line.strip() or is_row:
            if paragraph and unclosed_code_span("\n".join(text for _, text in paragraph)):
                report(paragraph[0][0], "code span opened with ` and never closed")
            paragraph = []
            if is_row and unclosed_code_span(line):
                report(number, "code span opened with ` and never closed")
        else:
            paragraph.append((number, line))

    # <details> blocks.
    prose = "\n".join(line for _, line in lines if line is not None)
    prose = re.sub(r"(`+)(?!`).*?(?<!`)\1(?!`)", "", prose, flags=re.DOTALL)   # `<details>` in a code span
    opened, closed = len(re.findall(r"<details\b", prose)), len(re.findall(r"</details>", prose))
    if opened != closed:
        report(1, "%d <details> for %d </details>" % (opened, closed))


def main():
    files = [os.path.abspath(f) for f in sys.argv[1:]] or markdown_files()
    problems = []
    for path in files:
        check(path, problems)
    for problem in problems:
        print(problem)
    print("%d Markdown files checked, %d problems" % (len(files), len(problems)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
