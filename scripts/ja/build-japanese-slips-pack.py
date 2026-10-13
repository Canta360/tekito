#!/usr/bin/env python3
"""Builds the japanese-slips Data Pack: what reads as Japanese kana, for
finding slips before conversion.

  slips.bin  a kana n-gram model (orders 1..4, Witten-Bell, as log
             probabilities and back-off weights) to rank many corrected
             readings quickly, and a small kana LSTM to score the best few
  chars.tsv  the characters the models know, one per line (id = line)

Steps (each writes into --work):

  prepare  readings from zenz readings_*.tsv (hiragana sentences), less what
           any --exclude-dir / --exclude-sha1 names, NFKC, as token ids
  ngram    counts orders 1..4 and their probabilities
  train    the kana LSTM on the GPU (AMD ROCm PyTorch on Windows works)
  pack     writes the pack into --out

slips.bin, little-endian:
  "TKKS", u32 version (1)
  u32 chars, order, embed, hidden
  per order n = 1..order: u32 count, then (each from a multiple of 64)
      u32 code[count] (n ids in base 128, oldest first, sorted)
      f16 log_prob[count]   ln P(last | the rest), interpolated
      f16 log_backoff[count]  ln of the weight when this n-gram is the
                              history of an unseen (n+1)-gram
  then the LSTM: f32 embedding[chars][embed]
      i8 input_weights[4*hidden][embed]  f32 input_scale[4*hidden]
      i8 hidden_weights[4*hidden][hidden] f32 hidden_scale[4*hidden]
      f32 bias[4*hidden] (gates i, f, g, o)
      i8 output[chars][hidden]  f32 output_scale[chars]  f32 output_bias[chars]
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import importlib.util
import json
import os
import re
import struct
import sys
import time
import unicodedata
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
ORDER = 4
EMBED, HIDDEN = 128, 768

NOTICE = """What reads as Japanese kana, for finding slips before conversion: kana
n-gram probabilities and a small character model, both computed by TEKITO
(scripts/ja/build-japanese-slips-pack.py) from the readings in the
zenz-v2.5-dataset (https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset),
its llm-jp-corpus-v3 part, made available under the Open Data Commons
Attribution License (ODC-BY 1.0): https://opendatacommons.org/licenses/by/1-0/
The pack holds statistics and weights only; it contains no sentences.
"""


def norm(text: str) -> str:
    return unicodedata.normalize("NFKC", text)


def excluded_test(args):
    spec = importlib.util.spec_from_file_location("w", Path(__file__).parent / "train-japanese-wordlm.py")
    w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    return w.excluded_test(args)


def prepare(args) -> None:
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    left_out = excluded_test(args)
    texts, dropped = [], 0
    for path in sorted(glob.glob(str(Path(args.readings_dir) / "readings_*.tsv"))):
        for line in open(path, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) < 4 or not f[2]:
                continue
            if left_out(f[3]):
                dropped += 1
                continue
            texts.append(norm(f[2]))
    counts: dict[str, int] = {}
    for t in texts:
        for c in t:
            counts[c] = counts.get(c, 0) + 1
    chars = ["<pad>", "<s>", "</s>", "<unk>"] + sorted(c for c, n in counts.items() if n >= 50 and c not in "\t\n")
    assert len(chars) < 128
    index = {c: i for i, c in enumerate(chars)}
    (work / "chars.tsv").write_text("\n".join(chars) + "\n", encoding="utf-8")
    tokens, starts = [], [0]
    for t in texts:
        tokens.extend([1] + [index.get(c, 3) for c in t[:200]] + [2])
        starts.append(len(tokens))
    np.save(work / "tokens.npy", np.asarray(tokens, dtype=np.uint8))
    np.save(work / "starts.npy", np.asarray(starts, dtype=np.int64))
    print(f"{len(texts)} readings, {dropped} left out, {len(chars)} characters, {len(tokens)} tokens")


def ngram(args) -> None:
    """Witten-Bell interpolation, kept as ln P per seen n-gram and the
    back-off weight of each n-gram as a history."""
    work = Path(args.work)
    tokens = np.load(work / "tokens.npy").astype(np.int64)
    starts = np.load(work / "starts.npy")
    chars = (work / "chars.tsv").read_text(encoding="utf-8").rstrip("\n").split("\n")
    ends = np.repeat(starts[1:], np.diff(starts))
    keys, counts = [], []
    for n in range(1, ORDER + 1):
        idx = np.nonzero(np.arange(len(tokens)) + n <= ends)[0]
        code = np.zeros(len(idx), dtype=np.int64)
        for j in range(n):
            code = code * 128 + tokens[idx + j]
        k, c = np.unique(code, return_counts=True)
        keys.append(k)
        counts.append(c.astype(np.float64))
        print(f"order {n}: {len(k):,}", flush=True)
    logp, logbo = [], []
    # Unigrams: add one.
    total = counts[0].sum()
    p = (counts[0] + 1) / (total + len(chars))
    logp.append(np.log(p))
    for n in range(2, ORDER + 1):
        k, c = keys[n - 1], counts[n - 1]
        hist = k // 128
        lower = k % (128 ** (n - 1))
        # The lower-order probability of the same last n-1 ids (always seen).
        at = np.searchsorted(keys[n - 2], lower)
        assert np.all(keys[n - 2][at] == lower)
        plower = np.exp(logp[n - 2][at])
        hkeys, first = np.unique(hist, return_index=True)
        htotal = np.add.reduceat(c, first)
        htypes = np.diff(np.append(first, len(hist))).astype(np.float64)
        hi = np.searchsorted(hkeys, hist)
        p = (c + htypes[hi] * plower) / (htotal[hi] + htypes[hi])
        logp.append(np.log(p))
        # Back-off weight of each (n-1)-gram as a history.
        bo = np.zeros(len(keys[n - 2]))
        pos = np.searchsorted(keys[n - 2], hkeys)
        assert np.all(keys[n - 2][pos] == hkeys)
        bo[pos] = np.log(htypes / (htotal + htypes))
        logbo.append(bo)
    logbo.append(np.zeros(len(keys[ORDER - 1])))
    np.savez(work / "ngram.npz", **{f"k{n}": keys[n - 1] for n in range(1, ORDER + 1)},
             **{f"p{n}": logp[n - 1] for n in range(1, ORDER + 1)},
             **{f"b{n}": logbo[n - 1] for n in range(1, ORDER + 1)})
    print("probabilities written")


def model_for(chars: int):
    import torch.nn as nn

    class KanaLM(nn.Module):
        def __init__(self):
            super().__init__()
            self.embed = nn.Embedding(chars, EMBED)
            self.lstm = nn.LSTM(EMBED, HIDDEN, batch_first=True)
            self.out = nn.Linear(HIDDEN, chars)

        def forward(self, x):
            h, _ = self.lstm(self.embed(x))
            return self.out(h)
    return KanaLM()


def train(args) -> None:
    import torch
    torch.backends.cudnn.enabled = False  # MIOpen's RNN kernels fail to build on Windows
    dev = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    if dev.type == "cuda":
        torch.cuda.set_per_process_memory_fraction(0.8)
    work = Path(args.work)
    tokens = np.load(work / "tokens.npy")
    starts = np.load(work / "starts.npy")
    chars = len((work / "chars.tsv").read_text(encoding="utf-8").rstrip("\n").split("\n"))
    n = len(starts) - 1
    keep = 5000
    model = model_for(chars).to(dev)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr)
    total = args.epochs * (n - keep) // args.batch
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=max(1, total), pct_start=0.02)

    def batch(ids):
        seqs = [tokens[starts[i]:starts[i + 1]] for i in ids]
        x = np.zeros((len(seqs), max(len(s) for s in seqs)), dtype=np.int64)
        for i, s in enumerate(seqs):
            x[i, :len(s)] = s
        return torch.from_numpy(x).to(dev)

    def loss_of(x):
        with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev.type == "cuda"):
            logits = model(x[:, :-1])
        return torch.nn.functional.cross_entropy(logits.float().reshape(-1, chars), x[:, 1:].reshape(-1), ignore_index=0)

    began, step = time.time(), 0
    order = np.arange(keep, n)
    for epoch in range(args.epochs):
        np.random.default_rng(epoch).shuffle(order)
        for b in range(0, len(order) - args.batch + 1, args.batch):
            loss = loss_of(batch(np.sort(order[b:b + args.batch])))
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            sched.step()
            step += 1
            if step % 2000 == 0:
                print(f"step {step}/{total} loss {loss.item():.3f} ({time.time() - began:.0f}s)", flush=True)
    model.eval()
    with torch.no_grad():
        kept = np.mean([loss_of(batch(np.arange(b, min(keep, b + 500)))).item() for b in range(0, keep, 500)])
    print(f"kept apart {kept:.3f} nats/char ({time.time() - began:.0f}s)", flush=True)
    torch.save(model.state_dict(), work / "kanalm.pt")
    sys.stdout.flush()
    os._exit(0)  # ROCm can hang on exit


def quantize_rows(m: np.ndarray):
    scale = np.abs(m).max(axis=1) / 127.0
    scale[scale == 0] = 1.0
    return np.clip(np.rint(m / scale[:, None]), -127, 127).astype(np.int8), scale.astype(np.float32)


def pack(args) -> None:
    import torch
    work, out = Path(args.work), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    chars = (work / "chars.tsv").read_text(encoding="utf-8").rstrip("\n").split("\n")
    d = np.load(work / "ngram.npz")
    state = {k: v.float().numpy() for k, v in torch.load(work / "kanalm.pt", map_location="cpu").items()}
    blob = bytearray(b"TKKS") + struct.pack("<5I", 1, len(chars), ORDER, EMBED, HIDDEN)

    def section(array: np.ndarray):
        blob.extend(b"\0" * (-len(blob) % 64))
        blob.extend(np.ascontiguousarray(array).tobytes())

    for n in range(1, ORDER + 1):
        k = d[f"k{n}"]
        assert k.max() < 2 ** 32
        blob.extend(struct.pack("<I", len(k)))
        section(k.astype(np.uint32))
        section(d[f"p{n}"].astype(np.float16))
        section(d[f"b{n}"].astype(np.float16))
    section(state["embed.weight"].astype(np.float32))
    wi, si = quantize_rows(state["lstm.weight_ih_l0"])
    wh, sh = quantize_rows(state["lstm.weight_hh_l0"])
    wo, so = quantize_rows(state["out.weight"])
    for array in (wi, si, wh, sh, (state["lstm.bias_ih_l0"] + state["lstm.bias_hh_l0"]).astype(np.float32),
                  wo, so, state["out.bias"].astype(np.float32)):
        section(array)
    (out / "slips.bin").write_bytes(bytes(blob))
    (out / "chars.tsv").write_text("\n".join(chars) + "\n", encoding="utf-8", newline="\n")
    (out / "NOTICE").write_text(NOTICE, encoding="utf-8", newline="\n")

    def sha(name: str) -> str:
        return hashlib.sha256((out / name).read_bytes()).hexdigest().upper()

    manifest = {
        "pack_id": "japanese-slips", "display_name": "Japanese Slip Finder", "schema_version": 1,
        "version": args.version, "language": "ja-JP", "type": "japanese-slips", "format": "ja-slips-v1",
        "file": "slips.bin", "chars_file": "chars.tsv", "entry_count": int(sum(len(d[f"k{n}"]) for n in range(1, ORDER + 1))),
        "sha256": {"file": sha("slips.bin"), "chars": sha("chars.tsv")},
        "license": "TEKITO-OWNED", "source": "scripts/ja/build-japanese-slips-pack.py on zenz-v2.5-dataset (ODC-BY 1.0)",
        "notice_file": "NOTICE",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"slips.bin {len(blob):,} bytes -> {out}")


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="step", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--work", required=True)
    p.add_argument("--readings-dir", required=True)
    p.add_argument("--exclude-dir", action="append", default=[])
    p.add_argument("--exclude-sha1", action="append", default=[])
    g = sub.add_parser("ngram")
    g.add_argument("--work", required=True)
    t = sub.add_parser("train")
    t.add_argument("--work", required=True)
    t.add_argument("--epochs", type=int, default=3)
    t.add_argument("--batch", type=int, default=512)
    t.add_argument("--lr", type=float, default=1.5e-3)
    k = sub.add_parser("pack")
    k.add_argument("--work", required=True)
    k.add_argument("--out", required=True)
    k.add_argument("--version", default="2026.10")
    args = parser.parse_args()
    {"prepare": prepare, "ngram": ngram, "train": train, "pack": pack}[args.step](args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
