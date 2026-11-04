"""The edge scorer: a small model that scores the words of the Japanese
converter's lattice (stage 2 of the conversion roadmap).

Each word of the lattice (a span of the reading written one way) gets a
score from a bidirectional Transformer that reads the text before the
caret, the reading and the converter's own first choice. A path's score is
the words' scores less the converter's costs (word costs and connection
costs, scaled by a learned factor); training maximizes the meant path's
probability over the whole lattice (a CRF).

  prepare  lattices (tekito_ja_eval --dump-lattices) into one file of arrays
  train    train, checking on the lines kept apart
  export   a trained model for the C++ EdgeScorer (Core/Japanese/EdgeScorer)

  python train-japanese-edges.py prepare --lattices <dir> --contexts <id, text tsv> --out <data.pt>
  python train-japanese-edges.py train --data <data.pt> --matrix <japanese-core/connection.bin>
         --keep-apart <ids tsv> --out <model dir>
"""
from __future__ import annotations

import argparse
import glob
import math
import random
import struct
import sys
import time
from collections import Counter
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

# Input tokens: padding, unknown, and the marks between the parts.
PAD, UNK, CLS, SEP_CONTEXT, SEP_READING = 0, 1, 2, 3, 4
SPECIAL = 5
CONTEXT_CHARS = 40
DRAFT_CHARS = 96
WORD_CHARS = 12


def fold(text: str) -> str:
    """Full-width letters, digits and marks as half-width, as the lattice
    dump matches the meant text (２つ目, 2つ目)."""
    return "".join(chr(ord(c) - 0xFEE0) if 0xFF01 <= ord(c) <= 0xFF5E else " " if c == "　" else c for c in text)


def read_matrix(path: Path) -> np.ndarray:
    """japanese-core/connection.bin (TKJM): the connection cost of a right id
    followed by a left id, as ConnectionMatrix::Cost gives it."""
    data = path.read_bytes()
    if data[:4] != b"TKJM":
        raise ValueError(f"{path} is no connection matrix")
    fields = struct.unpack_from("<9I", data, 4)
    _, size, _, scale, _, costs, _, _, _ = fields
    matrix = np.frombuffer(data, dtype=np.uint8, count=size * size, offset=costs).reshape(size, size)
    return matrix.astype(np.int32) * int(scale)


def prepare(args) -> None:
    contexts = {}
    for path in args.contexts:
        for line in open(path, encoding="utf-8"):
            key, _, text = line.rstrip("\n").partition("\t")
            if text:
                contexts[key] = text
    lines = []
    chars = Counter()
    for path in sorted(p for d in args.lattices for p in glob.glob(str(Path(d) / "lattice.*.tsv"))):
        for line in open(path, encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) < 10:
                continue
            words = [w.split(",", 6) for w in f[6].split("\x1f")] if f[6] else []
            joins = [tuple(int(x) for x in j.split(",")) for j in f[7].split()] if f[7] else []
            best = [int(x) for x in f[8].split()] if f[8] else []
            gold = [int(x) for x in f[9].split()] if f[9] else []
            context = contexts.get(f[0], "")[-CONTEXT_CHARS:]
            draft = "".join(words[k][6] for k in best)
            chars.update(f[1])
            chars.update(context)
            chars.update(draft)
            for w in words:
                chars.update(w[6])
            lines.append({
                "id": f[0], "reading": f[1], "meant": f[2], "context": context, "draft": draft,
                "begin": np.array([int(w[0]) for w in words], np.int16), "end": np.array([int(w[1]) for w in words], np.int16),
                "left": np.array([int(w[2]) for w in words], np.int16), "right": np.array([int(w[3]) for w in words], np.int16),
                "cost": np.array([int(w[4]) for w in words], np.int32), "known": np.array([int(w[5]) for w in words], np.int8),
                "text": "".join(w[6] for w in words), "joins": np.array(joins, np.int32).reshape(-1, 3),
                "best": best, "gold": gold,
            })
    vocab = ["<pad>", "<unk>", "<cls>", "<sep-context>", "<sep-reading>"]
    vocab += [c for c, n in chars.most_common() if n >= args.min_count][: args.vocab - SPECIAL]
    torch.save({"lines": lines, "vocab": vocab}, args.out)
    found = sum(1 for line in lines if line["gold"])
    print(f"{len(lines)} lines, {found} with the meant path, vocabulary {len(vocab)}")


