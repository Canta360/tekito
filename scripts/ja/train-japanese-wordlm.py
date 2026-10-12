#!/usr/bin/env python3
"""A small subword language model that chooses among whole-sentence
conversions, trained on a GPU (AMD ROCm PyTorch on Windows works).

Pieces (SentencePiece unigram) stand in for words: a character model knew
too little of words, and dictionary words cannot be found quickly enough
in text. The model: tied embeddings, one LSTM, a projection back to the
embedding size; a few MB once quantized.

Steps, each writing into --out:

  prepare   text (left context, sentence) from zenz lm.tsv, less what any
            --exclude-dir / --exclude-sha1 names; trains the pieces and
            writes the token ids.
  pretrain  next-piece prediction on the sentences, after their context.
  finetune  choosing the meant sentence from the lists tekito_ja_eval
            --write-sentences makes from typed keys (source, id, rank,
            meant, text), with the engine's rank as a prior; 2% of the
            lists are kept apart to check on.

  train-japanese-wordlm.py prepare  --out DIR --lm-tsv lm.tsv [--exclude-dir D] [--exclude-sha1 F]
  train-japanese-wordlm.py pretrain --out DIR [--epochs 1]
  train-japanese-wordlm.py finetune --out DIR --lists 'lists_*.tsv' --contexts contexts.tsv
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import math
import random
import time
from pathlib import Path

import numpy as np

SEP, EOS = 3, 2  # SentencePiece ids: 0 unk, 1 bos, 2 eos, 3 <sep> (between context and sentence)
CONTEXT_PIECES = 24
MAX_PIECES = 96


def load_keys_module():
    path = Path(__file__).resolve().parent / "make-reranker-keys.py"
    spec = importlib.util.spec_from_file_location("make_reranker_keys", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def excluded_test(args):
    import hashlib
    import re
    keys = load_keys_module()
    files = [str(p) for folder in args.exclude_dir for p in sorted(Path(folder).rglob("*"))
             if p.is_file() and p.suffix in (".tsv", ".txt", ".jsonl")]
    excluded = keys.read_excluded(files)
    hashes = set()
    for path in args.exclude_sha1:
        hashes.update(l.strip().lower() for l in open(path, encoding="utf-8") if l.strip())

    def test(text: str) -> bool:
        if text in excluded:
            return True
        if not hashes:
            return False
        parts = [text, *(p for p in re.split(r"(?<=[。！？.!?])", text) if p)]
        return any(hashlib.sha1(p.encode("utf-8")).hexdigest() in hashes for p in parts)
    return test


def prepare(args) -> None:
    import sentencepiece as spm
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    left_out = excluded_test(args)
    rows, dropped = [], 0
    for line in open(args.lm_tsv, encoding="utf-8"):
        context, _, text = line.rstrip("\n").partition("\t")
        text = text.strip()
        if not text:
            continue
        if left_out(text):
            dropped += 1
            continue
        rows.append((context.strip(), text))
    print(f"{len(rows)} sentences, {dropped} left out as measured")
    random.Random(0).shuffle(rows)
    sample = out / "pieces_sample.txt"
    with open(sample, "w", encoding="utf-8", newline="\n") as f:
        for _, text in rows[:args.piece_sample]:
            f.write(text + "\n")
    spm.SentencePieceTrainer.train(
        input=str(sample), model_prefix=str(out / "pieces"), vocab_size=args.vocab, model_type="unigram",
        character_coverage=0.9995, byte_fallback=True, user_defined_symbols=["<sep>"],
        unk_id=0, bos_id=1, eos_id=2, num_threads=16, input_sentence_size=args.piece_sample,
        shuffle_input_sentence=True, max_sentence_length=4096)
    sample.unlink()
    sp = spm.SentencePieceProcessor(model_file=str(out / "pieces.model"))
    assert sp.piece_to_id("<sep>") == SEP
    tokens, starts, targets = [], [0], []
    for begin in range(0, len(rows), 100000):
        chunk = rows[begin:begin + 100000]
        contexts = sp.encode([c for c, _ in chunk])
        texts = sp.encode([t for _, t in chunk])
        for c, t in zip(contexts, texts):
            t = t[:MAX_PIECES - 1] + [EOS]
            seq = c[-CONTEXT_PIECES:] + [SEP] + t
            targets.append(len(seq) - len(t))  # where the sentence starts
            tokens.extend(seq)
            starts.append(len(tokens))
    np.save(out / "tokens.npy", np.asarray(tokens, dtype=np.uint16))
    np.save(out / "starts.npy", np.asarray(starts, dtype=np.int64))
    np.save(out / "targets.npy", np.asarray(targets, dtype=np.int32))
    print(f"{len(tokens)} pieces written, vocab {sp.get_piece_size()}")


def make_model(vocab: int, embed: int, hidden: int):
    import torch
    import torch.nn as nn

    class PieceLM(nn.Module):
        def __init__(self):
            super().__init__()
            self.embed = nn.Embedding(vocab, embed)
            self.lstm = nn.LSTM(embed, hidden, batch_first=True)
            self.project = nn.Linear(hidden, embed)

        def sentence_logprob(self, x, start, lengths):
            """Sum of log p over each row's pieces from `start` (the
            sentence) to `lengths` (exclusive), given the pieces before.
            Only those positions are projected onto the vocabulary: all of
            them would not fit in the GPU's memory."""
            h, _ = self.lstm(self.embed(x[:, :-1]))
            positions = torch.arange(1, x.shape[1], device=x.device).unsqueeze(0)
            mask = (positions >= start.unsqueeze(1)) & (positions < lengths.unsqueeze(1))
            rows = torch.nonzero(mask, as_tuple=True)[0]
            logits = (self.project(h[mask]) @ self.embed.weight.T).float()
            logp = -torch.nn.functional.cross_entropy(logits, x[:, 1:][mask], reduction="none")
            return torch.zeros(x.shape[0], device=x.device).index_add_(0, rows, logp)

    return PieceLM()


