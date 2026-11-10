#!/usr/bin/env python3
"""Exports a TEKITO AI model (an LFM2 checkpoint fine-tuned by
train-japanese-ai-model.py) to ONNX for ONNX Runtime / Windows ML, as one
step that reads some tokens and carries the model's state in and out:

  inputs   input_ids [B, T] int64, position_ids [B, T] int64,
           attention_mask [B, P + T] int64 (1 for each past and new token
           that counts), targets [B, T] int64 (the token after each one),
           past_conv.<i> [B, hidden, 2] for each convolution layer,
           past_key.<i>, past_value.<i> [B, kv heads, P, head dim] for
           each attention layer
  outputs  target_logprobs [B, T] (ln p of each target after the tokens up
           to it), last_logprobs [B, vocab] (ln p of every token after the
           last one), present_conv.<i>, present_key.<i>, present_value.<i>

so a prompt is read once and its state copied for each sentence or beam,
and only ln p leaves the model (not the logits of every position).

  export-japanese-ai-onnx.py --model <checkpoint dir> --out <pack dir>
      [--window 128] [--shape 1x64 --shape 10x32] [--block 32]

writes model.onnx (any shapes, for the processor); for a GPU or an NPU,
body-<b>x<t>.onnx (fixed shapes, the state a window of past tokens), which
gives the last hidden state (hidden [B, T, hidden]) in place of ln p, with
head-<rows>.onnx (hidden [rows, hidden], targets [rows] -> target_logprobs
[rows]) for each shape that reads many tokens; and head.onnx (hidden [any,
hidden] -> logprobs [any, vocab], for the processor) for the last tokens.
An NPU compiles the output layer over the whole vocabulary very slowly as
part of a body, quickly on its own. All 4-bit, sharing one model.data; and
the tokenizer as vocab.txt and merges.txt.
"""

from __future__ import annotations

import argparse
import json
import math
import shutil
from pathlib import Path

import torch
import torch.nn.functional as F
from safetensors.torch import load_file

CONV_STATE = 2  # tokens a convolution layer keeps (its kernel less one)