class EdgeScorer(nn.Module):
    def __init__(self, vocab: int, parts: int, dim: int = 256, layers: int = 4, heads: int = 4, ff: int = 1024,
                 longest: int = 320):
        super().__init__()
        self.dim = dim
        self.chars = nn.Embedding(vocab, dim, padding_idx=PAD)
        self.places = nn.Embedding(longest, dim)
        layer = nn.TransformerEncoderLayer(dim, heads, ff, dropout=0.1, batch_first=True, norm_first=True)
        self.encoder = nn.TransformerEncoder(layer, layers)
        self.norm = nn.LayerNorm(dim)
        self.span = nn.Sequential(nn.Linear(2 * dim, dim), nn.GELU(), nn.Linear(dim, dim))
        part = 48
        self.left = nn.Embedding(parts, part)
        self.right = nn.Embedding(parts, part)
        self.word = nn.Sequential(nn.Linear(dim + 2 * part + 2, dim), nn.GELU(), nn.Linear(dim, dim))
        # A path's cost in nats: cost / scale (Mozc costs run about 500 a nat).
        self.cost_scale = nn.Parameter(torch.tensor(math.log(1.0 / 500.0)))

    def encode(self, tokens: torch.Tensor) -> torch.Tensor:
        places = torch.arange(tokens.shape[1], device=tokens.device)
        x = self.chars(tokens) + self.places(places)[None]
        return self.norm(self.encoder(x, src_key_padding_mask=tokens == PAD))

    def scores(self, hidden, line_of, first, last, word_chars, left, right, known, cost):
        """Each word's score: its span of the reading (hidden states at the
        first and last character) against the word (its characters, parts of
        speech, whether the dictionary has it, its cost)."""
        span = self.span(torch.cat([hidden[line_of, first], hidden[line_of, last]], dim=-1))
        mask = (word_chars != PAD).float()
        text = (self.chars(word_chars) * mask[..., None]).sum(1) / mask.sum(1, keepdim=True).clamp(min=1)
        word = self.word(torch.cat([text, self.left(left), self.right(right), known[:, None],
                                    (cost.float() / 5000.0)[:, None]], dim=-1))
        return (span * word).sum(-1) / math.sqrt(self.dim)


class Batches:
    """Lines as tensors for the model and the lattice sums."""

    def __init__(self, lines, vocab, matrix, device):
        self.index = {c: i for i, c in enumerate(vocab)}
        self.matrix = matrix
        self.device = device
        self.lines = lines

    def ids(self, text):
        return [self.index.get(c, UNK) for c in text]

    def make(self, lines):
        dev = self.device
        rows, reading_at = [], []
        for line in lines:
            row = [CLS] + self.ids(line["context"]) + [SEP_CONTEXT]
            reading_at.append(len(row))
            row += self.ids(line["reading"]) + [SEP_READING] + self.ids(line["draft"][:DRAFT_CHARS])
            rows.append(row)
        width = max(len(r) for r in rows)
        tokens = torch.zeros(len(rows), width, dtype=torch.long)
        for i, r in enumerate(rows):
            tokens[i, : len(r)] = torch.tensor(r)
        # Words of all lines in one list; their predecessors.
        line_of, first, last, chars, left, right, known, cost, begin, end = [], [], [], [], [], [], [], [], [], []
        offsets, gold, n_of = [], [], []
        pair_from, pair_to, pair_cost = [], [], []
        start_cost, end_cost = [], []
        for i, packed in enumerate(lines):
            line = {key: (value.tolist() if isinstance(value, np.ndarray) else value) for key, value in packed.items()}
            line["text"] = packed["text"].split("")
            base = len(begin)
            offsets.append(base)
            n = len(line["reading"])
            n_of.append(n)
            joins = {(a, b): c for a, b, c in line["joins"]}
            ending = {}
            for k in range(len(line["begin"])):
                ending.setdefault(line["end"][k], []).append(k)
            for k in range(len(line["begin"])):
                b, e = line["begin"][k], line["end"][k]
                line_of.append(i)
                first.append(reading_at[i] + b)
                last.append(reading_at[i] + e - 1)
                ids = self.ids(line["text"][k][:WORD_CHARS])
                chars.append(ids + [PAD] * (WORD_CHARS - len(ids)))
                left.append(line["left"][k])
                right.append(line["right"][k])
                known.append(float(line["known"][k]))
                cost.append(line["cost"][k])
                begin.append(b)
                end.append(e)
                start_cost.append(int(self.matrix[0, line["left"][k]]) if b == 0 else 0)
                end_cost.append(int(self.matrix[line["right"][k], 0]) if e == n else 0)
                for p in ending.get(b, []):
                    pair_from.append(base + p)
                    pair_to.append(base + k)
                    pair_cost.append(int(self.matrix[line["right"][p], line["left"][k]]) + joins.get((p, k), 0))
            gold.append([base + k for k in line["gold"]])
        t = lambda v, dtype=torch.long: torch.tensor(v, dtype=dtype, device=dev)
        return {
            "tokens": tokens.to(dev), "line_of": t(line_of), "first": t(first), "last": t(last),
            "chars": t(chars), "left": t(left), "right": t(right), "known": t(known, torch.float),
            "cost": t(cost), "begin": t(begin), "end": t(end), "n": t(n_of),
            "pair_from": t(pair_from), "pair_to": t(pair_to), "pair_cost": t(pair_cost),
            "start_cost": t(start_cost), "end_cost": t(end_cost), "gold": gold,
            "offsets": offsets, "count": len(lines),
        }


