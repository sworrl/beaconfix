#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Export a trained checkpoint to an end2end ONNX that is a drop-in for BeaconFix's Android PlateDetector
(android/app/src/main/java/org/sworrl/beaconfix/alpr/vision/Models.kt), i.e. the same I/O contract as the current
open-image-models `yolo-v9-t-640-license-plates-end2end.onnx`:

  input  "images"  float32 [1, 3, S, S]       RGB, CHW, 0..1, letterboxed (centred, grey 114/255 padding);
                                              S = 640 (DETECTOR) or 416 (DETECTOR_TILE, tiles/bursts), --size
  output "output0" float32 [N, 7]             one row per kept box: batch, x1, y1, x2, y2, class, score
                                              (input-pixel coordinates, xyxy; score = sigmoid class probability)

with the NMS in the graph (ONNX NonMaxSuppression, per class): at most 100 boxes, IoU 0.45, score >= 0.001 — the
values in the current model's graph. The app then keeps rows with score >= its own minScore (0.4).

The graph: YOLO v9-t main branch (the auxiliary branch is training-only and dropped), BatchNorm folded into the
convolutions and each RepConv's 3x3 + 1x1 branches merged into one 3x3 conv, box decode (anchor ± distances × stride),
then the NMS tail added with onnx.helper.

    python export_onnx.py runs/train/plates-v9t/checkpoints/best.ckpt -o outputs/plates-v9t-640-end2end.onnx
    python export_onnx.py runs/train/plates-v9t-416/checkpoints/best.ckpt --size 416 -o outputs/plates-v9t-416-end2end.onnx
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

import numpy as np
import onnx
import torch
from onnx import TensorProto, helper, numpy_helper
from torch import nn

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "third_party" / "YOLO"))



def load_model(ckpt_path: Path, which: str, class_num: int | None = None):
    from train import checkpoint_weights, compose_cfg
    from yolo.model.yolo import create_model

    cfg = compose_cfg(["task=train_plates", "dataset=oi_plates", "model=v9-t", "weight=False"])
    weights, source = checkpoint_weights(ckpt_path, which)

    head = [k for k in weights if k.endswith("class_conv.2.weight")]
    n_cls = class_num or int(weights[head[0]].shape[0])
    cfg.model.model.auxiliary = {}  # deploy: main branch only (aux layers come last, so indices are unchanged)
    model = create_model(cfg.model, weight_path=False, class_num=n_cls)
    own = model.state_dict()
    missing = [k for k in own if k not in weights]
    if missing:
        raise SystemExit(f"{ckpt_path}: {len(missing)} weights missing, e.g. {missing[:3]}")
    model.load_state_dict({k: weights[k] for k in own})
    return model.eval(), cfg, n_cls, source


def fuse_conv_bn(conv: nn.Conv2d, bn: nn.BatchNorm2d) -> nn.Conv2d:
    fused = nn.Conv2d(conv.in_channels, conv.out_channels, conv.kernel_size, conv.stride, conv.padding,
                      conv.dilation, conv.groups, bias=True)
    std = (bn.running_var + bn.eps).sqrt()
    w = conv.weight * (bn.weight / std).reshape(-1, 1, 1, 1)
    b = bn.bias - bn.running_mean * bn.weight / std
    if conv.bias is not None:
        b = b + conv.bias * bn.weight / std
    fused.weight.data.copy_(w)
    fused.bias.data.copy_(b)
    return fused


class FusedConv(nn.Module):
    def __init__(self, conv: nn.Conv2d, act: nn.Module):
        super().__init__()
        self.conv, self.act = conv, act

    def forward(self, x):
        return self.act(self.conv(x))