class Step(torch.nn.Module):
    def __init__(self, config: dict, weights: dict[str, torch.Tensor], window: int = 0, body: bool = False):
        super().__init__()
        # body: the last hidden state leaves in place of ln p (a head graph
        # gives ln p from it).
        self.body = body
        # window > 0: the state keeps that many past tokens, the oldest
        # pushed out as new ones come (fixed shapes, for an NPU).
        self.window = window
        self.c = config
        # Weights as parameters, so the export keeps each as an initializer.
        # The embedding is read twice: as a table of half floats for the
        # tokens read, and as the output layer ([hidden, vocab]), which then
        # quantizes like every other matrix.
        for name, value in weights.items():
            if name == "model.embed_tokens.weight":
                self.register_parameter("embed", torch.nn.Parameter(value.half(), requires_grad=False))
                name, value = "output.weight", value.t().contiguous()
            self.register_parameter(name.replace(".", "__"), torch.nn.Parameter(value.float(), requires_grad=False))
        self.hidden = config["hidden_size"]
        self.heads = config["num_attention_heads"]
        self.kv_heads = config["num_key_value_heads"]
        self.head_dim = self.hidden // self.heads
        self.eps = config["norm_eps"]
        self.types = config["layer_types"]
        theta = config.get("rope_parameters", {}).get("rope_theta", config.get("rope_theta", 1e6))
        self.register_buffer("inv_freq", 1.0 / (theta ** (torch.arange(0, self.head_dim, 2).float() / self.head_dim)),
                             persistent=False)
        self.conv_layers = [i for i, t in enumerate(self.types) if t == "conv"]
        self.attn_layers = [i for i, t in enumerate(self.types) if t == "full_attention"]

    def p(self, name: str) -> torch.Tensor:
        return getattr(self, name.replace(".", "__"))

    @staticmethod
    def lin(x, weight):
        # A matrix product over rows ([B * T, in]): an NPU cannot run it on
        # [B, 1, in] with B > 1 (beams reading one token each).
        lead = x.shape[:-1]
        return F.linear(x.reshape(-1, x.shape[-1]), weight).reshape(*lead, weight.shape[0])

    def norm(self, x, weight):
        return weight * (x * torch.rsqrt(x.pow(2).mean(-1, keepdim=True) + self.eps))

    def forward(self, input_ids, position_ids, attention_mask, targets, *states):
        n_conv = len(self.conv_layers)
        convs = list(states[:n_conv])
        keys = list(states[n_conv:n_conv + 2 * len(self.attn_layers):2])
        values = list(states[n_conv + 1:n_conv + 2 * len(self.attn_layers):2])
        B, T = input_ids.shape
        out_w = self.p("output.weight")
        x = F.embedding(input_ids, self.embed).float()
        freqs = position_ids[..., None].float() * self.inv_freq
        emb = torch.cat((freqs, freqs), dim=-1)
        cos, sin = emb.cos()[:, None], emb.sin()[:, None]  # [B, 1, T, D]
        # Attention: past keys where the mask says so, new ones causally.
        past = attention_mask.shape[1] - T
        new = torch.tril(torch.ones(T, T, dtype=torch.bool))
        allowed = torch.cat([torch.ones(T, past, dtype=torch.bool), new], dim=1)[None, None]
        allowed = allowed & (attention_mask[:, None, None, :] > 0)
        # Each token sees itself at least (padding too), and what it may not
        # see weighs as little as half precision can hold.
        place = torch.arange(T)
        itself = torch.cat([torch.zeros(T, past, dtype=torch.bool), place[:, None] == place[None, :]], dim=1)[None, None]
        bias = torch.where(allowed | itself, 0.0, -1e4)
        new_mask = attention_mask[:, past:].to(x.dtype)
        present = []
        ci = ai = 0
        for i, kind in enumerate(self.types):
            pre = f"model.layers.{i}."
            h = self.norm(x, self.p(pre + "operator_norm.weight"))
            if kind == "conv":
                # Padding reads as nothing (as transformers' LFM2 does), so a
                # prompt padded at its start carries the right state.
                h = h * new_mask[..., None]
                bcx = self.lin(h, self.p(pre + "conv.in_proj.weight")).transpose(1, 2)
                b, c, v = bcx.chunk(3, dim=1)
                bx = b * v  # [B, H, T]
                full = torch.cat([convs[ci], bx], dim=2)  # [B, H, 2 + T]
                w = self.p(pre + "conv.conv.weight")  # [H, 1, 3]
                # The short convolution as its three taps (each token and the
                # two before it), which every device runs as it is.
                out = (w[None, :, 0, 0:1] * full[:, :, 0:T] + w[None, :, 0, 1:2] * full[:, :, 1:T + 1] +
                       w[None, :, 0, 2:3] * full[:, :, 2:T + 2])  # [B, H, T]
                present.append(("conv", ci, full[:, :, -CONV_STATE:]))
                y = (c * out).transpose(1, 2)
                h = self.lin(y, self.p(pre + "conv.out_proj.weight"))
                ci += 1
            else:
                q = self.lin(h, self.p(pre + "self_attn.q_proj.weight")).view(B, T, self.heads, self.head_dim)
                k = self.lin(h, self.p(pre + "self_attn.k_proj.weight")).view(B, T, self.kv_heads, self.head_dim)
                v = self.lin(h, self.p(pre + "self_attn.v_proj.weight")).view(B, T, self.kv_heads, self.head_dim)
                q = self.norm(q, self.p(pre + "self_attn.q_layernorm.weight")).transpose(1, 2)
                k = self.norm(k, self.p(pre + "self_attn.k_layernorm.weight")).transpose(1, 2)
                v = v.transpose(1, 2)

                def rot(t):
                    half = t.shape[-1] // 2
                    return torch.cat((-t[..., half:], t[..., :half]), dim=-1)

                q = q * cos + rot(q) * sin
                k = k * cos + rot(k) * sin
                k = torch.cat([keys[ai], k], dim=2)
                v = torch.cat([values[ai], v], dim=2)
                present.append(("key", ai, k[:, :, T:] if self.window else k))
                present.append(("value", ai, v[:, :, T:] if self.window else v))
                rep = self.heads // self.kv_heads
                kk = k.repeat_interleave(rep, dim=1)
                vv = v.repeat_interleave(rep, dim=1)
                scores = torch.matmul(q, kk.transpose(2, 3)) / math.sqrt(self.head_dim) + bias
                attn = torch.matmul(torch.softmax(scores, dim=-1), vv)
                h = self.lin(attn.transpose(1, 2).reshape(B, T, self.hidden), self.p(pre + "self_attn.out_proj.weight"))
                ai += 1
            x = x + h
            g = self.norm(x, self.p(pre + "ffn_norm.weight"))
            x = x + self.lin(F.silu(self.lin(g, self.p(pre + "feed_forward.w1.weight"))) *
                             self.lin(g, self.p(pre + "feed_forward.w3.weight")),
                             self.p(pre + "feed_forward.w2.weight"))
        x = self.norm(x, self.p("model.embedding_norm.weight"))
        conv_out = [t for kind, _, t in present if kind == "conv"]
        kv_out = [t for kind, _, t in present if kind != "conv"]
        if self.body:
            return (x, *conv_out, *kv_out)
        logits = torch.matmul(x, out_w)
        # ln p as logit less the log of the sum over the vocabulary (an NPU
        # cannot hold a log-softmax over it).
        top = logits.max(dim=-1, keepdim=True).values
        total = (top + torch.log(torch.exp(logits - top).sum(dim=-1, keepdim=True))).squeeze(-1)
        target_logprobs = logits.gather(2, targets.unsqueeze(-1)).squeeze(-1) - total
        last_logprobs = logits[:, -1, :] - total[:, -1:]
        return (target_logprobs, last_logprobs, *conv_out, *kv_out)


