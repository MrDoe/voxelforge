#!/usr/bin/env python3
"""Structural lint for .opencode/wiki — no judgement, only structure.

Checks:
  1. split-from-body   — a `## ` heading with no content in the following lines
  2. orphan-rows       — table row lines outside any table
  3. dangling-links    — [[targets]] with no matching page (code spans excluded)
  4. index-coverage    — every page listed in index ## Pages EXACTLY ONCE
                         (presence is not uniqueness)
  5. dup-headings      — duplicate `## ` headings within one file
  6. stale-pages       — sourceRef files changed on disk since lastReviewed
  7. self-test         — positive controls prove the detectors fire

The one deliberate dead link ([[concepts/water-flooding]] in log.md,
a superseded timestamp) lives in ALLOWLIST, reported separately.

Usage: python3 .opencode/wiki/lint.py   (run from repo root)
Exit 0 = clean apart from allowlisted items; exit 1 = findings.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent          # .opencode/wiki
REPO = ROOT.parent.parent                                # repo root
ALLOWLIST = {("log.md", "concepts/water-flooding")}
VERBOSE = False


def strip_code(text):
    """Remove fenced blocks and inline code spans."""
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    return re.sub(r"`[^`]*`", "", text)


def headings_of(lines):
    """(lineno, title) for `## ` headings outside fences."""
    out, inf = [], False
    for i, l in enumerate(lines, 1):
        if l.lstrip().startswith("```"):
            inf = not inf
            continue
        if not inf and l.startswith("## "):
            out.append((i, l[3:].strip()))
    return out


def check_split_body(path, lines):
    bad, inf = [], False
    fence = [l.lstrip().startswith("```") for l in lines]
    for idx, l in enumerate(lines):
        if fence[idx]:
            inf = not inf
            continue
        if inf or not l.startswith("## "):
            continue
        body = [x for x in lines[idx + 1:idx + 4]
                if x.strip() and not x.lstrip().startswith("```")]
        if not body:
            bad.append(idx + 1)
    return bad


def check_orphan_rows(path, lines):
    def is_row(l):
        return l.lstrip().startswith("|")

    def is_sep(l):
        return bool(re.match(r"^\s*\|[\s:\-|]+\|\s*$", l))

    bad, inf, prev = [], False, False
    for i, l in enumerate(lines):
        if l.lstrip().startswith("```"):
            inf = not inf
            continue
        if inf:
            continue
        if is_row(l):
            nxt = lines[i + 1] if i + 1 < len(lines) else ""
            if not prev and not is_sep(nxt):
                bad.append(i + 1)
            prev = True
        elif l.strip() == "":
            pass
        else:
            prev = False
    return bad


def page_links(text):
    return re.findall(r"\[\[([^\]\|]+)", strip_code(text))


def check_dangling(pages, path, text):
    return sorted({l.strip() for l in page_links(text)
                   if l.strip() not in pages})


def check_index(pages, index_text):
    m = re.search(r"^## Pages\s*$", index_text, re.M)
    if not m:
        return {"no_pages_section": True}, []
    tail = index_text[m.end():]
    nxt = re.search(r"^## \S", tail, re.M)
    section = tail[:nxt.start()] if nxt else tail
    found = re.findall(r"\[\[([^\]\|]+)", section)
    counts = {}
    for l in found:
        counts[l.strip()] = counts.get(l.strip(), 0) + 1
    missing = sorted(p for p in pages if p not in counts)
    dupes = sorted(p for p, n in counts.items() if n > 1)
    unknown = sorted(p for p in counts if p not in pages)
    return missing, dupes, unknown


def parse_frontmatter(text):
    """title/lastReviewed/sourceRefs, both `[a, b]` and `- item` forms."""
    m = re.match(r"^---\n(.*?)\n---\n", text, re.S)
    fm = {"title": None, "lastReviewed": None, "sourceRefs": []}
    if not m:
        return fm
    head = m.group(1)
    t = re.search(r"^title:\s*(.+)$", head, re.M)
    r = re.search(r"^lastReviewed:\s*(\S+)", head, re.M)
    if t:
        fm["title"] = t.group(1).strip().strip("\"'")
    if r:
        fm["lastReviewed"] = r.group(1).strip()
    s = re.search(r"^sourceRefs:\s*\[(.*)\]", head, re.M)
    if s and s.group(1).strip():
        fm["sourceRefs"] = [x.strip() for x in s.group(1).split(",")]
    else:
        fm["sourceRefs"] = re.findall(r"^  - (\S.*?)\s*$", head, re.M)
    return fm


def repo_state(repo_rel):
    """(committed_date|None, dirty_bool|None, kind) for a repo-relative path.

    kind is one of OK / STALE / DIRTY / UNTRACKED / MISSING / GENERATED.
    GENERATED covers build/ outputs, which must never be sourceRefs.
    """
    if repo_rel.startswith("build/"):
        return None, None, "GENERATED"
    if re.match(r"https?://", repo_rel):
        return None, None, "URL"
    if not (REPO / repo_rel).exists():
        return None, None, "MISSING"
    r = subprocess.run(["git", "-C", str(REPO), "log", "-1",
                        "--format=%cs", "--", repo_rel],
                       capture_output=True, text=True)
    date = r.stdout.strip() or None
    s = subprocess.run(["git", "-C", str(REPO), "status", "--short",
                        "--", repo_rel], capture_output=True, text=True)
    dirty = bool(s.stdout.strip())
    if date is None:
        return None, dirty, "UNTRACKED"
    return date, dirty, "OK"


def check_stale(pages_meta, verbose):
    """Rollup per page; details only with --verbose.

    A committed change after lastReviewed makes a page SUSPECT (not proven
    stale — the change may be unrelated to what the page documents), and an
    uncommitted modification is invisible to commit dates, so dirty files
    are listed separately. Both need human judgement; the script only ranks.
    """
    rollup, details, other = [], [], []
    for page, fm in sorted(pages_meta.items()):
        since = fm["lastReviewed"]
        if not since:
            other.append((page, "NO lastReviewed", ""))
            continue
        newest, n_stale, dirty_refs = since, 0, []
        for ref in fm["sourceRefs"]:
            date, dirty, kind = repo_state(ref)
            if kind in ("GENERATED", "MISSING"):
                other.append((page, kind, ref))
            elif kind == "UNTRACKED":
                other.append((page, "UNTRACKED", ref))
            elif kind == "URL":
                continue
            else:
                if dirty:
                    dirty_refs.append(ref)
                if date and date > since:
                    n_stale += 1
                    newest = max(newest, date)
        if n_stale:
            rollup.append((page, since, newest, n_stale))
        for ref in dirty_refs:
            details.append((page, ref))
    return rollup, details, other


def self_test():
    """Positive controls: each detector must fire on synthetic input."""
    ok = True

    def check(name, got, want_nonempty=True):
        nonlocal ok
        fired = bool(got) == want_nonempty
        print(f"  self-test {name}: {'PASS' if fired else 'FAIL'}")
        ok = ok and fired

    check("split-body", check_split_body("t", ["## Lone", "", ""]),
          True)
    check("split-body-negative",
          check_split_body("t", ["## Fine", "", "body"]), False)
    check("orphan-row", check_orphan_rows("t", ["text", "", "| lone |"]),
          True)
    check("orphan-row-negative",
          check_orphan_rows("t", ["| a |", "|---|---|", "| b |"]), False)
    check("dangling", check_dangling({"a"}, "t", "see [[nope]]"), True)
    check("dangling-negative",
          check_dangling({"a"}, "t", "see [[a]] and `[[nope]]`"), False)
    miss, dupes, _ = check_index({"a", "b"}, "## Pages\n- [[a]]\n- [[a]]\n")
    check("index-dupe", dupes, True)
    check("index-missing", miss, True)
    fm = parse_frontmatter("---\ntitle: T\nlastReviewed: 2026-10-08\n"
                           "sourceRefs:\n  - a/b.md\n  - AGENTS.md\n---\n")
    check("frontmatter-listform",
          fm["sourceRefs"] == ["a/b.md", "AGENTS.md"], True)
    return ok


def main():
    global VERBOSE
    VERBOSE = "--verbose" in sys.argv
    print("== self-test ==")
    healthy = self_test()
    files = sorted(ROOT.rglob("*.md"))
    pages = {str(f.relative_to(ROOT).with_suffix(""))
             for f in files if f.name not in ("index.md", "log.md")}
    findings = 0   # hard: split/orphan/dangling/index/dupe/generated/missing
    advisory = 0   # soft: suspect/untracked/dirty — ranked, never fail

    print("== structure ==")
    for f in files:
        rel = str(f.relative_to(ROOT))
        lines = f.read_text().splitlines()
        for n in check_split_body(rel, lines):
            print(f"  split-from-body {rel}:{n}")
            findings += 1
        for n in check_orphan_rows(rel, lines):
            print(f"  orphan-row {rel}:{n}")
            findings += 1
        cur = [t for _, t in headings_of(lines)]
        for t in sorted({t for t in cur if cur.count(t) > 1}):
            print(f"  dup-heading {rel}: {t!r}")
            findings += 1
        for target in check_dangling(pages, rel, f.read_text()):
            if (rel, target) in ALLOWLIST:
                print(f"  dangling (allowlisted) {rel} -> {target}")
            else:
                print(f"  DANGLING {rel} -> {target}")
                findings += 1

    print("== index ==")
    miss, dupes, unknown = check_index(
        pages, (ROOT / "index.md").read_text())
    for p in miss:
        print(f"  UNINDEXED {p}")
        findings += 1
    for p in dupes:
        print(f"  DUPE-INDEX {p}")
        findings += 1
    for p in unknown:
        print(f"  UNKNOWN-INDEX {p}")
        findings += 1

    print("== staleness (sourceRefs vs lastReviewed) ==")
    meta = {str(f.relative_to(ROOT)): parse_frontmatter(f.read_text())
            for f in files
            if f.name not in ("index.md", "log.md")}
    rollup, dirty, other = check_stale(meta, VERBOSE)
    for page, since, newest, n in rollup:
        print(f"  SUSPECT {page}: {n} ref(s) committed {since}..{newest}")
        advisory += 1
    if VERBOSE:
        for page, ref in dirty:
            print(f"    dirty {page}: {ref}")
    elif dirty:
        print(f"  (dirty-tree refs on {len({p for p, _ in dirty})} page(s); "
              f"rerun --verbose for the list)")
    for page, kind, ref in other:
        if kind in ("GENERATED", "MISSING") or kind.startswith("NO "):
            print(f"  {kind} {page}: {ref}")
            findings += 1
        else:
            print(f"  {kind} (advisory) {page}: {ref}")
            advisory += 1

    print(f"== pages: {len(pages)} | hard findings: {findings} "
          f"| advisory: {advisory} "
          f"| self-test: {'PASS' if healthy else 'FAIL'} ==")
    # SUSPECT / UNTRACKED / DIRTY are advisory — they rank pages for human
    # review but never fail the gate. Only hard structural findings
    # (split, orphan, dangling, index gaps/dupes, dup headings, GENERATED or
    # MISSING refs, missing lastReviewed) fail it.
    return 0 if (findings == 0 and healthy) else 1


if __name__ == "__main__":
    sys.exit(main())
