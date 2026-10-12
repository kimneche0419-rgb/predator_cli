#!/usr/bin/env python3
"""Mine FIM training examples from the fixed local corpus (finetune/corpus/).

Boundary-aware, reproducible rewrite of the old random-byte cutter:

  * C      -- brace/paren-depth tracker; middles start/end only at
              statement / block boundaries (balanced braces, closed parens,
              line ends in ';' or '}').
  * Python -- bracket-depth + continuation tracker; middles start/end only
              at def/class/statement boundaries.
  * HTML   -- tag-balance scanner (void elements, comments, quoted attrs,
              <script>/<style> raw text); middles are tag-balanced runs.

The middle is ALWAYS a run of complete lines (1..40 lines, <=1200 chars,
non-empty guarded); prefix <= 2400 chars, suffix <= 800 chars, also aligned
to line boundaries.  Exact + whitespace-normalised dedup.  Deterministic
seed.  Qwen PSM order:

  {"prompt": "<|fim_prefix|>P<|fim_suffix|>S<|fim_middle|>", "completion": "M"}

Usage: python3 finetune/mine_github_dataset.py
"""
import json
import math
import random
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CORPUS = ROOT / "corpus"
OUT = ROOT / "data"

SEED = 42
TARGET_TOTAL = 25000
TARGET = {  # 40% / 35% / 25% of 25000
    "c": 10000,
    "py": 8750,
    "html": 6250,
}
POOL_CAP_FACTOR = 4          # candidate pool cap = TARGET * factor
PER_FILE_CAP = {"c": 40, "py": 40, "html": 300}
EXTS = {"c": (".c", ".h"), "py": (".py",), "html": (".html", ".htm")}
MAX_FILE_BYTES = 400_000

MIDDLE_MAX_LINES = 40
MIDDLE_MAX_CHARS = 1200
MIN_MIDDLE_CHARS = 16
PREFIX_MAX_CHARS = 2400
SUFFIX_MAX_CHARS = 800

FIM_PREFIX = "<|fim_prefix|>"
FIM_SUFFIX = "<|fim_suffix|>"
FIM_MIDDLE = "<|fim_middle|>"
WS_RE = re.compile(r"\s+")


# --------------------------------------------------------------------------
# Per-line scanners.  Each returns (lines, cum, unsafe, starts, ends):
#   lines      -- file split into lines
#   cum[k]     -- net depth delta contributed by line k-1 (k = 1..n)
#   unsafe[k-1]-- 1 if line k-1 cannot be a span boundary (starts/ends inside
#                 a comment/string/tag, or continues the previous line)
#   starts[k]  -- line k may START a middle (complete-statement boundary)
#   ends[k]    -- line k may END a middle (completes a statement/element)
# A middle spanning lines [i, j) is valid iff starts[i], unsafe[k]=0 for all
# k in [i, j), depth never dips below the start depth inside the span, and
# cum balance over the span is zero (balanced braces/tags).
# --------------------------------------------------------------------------

def scan_c(text):
    lines = text.split("\n")
    n = len(lines)
    cum = [0] * (n + 1)
    unsafe = [0] * n
    starts = [False] * n
    ends = [False] * n
    depth = paren = 0
    in_comment = in_str = in_chr = False
    prev_cont = False
    for idx, line in enumerate(lines):
        started_bad = in_comment or in_str or prev_cont
        d0 = depth
        j, cut = 0, len(line)
        while j < len(line):
            ch = line[j]
            if in_comment:
                if ch == "*" and line.startswith("*/", j):
                    in_comment = False
                    j += 2
                else:
                    j += 1
            elif in_str:
                if ch == "\\":
                    j += 2
                else:
                    if ch == '"':
                        in_str = False
                    j += 1
            elif in_chr:
                if ch == "\\":
                    j += 2
                else:
                    if ch == "'":
                        in_chr = False
                    j += 1
            elif ch == "/" and line.startswith("//", j):
                cut = j
                break
            elif ch == "/" and line.startswith("/*", j):
                in_comment = True
                j += 2
            elif ch == '"':
                in_str = True
                j += 1
            elif ch == "'":
                in_chr = True
                j += 1
            else:
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                elif ch == "(":
                    paren += 1
                elif ch == ")":
                    paren -= 1
                j += 1
        eff = line[:cut]
        cum[idx + 1] = depth - d0
        unsafe[idx] = 1 if (started_bad or in_comment or in_str) else 0
        prev_cont = (not in_comment and not in_str and not in_chr
                     and eff.rstrip().endswith("\\"))
        s = eff.strip()
        starts[idx] = not started_bad and paren == 0 and bool(s)
        ends[idx] = (not in_comment and not in_str and paren == 0
                     and (s.endswith(";") or s.endswith("}")))
    return lines, cum, unsafe, starts, ends


