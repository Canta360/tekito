#!/usr/bin/env python3
"""Trains a small character language model for choosing again among the
whole-sentence conversions of a reading (a reranker beside the dictionary
and connection costs).

The model reads a sentence a character at a time (a GRU) and says how
likely each next character is. It is trained to be self-normalizing: the
log of the softmax's denominator is pushed toward zero, so at conversion
time a character's score is one dot product, without summing over the
whole vocabulary, and a sentence's score is the sum of its characters'.

Text: the Leipzig Corpora Collection's Japanese sentences (as for the
japanese-lm pack), less every sentence of the evaluation corpora.

  train-japanese-reranker.py prepare --out DIR [--exclude TSV ...]
  train-japanese-reranker.py train --out DIR [--sentences N] [--epochs N]
  train-japanese-reranker.py rerank --out DIR --sentences-tsv TSV [--top N]

`rerank` reads what tekito_ja_eval --write-sentences wrote and reports how
often the meant sentence comes first when the model's score is added.
"""

from __future__ import annotations

import argparse
import collections
import math
import random
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LEIPZIG = ROOT / ".cache" / "tekito-data" / "leipzig-ja"
CORPORA = ["jpn_wikipedia_2021_1M", "jpn_newscrawl_2019_1M"]

UNK, BOS = 0, 1  # BOS also ends a sentence
VOCAB = 6000
MIN_LENGTH, MAX_LENGTH = 4, 100


def read_sentences(leipzig: Path) -> list[str]:
    sentences = []
    for name in CORPORA:
        path = leipzig / name / f"{name}-sentences.txt"
        with path.open(encoding="utf-8") as f:
            for line in f:
                _, _, text = line.rstrip("\n").partition("\t")
                text = text.strip()
                if MIN_LENGTH <= len(text) <= MAX_LENGTH:
                    sentences.append(text)
    return sentences


def read_excluded(paths: list[str]) -> set[str]:
    excluded = set()
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for line in f:
                fields = line.rstrip("\n").split("\t")
                excluded.update(field for field in fields[3:] if field)
    return excluded


def prepare(args) -> None:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    excluded = read_excluded(args.exclude)
    if args.text_tsv:
        # Lines of context, a tab, and text (the context may be empty): the
        # tab is a character of its own, so text is scored after its context.
        with open(args.text_tsv, encoding="utf-8") as f:
            sentences = [line.rstrip("\n") for line in f if line.rstrip("\n").split("\t")[-1] not in excluded]
    else:
        sentences = [s for s in read_sentences(Path(args.leipzig)) if s not in excluded]
    sentences = list(dict.fromkeys(sentences))
    random.Random(1).shuffle(sentences)
    counts = collections.Counter(ch for s in sentences for ch in s)
    vocab = [ch for ch, _ in counts.most_common(VOCAB - 2)]
    (out / "vocab.txt").write_text("\n".join(vocab) + "\n", encoding="utf-8")
    (out / "sentences.txt").write_text("\n".join(sentences) + "\n", encoding="utf-8")
    covered = sum(counts[ch] for ch in vocab) / sum(counts.values())
    print(f"{len(sentences)} sentences, {len(excluded)} excluded, vocab {len(vocab) + 2}, covers {covered:.4%}")


def load_vocab(out: Path) -> dict[str, int]:
    chars = (out / "vocab.txt").read_text(encoding="utf-8").split("\n")
    return {ch: i + 2 for i, ch in enumerate(c for c in chars if c)}


def encode(text: str, vocab: dict[str, int]) -> list[int]:
    return [vocab.get(ch, UNK) for ch in text]


def scored_text(text: str, vocab: dict[str, int], context: str = "") -> str:
    """A model trained with contexts reads text after its context and a tab."""
    return context + "\t" + text if "\t" in vocab else text


def make_model(vocab_size: int, embed: int, hidden: int):
    import torch
    from torch import nn

    class CharModel(nn.Module):
        def __init__(self):
            super().__init__()
            self.embed = nn.Embedding(vocab_size, embed)
            self.gru = nn.GRU(embed, hidden, batch_first=True)
            self.project = nn.Linear(hidden, embed)
            self.bias = nn.Parameter(torch.zeros(vocab_size))

        def forward(self, ids):
            states, _ = self.gru(self.embed(ids))
            # Output weights tied to the input embedding.
            return self.project(states) @ self.embed.weight.T + self.bias

    return CharModel()


