#!/usr/bin/env python3
"""Build a FIM (fill-in-the-middle) fine-tuning dataset from LVC suggestion logs.

Input:  one or more JSONL files from the editor's collection log
        ({ts, lang, suggestion, accepted}).
Output: JSONL for Hugging Face Trainer with Qwen-style FIM tokens:
        {"prompt": "<|fim_prefix|>...<|fim_suffix|>...<|fim_middle|>", "completion": "..."}

Accepted suggestions become positive examples. Rejected suggestions are used
as hard negatives only if --negatives is set (prefixed as contrast pairs).
"""
import argparse, json, random, sys

FIM_PRE = "<|fim_prefix|>"
FIM_SUF = "<|fim_suffix|>"
FIM_MID = "<|fim_middle|>"

def load_logs(paths):
    for p in paths:
        with open(p) as f:
            for ln, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    print(f"{p}:{ln}: bad JSON, skipped", file=sys.stderr)
                    continue
                yield rec

def make_example(rec, ctx_lines=8):
    """synthesize prefix/suffix context around the suggestion deterministically:
    we don't store the original buffer, so we wrap the suggestion in a
    structural context derived from its own first line (function-ish frame)."""
    s = rec["suggestion"].rstrip("\n")
    first = s.split("\n", 1)[0].strip()
    indent = first[: len(first) - len(first.lstrip())]
    prefix = "\n".join([f"{indent}// context {i}" for i in range(ctx_lines)]) + "\n" + indent
    suffix = "\n" + f"{indent}}}" if s.count("{") > s.count("}") else "\n"
    prompt = f"{FIM_PRE}{prefix}{FIM_SUF}{suffix}{FIM_MID}"
    return {"prompt": prompt, "completion": s}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+", help="suggestion JSONL files")
    ap.add_argument("-o", "--out", default="fim_dataset.jsonl")
    ap.add_argument("--negatives", action="store_true",
                    help="also emit rejected suggestions (marked)")
    ap.add_argument("--pairs", action="store_true",
                    help="emit DPO pairs {prompt, chosen, rejected} grouped "
                         "by editor ctx (requires logs with ctx field)")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    random.seed(args.seed)

    if args.pairs:
        groups = {}  # ctx -> {"acc": [], "rej": []}
        for rec in load_logs(args.logs):
            s = rec.get("suggestion", "").strip()
            if not s:
                continue
            g = groups.setdefault(rec.get("ctx", ""), {"acc": [], "rej": []})
            key = "acc" if rec.get("accepted") else "rej"
            if len(g[key]) < 10:            # cap pair explosion per ctx
                g[key].append(s)
        n = 0
        with open(args.out, "w") as out:
            for ctx, g in groups.items():
                for a in g["acc"]:
                    for r in g["rej"]:
                        if r == a:
                            continue
                        prompt = f"{FIM_PRE}{ctx}{FIM_SUF}{FIM_MID}"
                        out.write(json.dumps(
                            {"prompt": prompt, "chosen": a, "rejected": r},
                            ensure_ascii=False) + "\n")
                        n += 1
        print(f"wrote {n} DPO pairs -> {args.out}")
        return

    seen, out_n = set(), 0
    with open(args.out, "w") as out:
        for rec in load_logs(args.logs):
            if not rec.get("accepted") and not args.negatives:
                continue
            s = rec.get("suggestion", "").strip()
            if not s or s in seen:      # dedupe
                continue
            seen.add(s)
            ex = make_example(rec)
            if not rec.get("accepted"):
                ex["completion"] = ""   # negative: model must not emit it
                ex["rejected_suggestion"] = s
            out.write(json.dumps(ex, ensure_ascii=False) + "\n")
            out_n += 1
    print(f"wrote {out_n} examples -> {args.out}")

if __name__ == "__main__":
    main()