def scan_py(text):
    lines = text.split("\n")
    n = len(lines)
    cum = [0] * (n + 1)
    unsafe = [0] * n
    starts = [False] * n
    ends = [False] * n
    depth = 0
    quote = None      # active string delimiter: '"' or "'"
    triple = False
    prev_cont = False
    for idx, line in enumerate(lines):
        started_bad = quote is not None or prev_cont
        d0 = depth
        j, cut = 0, len(line)
        while j < len(line):
            ch = line[j]
            if quote is not None:
                if ch == "\\":
                    j += 2
                elif triple:
                    if line.startswith(quote * 3, j):
                        quote = None
                        j += 3
                    else:
                        j += 1
                else:
                    if ch == quote:
                        quote = None
                    j += 1
            elif ch == "#":
                cut = j
                break
            elif ch in "\"'":
                triple = line.startswith(ch * 3, j)
                quote = ch
                j += 3 if triple else 1
            elif ch in "([{":
                depth += 1
                j += 1
            elif ch in ")]}":
                depth -= 1
                j += 1
            else:
                j += 1
        eff = line[:cut]
        line_bad = quote is not None and not triple
        if line_bad:
            quote = None  # unterminated single-line string: stop at this line
        cum[idx + 1] = depth - d0
        unsafe[idx] = 1 if (started_bad or line_bad) else 0
        prev_cont = eff.rstrip().endswith("\\")
        s = eff.strip()
        starts[idx] = not started_bad and d0 == 0 and bool(s) and not s.startswith("#")
        ends[idx] = quote is None and depth == 0 and bool(s) and not eff.rstrip().endswith("\\")
    return lines, cum, unsafe, starts, ends


HTML_VOID = frozenset(
    "area base br col embed hr img input link meta param source track wbr".split()
)
HTML_RAWTEXT = frozenset({"script", "style"})


def scan_html(text):
    lines = text.split("\n")
    n = len(lines)
    cum = [0] * (n + 1)
    unsafe = [0] * n
    starts = [False] * n
    ends = [False] * n
    depth = 0
    in_comment = in_tag = False
    in_quote = None
    raw = None            # "script"/"style" while inside raw text
    buf = ""              # tag text accumulated so far
    for idx, line in enumerate(lines):
        started_bad = in_comment or in_tag or in_quote is not None
        d0 = depth
        low = line.lower()
        j = 0
        while j < len(line):
            if raw is not None:
                k = low.find("</" + raw, j)
                if k == -1:
                    j = len(line)
                    break
                j = k
            ch = line[j]
            if in_comment:
                if line.startswith("-->", j):
                    in_comment = False
                    j += 3
                else:
                    j += 1
            elif in_quote is not None:
                if ch == in_quote:
                    in_quote = None
                j += 1
            elif in_tag:
                if ch in "\"'":
                    in_quote = ch
                    j += 1
                elif ch == ">":
                    b = buf.strip()
                    if not b.startswith(("!", "?")):
                        closer = b.startswith("/")
                        body = b[1:] if closer else b
                        name = re.split(r"[\s/]", body, maxsplit=1)[0].lower() if body else ""
                        if name:
                            if closer:
                                depth -= 1
                                if name in HTML_RAWTEXT:
                                    raw = None
                            elif not b.endswith("/") and name not in HTML_VOID:
                                depth += 1
                                if name in HTML_RAWTEXT:
                                    raw = name
                    in_tag = False
                    buf = ""
                    j += 1
                else:
                    buf += ch
                    j += 1
            elif line.startswith("<!--", j):
                in_comment = True
                j += 4
            elif ch == "<":
                in_tag = True
                buf = "/"
                j += 2 if line.startswith("</", j) else 1
            else:
                j += 1
        cum[idx + 1] = depth - d0
        ended_bad = in_comment or in_tag or in_quote is not None
        unsafe[idx] = 1 if (started_bad or ended_bad) else 0
        s = line.strip()
        starts[idx] = not started_bad and bool(s)
        ends[idx] = not ended_bad and bool(s)
    return lines, cum, unsafe, starts, ends