def lattice_sums(batch, emit, scale, best=False):
    """Over each line's lattice: the log of the sum of exp(path score), or
    with `best` the best path's score and its words. A word's score is its
    emit less its cost; going from p to k costs pair_cost."""
    words = emit.shape[0]
    begin, end = batch["begin"], batch["end"]
    alpha = torch.full((words,), -1e30, device=emit.device)
    back = torch.full((words,), -1, dtype=torch.long, device=emit.device)
    own = emit - scale * batch["cost"].float()
    starts = begin == 0
    alpha = torch.where(starts, own - scale * batch["start_cost"].float(), alpha)
    pair_from, pair_to = batch["pair_from"], batch["pair_to"]
    pair_score = -scale * batch["pair_cost"].float()
    longest = int(batch["n"].max())
    by_begin = begin[pair_to]
    for i in range(1, longest):
        sel = by_begin == i
        if not bool(sel.any()):
            continue
        frm, to = pair_from[sel], pair_to[sel]
        value = alpha[frm] + pair_score[sel]
        if best:
            top = torch.full((words,), -1e30, device=emit.device).scatter_reduce(0, to, value, "amax")
            # The predecessor that gave it.
            hit = value >= top[to] - 1e-6
            arg = torch.full((words,), -1, dtype=torch.long, device=emit.device)
            arg.scatter_(0, to[hit], frm[hit])
            targets = torch.unique(to)
            alpha = alpha.index_copy(0, targets, top[targets] + own[targets])
            back = back.index_copy(0, targets, arg[targets])
        else:
            peak = torch.full((words,), -1e30, device=emit.device).scatter_reduce(0, to, value, "amax")
            total = torch.zeros(words, device=emit.device).index_add(0, to, torch.exp(value - peak[to]))
            targets = torch.unique(to)
            new = peak[targets] + torch.log(total[targets]) + own[targets]
            alpha = alpha.index_copy(0, targets, new)
    finals = alpha - scale * batch["end_cost"].float()
    ends = end == batch["n"][batch["line_of"]]
    line_of = batch["line_of"]
    lines = batch["count"]
    if best:
        top = torch.full((lines,), -1e30, device=emit.device).scatter_reduce(
            0, line_of[ends], finals[ends], "amax")
        return top, back, finals, ends
    peak = torch.full((lines,), -1e30, device=emit.device).scatter_reduce(0, line_of[ends], finals[ends], "amax")
    total = torch.zeros(lines, device=emit.device).index_add(0, line_of[ends], torch.exp(finals[ends] - peak[line_of[ends]]))
    return peak + torch.log(total)