def device():
    import torch
    torch.backends.cudnn.enabled = False  # MIOpen's RNN kernels fail to build on Windows
    if not torch.cuda.is_available():
        return torch.device("cpu")
    # Past the GPU's own memory, Windows lends it the PC's, and everything slows to a crawl.
    torch.cuda.set_per_process_memory_fraction(0.8)
    return torch.device("cuda")


def pad(seqs, dev):
    import torch
    width = max(len(s) for s in seqs)
    x = torch.zeros((len(seqs), width), dtype=torch.long)
    for i, s in enumerate(seqs):
        x[i, :len(s)] = torch.as_tensor(s, dtype=torch.long)
    return x.to(dev, non_blocking=True)


def pretrain(args) -> None:
    import torch
    out = Path(args.out)
    dev = device()
    tokens = np.load(out / "tokens.npy")
    starts = np.load(out / "starts.npy")
    targets = np.load(out / "targets.npy")
    n = len(targets)
    keep = n // 100  # 1% kept apart
    vocab = int(tokens.max()) + 1
    import sentencepiece as spm
    vocab = max(vocab, spm.SentencePieceProcessor(model_file=str(out / "pieces.model")).get_piece_size())
    model = make_model(vocab, args.embed, args.hidden).to(dev)
    print(f"parameters {sum(p.numel() for p in model.parameters()):,} on {dev}")
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01)
    steps_total = args.epochs * ((n - keep) // args.batch)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=max(1, steps_total), pct_start=0.02)

    def batch(ids):
        seqs = [tokens[starts[i]:starts[i + 1]] for i in ids]
        x = pad(seqs, dev)
        lengths = torch.as_tensor([len(s) for s in seqs], device=dev)
        start = torch.as_tensor(targets[ids], device=dev)
        return x, start, lengths

    def check():
        model.eval()
        total = pieces = 0.0
        with torch.no_grad():
            for b in range(0, keep, 512):
                ids = np.arange(b, min(keep, b + 512))
                x, start, lengths = batch(ids)
                with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev.type == "cuda"):
                    total -= model.sentence_logprob(x, start, lengths).sum().item()
                pieces += (lengths - start).sum().item()
        model.train()
        return math.exp(total / pieces)

    amp = {"fp16": torch.float16, "bf16": torch.bfloat16, "none": None}[args.amp] if dev.type == "cuda" else None
    order = np.arange(keep, n)
    began = time.time()
    step = 0
    for epoch in range(args.epochs):
        np.random.default_rng(epoch).shuffle(order)
        for b in range(0, len(order) - args.batch + 1, args.batch):
            ids = np.sort(order[b:b + args.batch])
            x, start, lengths = batch(ids)
            with torch.autocast("cuda", dtype=amp, enabled=amp is not None):
                logp = model.sentence_logprob(x, start, lengths)
            loss = -logp.sum() / (lengths - start).sum()
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            sched.step()
            step += 1
            if step < 2000 and step % 200 == 0:
                print(f"step {step} loss {loss.item():.3f}", flush=True)
            if args.max_steps and step >= args.max_steps:
                break
            if step == 100 and dev.type == "cuda":
                print(f"100 steps in {time.time() - began:.0f}s, GPU memory peak "
                      f"{torch.cuda.max_memory_allocated() / 2**30:.1f} GB", flush=True)
            if step % args.log_every == 0:
                print(f"epoch {epoch} step {step}/{steps_total} loss {loss.item():.3f} "
                      f"kept-apart perplexity {check():.1f} ({time.time() - began:.0f}s)", flush=True)
                torch.save({"model": model.state_dict(), "embed": args.embed, "hidden": args.hidden,
                            "vocab": vocab}, out / "pretrained.pt")
    print(f"done: kept-apart perplexity {check():.1f} ({time.time() - began:.0f}s)")
    torch.save({"model": model.state_dict(), "embed": args.embed, "hidden": args.hidden, "vocab": vocab},
               out / "pretrained.pt")