def train(args) -> None:
    import torch

    torch.manual_seed(1)
    out = Path(args.out)
    vocab = load_vocab(out)
    sentences = (out / "sentences.txt").read_text(encoding="utf-8").split("\n")
    sentences = [s for s in sentences if s]
    held = sentences[:2000]
    sentences = sentences[2000 : 2000 + args.sentences] if args.sentences else sentences[2000:]
    model = make_model(len(vocab) + 2, args.embed, args.hidden)
    print(f"{sum(p.numel() for p in model.parameters())} parameters, {len(sentences)} sentences")
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)

    def batches(texts, size):
        order = sorted(range(len(texts)), key=lambda i: len(texts[i]))
        chunks = [order[i : i + size] for i in range(0, len(order), size)]
        random.shuffle(chunks)
        for chunk in chunks:
            seqs = [[BOS] + encode(texts[i], vocab) + [BOS] for i in chunk]
            width = max(map(len, seqs))
            ids = torch.full((len(seqs), width), -1, dtype=torch.long)
            for row, seq in enumerate(seqs):
                ids[row, : len(seq)] = torch.tensor(seq)
            yield ids

    def loss_of(ids):
        inputs, targets = ids[:, :-1].clamp(min=0), ids[:, 1:]
        logits = model(inputs)
        mask = targets >= 0
        log_z = torch.logsumexp(logits, dim=-1)
        picked = logits.gather(-1, targets.clamp(min=0).unsqueeze(-1)).squeeze(-1)
        nll = ((log_z - picked) * mask).sum() / mask.sum()
        # Self-normalizing: log Z near zero, so a score needs no sum over the vocabulary.
        norm = ((log_z**2) * mask).sum() / mask.sum()
        return nll, norm

    def held_out():
        model.eval()
        total, count, norms = 0.0, 0, 0.0
        with torch.no_grad():
            for ids in batches(held, 256):
                nll, norm = loss_of(ids)
                n = int((ids[:, 1:] >= 0).sum())
                total += float(nll) * n
                norms += float(norm) * n
                count += n
        model.train()
        return total / count, norms / count

    random.seed(1)
    step, start = 0, time.time()
    for epoch in range(args.epochs):
        for ids in batches(sentences, args.batch):
            nll, norm = loss_of(ids)
            loss = nll + args.alpha * norm
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            step += 1
            if step % 500 == 0:
                h, n = held_out()
                print(f"epoch {epoch} step {step} held nll {h:.3f} log_z^2 {n:.3f} ({time.time() - start:.0f}s)", flush=True)
                torch.save(model.state_dict(), out / "model.pt")
    h, n = held_out()
    print(f"done: held nll {h:.3f} log_z^2 {n:.3f} ({time.time() - start:.0f}s)")
    torch.save(model.state_dict(), out / "model.pt")
    (out / "shape.txt").write_text(f"{len(vocab) + 2} {args.embed} {args.hidden}\n")


def read_lists(path: str, top: int, contexts: dict[str, str] | None = None):
    """Each reading's sentences as (rank, meant, text), with its context
    (the text before it) when `contexts` has its id."""
    rows = collections.defaultdict(list)
    with open(path, encoding="utf-8") as f:
        for line in f:
            source, sid, rank, meant, text = line.rstrip("\n").split("\t")
            if int(rank) < top:
                rows[(source, sid)].append((int(rank), meant == "1", text))
    contexts = contexts or {}
    return [(contexts.get(sid, ""), group) for (_, sid), group in rows.items()]


def read_contexts(path: str | None) -> dict[str, str]:
    if not path:
        return {}
    with open(path, encoding="utf-8") as f:
        return dict(line.rstrip("\n").partition("\t")[::2] for line in f)