class TargetHead(torch.nn.Module):
    """ln p of each row's target from its hidden state (a body's output)."""

    def __init__(self, step: Step):
        super().__init__()
        self.step = step

    def forward(self, hidden, targets):
        logits = torch.matmul(hidden, self.step.p("output.weight"))
        top = logits.max(dim=-1, keepdim=True).values
        total = (top + torch.log(torch.exp(logits - top).sum(dim=-1, keepdim=True))).squeeze(-1)
        return logits.gather(1, targets.unsqueeze(-1)).squeeze(-1) - total


class LastHead(torch.nn.Module):
    """ln p of every token after each row's hidden state."""

    def __init__(self, step: Step):
        super().__init__()
        self.step = step

    def forward(self, hidden):
        return torch.log_softmax(torch.matmul(hidden, self.step.p("output.weight")), dim=-1)


def names(step: Step) -> tuple[list[str], list[str]]:
    ins = ["input_ids", "position_ids", "attention_mask", "targets"]
    outs = ["target_logprobs", "last_logprobs"]
    ins += [f"past_conv.{i}" for i in range(len(step.conv_layers))]
    outs += [f"present_conv.{i}" for i in range(len(step.conv_layers))]
    for i in range(len(step.attn_layers)):
        ins += [f"past_key.{i}", f"past_value.{i}"]
        outs += [f"present_key.{i}", f"present_value.{i}"]
    return ins, outs


def empty_states(step: Step, batch: int):
    s = [torch.zeros(batch, step.hidden, CONV_STATE) for _ in step.conv_layers]
    for _ in step.attn_layers:
        s += [torch.zeros(batch, step.kv_heads, 0, step.head_dim), torch.zeros(batch, step.kv_heads, 0, step.head_dim)]
    return s


def quantize(path: Path, block: int) -> None:
    """Every matrix of the graph at `path` as 4-bit blocks (MatMulNBits)."""
    import onnx
    from onnxruntime.quantization.matmul_nbits_quantizer import MatMulNBitsQuantizer
    model = onnx.load(str(path))
    q = MatMulNBitsQuantizer(model, block_size=block, is_symmetric=True, accuracy_level=4)
    q.process()
    onnx.save(q.model.model, str(path))


