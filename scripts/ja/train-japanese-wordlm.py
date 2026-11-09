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
  distill   the finetuned model taught on more lists to choose as a large
            conversion model does (its ln p of each sentence, as ftscore
            writes them), and the meant sentence where the list has it;
            lists without it count too. Written to --write.
  mwer      a model taught on the sentences the composer itself handed the
            check (TEKITO_EVAL_CHECKED: as read, with the whole texts),
            scored as the check scores them (scale, ramp, prior by rank,
            the last prior for whole texts), to lower the expected
            character errors against the meant text; --keep-apart lists
            only check. Written to --write.

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
    for lm_tsv in args.lm_tsv:
        for line in open(lm_tsv, encoding="utf-8"):
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
    if args.pieces:
        # Pieces another model has, so what was built for it still fits.
        import shutil
        for suffix in (".model", ".vocab"):
            shutil.copyfile(Path(args.pieces).with_suffix(suffix), out / f"pieces{suffix}")
    else:
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
    if args.apart_max:
        apart = apart[:args.apart_max]
    if args.max_lists:
        train_ids = train_ids[:args.max_lists]
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
        scores.lp, scores.pieces = lp, lengths - torch.as_tensor(starts, device=dev)
        return scale * lp + prior[torch.as_tensor(ranks, device=dev)], owners

    def loss_and_right(batch_ids):
        s, owners = scores(batch_ids)
        loss, right, at = 0.0, 0, 0
        for n, k in enumerate(batch_ids):
            count = len(lists[k][0])
            row = s[at:at + count]
            meant = lists[k][1]
            loss = loss + torch.nn.functional.cross_entropy(row.unsqueeze(0), torch.tensor([meant], device=dev))
            # The meant sentence still read as likely text (--lm-weight), so
            # the model stays a language model and does not learn the
            # lists' quirks alone.
            if args.lm_weight:
                loss = loss - args.lm_weight * scores.lp[at + meant] / scores.pieces[at + meant]
            at += count
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
            if step % args.check_every == 0:
                before, after = check()
                print(f"epoch {epoch} step {step}: kept apart engine first {before:.1f}%, with the model "
                      f"{after:.1f}%, scale {scale.item():.3f} ({time.time() - began:.0f}s)", flush=True)
        torch.save({"model": model.state_dict(), "embed": saved["embed"], "hidden": saved["hidden"],
                    "vocab": saved["vocab"], "prior": prior.detach().cpu(), "scale": scale.item()},
                   out / "finetuned.pt")
    before, after = check()
    print(f"done: kept apart engine first {before:.1f}%, with the model {after:.1f}%")


