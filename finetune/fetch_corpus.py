#!/usr/bin/env python3
"""Shallow-clone a small, reputable open-source corpus for FIM mining.

Clones each repository once (git clone --depth 1 --single-branch) into
    finetune/corpus/<lang>/<name>/
Re-running skips repositories that are already present, so the corpus stays
fixed on disk and dataset mining stays reproducible. Individual clone
failures are logged and skipped (robust); the script exits non-zero only if
some language ends up with no repository at all.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CORPUS = ROOT / "corpus"
CLONE_TIMEOUT = 900  # seconds per repository

REPOS = {
    "c": [
        ("redis", "https://github.com/redis/redis.git"),
        ("nginx", "https://github.com/nginx/nginx.git"),
        ("curl", "https://github.com/curl/curl.git"),
        ("sqlite", "https://github.com/sqlite/sqlite.git"),
        ("memcached", "https://github.com/memcached/memcached.git"),
    ],
    "py": [
        ("flask", "https://github.com/pallets/flask.git"),
        ("requests", "https://github.com/psf/requests.git"),
        ("rich", "https://github.com/Textualize/rich.git"),
        ("httpie", "https://github.com/httpie/cli.git"),
    ],
    "html": [
        ("htmx", "https://github.com/bigskysoftware/htmx.git"),
        ("bootstrap", "https://github.com/twbs/bootstrap.git"),
    ],
}

EXTS = {"c": {".c", ".h"}, "py": {".py"}, "html": {".html", ".htm"}}


def count_files(root: Path, exts) -> int:
    n = 0
    for p in root.rglob("*"):
        if ".git" in p.parts:
            continue
        if p.is_file() and p.suffix.lower() in exts:
            n += 1
    return n


def main() -> None:
    if shutil.which("git") is None:
        print("error: git not found on PATH", file=sys.stderr)
        sys.exit(1)

    failures = []
    for lang, repos in REPOS.items():
        (CORPUS / lang).mkdir(parents=True, exist_ok=True)
        for name, url in repos:
            dest = CORPUS / lang / name
            if (dest / ".git").exists():
                print(f"[present] {lang}/{name}: {dest}")
                continue
            print(f"[clone  ] {lang}/{name} <- {url}", flush=True)
            try:
                subprocess.run(
                    ["git", "-c", "advice.detachedHead=false",
                     "clone", "--depth", "1", "--single-branch", url, str(dest)],
                    check=True, timeout=CLONE_TIMEOUT,
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                )
            except (subprocess.SubprocessError, OSError) as exc:
                detail = getattr(exc, "stderr", None)
                msg = str(exc) if detail in (None, b"") else f"{exc}: {detail.decode(errors='replace')[:200]}"
                failures.append(f"{lang}/{name}: {msg}")
                print(f"[FAILED ] {lang}/{name}: {msg}", file=sys.stderr, flush=True)
                shutil.rmtree(dest, ignore_errors=True)
                continue
            print(f"[ok     ] {lang}/{name}")

    print("\n== corpus summary ==")
    for lang in REPOS:
        lang_dir = CORPUS / lang
        have_any = False
        for name, _url in REPOS[lang]:
            sub = lang_dir / name
            ok = (sub / ".git").exists()
            have_any = have_any or ok
            n = count_files(sub, EXTS[lang]) if ok else 0
            print(f"  {lang}/{name}: {'ok' if ok else 'MISSING'} ({n} files)")
        total = count_files(lang_dir, EXTS[lang]) if lang_dir.exists() else 0
        print(f"  {lang}: total {total} files")
        if not have_any:
            print(f"error: no corpus cloned for language '{lang}'", file=sys.stderr)
            sys.exit(1)
    if failures:
        print(f"\nnote: {len(failures)} repository clone(s) failed and were skipped", file=sys.stderr)


if __name__ == "__main__":
    main()