def share_weights(graphs: list[Path], data: Path) -> None:
    """The graphs' weights in one file `data`, each once (graphs that hold
    the same weights point to the same bytes)."""
    import hashlib
    import onnx
    from onnx.external_data_helper import set_external_data
    stored: dict[str, tuple[int, int]] = {}
    with open(data, "wb") as out:
        for path in graphs:
            model = onnx.load(str(path))
            for tensor in model.graph.initializer:
                raw = tensor.raw_data
                if len(raw) < 1024:
                    continue
                key = hashlib.sha1(raw + str(tensor.data_type).encode() + str(list(tensor.dims)).encode()).hexdigest()
                if key not in stored:
                    out.write(b"\0" * (-out.tell() % 4096))
                    stored[key] = (out.tell(), len(raw))
                    out.write(raw)
                offset, length = stored[key]
                set_external_data(tensor, location=data.name, offset=offset, length=length)
                tensor.ClearField("raw_data")
                tensor.data_location = onnx.TensorProto.EXTERNAL
            onnx.save(model, str(path))


def export_pack(config: dict, weights: dict, model_dir: Path, out: Path, args) -> int:
    """An AI model pack's model: model.onnx (any shapes, for the processor),
    fixed-shape bodies body-<batch>x<tokens>.onnx with their heads (for a
    GPU or an NPU), 4-bit, over one file of weights; and the tokenizer's
    lists."""
    import onnx
    work = out / "work"
    work.mkdir(parents=True, exist_ok=True)
    graphs = []
    step = Step(config, weights).eval()
    ins, outs = names(step)
    export_dynamic(step, ins, outs, work / "model.onnx", args.opset)
    graphs.append(work / "model.onnx")
    fixed = Step(config, weights, args.window, body=True).eval()
    body_outs = ["hidden"] + outs[2:]
    for shape in args.shape:
        graphs.append(export_shape(fixed, ins, body_outs, work, shape, args.window, args.opset))
        b, t = (int(x) for x in shape.lower().split("x"))
        if t > 1 and b > 1:
            graphs.append(export_target_head(step, work, b * t, args.opset))
    graphs.append(export_last_head(step, work, args.opset))
    for g in graphs:
        quantize(g, args.block)
        print("quantized", g.name, flush=True)
    finals = []
    for g in graphs:
        final = out / g.name
        onnx.save(onnx.load(str(g)), str(final))
        finals.append(final)
    share_weights(finals, out / "model.data")
    shutil.rmtree(work)
    write_tokenizer(model_dir / "tokenizer.json", out)
    print("pack model written to", out)
    return 0


def write_tokenizer(tokenizer: Path, out: Path) -> None:
    """vocab.txt (one token per line in id order) and merges.txt (one pair
    per line in rank order), as TekitoAI's ByteBpe reads them."""
    t = json.loads(tokenizer.read_text(encoding="utf-8"))
    vocab = dict(t["model"]["vocab"])
    for added in t["added_tokens"]:
        vocab[added["content"]] = added["id"]
    rows = [""] * (max(vocab.values()) + 1)
    for token, i in vocab.items():
        rows[i] = token
    assert all("\n" not in r and "\r" not in r for r in rows)
    (out / "vocab.txt").write_text("\n".join(rows) + "\n", encoding="utf-8", newline="\n")
    merges = [m if isinstance(m, str) else " ".join(m) for m in t["model"]["merges"]]
    merges_text = "\n".join(merges) + "\n"
    (out / "merges.txt").write_text(merges_text, encoding="utf-8", newline="\n")