def sentence_scores(model, texts: list[str], vocab: dict[str, int], contexts: list[str] | None = None):
    """Each text's score, the sum of its characters' self-normalized scores
    (after its context; the context adds the same to every text of a list)."""
    import torch

    contexts = contexts or [""] * len(texts)
    seqs = [[BOS] + encode(scored_text(t, vocab, c), vocab) + [BOS] for t, c in zip(texts, contexts)]
    width = max(map(len, seqs))
    ids = torch.full((len(seqs), width), -1, dtype=torch.long)
    for row, seq in enumerate(seqs):
        ids[row, : len(seq)] = torch.tensor(seq)
    logits = model(ids[:, :-1].clamp(min=0))
    targets = ids[:, 1:]
    mask = targets >= 0
    picked = logits.gather(-1, targets.clamp(min=0).unsqueeze(-1)).squeeze(-1)
    return (picked * mask).sum(1)


def finetune(args) -> None:
    """Teaches the model which of a reading's whole-sentence conversions was
    meant (lists from tekito_ja_eval --write-sentences over sentences with
    readings from --guess-readings): the model's score and the rank the
    dictionary gave are weighed together, and a softmax over each list
    should put the meant sentence first."""
    import torch

    out = Path(args.out)
    vocab = load_vocab(out)
    size, embed, hidden = map(int, (out / "shape.txt").read_text().split())
    model = make_model(size, embed, hidden)
    model.load_state_dict(torch.load(out / "model.pt"))
    lists = [(c, g) for c, g in read_lists(args.sentences_tsv, args.top, read_contexts(args.contexts))
             if any(m for _, m, _ in g) and len(g) > 1]
    random.Random(1).shuffle(lists)
    # Lists kept apart to see whether what is learned holds for new ones.
    held, lists = lists[:5000], lists[5000:]
    print(f"{len(lists)} lists with the meant sentence among the first {args.top}, {len(held)} held out")

    def held_right() -> float:
        right = 0
        with torch.no_grad():
            for b in range(0, len(held), 64):
                batch = held[b : b + 64]
                scores = sentence_scores(model, [t for _, g in batch for _, _, t in g], vocab,
                                         [c for c, g in batch for _ in g])
                at = 0
                for _, g in batch:
                    ranks = torch.tensor([math.log1p(r) for r, _, _ in g])
                    final = scale * scores[at : at + len(g)] - rank_weight * ranks
                    at += len(g)
                    right += g[int(final.argmax())][1]
        return 100 * right / len(held)
    # How much the model's score counts, and the rank (as log(1 + rank)).
    scale = torch.nn.Parameter(torch.tensor(0.3))
    rank_weight = torch.nn.Parameter(torch.tensor(2.0))
    optimizer = torch.optim.Adam([*model.parameters(), scale, rank_weight], lr=args.lr)
    baseline = 100 * sum(any(m for r, m, _ in g if r == 0) for _, g in held) / len(held)
    print(f"held out: first right {baseline:.1f}% by rank alone, {held_right():.1f}% with the model", flush=True)
    start = time.time()
    for epoch in range(args.epochs):
        right = total = 0
        for b in range(0, len(lists), args.batch):
            batch = lists[b : b + args.batch]
            texts = [t for _, g in batch for _, _, t in g]
            scores = sentence_scores(model, texts, vocab, [c for c, g in batch for _ in g])
            loss = 0.0
            at = 0
            for _, g in batch:
                ranks = torch.tensor([math.log1p(r) for r, _, _ in g])
                final = scale * scores[at : at + len(g)] - rank_weight * ranks
                at += len(g)
                meant = next(i for i, (_, m, _) in enumerate(g) if m)
                loss = loss + torch.nn.functional.cross_entropy(final.unsqueeze(0), torch.tensor([meant]))
                right += int(final.argmax()) == meant
                total += 1
            optimizer.zero_grad()
            (loss / len(batch)).backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            if (b // args.batch) % 2000 == 0:
                print(f"epoch {epoch} lists {b}: first right {100 * right / max(total, 1):.1f}%, held out {held_right():.1f}% "
                      f"scale {scale.item():.3f} rank {rank_weight.item():.3f} ({time.time() - start:.0f}s)", flush=True)
                right = total = 0
    target = out / "finetuned"
    target.mkdir(exist_ok=True)
    torch.save(model.state_dict(), target / "model.pt")
    (target / "shape.txt").write_text(f"{size} {embed} {hidden}\n")
    (target / "weights.txt").write_text(f"{scale.item()} {rank_weight.item()}\n")
    (target / "vocab.txt").write_text((out / "vocab.txt").read_text(encoding="utf-8"), encoding="utf-8")


def rerank(args) -> None:
    import torch

    out = Path(args.out)
    vocab = load_vocab(out)
    size, embed, hidden = map(int, (out / "shape.txt").read_text().split())
    model = make_model(size, embed, hidden)
    model.load_state_dict(torch.load(out / "model.pt"))
    model.eval()

    rows = collections.defaultdict(list)
    with open(args.sentences_tsv, encoding="utf-8") as f:
        for line in f:
            source, sid, rank, meant, text = line.rstrip("\n").split("\t")
            if int(rank) < args.top:
                rows[(source, sid)].append((int(rank), meant == "1", text))

    def scores(texts):
        seqs = [[BOS] + encode(scored_text(t, vocab), vocab) + [BOS] for t in texts]
        width = max(map(len, seqs))
        ids = torch.full((len(seqs), width), -1, dtype=torch.long)
        for row, seq in enumerate(seqs):
            ids[row, : len(seq)] = torch.tensor(seq)
        with torch.no_grad():
            logits = model(ids[:, :-1].clamp(min=0))
        targets = ids[:, 1:]
        mask = targets >= 0
        picked = logits.gather(-1, targets.clamp(min=0).unsqueeze(-1)).squeeze(-1)
        exact = picked - torch.logsumexp(logits, dim=-1)
        return ((exact * mask).sum(1).tolist(), (picked * mask).sum(1).tolist())

    scored = []
    for key, items in rows.items():
        exact, quick = scores([t for _, _, t in items])
        scored.append((key[0], [(r, m, e, q) for (r, m, _), e, q in zip(items, exact, quick)]))

    # The model's score per character beside the rank the dictionary gave,
    # weighed half on one half of the sentences and checked on the other.
    def first_right(group, weight, which):
        best = max(group, key=lambda x: x[2 + which] - weight * math.log1p(x[0]))
        return best[1]

    for which, name in ((0, "exact"), (1, "self-normalized")):
        tune = [g for i, (_, g) in enumerate(scored) if i % 2 == 0]
        weights = [0, 0.25, 0.5, 1, 1.5, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32, 1e9]
        weight = max(weights, key=lambda w: sum(first_right(g, w, which) for g in tune))
        by_source = collections.defaultdict(lambda: [0, 0, 0])
        for i, (source, group) in enumerate(scored):
            if i % 2 == 0:
                continue
            for s in (source, "all"):
                by_source[s][0] += 1
                by_source[s][1] += any(m for r, m, _, _ in group if r == 0)
                by_source[s][2] += first_right(group, weight, which)
        for s, (n, before, after) in sorted(by_source.items()):
            print(f"{name} weight {weight}: {s}: rows={n} top1 {100 * before / n:.1f}% -> {100 * after / n:.1f}%")


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--out", required=True)
    p.add_argument("--exclude", action="append", default=[])
    p.add_argument("--leipzig", default=str(LEIPZIG))
    p.add_argument("--text-tsv")
    t = sub.add_parser("train")
    t.add_argument("--out", required=True)
    t.add_argument("--sentences", type=int, default=0)
    t.add_argument("--epochs", type=int, default=1)
    t.add_argument("--batch", type=int, default=64)
    t.add_argument("--embed", type=int, default=128)
    t.add_argument("--hidden", type=int, default=256)
    t.add_argument("--lr", type=float, default=2e-3)
    t.add_argument("--alpha", type=float, default=0.1)
    r = sub.add_parser("rerank")
    r.add_argument("--out", required=True)
    r.add_argument("--sentences-tsv", required=True)
    r.add_argument("--top", type=int, default=10)
    f = sub.add_parser("finetune")
    f.add_argument("--out", required=True)
    f.add_argument("--sentences-tsv", required=True)
    f.add_argument("--top", type=int, default=10)
    f.add_argument("--contexts")
    f.add_argument("--epochs", type=int, default=1)
    f.add_argument("--batch", type=int, default=16)
    f.add_argument("--lr", type=float, default=3e-4)
    args = parser.parse_args()
    {"prepare": prepare, "train": train, "finetune": finetune, "rerank": rerank}[args.command](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
