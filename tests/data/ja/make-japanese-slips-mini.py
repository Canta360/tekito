#!/usr/bin/env python3
"""Writes japanese-slips-mini for the tests with scripts/ja/build-japanese-slips-pack.py
itself: n-grams counted from a few kana sentences, a tiny LSTM with random
weights, and expected.tsv with ln p of some kana by the n-grams (as the
pack stores them) and by the LSTM in float (the C++ side reads activations
as int8, so it agrees to a few hundredths).

Needs numpy and torch:  python tests/data/ja/make-japanese-slips-mini.py
"""

from __future__ import annotations

import argparse
import importlib.util
import shutil
import tempfile
from pathlib import Path

import numpy as np
import torch

HERE = Path(__file__).resolve().parent
OUT = HERE / "japanese-slips-mini"
SENTENCES = ["きょうはいいてんきですね。", "きのうはあめでした。", "あしたははれるでしょう。", "わたしはがくせいです。",
             "こんにちは。", "ありがとうございます。", "きょうはあめですね。", "てんきがいいです。"] * 3
TEXTS = ["きょうはいいてんきですね。", "きょうはいいてんきでうね。", "こんにちは", "ありがとう", "xyz", ""]


def main() -> int:
    spec = importlib.util.spec_from_file_location("b", HERE.parents[2] / "scripts" / "ja" / "build-japanese-slips-pack.py")
    b = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(b)
    b.EMBED, b.HIDDEN = 16, 16
    work = Path(tempfile.mkdtemp())
    chars = ["<pad>", "<s>", "</s>", "<unk>"] + sorted({c for t in SENTENCES for c in t})
    index = {c: i for i, c in enumerate(chars)}
    (work / "chars.tsv").write_text("\n".join(chars) + "\n", encoding="utf-8")
    tokens, starts = [], [0]
    for t in SENTENCES:
        tokens.extend([1] + [index[c] for c in t] + [2])
        starts.append(len(tokens))
    np.save(work / "tokens.npy", np.asarray(tokens, dtype=np.uint8))
    np.save(work / "starts.npy", np.asarray(starts, dtype=np.int64))
    b.ngram(argparse.Namespace(work=str(work)))
    torch.manual_seed(5)
    model = b.model_for(len(chars))
    torch.save(model.state_dict(), work / "kanalm.pt")
    if OUT.exists():
        shutil.rmtree(OUT)
    b.pack(argparse.Namespace(work=str(work), out=str(OUT), version="test"))

    d = np.load(work / "ngram.npz")
    keys = [d[f"k{n}"] for n in range(1, b.ORDER + 1)]
    logp = [d[f"p{n}"].astype(np.float16).astype(np.float64) for n in range(1, b.ORDER + 1)]
    logbo = [d[f"b{n}"].astype(np.float16).astype(np.float64) for n in range(1, b.ORDER + 1)]

    def find(n, code):
        at = np.searchsorted(keys[n - 1], code)
        return at if at < len(keys[n - 1]) and keys[n - 1][at] == code else None

    def ngram_lp(ids):
        total = 0.0
        for at in range(1, len(ids)):
            backoff = 0.0
            for n in range(min(b.ORDER, at + 1), 0, -1):
                code = 0
                for j in range(at - n + 1, at + 1):
                    code = code * 128 + ids[j]
                found = find(n, code)
                if found is not None:
                    total += backoff + logp[n - 1][found]
                    break
                if n == 1:
                    total += backoff - 20.0
                    break
                history = find(n - 1, code // 128)
                if history is not None:
                    backoff += logbo[n - 2][history]
        return total

    state = {k: v.float().numpy() for k, v in model.state_dict().items()}
    q = lambda m: b.quantize_rows(m)
    wi, si = q(state["lstm.weight_ih_l0"]); wh, sh = q(state["lstm.weight_hh_l0"]); wo, so = q(state["out.weight"])
    Wi, Wh, Wo = wi * si[:, None], wh * sh[:, None], wo * so[:, None]
    bias = state["lstm.bias_ih_l0"] + state["lstm.bias_hh_l0"]
    E, ob, H = state["embed.weight"], state["out.bias"], b.HIDDEN
    sig = lambda x: 1 / (1 + np.exp(-x))

    def neural_lp(ids):
        h, c = np.zeros(H), np.zeros(H)
        total = 0.0
        for at, i in enumerate(ids):
            g = Wi @ E[i] + Wh @ h + bias
            c = sig(g[H:2 * H]) * c + sig(g[:H]) * np.tanh(g[2 * H:3 * H])
            h = sig(g[3 * H:]) * np.tanh(c)
            if at + 1 < len(ids):
                logits = Wo @ h + ob
                total += logits[ids[at + 1]] - (logits.max() + np.log(np.exp(logits - logits.max()).sum()))
        return total

    with open(OUT / "expected.tsv", "w", encoding="utf-8", newline="\n") as f:
        for t in TEXTS:
            ids = [1] + [index.get(c, 3) for c in t] + [2]
            f.write(f"{t}\t{ngram_lp(ids):.4f}\t{neural_lp(ids):.4f}\n")
    del d
    shutil.rmtree(work, ignore_errors=True)
    print("written", OUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
