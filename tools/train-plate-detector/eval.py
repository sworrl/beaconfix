#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Score any end2end plate-detector ONNX (BeaconFix's contract: images [1,3,S,S] -> output0 [N,7], S = 640 or 416) on
the sets fetch_data.py writes, with the Android PlateDetector's preprocessing (letterbox to S centred, grey 114, box filter
when shrinking, RGB 0..1, CHW) and its box mapping back to the image.

Sets (any that exist under --data):
  test        Open Images V7 test split, all plate images      -> COCO mAP50-95, mAP50, AP small/medium/large
  test_small  held-out: test images whose plates are all < 32 px wide at 640 (distant plates, dash-cam range)
  test_neg    held-out: test images with cars and a human-verified "no plate" label -> false positives / image
              (an upper bound: a few of these images do show plates or plate-like dealer badges)
Any other set: --sets <name> reads annotations/instances_<name>.json and images/<name>/.
At the app's operating point (--min-score, default 0.4, AlprPipeline's minScore) it also gives precision and recall
(IoU >= 0.5) and the fraction of plate-free images with a false alarm.

    python eval.py models/current-end2end.onnx outputs/plates-v9t-640-end2end.onnx
    python eval.py --sets test --limit 200 model.onnx --json results.json
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image

SETS = {"test": "instances_test.json", "test_small": "instances_test_small.json", "test_neg": "instances_test_neg.json"}
IMAGE_DIR = {"test": "test", "test_small": "test", "test_neg": "test_neg"}


def letterbox(img: Image.Image, SIZE: int):
    """As CropMath.letterbox + PlateDetector.detect: returns the CHW tensor and (r, dw, dh)."""
    w, h = img.size
    r = min(SIZE / w, SIZE / h)
    nw, nh = round(w * r), round(h * r)
    dw, dh = (SIZE - nw) / 2, (SIZE - nh) / 2
    left, top = round(dw - 0.1), round(dh - 0.1)
    filt = Image.Resampling.BOX if r < 1 else Image.Resampling.BILINEAR
    small = np.asarray(img.convert("RGB").resize((nw, nh), filt), dtype=np.float32) / 255.0
    chw = np.full((3, SIZE, SIZE), 114 / 255.0, dtype=np.float32)
    chw[:, top:top + nh, left:left + nw] = small.transpose(2, 0, 1)
    return chw[None], (r, dw, dh)


def iou(a, b):
    x1, y1 = max(a[0], b[0]), max(a[1], b[1])
    x2, y2 = min(a[2], b[2]), min(a[3], b[3])
    inter = max(0.0, x2 - x1) * max(0.0, y2 - y1)
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


def run_model(sess: ort.InferenceSession, gt: dict, img_dir: Path, limit: int):
    name = sess.get_inputs()[0].name
    size = sess.get_inputs()[0].shape[-1]
    dets, times = [], []
    images = gt["images"][:limit] if limit else gt["images"]
    for im in images:
        with Image.open(img_dir / im["file_name"]) as img:
            x, (r, dw, dh) = letterbox(img, size)
        t0 = time.perf_counter()
        rows = sess.run(None, {name: x})[0]
        times.append(time.perf_counter() - t0)
        for row in rows:
            x1, y1, x2, y2 = (row[1] - dw) / r, (row[2] - dh) / r, (row[3] - dw) / r, (row[4] - dh) / r
            x1, x2 = max(0.0, x1), min(float(im["width"]), x2)
            y1, y2 = max(0.0, y1), min(float(im["height"]), y2)
            if x2 - x1 <= 0 or y2 - y1 <= 0:
                continue
            dets.append({"image_id": im["id"], "category_id": gt["categories"][0]["id"],
                         "bbox": [float(x1), float(y1), float(x2 - x1), float(y2 - y1)], "score": float(row[6])})
    return images, dets, times


def coco_map(gt: dict, images: list, dets: list) -> dict:
    from pycocotools.coco import COCO
    from pycocotools.cocoeval import COCOeval

    ids = {im["id"] for im in images}
    sub = dict(gt, images=images, annotations=[a for a in gt["annotations"] if a["image_id"] in ids])
    with contextlib.redirect_stdout(io.StringIO()):
        cg = COCO()
        cg.dataset = sub
        cg.createIndex()
        if not dets:
            return {"mAP50-95": 0.0, "mAP50": 0.0}
        cd = cg.loadRes(dets)
        ev = COCOeval(cg, cd, "bbox")
        ev.params.imgIds = sorted(ids)
        ev.params.maxDets = [1, 10, 100]
        ev.evaluate(); ev.accumulate(); ev.summarize()
    s = ev.stats
    clean = lambda v: None if v < 0 else round(float(v), 4)  # -1: no GT of that size
    return {"mAP50-95": clean(s[0]), "mAP50": clean(s[1]), "mAP75": clean(s[2]),
            "AP_small": clean(s[3]), "AP_medium": clean(s[4]), "AP_large": clean(s[5]), "AR100": clean(s[8])}


