"""Batched forward-loss scan over the dataset; flags rows whose CE loss is
nan/inf or abnormally high (poison rows NaNing the training run)."""
import json, math, time
import numpy as np
import mlx.core as mx
from mlx_lm.utils import load

model, tok = load("mlx-community/Qwen2.5-Coder-1.5B-4bit")

rows = [json.loads(l) for l in open("finetune/data_clean/train.jsonl")]
enc = []
for r in rows:
    p = tok.encode(r["prompt"], add_special_tokens=False)
    ids = tok.encode(r["prompt"] + r["completion"], add_special_tokens=False)[:1024]
    enc.append((ids, min(len(p), len(ids))))
print(f"tokenized {len(enc)} rows", flush=True)

order = sorted(range(len(enc)), key=lambda i: len(enc[i][0]))
bad = []
t0 = time.time()
B = 2
for bi in range(0, len(order), B):
    bidx = order[bi:bi + B]
    batch = [enc[i] for i in bidx]
    L = min(1024, 32 * ((max(len(ids) for ids, _ in batch) + 31) // 32))
    x = np.zeros((len(batch), L), dtype=np.int32)
    mask = np.zeros((len(batch), L), dtype=bool)
    for k, (ids, plen) in enumerate(batch):
        x[k, :len(ids)] = ids
        mask[k, plen:L] = True
    logits = model(mx.array(x)).astype(mx.float32)
    ov = mx.abs(logits).max()
    tgt = mx.array(x[:, 1:])
    idx = tgt[..., None]
    lp = mx.log_softmax(logits[:, :-1], axis=-1)
    logp = mx.take_along_axis(lp, idx, axis=-1)[..., 0]
    tm = mx.array(mask[:, 1:])
    cnt = tm.sum(axis=1)
    tot = mx.where(tm, logp, mx.array(0.0)).sum(axis=1)
    loss = -tot / mx.maximum(cnt, 1)
    mx.eval(loss, cnt, ov)
    ovv = float(ov)
    lv = loss.tolist()
    cv = cnt.tolist()
    for k, i in enumerate(bidx):
        if int(cv[k]) == 0:
            bad.append((i, float("nan"), "all-masked"))
        elif ovv > 5e4:
            bad.append((i, float("inf"), "logit-overflow"))
        elif not math.isfinite(lv[k]) or lv[k] > 12.0:
            bad.append((i, float(lv[k]), "high-loss"))
    mx.clear_cache()
    if (bi // B) % 100 == 0:
        print(f"{bi}/{len(order)} {time.time()-t0:.0f}s bad={len(bad)}", flush=True)

print(f"DONE train: {len(rows)} rows, {len(bad)} flagged, {time.time()-t0:.0f}s", flush=True)
with open("finetune/scan_train.jsonl", "w") as f:
    for i, v, kind in bad:
        f.write(json.dumps({"row": i, "loss": v, "kind": kind,
                            "prompt_tail": rows[i]["prompt"][-100:],
                            "completion_head": rows[i]["completion"][:100]}) + "\n")