SCANNERS = {"c": scan_c, "py": scan_py, "html": scan_html}


# --------------------------------------------------------------------------
# Span finding + materialisation
# --------------------------------------------------------------------------

def find_spans(meta, max_lines=MIDDLE_MAX_LINES):
    """All (i, j) line spans that are safe, balanced, and boundary-aligned."""
    lines, cum, unsafe, starts, ends = meta
    n = len(lines)
    spans = []
    for i in range(n):
        if not starts[i]:
            continue
        run = 0
        for j in range(i + 1, min(n, i + max_lines) + 1):
            if unsafe[j - 1]:
                break  # any span containing this line crosses a boundary
            run += cum[j]
            if run < 0:
                break  # closes something opened before the span
            if run == 0 and ends[j - 1]:
                spans.append((i, j))
    return spans


def build_prefix(lines, i, budget):
    """Whole lines before i, walking back; ≤ budget chars, ends at middle."""
    parts, total = [], 0
    for k in range(i - 1, -1, -1):
        piece = lines[k] + "\n"
        if total + len(piece) <= budget:
            parts.append(piece)
            total += len(piece)
        else:
            if k == i - 1 and budget - total > 0:
                parts.append(piece[-(budget - total):])  # tail slice keeps adjacency
            break
    return "".join(reversed(parts))


def build_suffix(lines, j, budget):
    """Whole lines after j, walking forward; ≤ budget chars, starts at middle."""
    if j >= len(lines):
        return ""
    parts, total = [], 0
    first = "\n" + lines[j]
    if len(first) > budget:
        return first[:budget]
    parts.append(first)
    total = len(first)
    for k in range(j + 1, len(lines)):
        piece = "\n" + lines[k]
        if total + len(piece) > budget:
            break
        parts.append(piece)
        total += len(piece)
    return "".join(parts)


def collect_pool(lang, files):
    """Materialise boundary-aligned FIM candidates for one language."""
    per_file_cap = PER_FILE_CAP[lang]
    pool_cap = TARGET[lang] * POOL_CAP_FACTOR
    pool = []
    for p in files:
        if len(pool) >= pool_cap:
            break
        try:
            if p.stat().st_size > MAX_FILE_BYTES:
                continue
            raw = p.read_bytes()
        except OSError:
            continue
        if b"\x00" in raw[:8192]:
            continue  # binary
        text = raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n")
        if text.count("\n") < 5:
            continue
        meta = SCANNERS[lang](text)
        spans = find_spans(meta)
        rng = random.Random(f"{SEED}:{lang}:{p.as_posix()}")
        rng.shuffle(spans)
        made = 0
        for i, j in spans:
            if made >= per_file_cap or len(pool) >= pool_cap:
                break
            mid = "\n".join(meta[0][i:j])
            if not (MIN_MIDDLE_CHARS <= len(mid) <= MIDDLE_MAX_CHARS):
                continue
            if not mid.strip():
                continue  # empty-middle guard
            prefix = build_prefix(meta[0], i, PREFIX_MAX_CHARS)
            suffix = build_suffix(meta[0], j, SUFFIX_MAX_CHARS)
            pool.append({
                "prompt": f"{FIM_PREFIX}{prefix}{FIM_SUFFIX}{suffix}{FIM_MIDDLE}",
                "completion": mid,
            })
            made += 1
    return pool


