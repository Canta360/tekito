#!/usr/bin/env python3
"""Writes the japanese-reranker Data Pack from a model trained by
train-japanese-wordlm.py: the pieces (pieces.tsv) and the weights
(reranker.bin), int8 where it matters for size.

reranker.bin, little-endian:
  "TKRR", u32 version (1)
  u32 vocab, embed, hidden, top
  f32 scale, weight, ramp_chars, margin, unk_score
  f32 prior[top]
  then, each starting at a multiple of 64 bytes:
  i8  embedding[vocab][embed]     f32 embedding_scale[vocab]
  i8  input_weights[4*hidden][embed]   f32 input_scale[4*hidden]
  i8  hidden_weights[4*hidden][hidden] f32 hidden_scale[4*hidden]
  f32 bias[4*hidden]   (gates i, f, g, o as PyTorch orders them)
  f32 projection[embed][hidden]   f32 projection_bias[embed]
Each int8 row is its float row divided by its scale (max |w| / 127).

pieces.tsv: id, type (1 normal, 2 unknown, 3 control, 4 user, 6 byte),
score, piece; the text is normalized NFKC, spaces are U+2581 and one
leads the text (SentencePiece's nmt_nfkc with a dummy prefix).

  build-japanese-reranker-pack.py --model DIR --out data/ja/japanese-reranker
      [--weight 1.0 --ramp-chars 10 --margin 0.5]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

import numpy as np
import torch

NOTICE = """The language model that checks whole sentences, trained by TEKITO
(scripts/ja/train-japanese-wordlm.py) on text from the zenz-v2.5-dataset
(https://huggingface.co/datasets/Miwa-Keita/zenz-v2.5-dataset), its
llm-jp-corpus-v3 part, which is made available under the Open Data Commons
Attribution License (ODC-BY 1.0): https://opendatacommons.org/licenses/by/1-0/
The pack holds the model's weights and word pieces only; it contains no
sentences from the dataset.
"""


def quantize_rows(matrix: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    scale = np.abs(matrix).max(axis=1) / 127.0
    scale[scale == 0] = 1.0
    q = np.clip(np.rint(matrix / scale[:, None]), -127, 127).astype(np.int8)
    return q, scale.astype(np.float32)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--weight", type=float, default=1.0)
    parser.add_argument("--ramp-chars", type=float, default=10)
    parser.add_argument("--margin", type=float, default=0.5)
    parser.add_argument("--version", default="2026.10")
    args = parser.parse_args()

    from sentencepiece import sentencepiece_model_pb2 as pb
    model_dir, out = Path(args.model), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    proto = pb.ModelProto()
    proto.ParseFromString((model_dir / "pieces.model").read_bytes())
    spec = proto.normalizer_spec
    assert spec.name == "nmt_nfkc" and spec.add_dummy_prefix and spec.escape_whitespaces, spec.name
    assert proto.trainer_spec.byte_fallback and proto.trainer_spec.model_type == 1

    saved = torch.load(model_dir / "finetuned.pt", map_location="cpu")
    state = {k: v.float().numpy() for k, v in saved["model"].items()}
    vocab, embed, hidden = saved["vocab"], saved["embed"], saved["hidden"]
    prior = saved["prior"].float().numpy()
    top = len(prior)
    assert len(proto.pieces) == vocab, (len(proto.pieces), vocab)

    scores = [p.score for p in proto.pieces if p.type == 1]
    unk_score = min(scores) - 10.0  # SentencePiece's unknown penalty
    lines = []
    for i, p in enumerate(proto.pieces):
        assert "\t" not in p.piece and "\n" not in p.piece
        lines.append(f"{i}\t{p.type}\t{p.score:.6f}\t{p.piece}")
    (out / "pieces.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")

    emb_q, emb_s = quantize_rows(state["embed.weight"])
    wih_q, wih_s = quantize_rows(state["lstm.weight_ih_l0"])
    whh_q, whh_s = quantize_rows(state["lstm.weight_hh_l0"])
    bias = (state["lstm.bias_ih_l0"] + state["lstm.bias_hh_l0"]).astype(np.float32)
    proj_w = state["project.weight"].astype(np.float32)
    proj_b = state["project.bias"].astype(np.float32)
    assert proj_w.shape == (embed, hidden) and wih_q.shape == (4 * hidden, embed)

    blob = bytearray(b"TKRR")
    blob += struct.pack("<5I", 1, vocab, embed, hidden, top)
    blob += struct.pack("<5f", saved["scale"], args.weight, args.ramp_chars, args.margin, unk_score)
    blob += prior.astype(np.float32).tobytes()
    for array in (emb_q, emb_s, wih_q, wih_s, whh_q, whh_s, bias, proj_w, proj_b):
        blob += b"\0" * (-len(blob) % 64)
        blob += np.ascontiguousarray(array).tobytes()
    (out / "reranker.bin").write_bytes(bytes(blob))
    (out / "NOTICE").write_text(NOTICE, encoding="utf-8", newline="\n")

    def sha(name: str) -> str:
        return hashlib.sha256((out / name).read_bytes()).hexdigest().upper()

    manifest = {
        "pack_id": "japanese-reranker",
        "display_name": "Japanese Sentence Check",
        "schema_version": 1,
        "version": args.version,
        "language": "ja-JP",
        "type": "japanese-reranker",
        "format": "ja-rerank-v1",
        "file": "reranker.bin",
        "pieces_file": "pieces.tsv",
        "entry_count": vocab,
        "sha256": {"file": sha("reranker.bin"), "pieces": sha("pieces.tsv")},
        "license": "TEKITO-OWNED",
        "source": "scripts/ja/train-japanese-wordlm.py on zenz-v2.5-dataset (ODC-BY 1.0)",
        "notice_file": "NOTICE",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"reranker.bin {len(blob):,} bytes, {vocab} pieces -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