def operating_point(gt: dict, images: list, dets: list, min_score: float) -> dict:
    """Greedy IoU >= 0.5 matching of detections >= min_score; group-of (crowd) boxes absorb matches without counting."""
    by_img = {}
    for d in dets:
        if d["score"] >= min_score:
            by_img.setdefault(d["image_id"], []).append(d)
    gts = {}
    for a in gt["annotations"]:
        gts.setdefault(a["image_id"], []).append(a)
    tp = fp = n_gt = 0
    fp_images = 0
    for im in images:
        g = gts.get(im["id"], [])
        boxes = [(a["bbox"][0], a["bbox"][1], a["bbox"][0] + a["bbox"][2], a["bbox"][1] + a["bbox"][3], a["iscrowd"]) for a in g]
        n_gt += sum(1 for b in boxes if not b[4])
        used = set()
        img_fp = 0
        for d in sorted(by_img.get(im["id"], []), key=lambda d: -d["score"]):
            x, y, w, h = d["bbox"]
            db = (x, y, x + w, y + h)
            best, bi = 0.0, -1
            for i, b in enumerate(boxes):
                if i in used and not b[4]:
                    continue
                v = iou(db, b)
                if v > best:
                    best, bi = v, i
            if best >= 0.5:
                if not boxes[bi][4]:
                    used.add(bi)
                    tp += 1
            else:
                fp += 1
                img_fp += 1
        fp_images += img_fp > 0
    return {"min_score": min_score, "tp": tp, "fp": fp, "gt": n_gt,
            "precision": round(tp / (tp + fp), 4) if tp + fp else None,
            "recall": round(tp / n_gt, 4) if n_gt else None,
            "fp_per_image": round(fp / len(images), 4) if images else None,
            "images_with_fp": round(fp_images / len(images), 4) if images else None}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="+", type=Path)
    ap.add_argument("--data", type=Path, default=Path("data/oi-plates"))
    ap.add_argument("--sets", default=",".join(SETS))
    ap.add_argument("--min-score", type=float, default=0.4)
    ap.add_argument("--limit", type=int, default=0, help="first N images of each set (0 = all)")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--json", type=Path)
    args = ap.parse_args()

    so = ort.SessionOptions()
    so.intra_op_num_threads = args.threads
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    results = {}
    for model in args.models:
        sess = ort.InferenceSession(str(model), so, providers=["CPUExecutionProvider"])
        i, o = sess.get_inputs()[0], sess.get_outputs()[0]
        assert len(i.shape) == 4 and i.shape[:2] == [1, 3] and i.shape[2] == i.shape[3] and len(o.shape) == 2 and o.shape[1] == 7, \
            f"{model}: not the end2end contract ({i.shape} -> {o.shape})"
        res = results[str(model)] = {}
        for set_name in [s.strip() for s in args.sets.split(",")]:
            ann = args.data / "annotations" / SETS.get(set_name, f"instances_{set_name}.json")  # any COCO set
            if not ann.exists():
                continue
            gt = json.loads(ann.read_text())
            if not gt["images"]:
                continue
            t0 = time.time()
            images, dets, times = run_model(sess, gt, args.data / "images" / IMAGE_DIR.get(set_name, set_name), args.limit)
            r = {"images": len(images), "boxes": sum(1 for a in gt["annotations"] if a["image_id"] in {im["id"] for im in images}),
                 "ms_per_image": round(1000 * float(np.median(times)), 1)}
            if gt["annotations"]:
                r.update(coco_map(gt, images, dets))
            r["at_min_score"] = operating_point(gt, images, dets, args.min_score)
            res[set_name] = r
            print(f"{model.name:45s} {set_name:10s} {json.dumps(r)}  ({time.time() - t0:.0f} s)", flush=True)

    print()
    print(f"| model | set | images | mAP50 | mAP50-95 | AP_small | P@{args.min_score} | R@{args.min_score} | FP/img@{args.min_score} | ms |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for model, res in results.items():
        for set_name, r in res.items():
            op = r["at_min_score"]
            f = lambda v: "-" if v is None else (f"{v:.3f}" if isinstance(v, float) else str(v))
            print(f"| {Path(model).name} | {set_name} | {r['images']} | {f(r.get('mAP50'))} | {f(r.get('mAP50-95'))} | "
                  f"{f(r.get('AP_small'))} | {f(op['precision'])} | {f(op['recall'])} | {f(op['fp_per_image'])} | {r['ms_per_image']} |")
    if args.json:
        args.json.write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