@torch.no_grad()
def fuse(model: nn.Module) -> nn.Module:
    """Fold BN into convs, and merge RepConv (3x3 + 1x1, both with BN) into one 3x3 conv."""
    from yolo.model.module import Conv, RepConv

    for name, child in list(model.named_children()):
        if isinstance(child, RepConv):
            c1 = fuse_conv_bn(child.conv1.conv, child.conv1.bn)
            c2 = fuse_conv_bn(child.conv2.conv, child.conv2.bn)
            k = c1.kernel_size[0]
            assert c1.groups == c2.groups == 1 and c2.kernel_size[0] == 1 and k % 2 == 1
            c1.weight.data += nn.functional.pad(c2.weight.data, [k // 2] * 4)
            c1.bias.data += c2.bias.data
            fused = FusedConv(c1, child.act)
            for attr in ("layer_type", "source", "in_c", "out_c", "output", "tags", "external", "usable"):
                if hasattr(child, attr):  # a top-level YOLO layer carries its wiring as attributes
                    setattr(fused, attr, getattr(child, attr))
            setattr(model, name, fused)
        elif isinstance(child, Conv):
            child.conv, child.bn = fuse_conv_bn(child.conv, child.bn), nn.Identity()  # in place: keeps its wiring
        else:
            fuse(child)
    return model


class Decode(nn.Module):
    """Network + box decode: boxes [B, A, 4] xyxy input pixels, scores [B, C, A] (sigmoid)."""

    def __init__(self, model, cfg, size: int):
        super().__init__()
        from yolo.utils.bounding_box_utils import Vec2Box

        self.model = model
        self.vec2box = Vec2Box(model, cfg.model.anchor, [size, size], "cpu")

    def forward(self, images):
        cls, _, boxes = self.vec2box(self.model(images)["Main"])
        return boxes, cls.sigmoid().transpose(1, 2)


def add_nms(m: onnx.ModelProto, max_det: int, iou: float, score: float) -> onnx.ModelProto:
    g = m.graph
    boxes, scores = g.output[0].name, g.output[1].name
    del g.output[:]
    g.initializer.extend([
        numpy_helper.from_array(np.array([max_det], np.int64), "nms_max_output_boxes_per_class"),
        numpy_helper.from_array(np.array([iou], np.float32), "nms_iou_threshold"),
        numpy_helper.from_array(np.array([score], np.float32), "nms_score_threshold"),
        numpy_helper.from_array(np.array([0, 2], np.int64), "idx_batch_box"),
        numpy_helper.from_array(np.array(0, np.int64), "idx_0"),
        numpy_helper.from_array(np.array(1, np.int64), "idx_1"),
        numpy_helper.from_array(np.array([1], np.int64), "axes_1"),
    ])
    n = helper.make_node
    g.node.extend([
        # selected: [K, 3] = (batch, class, box index)
        n("NonMaxSuppression", [boxes, scores, "nms_max_output_boxes_per_class", "nms_iou_threshold",
                                "nms_score_threshold"], ["selected"], center_point_box=0, name="end2end/nms"),
        n("Gather", ["selected", "idx_batch_box"], ["sel_batch_box"], axis=1, name="end2end/batch_box_idx"),
        n("GatherND", [boxes, "sel_batch_box"], ["sel_boxes"], name="end2end/boxes"),          # [K, 4]
        n("GatherND", [scores, "selected"], ["sel_scores"], name="end2end/scores"),             # [K]
        n("Unsqueeze", ["sel_scores", "axes_1"], ["sel_scores_col"], name="end2end/scores_col"),
        n("Gather", ["selected", "idx_0"], ["sel_b"], axis=1, name="end2end/batch_idx"),
        n("Gather", ["selected", "idx_1"], ["sel_c"], axis=1, name="end2end/class_idx"),
        n("Unsqueeze", ["sel_b", "axes_1"], ["sel_b_col"], name="end2end/batch_col"),
        n("Unsqueeze", ["sel_c", "axes_1"], ["sel_c_col"], name="end2end/class_col"),
        n("Cast", ["sel_b_col"], ["sel_b_f"], to=TensorProto.FLOAT, name="end2end/batch_f"),
        n("Cast", ["sel_c_col"], ["sel_c_f"], to=TensorProto.FLOAT, name="end2end/class_f"),
        n("Concat", ["sel_b_f", "sel_boxes", "sel_c_f", "sel_scores_col"], ["output0"], axis=1, name="end2end/rows"),
    ])
    g.output.append(helper.make_tensor_value_info("output0", TensorProto.FLOAT, ["N", 7]))
    return m


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ckpt", type=Path)
    ap.add_argument("-o", "--out", type=Path, default=Path("outputs/plates-v9t-640-end2end.onnx"))
    ap.add_argument("--size", type=int, default=640, help="input size: 640 (DETECTOR) or 416 (DETECTOR_TILE)")
    ap.add_argument("--weights", choices=["ema", "raw"], default="ema")
    ap.add_argument("--max-det", type=int, default=100)
    ap.add_argument("--iou", type=float, default=0.45)
    ap.add_argument("--score", type=float, default=0.001)
    ap.add_argument("--opset", type=int, default=17)
    ap.add_argument("--check-image", type=Path, help="image for the onnx-vs-torch check (default: first val image)")
    args = ap.parse_args()

    model, cfg, n_cls, source = load_model(args.ckpt, args.weights)
    print(f"loaded {args.ckpt} ({source}), {n_cls} class(es), "
          f"{sum(p.numel() for p in model.parameters()) / 1e6:.2f} M parameters")
    SIZE = args.size
    assert SIZE % 32 == 0, "the input size must be a multiple of 32"
    ref = Decode(model, cfg, SIZE).eval()
    x = torch.rand(1, 3, SIZE, SIZE)
    with torch.no_grad():
        rb, rs = ref(x)
        net = Decode(fuse(model), cfg, SIZE).eval()
        fb, fs = net(x)
    print(f"fusion check: max |Δbox| {float((rb - fb).abs().max()):.2e} px, max |Δscore| {float((rs - fs).abs().max()):.2e}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    raw = args.out.with_suffix(".raw.onnx")
    torch.onnx.export(net, x, str(raw), input_names=["images"], output_names=["boxes", "scores"],
                      opset_version=args.opset, do_constant_folding=True, dynamo=False)
    m = add_nms(onnx.load(str(raw)), args.max_det, args.iou, args.score)
    raw.unlink()
    m.producer_name = "beaconfix tools/train-plate-detector"
    for k, v in {
        "model": "YOLOv9-t (MultimediaTechLab/YOLO, MIT), plates only, end2end with NMS",
        "input": f"images float32[1,3,{SIZE},{SIZE}] RGB 0..1 CHW, letterboxed with grey 114",
        "output": "output0 float32[N,7] = batch, x1, y1, x2, y2, class, score (input pixels)",
        "nms": f"max {args.max_det}/class, IoU {args.iou}, score >= {args.score}",
        "training_data": "Open Images V7 'Vehicle registration plate': images CC BY 2.0 (see ATTRIBUTION.csv), "
                         "annotations CC BY 4.0 Google LLC",
        "weights": source,
    }.items():
        m.metadata_props.append(onnx.StringStringEntryProto(key=k, value=v))
    onnx.checker.check_model(m)
    onnx.save(m, str(args.out))

    # the exported graph must reproduce the PyTorch model + the repo's own NMS
    import onnxruntime as ort
    from torchvision.ops import nms

    sess = ort.InferenceSession(str(args.out), providers=["CPUExecutionProvider"])
    i, o = sess.get_inputs()[0], sess.get_outputs()[0]
    print(f"onnxruntime: input {i.name} {i.shape} {i.type}; output {o.name} {o.shape} {o.type}")
    sample = next(iter(sorted((HERE / "data/oi-plates/images/val").glob("*.jpg"))), None) if args.check_image is None \
        else args.check_image
    if sample is not None:
        from PIL import Image

        from eval import letterbox

        with Image.open(sample) as img:
            xs = torch.from_numpy(letterbox(img, SIZE)[0])
        print(f"check input: {sample.name}, letterboxed as the app does")
    else:
        xs = torch.rand(1, 3, SIZE, SIZE) * 0.2 + 0.4
    rows = sess.run(None, {"images": xs.numpy()})[0]
    with torch.no_grad():
        b, s = net(xs)
    s = s[0, 0]
    keep = s >= args.score
    kb, ks = b[0][keep], s[keep]
    ki = nms(kb, ks, args.iou)[: args.max_det]
    assert rows.ndim == 2 and rows.shape[1] == 7, rows.shape
    if len(ki):
        o = rows[np.argsort(-rows[:, 6])]
        t_s, t_b = ks[ki].numpy(), kb[ki].numpy()
        n = min(len(o), len(t_s))
        # as sets (near-equal scores may come out in a different order): every ONNX box has its twin in torch's
        box_gap = np.abs(o[:, None, 1:5] - t_b[None]).max(-1).min(1).max()
        print(f"rows {rows.shape}; torch+torchvision NMS keeps {len(ki)}; "
              f"max |Δscore| {np.abs(o[:n, 6] - t_s[:n]).max():.2e} (sorted), every ONNX box within {box_gap:.2e} px "
              f"of a torch box")
    else:
        print(f"rows {rows.shape} (no box above {args.score} on the check input; expected only for an untrained model)")
    digest = hashlib.sha256(args.out.read_bytes()).hexdigest()
    print(f"wrote {args.out}  {args.out.stat().st_size} bytes  sha256 {digest}")


if __name__ == "__main__":
    main()