def export_dynamic(step: Step, ins, outs, path: Path, opset: int) -> Path:
    """The step with any batch, tokens and past (for the processor)."""
    B, T, P = 2, 3, 4
    sample = [torch.randint(0, 1000, (B, T)), torch.arange(P, P + T).repeat(B, 1),
              torch.ones(B, P + T, dtype=torch.int64), torch.randint(0, 1000, (B, T))]
    states = [torch.zeros(B, step.hidden, CONV_STATE) for _ in step.conv_layers]
    for _ in step.attn_layers:
        states += [torch.zeros(B, step.kv_heads, P, step.head_dim), torch.zeros(B, step.kv_heads, P, step.head_dim)]
    dynamic = {"input_ids": {0: "batch", 1: "tokens"}, "position_ids": {0: "batch", 1: "tokens"},
               "attention_mask": {0: "batch", 1: "all"}, "targets": {0: "batch", 1: "tokens"},
               "target_logprobs": {0: "batch", 1: "tokens"}, "last_logprobs": {0: "batch"}}
    for n in ins[4:] + outs[2:]:
        dynamic[n] = {0: "batch"} if "conv" in n else {0: "batch", 2: "past" if n.startswith("past") else "all"}
    with torch.no_grad():
        torch.onnx.export(step, tuple(sample + states), str(path), input_names=ins, output_names=outs,
                          dynamic_axes=dynamic, opset_version=opset, dynamo=False)
    return path


def export_target_head(step: Step, directory: Path, rows: int, opset: int) -> Path:
    """head-<rows>.onnx: ln p of each of `rows` targets (fixed, for a GPU or an NPU)."""
    path = directory / f"head-{rows}.onnx"
    with torch.no_grad():
        torch.onnx.export(TargetHead(step).eval(), (torch.zeros(rows, step.hidden), torch.zeros(rows, dtype=torch.int64)),
                          str(path), input_names=["hidden", "targets"], output_names=["target_logprobs"],
                          opset_version=opset, dynamo=False)
    return path


def export_last_head(step: Step, directory: Path, opset: int) -> Path:
    """head.onnx: ln p of every token after any number of hidden states."""
    path = directory / "head.onnx"
    with torch.no_grad():
        torch.onnx.export(LastHead(step).eval(), (torch.zeros(2, step.hidden),), str(path), input_names=["hidden"],
                          output_names=["logprobs"], dynamic_axes={"hidden": {0: "rows"}, "logprobs": {0: "rows"}},
                          opset_version=opset, dynamo=False)
    return path


def export_shape(step: Step, ins, outs, directory: Path, shape: str, window: int, opset: int) -> Path:
    """The body (with a window) for one fixed batch x tokens: body-<b>x<t>.onnx."""
    B, T = (int(x) for x in shape.lower().split("x"))
    sample = [torch.zeros(B, T, dtype=torch.int64), torch.zeros(B, T, dtype=torch.int64),
              torch.ones(B, window + T, dtype=torch.int64), torch.zeros(B, T, dtype=torch.int64)]
    states = [torch.zeros(B, step.hidden, CONV_STATE) for _ in step.conv_layers]
    for _ in step.attn_layers:
        states += [torch.zeros(B, step.kv_heads, window, step.head_dim)] * 2
    path = directory / f"body-{B}x{T}.onnx"
    with torch.no_grad():
        torch.onnx.export(step, tuple(sample + states), str(path), input_names=ins, output_names=outs,
                          opset_version=opset, dynamo=False)
    return path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--opset", type=int, default=17)
    # Fixed shapes for a GPU or an NPU: the past the state keeps, and a graph
    # for each batch x tokens.
    ap.add_argument("--window", type=int, default=128)
    ap.add_argument("--shape", action="append", default=[], help="batch x tokens, e.g. 10x32")
    ap.add_argument("--block", type=int, default=32, help="weights quantized in blocks of this many")
    args = ap.parse_args()
    if not args.shape:
        args.shape = ["1x64", "10x32"]
    model_dir, out = Path(args.model), Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    config = json.loads((model_dir / "config.json").read_text(encoding="utf-8"))
    return export_pack(config, load_file(model_dir / "model.safetensors"), model_dir, out, args)


if __name__ == "__main__":
    raise SystemExit(main())
