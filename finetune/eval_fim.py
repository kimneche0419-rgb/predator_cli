#!/usr/bin/env python3
"""Offline FIM evaluation harness.

HumanEval-Infilling (single_line / multi_line / random_span) 을 로컬 Ollama로
greedy(n=1, temperature=0) 추론해 exact-match pass@1(= exact-match rate)을 집계한다.

프롬프트 형식: Qwen PSM (raw=true)
    <|fim_prefix|>{prefix}<|fim_suffix|>{suffix}<|fim_middle|>

후처리 규칙:
  - single_line  : 생성물을 첫 "\n" 에서 절단
  - multi_line / random_span : 생성 줄 중 suffix에 (부분 문자열로) 처음 일치하는
    줄에서 절단. 일치하는 줄이 없으면 전체 유지.

요청 간 간섭 방지를 위해 항상 순차 실행한다(모델별로 몰아서 실행해
Ollama 프롬프트 prefix 캐시를 재사용). 표준 라이브러리만 사용한다.
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from statistics import mean

DEFAULT_MODELS = ["lvc-coder:latest", "qwen2.5-coder:3b"]
DEFAULT_BASE_URL = "http://localhost:11434"
DEFAULT_OUT = "finetune/eval/results.md"
DEFAULT_DATA_DIR = "finetune/eval/data"

DATA_URL = "https://raw.githubusercontent.com/openai/human-eval-infilling/master/data/{name}"
SUBSET_FILES = {
    "single_line": "HumanEval-SingleLineInfilling.jsonl.gz",
    "multi_line": "HumanEval-MultiLineInfilling.jsonl.gz",
    "random_span": "HumanEval-RandomSpanInfilling.jsonl.gz",
}
STOP_TOKENS = ["<|endoftext|>", "<|fim_pad|>"]


# ---------------------------------------------------------------- data loading


def ensure_data(data_dir: Path) -> None:
    """서브셋 파일이 없으면 raw.githubusercontent에서 내려받는다."""
    data_dir.mkdir(parents=True, exist_ok=True)
    for name in SUBSET_FILES.values():
        path = data_dir / name
        if path.exists() and path.stat().st_size > 0:
            continue
        url = DATA_URL.format(name=name)
        print(f"[data] downloading {url} -> {path}", file=sys.stderr)
        with urllib.request.urlopen(url, timeout=120) as resp:
            path.write_bytes(resp.read())


def load_subset(data_dir: Path, subset: str, limit: int | None) -> list[dict]:
    path = data_dir / SUBSET_FILES[subset]
    problems = []
    with gzip.open(path, "rt") as fp:
        for line in fp:
            if line.strip():
                problems.append(json.loads(line))
                if limit is not None and len(problems) >= limit:
                    break
    return problems


# ------------------------------------------------------------------- inference


def fim_prompt(prefix: str, suffix: str) -> str:
    return f"<|fim_prefix|>{prefix}<|fim_suffix|>{suffix}<|fim_middle|>"


def ollama_generate(
    base_url: str, model: str, prompt: str, num_predict: int, timeout: float
) -> str:
    body = json.dumps(
        {
            "model": model,
            "prompt": prompt,
            "raw": True,
            "stream": False,
            "options": {
                "num_predict": num_predict,
                "temperature": 0,
                "stop": STOP_TOKENS,
            },
        }
    ).encode("utf-8")
    req = urllib.request.Request(
        f"{base_url}/api/generate",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    # Ollama의 일시적 5xx 만 재시도 (순차 실행 유지)
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
            return payload.get("response", "")
        except urllib.error.HTTPError as exc:
            if exc.code < 500 or attempt == 2:
                raise
            time.sleep(1.0 * (attempt + 1))


# --------------------------------------------------------------- postprocess


def postprocess(raw: str, subset: str, suffix: str) -> str:
    if subset == "single_line":
        return raw.split("\n", 1)[0]
    # multi_line / random_span: suffix와 처음 일치하는 줄에서 절단
    lines = []
    for line in raw.split("\n"):
        if line and line in suffix:
            break
        lines.append(line)
    return "\n".join(lines)


def exact_match(completion: str, canonical: str) -> bool:
    return completion.strip() == canonical.strip()


# --------------------------------------------------------------------- runner


def slug(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-")


def run_model_subset(
    base_url: str,
    model: str,
    subset: str,
    problems: list[dict],
    num_predict: int,
    timeout: float,
) -> list[dict]:
    results = []
    for i, prob in enumerate(problems):
        prompt = fim_prompt(prob["prompt"], prob["suffix"])
        t0 = time.perf_counter()
        error = None
        raw = ""
        try:
            raw = ollama_generate(base_url, model, prompt, num_predict, timeout)
        except Exception as exc:  # 한 샘플 실패가 전체 실행을 죽이지 않도록
            error = f"{type(exc).__name__}: {exc}"
        latency = time.perf_counter() - t0
        completion = "" if error else postprocess(raw, subset, prob["suffix"])
        em = (not error) and exact_match(completion, prob["canonical_solution"])
        results.append(
            {
                "task_id": prob["task_id"],
                "raw": raw,
                "completion": completion,
                "canonical": prob["canonical_solution"],
                "exact_match": em,
                "latency_s": latency,
                "error": error,
            }
        )
        status = "ERR" if error else ("EM" if em else "miss")
        sys.stderr.write(
            f"\r[{model} | {subset}] {i + 1}/{len(problems)} "
            f"last={status} lat={latency:.2f}s   "
        )
        sys.stderr.flush()
    sys.stderr.write("\n")
    return results


def summarize(results: list[dict]) -> dict:
    latencies = [r["latency_s"] for r in results]
    return {
        "n": len(results),
        "em": sum(r["exact_match"] for r in results),
        "avg_latency_s": mean(latencies) if latencies else 0.0,
        "total_s": sum(latencies),
        "errors": sum(r["error"] is not None for r in results),
    }


# --------------------------------------------------------------------- report


def samples_block(results: list[dict], count: int = 3) -> str:
    """후처리 전/후 비교 샘플 (raw 출력 vs 처리 후 vs 정답)."""
    blocks = []
    picked = results[:count]
    if len(picked) < count:  # 에러 등으로 부족하면 정상 샘플로 보충
        picked += [r for r in results[count:] if r["error"] is None][: count - len(picked)]
    for r in picked:
        fence = "~~~"
        while fence in r["raw"] or fence in r["completion"] or fence in r["canonical"]:
            fence += "~"
        blocks.append(
            f"- `{r['task_id']}` ({'EM' if r['exact_match'] else 'miss'})\n"
            f"  - raw:\n\n{fence}\n{r['raw']}{fence}\n"
            f"  - postprocessed:\n\n{fence}\n{r['completion']}{fence}\n"
            f"  - canonical:\n\n{fence}\n{r['canonical']}{fence}\n"
        )
    return "\n".join(blocks)


def write_report(
    out_path: Path,
    models: list[str],
    subsets: list[str],
    all_results: dict[tuple[str, str], list[dict]],
    args: argparse.Namespace,
) -> None:
    lines = [
        "# FIM offline eval (HumanEval-Infilling, greedy exact-match)",
        "",
        f"- date: {time.strftime('%Y-%m-%d %H:%M:%S')}",
        f"- engine: local Ollama `{args.base_url}` (`/api/generate`, `raw=true`), 순차 실행",
        "- prompt: `<|fim_prefix|>{prefix}<|fim_suffix|>{suffix}<|fim_middle|>` (Qwen PSM)",
        f"- sampling: greedy (temperature=0, n=1), num_predict={args.num_predict}, "
        f"stop={STOP_TOKENS}",
        "- metric: exact-match rate (postprocessed completion == canonical_solution, "
        "strip 비교). greedy n=1 이므로 exact-match rate == pass@1.",
        "- postprocess: single_line=첫 `\\n` 절단, multi_line/random_span=suffix와 "
        "첫 일치하는 줄에서 절단",
        f"- data: HumanEval-Infilling, subset별 선두 {args.limit or '전체'} 샘플",
        "",
        "## Results",
        "",
        "| model | subset | n | exact match | EM rate | avg latency (s) | total (s) | errors |",
        "|---|---|---|---|---|---|---|---|",
    ]
    for model in models:
        for subset in subsets:
            rs = all_results[(model, subset)]
            s = summarize(rs)
            rate = 100.0 * s["em"] / s["n"] if s["n"] else 0.0
            lines.append(
                f"| `{model}` | {subset} | {s['n']} | {s['em']} | {rate:.1f}% | "
                f"{s['avg_latency_s']:.2f} | {s['total_s']:.1f} | {s['errors']} |"
            )
    lines += ["", "## Samples (raw vs postprocessed vs canonical)", ""]
    for model in models:
        for subset in subsets:
            lines += [f"### `{model}` — {subset}", "", samples_block(all_results[(model, subset)])]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n")


# ------------------------------------------------------------------------ cli


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--model",
        action="append",
        default=None,
        metavar="M",
        help="ollama model (repeatable; default: " + " ".join(DEFAULT_MODELS) + ")",
    )
    p.add_argument(
        "--subset",
        action="append",
        default=None,
        choices=sorted(SUBSET_FILES),
        help="subset to evaluate (repeatable; default: all)",
    )
    p.add_argument("--limit", type=int, default=None, help="max samples per subset")
    p.add_argument("--out", default=DEFAULT_OUT, help="output markdown path")
    p.add_argument("--data-dir", default=DEFAULT_DATA_DIR)
    p.add_argument("--base-url", default=DEFAULT_BASE_URL)
    p.add_argument("--num-predict", type=int, default=256)
    p.add_argument("--timeout", type=float, default=300.0, help="per-request timeout (s)")
    p.add_argument(
        "--run-dir",
        default=None,
        help="dir for raw jsonl dumps (default: <out parent>/runs)",
    )
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    models = args.model or DEFAULT_MODELS
    subsets = args.subset or sorted(SUBSET_FILES)
    data_dir = Path(args.data_dir)
    out_path = Path(args.out)
    run_dir = Path(args.run_dir) if args.run_dir else out_path.parent / "runs"

    ensure_data(data_dir)

    all_results: dict[tuple[str, str], list[dict]] = {}
    for model in models:  # 모델별로 몰아서 순차 실행 (prefix 캐시 재사용)
        for subset in subsets:
            problems = load_subset(data_dir, subset, args.limit)
            results = run_model_subset(
                args.base_url, model, subset, problems, args.num_predict, args.timeout
            )
            all_results[(model, subset)] = results
            run_dir.mkdir(parents=True, exist_ok=True)
            dump = run_dir / f"{slug(model)}_{subset}.jsonl"
            with dump.open("w") as fp:
                for r in results:
                    fp.write(json.dumps(r, ensure_ascii=False) + "\n")
            s = summarize(results)
            print(
                f"[done] {model} | {subset}: EM {s['em']}/{s['n']} "
                f"({100.0 * s['em'] / s['n']:.1f}%), avg {s['avg_latency_s']:.2f}s, "
                f"errors {s['errors']} -> {dump}",
                file=sys.stderr,
            )

    write_report(out_path, models, subsets, all_results, args)
    print(f"[report] wrote {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