def refit_prior(args) -> None:
    """The rank prior and scale fitted again, the model kept as it is, on
    lists in the order the engine now puts sentences in (tekito_ja_eval
    --ai probe with TEKITO_EVAL_ASKED): finetuned.pt with them replaced is
    written to --write."""
    import torch
    import sentencepiece as spm
    out = Path(args.out)
    dev = device()
    sp = spm.SentencePieceProcessor(model_file=str(out / "pieces.model"))
    saved = torch.load(out / "finetuned.pt", map_location="cpu")
    model = make_model(saved["vocab"], saved["embed"], saved["hidden"]).to(dev)
    model.load_state_dict(saved["model"])
    model.eval()
    top = len(saved["prior"])
    contexts = {}
    if args.contexts:
        for line in open(args.contexts, encoding="utf-8"):
            k, _, c = line.rstrip("\n").partition("\t")
            contexts[k] = c
    lists = read_lists(args.lists, top)
    ids = sorted(lists)
    random.Random(0).shuffle(ids)
    apart = set(ids[:max(1, len(ids) // 10)])
    # Each sentence's ln p once, as the engine reads it (after the context).
    lps, ranks, owners, meant = [], [], [], []
    began = time.time()
    with torch.no_grad():
        for b in range(0, len(ids), 64):
            seqs, starts = [], []
            for k in ids[b:b + 64]:
                texts, m = lists[k]
                c = sp.encode(contexts.get(k, ""))[-CONTEXT_PIECES:] + [SEP]
                meant.append(m)
                for r, t in enumerate(sp.encode(texts)):
                    seqs.append(c + t[:MAX_PIECES - 1] + [EOS])
                    starts.append(len(c))
                    ranks.append(r)
                    owners.append(len(meant) - 1)
            x = pad(seqs, dev)
            lengths = torch.as_tensor([len(s) for s in seqs], device=dev)
            with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev.type == "cuda"):
                lps.append(model.sentence_logprob(x, torch.as_tensor(starts, device=dev), lengths).float().cpu())
    lp = torch.cat(lps).double()
    ranks, owners = torch.as_tensor(ranks), torch.as_tensor(owners)
    meant = torch.as_tensor(meant)
    held = torch.as_tensor([k in apart for k in ids])
    print(f"{len(ids)} lists ({int(held.sum())} kept apart), {len(lp)} sentences read ({time.time() - began:.0f}s)")

    def scores(prior, scale):
        return scale * lp + prior[ranks]

    def loss_and_right(prior, scale, which):
        s = scores(prior, scale)
        # Each list's log-softmax: its scores less their log-sum-exp.
        largest = torch.full((len(meant),), -1e30, dtype=s.dtype).scatter_reduce(0, owners, s, "amax")
        sums = torch.zeros(len(meant), dtype=s.dtype).index_add_(0, owners, (s - largest[owners]).exp())
        logsum = largest + sums.log()
        firsts = torch.zeros(len(meant), dtype=torch.long).scatter_reduce(0, owners, torch.arange(len(s)), "amin",
                                                                          include_self=False)
        picked = s[firsts + meant] - logsum
        best = torch.full((len(meant),), -1e30, dtype=s.dtype).scatter_reduce(0, owners, s, "amax")
        right = (s[firsts + meant] >= best)
        return -picked[which].mean(), 100.0 * right[which].double().mean().item()

    old_prior = saved["prior"].double()
    old_scale = torch.tensor(float(saved["scale"]), dtype=torch.double)
    prior = old_prior.clone().requires_grad_(True)
    scale = old_scale.clone().requires_grad_(not args.keep_scale)
    opt = torch.optim.LBFGS([prior] if args.keep_scale else [prior, scale], max_iter=200,
                            line_search_fn="strong_wolfe")

    def closure():
        opt.zero_grad()
        loss, _ = loss_and_right(prior, scale, ~held)
        loss.backward()
        return loss
    opt.step(closure)
    with torch.no_grad():
        # Only differences count: the first's prior stays where it was.
        new_prior = prior - prior[0] + old_prior[0]
        for name, p, c in (("before", old_prior, old_scale), ("after", new_prior, scale)):
            loss, right = loss_and_right(p, c, held)
            print(f"{name}: kept apart loss {loss.item():.4f}, meant chosen {right:.1f}%, scale {c.item():.4f}, "
                  f"prior {' '.join(f'{v:.3f}' for v in p.tolist())}")
        print(f"engine first meant (kept apart): {100.0 * (meant[held] == 0).double().mean().item():.1f}%")
    saved["prior"] = new_prior.float()
    saved["scale"] = float(scale.item())
    Path(args.write).parent.mkdir(parents=True, exist_ok=True)
    torch.save(saved, args.write)
    print(f"-> {args.write}")


def distill(args) -> None:
    import shutil
    import torch
    import sentencepiece as spm
    out = Path(args.out)
    dev = device()
    sp = spm.SentencePieceProcessor(model_file=str(out / "pieces.model"))
    saved = torch.load(out / "finetuned.pt", map_location="cpu")
    model = make_model(saved["vocab"], saved["embed"], saved["hidden"]).to(dev)
    model.load_state_dict(saved["model"])
    top = len(saved["prior"])
    contexts = {}
    if args.contexts:
        for line in open(args.contexts, encoding="utf-8"):
            k, _, c = line.rstrip("\n").partition("\t")
            contexts[k] = c
    # id -> (texts by rank, meant or -1); the teacher's ln p by id and rank.
    lists = {}
    for pattern in args.lists:
        for path in sorted(glob.glob(pattern)):
            for line in open(path, encoding="utf-8", errors="replace"):
                f = line.rstrip("\n").split("\t")
                if len(f) < 5 or int(f[2]) >= top:
                    continue
                texts, meant = lists.setdefault(f[1], ([], [-1]))
                if f[3] == "1" and meant[0] < 0:
                    meant[0] = len(texts)
                texts.append(f[4])
    taught = {}
    for pattern in args.teacher:
        for path in sorted(glob.glob(pattern)):
            for line in open(path, encoding="utf-8", errors="replace"):
                f = line.rstrip("\n").split("\t")
                if len(f) >= 5 and int(f[2]) < top:
                    taught.setdefault(f[1], {})[int(f[2])] = float(f[4])
    data = {}
    for k, (texts, meant) in lists.items():
        lp = taught.get(k, {})
        if len(texts) > 1 and all(r in lp for r in range(len(texts))):
            data[k] = (texts, meant[0], [lp[r] for r in range(len(texts))])
    ids = sorted(data)
    random.Random(0).shuffle(ids)
    apart = ids[:max(1, len(ids) * 5 // 100)]
    train_ids = ids[len(apart):]
    with_meant = sum(data[k][1] >= 0 for k in ids)
    print(f"{len(ids)} lists with the teacher's scores ({with_meant} with the meant sentence; "
          f"{len(train_ids)} to learn from, {len(apart)} kept apart)", flush=True)
    prior = torch.nn.Parameter(saved["prior"].float().to(dev))
    scale = torch.nn.Parameter(torch.tensor(float(saved["scale"]), device=dev))
    opt = torch.optim.AdamW([{"params": model.parameters(), "lr": args.lr},
                             {"params": [prior, scale], "lr": 1e-3}], weight_decay=0.0)

    def scores(batch_ids):
        seqs, starts, ranks = [], [], []
        for k in batch_ids:
            c = sp.encode(contexts.get(k, ""))[-CONTEXT_PIECES:] + [SEP]
            for r, t in enumerate(sp.encode(data[k][0])):
                seqs.append(c + t[:MAX_PIECES - 1] + [EOS])
                starts.append(len(c))
                ranks.append(r)
        x = pad(seqs, dev)
        lengths = torch.as_tensor([len(s) for s in seqs], device=dev)
        with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev.type == "cuda"):
            lp = model.sentence_logprob(x, torch.as_tensor(starts, device=dev), lengths).float()
        return scale * lp + prior[torch.as_tensor(ranks, device=dev)]

    def loss_and_counts(batch_ids):
        s = scores(batch_ids)
        loss, at = 0.0, 0
        # With the meant sentence, engine first right, teacher right, model
        # right; the model choosing as the teacher does.
        counts = [0, 0, 0, 0, 0]
        for k in batch_ids:
            texts, meant, teacher = data[k]
            row = s[at:at + len(texts)]
            at += len(texts)
            t = torch.tensor(teacher, device=dev)
            target = torch.softmax(t / args.temperature, 0)
            logp = torch.log_softmax(row, 0)
            loss = loss + (1 - args.alpha) * -(target * logp).sum()
            if meant >= 0:
                loss = loss + args.alpha * -logp[meant]
                counts[0] += 1
                counts[1] += int(meant == 0)
                counts[2] += int(int(t.argmax()) == meant)
                counts[3] += int(int(row.argmax()) == meant)
            counts[4] += int(int(row.argmax()) == int(t.argmax()))
        return loss / len(batch_ids), counts

    def check():
        model.eval()
        total = [0, 0, 0, 0, 0]
        with torch.no_grad():
            for b in range(0, len(apart), 64):
                _, c = loss_and_counts(apart[b:b + 64])
                total = [x + y for x, y in zip(total, c)]
        model.train()
        n = max(1, total[0])
        return (f"kept apart (meant in {total[0]}): engine first {100 * total[1] / n:.1f}%, teacher "
                f"{100 * total[2] / n:.1f}%, model {100 * total[3] / n:.1f}%; model as teacher "
                f"{100 * total[4] / len(apart):.1f}%")

    print(f"before: {check()}", flush=True)
    began = time.time()
    step = 0
    for epoch in range(args.epochs):
        random.Random(epoch).shuffle(train_ids)
        for b in range(0, len(train_ids), args.batch):
            loss, _ = loss_and_counts(train_ids[b:b + args.batch])
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            step += 1
            if step % 500 == 0:
                print(f"epoch {epoch} step {step}: {check()}, scale {scale.item():.3f} ({time.time() - began:.0f}s)",
                      flush=True)
    print(f"done: {check()}")
    write = Path(args.write)
    write.mkdir(parents=True, exist_ok=True)
    shutil.copy(out / "pieces.model", write / "pieces.model")
    torch.save({"model": model.state_dict(), "embed": saved["embed"], "hidden": saved["hidden"],
                "vocab": saved["vocab"], "prior": prior.detach().cpu(), "scale": scale.item()},
               write / "finetuned.pt")
    print(f"-> {write}")


def char_errors(a: str, b: str) -> int:
    row = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        prev, row[0] = row[0], i
        for j, cb in enumerate(b, 1):
            prev, row[j] = row[j], min(row[j] + 1, row[j - 1] + 1, prev + (ca != cb))
    return row[-1]


def mwer(args) -> None:
    import shutil
    import unicodedata
    import torch
    import sentencepiece as spm
    out = Path(args.out)
    dev = device()
    sp = spm.SentencePieceProcessor(model_file=str(out / "pieces.model"))
    saved = torch.load(out / "finetuned.pt", map_location="cpu")
    model = make_model(saved["vocab"], saved["embed"], saved["hidden"]).to(dev)
    model.load_state_dict(saved["model"])
    top = len(saved["prior"])
    meant_texts = {}
    for line in open(args.meant, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 3:
            meant_texts[f[0]] = [unicodedata.normalize("NFKC", t) for t in f[2:]]
    contexts = {}
    if args.contexts:
        for line in open(args.contexts, encoding="utf-8"):
            k, _, c = line.rstrip("\n").partition("\t")
            contexts[k] = c

    # id -> rows of (rank or -1 for a whole text, text as read, character
    # errors of the text as shown against the nearest meant text, meant).
    def load(patterns):
        lists = {}
        for pattern in patterns:
            for path in sorted(glob.glob(pattern)):
                for line in open(path, encoding="utf-8", errors="replace"):
                    f = line.rstrip("\n").split("\t")
                    if len(f) < 8 or f[1] not in meant_texts:
                        continue
                    k, whole = int(f[2]), f[3] == "1"
                    if not whole and k >= top:
                        continue
                    shown = unicodedata.normalize("NFKC", f[6])
                    errors = 0 if f[4] == "1" else min(char_errors(shown, m) for m in meant_texts[f[1]])
                    lists.setdefault(f[1], []).append((-1 if whole else k, f[7], errors, f[4] == "1"))
        return {k: v for k, v in lists.items() if len(v) > 1 and v[0][0] == 0}

    data = load(args.lists)
    apart = load(args.keep_apart) if args.keep_apart else {}
    for k in apart:
        data.pop(k, None)
    train_ids = sorted(data)
    print(f"{len(train_ids)} lists to learn from, {len(apart)} kept apart", flush=True)
    prior = torch.nn.Parameter(saved["prior"].float().to(dev))
    scale = torch.nn.Parameter(torch.tensor(float(saved["scale"]), device=dev))
    opt = torch.optim.AdamW([{"params": model.parameters(), "lr": args.lr},
                             {"params": [prior, scale], "lr": 1e-3}], weight_decay=0.0)

    def scores(lists, batch_ids):
        seqs, starts, priors, ramps = [], [], [], []
        for k in batch_ids:
            c = sp.encode(contexts.get(k, ""))[-CONTEXT_PIECES:] + [SEP]
            rows = lists[k]
            ramp = min(1.0, len(rows[0][1]) / args.ramp_chars) if args.ramp_chars > 0 else 1.0
            for rank, text, _, _ in rows:
                t = sp.encode(text)
                seqs.append(c + t[:MAX_PIECES - 1] + [EOS])
                starts.append(len(c))
                priors.append(rank)
                ramps.append(ramp)
        x = pad(seqs, dev)
        lengths = torch.as_tensor([len(s) for s in seqs], device=dev)
        with torch.autocast("cuda", dtype=torch.bfloat16, enabled=dev.type == "cuda"):
            lp = model.sentence_logprob(x, torch.as_tensor(starts, device=dev), lengths).float()
        ranks = torch.as_tensor(priors, device=dev)
        p = torch.where(ranks >= 0, prior[ranks.clamp(min=0)], prior[-1] - args.whole_penalty)
        return torch.as_tensor(ramps, device=dev) * scale * lp + p

    def loss_and_counts(lists, batch_ids):
        s = scores(lists, batch_ids)
        loss, at = 0.0, 0
        # Lists, first right, chosen right (with the margin), characters, errors first, errors chosen.
        counts = [0, 0, 0, 0, 0, 0]
        for k in batch_ids:
            rows = lists[k]
            row = s[at:at + len(rows)]
            at += len(rows)
            errors = torch.tensor([float(r[2]) for r in rows], device=dev)
            p = torch.softmax(row / args.temperature, 0)
            loss = loss + (p * (errors - errors.mean())).sum()
            meant = next((i for i, r in enumerate(rows) if r[3]), -1)
            if meant >= 0 and args.ce > 0:
                loss = loss - args.ce * torch.log_softmax(row, 0)[meant]
            best = int(row.argmax())
            chosen = best if float(row[best] - row[0]) > args.margin else 0
            counts[0] += 1
            counts[1] += int(rows[0][3])
            counts[2] += int(rows[chosen][3])
            counts[3] += len(rows[0][1])
            counts[4] += rows[0][2]
            counts[5] += rows[chosen][2]
        return loss / len(batch_ids), counts

    def check():
        if not apart:
            return "no lists kept apart"
        model.eval()
        total = [0] * 6
        ids = sorted(apart)
        with torch.no_grad():
            for b in range(0, len(ids), 64):
                _, c = loss_and_counts(apart, ids[b:b + 64])
                total = [x + y for x, y in zip(total, c)]
        model.train()
        n = max(1, total[0])
        return (f"kept apart {total[0]}: first {100 * total[1] / n:.1f}%, chosen {100 * total[2] / n:.1f}%; "
                f"errors {total[4]} -> {total[5]} (of {total[3]} characters)")

    print(f"before: {check()}", flush=True)
    began = time.time()
    step = 0
    for epoch in range(args.epochs):
        random.Random(epoch).shuffle(train_ids)
        for b in range(0, len(train_ids), args.batch):
            loss, _ = loss_and_counts(data, train_ids[b:b + args.batch])
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            step += 1
            if step % 500 == 0:
                print(f"epoch {epoch} step {step}: {check()}, scale {scale.item():.3f} ({time.time() - began:.0f}s)",
                      flush=True)
    print(f"done: {check()}")
    write = Path(args.write)
    write.mkdir(parents=True, exist_ok=True)
    shutil.copy(out / "pieces.model", write / "pieces.model")
    torch.save({"model": model.state_dict(), "embed": saved["embed"], "hidden": saved["hidden"],
                "vocab": saved["vocab"], "prior": prior.detach().cpu(), "scale": scale.item()},
               write / "finetuned.pt")
    print(f"-> {write}")


def rerank(args) -> None:
    """Chooses again among each row's list (tekito_ja_eval --write-sentences)
    with the finetuned model, on the CPU, and writes source, id, text: the
    same layout as TEKITO_EVAL_OUTPUT. The meant column is not looked at."""
    import torch
    import sentencepiece as spm
    torch.backends.cudnn.enabled = False
    out = Path(args.out)
    sp = spm.SentencePieceProcessor(model_file=str(out / "pieces.model"))
    saved = torch.load(Path(args.model) if args.model else out / "finetuned.pt", map_location="cpu")
    model = make_model(saved["vocab"], saved["embed"], saved["hidden"])
    model.load_state_dict(saved["model"])
    model.eval()
    prior, scale = saved["prior"], saved["scale"]
    top = len(prior)
    # Each list starts at rank 0 (an id can come twice in a set).
    rows: list[tuple[str, str, list[str]]] = []
    for line in open(args.lists, encoding="utf-8", errors="replace"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 5:
            continue
        if int(f[2]) == 0:
            rows.append((f[0], f[1], []))
        if int(f[2]) < top and rows:
            rows[-1][2].append(f[4])
    with open(args.write, "w", encoding="utf-8", newline="\n") as w, torch.no_grad():
        for source, row_id, texts in rows:
            seqs = [[SEP] + t[:MAX_PIECES - 1] + [EOS] for t in sp.encode(texts)]
            x = pad(seqs, torch.device("cpu"))
            lengths = torch.as_tensor([len(s) for s in seqs])
            lp = model.sentence_logprob(x, torch.ones(len(seqs), dtype=torch.long), lengths)
            # Short text gives the model little to go on (names, rare words):
            # it counts in full from --ramp-chars characters, and must beat
            # the engine's first by --margin nats to replace it.
            ramp = min(1.0, len(texts[0]) / args.ramp_chars) if args.ramp_chars > 0 else 1.0
            scores = args.weight * ramp * scale * lp + prior[:len(seqs)]
            best = int(scores.argmax())
            if scores[best] - scores[0] <= args.margin:
                best = 0
            w.write(f"{source}\t{row_id}\t{texts[best]}\n")
    print(f"{len(rows)} rows chosen again -> {args.write}")


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="step", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--out", required=True)
    p.add_argument("--lm-tsv", action="append", required=True)
    p.add_argument("--pieces", help="an existing pieces.model to use instead of training new pieces")
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
    f.add_argument("--check-every", type=int, default=500)
    f.add_argument("--lm-weight", type=float, default=0.0, help="how much the meant sentence's ln p per piece counts too")
    f.add_argument("--apart-max", type=int, default=0, help="check on at most this many of the lists kept apart")
    f.add_argument("--max-lists", type=int, default=0, help="learn from at most this many lists")
    q = sub.add_parser("refit-prior")
    q.add_argument("--out", required=True)
    q.add_argument("--lists", action="append", required=True)
    q.add_argument("--contexts")
    q.add_argument("--write", required=True)
    q.add_argument("--keep-scale", action="store_true", help="fit the prior alone")
    d = sub.add_parser("distill")
    d.add_argument("--out", required=True, help="the finetuned model's directory")
    d.add_argument("--lists", action="append", required=True)
    d.add_argument("--teacher", action="append", required=True, help="ftscore's scores of the same lists")
    d.add_argument("--contexts")
    d.add_argument("--write", required=True, help="directory for the taught model")
    d.add_argument("--temperature", type=float, default=2.0)
    d.add_argument("--alpha", type=float, default=0.5, help="how much the meant sentence counts")
    d.add_argument("--epochs", type=int, default=2)
    d.add_argument("--batch", type=int, default=32)
    d.add_argument("--lr", type=float, default=1e-4)
    w = sub.add_parser("mwer")
    w.add_argument("--out", required=True, help="the model to start from")
    w.add_argument("--lists", action="append", required=True, help="TEKITO_EVAL_CHECKED files")
    w.add_argument("--keep-apart", action="append", help="TEKITO_EVAL_CHECKED files to check on only")
    w.add_argument("--meant", required=True, help="id, reading, meant text(s)")
    w.add_argument("--contexts")
    w.add_argument("--write", required=True)
    w.add_argument("--temperature", type=float, default=1.0)
    w.add_argument("--ce", type=float, default=0.1, help="how much the meant sentence's cross-entropy counts")
    w.add_argument("--ramp-chars", type=float, default=10)
    w.add_argument("--margin", type=float, default=0.5)
    w.add_argument("--whole-penalty", type=float, default=0.0)
    w.add_argument("--epochs", type=int, default=2)
    w.add_argument("--batch", type=int, default=32)
    w.add_argument("--lr", type=float, default=1e-4)
    r = sub.add_parser("rerank")
    r.add_argument("--out", required=True)
    r.add_argument("--model")
    r.add_argument("--lists", required=True)
    r.add_argument("--write", required=True)
    # Chosen on dev sets only (fresh, jsut, mozc_ajimee, ajimee, freshtypo, pub_mozc).
    r.add_argument("--weight", type=float, default=1.0)
    r.add_argument("--ramp-chars", type=float, default=10)
    r.add_argument("--margin", type=float, default=0.5)
    args = parser.parse_args()
    {"prepare": prepare, "pretrain": pretrain, "finetune": finetune, "refit-prior": refit_prior, "distill": distill,
     "mwer": mwer, "rerank": rerank}[args.step](args)
    return 0


if __name__ == "__main__":
    code = main()
    # ROCm on Windows can hang while the process tears down, keeping the
    # GPU's memory: everything is written by now, so leave at once.
    import os
    import sys
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(code)