def gold_scores(batch, emit, scale):
    out = []
    pair = {}
    pf, pt, pc = batch["pair_from"].tolist(), batch["pair_to"].tolist(), batch["pair_cost"]
    for j, (a, b) in enumerate(zip(pf, pt)):
        pair[(a, b)] = j
    own = emit - scale * batch["cost"].float()
    for path in batch["gold"]:
        if not path:
            out.append(None)
            continue
        idx = torch.tensor(path, device=emit.device)
        score = own[idx].sum() - scale * batch["start_cost"][path[0]].float() - scale * batch["end_cost"][path[-1]].float()
        links = torch.tensor([pair[(a, b)] for a, b in zip(path, path[1:])], dtype=torch.long, device=emit.device)
        if len(links):
            score = score - scale * pc[links].float().sum()
        out.append(score)
    return out


def decode(batch, emit, scale):
    """Each line's best path, as its words' texts joined."""
    top, back, finals, ends = lattice_sums(batch, emit, scale, best=True)
    line_of = batch["line_of"].tolist()
    finals_l, ends_l, back_l = finals.tolist(), ends.tolist(), back.tolist()
    best_end = {}
    for k, (line, value, is_end) in enumerate(zip(line_of, finals_l, ends_l)):
        if is_end and (line not in best_end or value > best_end[line][0]):
            best_end[line] = (value, k)
    paths = []
    for line in range(batch["count"]):
        if line not in best_end:
            paths.append([])
            continue
        k, path = best_end[line][1], []
        while k >= 0:
            path.append(k)
            k = back_l[k]
        paths.append(path[::-1])
    return paths


def check(model, batches, lines, device, size=64):
    model.eval()
    right = cheapest = 0
    with torch.no_grad():
        for start in range(0, len(lines), size):
            chunk = lines[start:start + size]
            batch = batches.make(chunk)
            hidden = model.encode(batch["tokens"])
            emit = model.scores(hidden, batch["line_of"], batch["first"], batch["last"], batch["chars"],
                                batch["left"], batch["right"], batch["known"], batch["cost"])
            scale = torch.exp(model.cost_scale)
            for line, path, offset in zip(chunk, decode(batch, emit, scale), batch["offsets"]):
                texts = line["text"].split("")
                meant = fold(line["meant"])
                right += fold("".join(texts[k - offset] for k in path)) == meant
                cheapest += fold("".join(texts[k] for k in line["best"])) == meant
    model.train()
    return right / max(len(lines), 1), cheapest / max(len(lines), 1)


def kanji(text: str) -> bool:
    return any("一" <= c <= "鿿" or c == "々" for c in text)


def weigh_confusions(lines, weight: int, least: int):
    """Lines that hold a pair of words the converter often confuses, more than
    once: where its first choice and the meant path take the same span of the
    reading with different words (選択 for 洗濯), the pairs seen in at least
    `least` lines; a line holds a pair when one of its words is on the meant
    path and the other is in its lattice at the same span."""
    def spans(line):
        words = line["text"].split("\x1f")
        return {(int(line["begin"][k]), int(line["end"][k]), words[k]) for k in range(len(words))}, words

    pairs = Counter()
    for line in lines:
        if line["best"] == line["gold"]:
            continue
        _, words = spans(line)
        best = {(int(line["begin"][k]), int(line["end"][k])): words[k] for k in line["best"]}
        for k in line["gold"]:
            span = (int(line["begin"][k]), int(line["end"][k]))
            other = best.get(span)
            # Two ways in kanji (選択 and 洗濯), not kanji against kana (a
            # matter of how a word is written).
            if other and other != words[k] and kanji(other) and kanji(words[k]):
                pairs[frozenset((other, words[k]))] += 1
    common = {p for p, n in pairs.items() if n >= least and len(p) == 2}
    print("most confused:", " ".join("/".join(sorted(p)) for p, _ in pairs.most_common(30)), flush=True)
    by_word = {}
    for p in common:
        a, b = tuple(p)
        by_word.setdefault(a, set()).add(b)
        by_word.setdefault(b, set()).add(a)
    out = list(lines)
    held = 0
    for line in lines:
        everything, words = spans(line)
        for k in line["gold"]:
            b, e = int(line["begin"][k]), int(line["end"][k])
            if any((b, e, o) in everything for o in by_word.get(words[k], ())):
                out.extend([line] * (weight - 1))
                held += 1
                break
    print(f"confused pairs {len(common)} (of {len(pairs)}), lines holding one {held}", flush=True)
    return out