def p95(values):
    vs = sorted(values)
    return vs[min(len(vs) - 1, math.ceil(0.95 * len(vs)) - 1)]


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------

def main():
    selected = []          # (lang, row)
    exact_dupes = norm_dupes = 0
    seen_exact, seen_norm = set(), set()

    for lang in ("c", "py", "html"):
        files = sorted(
            p for p in (CORPUS / lang).rglob("*")
            if ".git" not in p.parts and p.is_file()
            and p.suffix.lower() in EXTS[lang]
        )
        random.Random(f"{SEED}:files:{lang}").shuffle(files)
        pool = collect_pool(lang, files)
        random.Random(f"{SEED}:pool:{lang}").shuffle(pool)
        kept = 0
        for row in pool:
            if kept >= TARGET[lang]:
                break
            mid = row["completion"]
            if mid in seen_exact:
                exact_dupes += 1
                continue
            norm = WS_RE.sub(" ", mid).strip()
            if norm in seen_norm:
                norm_dupes += 1
                continue
            seen_exact.add(mid)
            seen_norm.add(norm)
            selected.append((lang, row))
            kept += 1
        print(f"[{lang}] files={len(files)} pool={len(pool)} kept={kept} (target {TARGET[lang]})")

    # sanity: no empty middles, middles are 1..MIDDLE_MAX_LINES complete lines
    empty = sum(1 for _l, r in selected if not r["completion"].strip())
    line_viol = sum(
        1 for _l, r in selected
        if not (1 <= r["completion"].count("\n") + 1 <= MIDDLE_MAX_LINES)
    )

    # global 95 / 2.5 / 2.5 split
    random.Random(f"{SEED}:split").shuffle(selected)
    n = len(selected)
    n_va = round(n * 0.025)
    n_te = round(n * 0.025)
    splits = {
        "valid": selected[:n_va],
        "test": selected[n_va:n_va + n_te],
        "train": selected[n_va + n_te:],
    }
    OUT.mkdir(parents=True, exist_ok=True)
    for name, rows in splits.items():
        with open(OUT / f"{name}.jsonl", "w", encoding="utf-8") as f:
            for _lang, row in rows:
                f.write(json.dumps(
                    {"prompt": row["prompt"], "completion": row["completion"]},
                    ensure_ascii=False,
                ) + "\n")

    # ---- stats ----
    print(f"\n== dataset stats ==")
    print(f"total: {n} (target {TARGET_TOTAL})")
    for lang in ("c", "py", "html"):
        lens = [len(r["completion"]) for l, r in selected if l == lang]
        if not lens:
            print(f"{lang}: n=0")
            continue
        print(f"{lang}: n={len(lens)} mean_chars={sum(lens)/len(lens):.1f} "
              f"p95_chars={p95(lens)}")
    print(f"empty completions: {empty}")
    print(f"line-count violations (1..{MIDDLE_MAX_LINES}): {line_viol}")
    print(f"dedup: exact_removed={exact_dupes} normalized_removed={norm_dupes}")
    print(f"split: train={len(splits['train'])} valid={len(splits['valid'])} "
          f"test={len(splits['test'])} (95/2.5/2.5)")
    for name in ("train", "valid", "test"):
        print(f"wrote {OUT / (name + '.jsonl')}: {len(splits[name])} rows")
    if n < TARGET_TOTAL:
        print(f"note: corpus exhausted at {n} < {TARGET_TOTAL} target", file=sys.stderr)


if __name__ == "__main__":
    main()
