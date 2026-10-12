#!/usr/bin/env python3
"""Writes japanese-reranker-mini, a tiny pack for the tests: a few pieces,
random weights, and expected.tsv with what SentencePiece reads each test
text as and ln p of it in float (the C++ side reads activations as int8,
so it agrees to a few hundredths).

Needs numpy, sentencepiece and protobuf, and a trained pieces.model for its
normalizer (nmt_nfkc):

  python tests/data/ja/make-japanese-reranker-mini.py PIECES.model
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

import numpy as np
import sentencepiece as spm
from sentencepiece import sentencepiece_model_pb2 as pb

OUT = Path(__file__).resolve().parent / "japanese-reranker-mini"
EMBED, HIDDEN, TOP = 16, 16, 3
NORMAL = ["▁", "今日", "▁今日", "は", "晴れ", "です", "。", "雨", "天気", "▁abc", "a", "b", "c", "12", "1", "2",
          "▁は", "今", "日", "晴", "れ", "で", "す"]
TEXTS = ["今日は晴れです。", "今日は雨です。", "今日は天気です。", "abc 12", "１２", "今日は☃", "  今日  は ", ""]


def main() -> int:
    source = pb.ModelProto()
    source.ParseFromString(Path(sys.argv[1]).read_bytes())
    proto = pb.ModelProto()
    proto.normalizer_spec.CopyFrom(source.normalizer_spec)
    proto.trainer_spec.CopyFrom(source.trainer_spec)
    pieces = [("<unk>", 0.0, 2), ("<s>", 0.0, 3), ("</s>", 0.0, 3), ("<sep>", 0.0, 4)]
    pieces += [(f"<0x{b:02X}>", 0.0, 6) for b in range(256)]
    rng = np.random.default_rng(7)
    pieces += [(p, float(-1 - len(p) * 0.1 - i * 0.01), 1) for i, p in enumerate(NORMAL)]
    for text, score, kind in pieces:
        piece = proto.pieces.add()
        piece.piece, piece.score, piece.type = text, score, kind
    proto.trainer_spec.vocab_size = len(pieces)
    sp = spm.SentencePieceProcessor(model_proto=proto.SerializeToString())
    vocab = len(pieces)

    emb = rng.normal(0, 0.5, (vocab, EMBED)).astype(np.float32)
    wih = rng.normal(0, 0.3, (4 * HIDDEN, EMBED)).astype(np.float32)
    whh = rng.normal(0, 0.3, (4 * HIDDEN, HIDDEN)).astype(np.float32)
    bias = rng.normal(0, 0.1, 4 * HIDDEN).astype(np.float32)
    proj = rng.normal(0, 0.3, (EMBED, HIDDEN)).astype(np.float32)
    proj_b = rng.normal(0, 0.1, EMBED).astype(np.float32)

    def quantize(m):
        s = np.abs(m).max(axis=1) / 127.0
        q = np.clip(np.rint(m / s[:, None]), -127, 127).astype(np.int8)
        return q, s.astype(np.float32)

    emb_q, emb_s = quantize(emb)
    wih_q, wih_s = quantize(wih)
    whh_q, whh_s = quantize(whh)
    # The reference reads the int8 weights back as floats, as the pack has them.
    E, Wi, Wh = emb_q * emb_s[:, None], wih_q * wih_s[:, None], whh_q * whh_s[:, None]

    def step(h, c, piece):
        g = Wi @ E[piece] + Wh @ h + bias
        i, f, gg, o = (1 / (1 + np.exp(-g[:HIDDEN])), 1 / (1 + np.exp(-g[HIDDEN:2 * HIDDEN])),
                       np.tanh(g[2 * HIDDEN:3 * HIDDEN]), 1 / (1 + np.exp(-g[3 * HIDDEN:])))
        c = f * c + i * gg
        return o * np.tanh(c), c

    def logprob(ids):
        h, c = step(np.zeros(HIDDEN), np.zeros(HIDDEN), 3)
        total = 0.0
        for piece in ids + [2]:
            logits = E @ (proj @ h + proj_b)
            total += logits[piece] - (logits.max() + np.log(np.exp(logits - logits.max()).sum()))
            h, c = step(h, c, piece)
        return total

    prior = np.array([0.0, -0.5, -1.0], dtype=np.float32)
    scale, weight, ramp, margin = 1.0, 1.0, 0.0, 0.0
    unk = min(p[1] for p in pieces if p[2] == 1) - 10.0
    OUT.mkdir(exist_ok=True)
    blob = bytearray(b"TKRR") + struct.pack("<5I", 1, vocab, EMBED, HIDDEN, TOP)
    blob += struct.pack("<5f", scale, weight, ramp, margin, unk) + prior.tobytes()
    for array in (emb_q, emb_s, wih_q, wih_s, whh_q, whh_s, bias, proj, proj_b):
        blob += b"\0" * (-len(blob) % 64)
        blob += np.ascontiguousarray(array).tobytes()
    (OUT / "reranker.bin").write_bytes(bytes(blob))
    lines = [f"{i}\t{kind}\t{score:.6f}\t{text}" for i, (text, score, kind) in enumerate(pieces)]
    (OUT / "pieces.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (OUT / "NOTICE").write_text("Random weights for tests only.\n", encoding="utf-8", newline="\n")
    (OUT / "manifest.json").write_text(
        '{\n  "pack_id": "japanese-reranker",\n  "type": "japanese-reranker",\n  "format": "ja-rerank-v1",\n'
        '  "file": "reranker.bin",\n  "license": "TEKITO-OWNED",\n  "notice_file": "NOTICE"\n}\n',
        encoding="utf-8", newline="\n")
    with open(OUT / "expected.tsv", "w", encoding="utf-8", newline="\n") as f:
        for text in TEXTS:
            ids = sp.encode(text)
            f.write(f"{text}\t{' '.join(map(str, ids))}\t{logprob(ids):.5f}\n")
    print(f"{vocab} pieces, {len(blob)} bytes -> {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
