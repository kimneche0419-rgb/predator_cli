"""Reproduce the deterministic (seed=42) batch order and print the examples
trained in the iters window where NaN appeared. Usage:
    python3 finetune/reproduce_perm.py 228 232
"""
import json, sys
import numpy as np
np.random.seed(42)  # match the seeded wrapper BEFORE any loads
from mlx_lm.utils import load
from mlx_lm.tokenizer_utils import TokenizerWrapper

model, tok = load("mlx-community/Qwen2.5-Coder-1.5B-4bit")

rows = [json.loads(l) for l in open("finetune/data_clean/train.jsonl")]

# replicate mlx-lm training dataset: chat-template tokenization + length sort
def tokenize(r):
    msgs = [{"role": "user", "content": r["prompt"]},
            {"role": "assistant", "content": r["completion"]}]
    tokens = tok.apply_chat_template(msgs, return_dict=False)
    off = len(tok.apply_chat_template(msgs[:-1], add_generation_prompt=True,
                                      return_dict=False))
    return (tokens, off)

lens = []
for i, r in enumerate(rows):
    t, o = tokenize(r)
    lens.append(len(t[:1024]))
order = sorted(range(len(rows)), key=lambda i: lens[i])

B = 1  # batch_size 1 in config
batch_idx = [order[i:i + B] for i in range(0, len(order) - B + 1, B)]
print(f"{len(rows)} rows, {len(batch_idx)} single batches")

np.random.seed(42)
GA = 4
lo, hi = int(sys.argv[1]), int(sys.argv[2])  # iter window (1-based iters)
# iter N consumed batches [(N-1)*GA, N*GA) of the first permutation
perm = np.random.permutation(len(batch_idx))
for it in range(lo, hi + 1):
    for b in range((it - 1) * GA, it * GA):
        rows_i = batch_idx[int(perm[b])]
        print(f"iter {it} batch {b}: rows {rows_i}")
        for ri in rows_i:
            r = rows[ri]
            print(f"  row {ri}: prompt_tail={r['prompt'][-70:]!r}")
            print(f"        completion={r['completion'][:100]!r}")