def train(args) -> None:
    random.seed(0)
    torch.manual_seed(0)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    # More than one data file: the first one's vocabulary, every file's lines.
    data = torch.load(args.data[0], weights_only=False)
    lines, vocab = data["lines"], data["vocab"]
    for path in args.data[1:]:
        lines = lines + torch.load(path, weights_only=False)["lines"]
    apart = set()
    for path in args.keep_apart:
        for line in open(path, encoding="utf-8"):
            parts = line.rstrip("\n").split("\t")
            apart.add(parts[1] if len(parts) > 1 else parts[0])
    held = [line for line in lines if line["id"] in apart]
    taught = [line for line in lines if line["id"] not in apart and line["gold"]]
    if args.confusion_weight > 1:
        taught = weigh_confusions(taught, args.confusion_weight, args.confusion_min)
    print(f"training on {len(taught)} lines, checking on {len(held)}", flush=True)
    matrix = read_matrix(Path(args.matrix))
    parts = int(matrix.shape[0])
    batches = Batches(lines, vocab, matrix, device)
    model = EdgeScorer(len(vocab), parts, dim=args.dim, layers=args.layers, heads=args.heads, ff=args.ff).to(device)
    print(f"parameters {sum(p.numel() for p in model.parameters()) / 1e6:.1f}M", flush=True)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01)
    steps = args.epochs * math.ceil(len(taught) / args.batch)
    schedule = torch.optim.lr_scheduler.OneCycleLR(optimizer, max_lr=args.lr, total_steps=steps, pct_start=0.05)
    sample = held[: args.check_lines]
    right, cheapest = check(model, batches, sample, device)
    print(f"before: meant {100 * right:.2f}% (converter alone {100 * cheapest:.2f}%)", flush=True)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    step = 0
    best_right = -1.0
    for epoch in range(args.epochs):
        random.shuffle(taught)
        started, loss_sum = time.time(), 0.0
        for start in range(0, len(taught), args.batch):
            chunk = taught[start:start + args.batch]
            batch = batches.make(chunk)
            hidden = model.encode(batch["tokens"])
            emit = model.scores(hidden, batch["line_of"], batch["first"], batch["last"], batch["chars"],
                                batch["left"], batch["right"], batch["known"], batch["cost"])
            scale = torch.exp(model.cost_scale)
            logz = lattice_sums(batch, emit, scale)
            golds = gold_scores(batch, emit, scale)
            loss = torch.stack([logz[i] - g for i, g in enumerate(golds) if g is not None]).mean()
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            schedule.step()
            step += 1
            loss_sum += float(loss)
            if step % 200 == 0:
                print(f"epoch {epoch} step {step}/{steps} loss {loss_sum / 200:.4f} scale 1/{1 / float(scale):.0f} "
                      f"{time.time() - started:.0f}s", flush=True)
                loss_sum = 0.0
        right, cheapest = check(model, batches, sample, device)
        print(f"epoch {epoch}: meant {100 * right:.2f}% (converter alone {100 * cheapest:.2f}%)", flush=True)
        if right > best_right:
            best_right = right
            torch.save({"model": model.state_dict(), "vocab": vocab, "args": vars(args)}, out / "model.pt")
    right, cheapest = check(model, batches, held, device)
    print(f"all kept apart ({len(held)}): meant {100 * right:.2f}% (converter alone {100 * cheapest:.2f}%)", flush=True)