def read_lists(patterns: list[str], top: int):
    """id -> (texts by rank, index of the meant one); lists without the
    meant sentence in the first `top`, or with one sentence, are left out."""
    lists = {}
    for pattern in patterns:
        for path in sorted(glob.glob(pattern)):
            # A file still being written can end mid-character.
            for line in open(path, encoding="utf-8", errors="replace"):
                f = line.rstrip("\n").split("\t")
                if len(f) < 5:
                    continue
                rank = int(f[2])
                if rank >= top:
                    continue
                texts, meant = lists.setdefault(f[1], ([], [-1]))
                if f[3] == "1" and meant[0] < 0:
                    meant[0] = len(texts)
                texts.append(f[4])
    return {k: (t, m[0]) for k, (t, m) in lists.items() if m[0] >= 0 and len(t) > 1}


def finetune(args) -> None:
    import torch
    import sentencepiece as spm
    out = Path(args.out)
    dev = device()
    sp = spm.SentencePieceProcessor(model_file=str(out / "pieces.model"))
    saved = torch.load(out / "pretrained.pt", map_location="cpu")
    model = make_model(saved["vocab"], saved["embed"], saved["hidden"]).to(dev)
    model.load_state_dict(saved["model"])
    contexts = {}
    if args.contexts:
        for line in open(args.contexts, encoding="utf-8"):
            k, _, c = line.rstrip("\n").partition("\t")
            contexts[k] = c
    lists = read_lists(args.lists, args.top)
    left_out = excluded_test(args)
    lists = {k: v for k, v in lists.items() if not left_out(v[0][v[1]])}
    ids = sorted(lists)
    random.Random(0).shuffle(ids)
    apart = ids[:max(1, len(ids) * 2 // 100)]
    train_ids = ids[len(apart):]
    print(f"{len(lists)} lists ({len(train_ids)} to learn from, {len(apart)} kept apart)")
    # The engine's rank as a prior, and how much the model counts.
    prior = torch.nn.Parameter(torch.zeros(args.top, device=dev))
    scale = torch.nn.Parameter(torch.tensor(0.3, device=dev))
    opt = torch.optim.AdamW([{"params": model.parameters(), "lr": args.lr},
                             {"params": [prior, scale], "lr": 1e-2}], weight_decay=0.0)

    def scores(batch_ids):
        seqs, starts, owners, ranks = [], [], [], []
        for n, k in enumerate(batch_ids):
            texts, _ = lists[k]
            c = sp.encode(contexts.get(k, ""))[-CONTEXT_PIECES:] + [SEP]
            for r, t in enumerate(sp.encode(texts)):
                seqs.append(c + t[:MAX_PIECES - 1] + [EOS])
                starts.append(len(c))
                owners.append(n)
                ranks.append(r)
        x = pad(seqs, dev)
        lengths = torch.as_tensor([len(s) for s in seqs], device=dev)
        with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev.type == "cuda"):
            lp = model.sentence_logprob(x, torch.as_tensor(starts, device=dev), lengths).float()
        return scale * lp + prior[torch.as_tensor(ranks, device=dev)], owners

    def loss_and_right(batch_ids):
        s, owners = scores(batch_ids)
        loss, right, at = 0.0, 0, 0
        for n, k in enumerate(batch_ids):
            count = len(lists[k][0])
            row = s[at:at + count]
            at += count
            meant = lists[k][1]
            loss = loss + torch.nn.functional.cross_entropy(row.unsqueeze(0), torch.tensor([meant], device=dev))
            right += int(row.argmax().item() == meant)
        return loss / len(batch_ids), right

    def check():
        model.eval()
        right = first = 0
        with torch.no_grad():
            for b in range(0, len(apart), 64):
                chunk = apart[b:b + 64]
                _, r = loss_and_right(chunk)
                right += r
                first += sum(lists[k][1] == 0 for k in chunk)
        model.train()
        return 100 * first / len(apart), 100 * right / len(apart)

    before, after = check()
    print(f"kept apart: engine first {before:.1f}%, with the model {after:.1f}% (before finetuning)", flush=True)
    began = time.time()
    step = 0
    for epoch in range(args.epochs):
        random.Random(epoch).shuffle(train_ids)
        for b in range(0, len(train_ids), args.batch):
            loss, _ = loss_and_right(train_ids[b:b + args.batch])
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            step += 1
            if step % 500 == 0:
                before, after = check()
                print(f"epoch {epoch} step {step}: kept apart engine first {before:.1f}%, with the model "
                      f"{after:.1f}%, scale {scale.item():.3f} ({time.time() - began:.0f}s)", flush=True)
        torch.save({"model": model.state_dict(), "embed": saved["embed"], "hidden": saved["hidden"],
                    "vocab": saved["vocab"], "prior": prior.detach().cpu(), "scale": scale.item()},
                   out / "finetuned.pt")
    before, after = check()
    print(f"done: kept apart engine first {before:.1f}%, with the model {after:.1f}%")


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="step", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--out", required=True)
    p.add_argument("--lm-tsv", required=True)
    p.add_argument("--exclude-dir", action="append", default=[])
    p.add_argument("--exclude-sha1", action="append", default=[])
    p.add_argument("--vocab", type=int, default=16000)
    p.add_argument("--piece-sample", type=int, default=2000000)
    t = sub.add_parser("pretrain")
    t.add_argument("--out", required=True)
    t.add_argument("--epochs", type=int, default=1)
    t.add_argument("--batch", type=int, default=256)
    t.add_argument("--embed", type=int, default=128)
    t.add_argument("--hidden", type=int, default=384)
    t.add_argument("--lr", type=float, default=1e-3)
    t.add_argument("--log-every", type=int, default=2000)
    t.add_argument("--max-steps", type=int, default=0)
    t.add_argument("--amp", choices=["fp16", "bf16", "none"], default="bf16")
    f = sub.add_parser("finetune")
    f.add_argument("--out", required=True)
    f.add_argument("--lists", action="append", required=True)
    f.add_argument("--contexts")
    f.add_argument("--exclude-dir", action="append", default=[])
    f.add_argument("--exclude-sha1", action="append", default=[])
    f.add_argument("--top", type=int, default=10)
    f.add_argument("--epochs", type=int, default=1)
    f.add_argument("--batch", type=int, default=32)
    f.add_argument("--lr", type=float, default=3e-4)
    args = parser.parse_args()
    {"prepare": prepare, "pretrain": pretrain, "finetune": finetune}[args.step](args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