def export(args) -> None:
    """model.pt as the C++ EdgeScorer reads it: <out>/edges.bin (TKES, then
    the sizes and each tensor in float32, in the order below) and
    <out>/chars.txt (each character's code point in hex, in token order)."""
    saved = torch.load(args.model, map_location="cpu", weights_only=False)
    state, vocab, settings = saved["model"], saved["vocab"], saved["args"]
    dim, layers, heads, ff = settings["dim"], settings["layers"], settings["heads"], settings["ff"]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    names = ["chars.weight", "places.weight"]
    for i in range(layers):
        prefix = f"encoder.layers.{i}."
        names += [prefix + n for n in ("norm1.weight", "norm1.bias", "self_attn.in_proj_weight", "self_attn.in_proj_bias",
                                       "self_attn.out_proj.weight", "self_attn.out_proj.bias", "norm2.weight", "norm2.bias",
                                       "linear1.weight", "linear1.bias", "linear2.weight", "linear2.bias")]
    names += ["norm.weight", "norm.bias", "span.0.weight", "span.0.bias", "span.2.weight", "span.2.bias",
              "left.weight", "right.weight", "word.0.weight", "word.0.bias", "word.2.weight", "word.2.bias", "cost_scale"]
    with open(out / "edges.bin", "wb") as f:
        f.write(b"TKES")
        longest = state["places.weight"].shape[0]
        parts = state["left.weight"].shape[0]
        part = state["left.weight"].shape[1]
        f.write(struct.pack("<9I", 1, len(vocab), dim, layers, heads, ff, longest, parts, part))
        for name in names:
            tensor = state[name].float().contiguous().numpy()
            f.write(tensor.astype("<f4").tobytes())
    with open(out / "chars.txt", "w", encoding="ascii", newline=chr(10)) as f:
        for i, c in enumerate(vocab):
            f.write(("0" if i < SPECIAL else f"{ord(c):x}") + chr(10))
    print(f"wrote {out / 'edges.bin'} ({(out / 'edges.bin').stat().st_size / 1e6:.1f} MB), {len(vocab)} characters")
    if args.data:
        # Reference scores for a few lines (test.tsv), to check the C++ scorer
        # against: context, reading, draft, then each word as
        # begin,end,left,right,cost,known,score,text.
        lines = torch.load(args.data, weights_only=False)["lines"][: args.test_lines]
        model = EdgeScorer(len(vocab), parts, dim=dim, layers=layers, heads=heads, ff=ff, longest=longest)
        model.load_state_dict(state)
        model.eval()
        batches = Batches(lines, vocab, np.zeros((parts, parts), np.int32), torch.device("cpu"))
        sep = chr(9)
        with torch.no_grad(), open(out / "test.tsv", "w", encoding="utf-8", newline=chr(10)) as f:
            for line in lines:
                batch = batches.make([line])
                hidden = model.encode(batch["tokens"])
                emit = model.scores(hidden, batch["line_of"], batch["first"], batch["last"], batch["chars"],
                                    batch["left"], batch["right"], batch["known"], batch["cost"]).tolist()
                texts = line["text"].split(chr(31))
                words = [f"{b},{e},{l},{r},{c},{k},{v:.6f},{t}" for b, e, l, r, c, k, v, t in
                         zip(line["begin"], line["end"], line["left"], line["right"], line["cost"], line["known"], emit, texts)]
                f.write(sep.join([line["context"], line["reading"], line["draft"]] + words) + chr(10))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="step", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--lattices", action="append", required=True, help="a folder of lattice.*.tsv (more than once)")
    p.add_argument("--contexts", action="append", default=[], help="id TAB text before (more than once)")
    p.add_argument("--out", required=True)
    p.add_argument("--vocab", type=int, default=8000)
    p.add_argument("--min-count", type=int, default=3)
    t = sub.add_parser("train")
    t.add_argument("--data", action="append", required=True, help="data.pt (more than once; the first one's vocabulary)")
    t.add_argument("--matrix", required=True)
    t.add_argument("--confusion-weight", type=int, default=1,
                   help="times a line holding an often-confused pair of words is taught")
    t.add_argument("--confusion-min", type=int, default=10, help="lines a pair must be confused in")
    t.add_argument("--keep-apart", action="append", default=[])
    t.add_argument("--out", required=True)
    t.add_argument("--dim", type=int, default=256)
    t.add_argument("--layers", type=int, default=4)
    t.add_argument("--heads", type=int, default=4)
    t.add_argument("--ff", type=int, default=1024)
    t.add_argument("--epochs", type=int, default=3)
    t.add_argument("--batch", type=int, default=64)
    t.add_argument("--lr", type=float, default=5e-4)
    t.add_argument("--check-lines", type=int, default=2000)
    e = sub.add_parser("export")
    e.add_argument("--model", required=True)
    e.add_argument("--out", required=True)
    e.add_argument("--data", help="data.pt, for reference scores of a few lines (test.tsv)")
    e.add_argument("--test-lines", type=int, default=5)
    args = parser.parse_args()
    {"prepare": prepare, "train": train, "export": export}[args.step](args)


if __name__ == "__main__":
    main()
