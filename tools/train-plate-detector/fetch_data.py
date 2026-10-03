#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Download the Open Images V7 "Vehicle registration plate" boxes and only the images that carry them, and write a
COCO-format dataset the MIT YOLO code (MultimediaTechLab/YOLO) reads directly:

    <out>/images/{train,val,test}/<ImageID>.jpg
    <out>/annotations/instances_{train,val,test}.json        boxes in stored-image pixels, group-of boxes as iscrowd=1
    <out>/annotations/instances_test_small.json             held-out: test images whose plates are all < 32 px wide
                                                            once the image is letterboxed to 640 (dash-cam distances)
    <out>/images/test_neg/ + annotations/instances_test_neg.json
                                                            held-out: test images with cars that a human verified to
                                                            contain NO plate (false positives per image)
    <out>/ATTRIBUTION.csv                                   every stored image: license, author, title, source URLs
    <out>/ATTRIBUTION.md                                    license tally + the notices to ship with a model
    <out>/manifest.json                                     sources, counts, what was skipped and why

Licenses (Open Images V7, https://storage.googleapis.com/openimages/web/factsfigures_v7.html):
  annotations: CC BY 4.0, Google LLC; images: "listed as having a CC BY 2.0 license" (Google makes no warranty and
  asks users to verify each image, which is why every image's own License/Author columns are copied to the manifest
  and any image whose listed license is not CC BY is dropped).

Sources: the official CSVs (box annotations, image IDs with license/author/rotation, human-verified image labels)
from storage.googleapis.com/openimages, images from the CVDF bucket open-images-dataset.s3.amazonaws.com. The 2.3 GB
train box CSV and 640 MB train image list are streamed and filtered on the fly; only the matching rows are kept
(under <out>/../raw/), so a re-run does not download them again.

    python fetch_data.py                       # everything (~8-9k images, ~2-3 GB at --max-side 1280)
    python fetch_data.py --limit 200           # at most 200 images per split (smoke tests)
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import random
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path

import requests
from PIL import Image

GCS = "https://storage.googleapis.com/openimages"
SOURCES = {
    "classes": f"{GCS}/v7/oidv7-class-descriptions-boxable.csv",
    "boxes": {
        "train": f"{GCS}/v6/oidv6-train-annotations-bbox.csv",
        "validation": f"{GCS}/v5/validation-annotations-bbox.csv",
        "test": f"{GCS}/v5/test-annotations-bbox.csv",
    },
    "images": {
        "train": f"{GCS}/2018_04/train/train-images-boxable-with-rotation.csv",
        "validation": f"{GCS}/2018_04/validation/validation-images-with-rotation.csv",
        "test": f"{GCS}/2018_04/test/test-images-with-rotation.csv",
    },
    "test_imagelabels": f"{GCS}/v5/test-annotations-human-imagelabels-boxable.csv",
    "image_bucket": "https://open-images-dataset.s3.amazonaws.com/{split}/{image_id}.jpg",
}
SPLIT_DIR = {"train": "train", "validation": "val", "test": "test"}
PLATE = "Vehicle registration plate"
CAR = "/m/0k4j"
ANNOTATION_LICENSE = "CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/), Google LLC"
SMALL_PX = 32          # "small" plate: narrower than this once the image is letterboxed to 640
LETTERBOX = 640

session = requests.Session()
session.headers["User-Agent"] = "beaconfix-train-plate-detector/1.0"


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def stream_lines(url: str):
    """The lines of a remote CSV, streamed (never held in memory or on disk whole)."""
    for attempt in range(5):
        try:
            with session.get(url, stream=True, timeout=60) as r:
                r.raise_for_status()
                total = int(r.headers.get("content-length", 0))
                done, last = 0, time.time()
                buf = b""
                for chunk in r.iter_content(chunk_size=4 << 20):
                    done += len(chunk)
                    buf += chunk
                    *lines, buf = buf.split(b"\n")
                    for line in lines:
                        yield line.decode("utf-8")
                    if time.time() - last > 15 and total:
                        log(f"  {url.rsplit('/', 1)[-1]}: {done / total:5.1%} of {total / 1e6:.0f} MB")
                        last = time.time()
                if buf:
                    yield buf.decode("utf-8")
                return
        except (requests.RequestException, ConnectionError) as e:
            if attempt == 4:
                raise
            log(f"  retry {url}: {e}")
            time.sleep(5 * (attempt + 1))


def filtered_csv(url: str, cache: Path, keep) -> list[dict]:
    """Rows of the CSV at [url] for which keep(line) is true, cached in [cache]."""
    if not cache.exists():
        log(f"streaming {url}")
        tmp = cache.with_suffix(".part")
        n = 0
        with open(tmp, "w", newline="") as f:
            it = stream_lines(url)
            header = next(it)
            f.write(header.rstrip("\r") + "\n")
            for line in it:
                if line and keep(line):
                    f.write(line.rstrip("\r") + "\n")
                    n += 1
        tmp.rename(cache)
        log(f"  kept {n} rows -> {cache}")
    with open(cache, newline="") as f:
        return list(csv.DictReader(f))


def class_mids(names: list[str]) -> dict[str, str]:
    rows = list(csv.reader(io.StringIO(session.get(SOURCES["classes"], timeout=60).text)))
    by_name = {name: mid for mid, name in rows}
    missing = [n for n in names if n not in by_name]
    if missing:
        sys.exit(f"unknown Open Images classes: {missing}")
    return {by_name[n]: n for n in names}


def download_image(split: str, image_id: str, dest: Path, max_side: int) -> tuple[str, str | None, tuple[int, int] | None]:
    """Fetch one image; returns (id, error or None, stored (w, h))."""
    if dest.exists():
        try:
            with Image.open(dest) as im:
                return image_id, None, im.size
        except Exception:
            dest.unlink()
    url = SOURCES["image_bucket"].format(split=split, image_id=image_id)
    for attempt in range(4):
        try:
            r = session.get(url, timeout=60)
            if r.status_code == 404:
                return image_id, "missing from bucket", None
            r.raise_for_status()
            data = r.content
            break
        except requests.RequestException as e:
            if attempt == 3:
                return image_id, f"download failed: {e}", None
            time.sleep(2 * (attempt + 1))
    try:
        im = Image.open(io.BytesIO(data))
        im.load()
    except Exception as e:
        return image_id, f"undecodable: {e}", None
    orientation = im.getexif().get(0x0112, 1)
    if orientation not in (0, 1):
        # boxes were drawn on the displayed image; the YOLO loader (and the app) ignore EXIF orientation
        return image_id, f"EXIF orientation {orientation}", None
    w, h = im.size
    tmp = dest.with_suffix(".part")
    if max_side and max(w, h) > max_side:
        s = max_side / max(w, h)
        im = im.convert("RGB").resize((round(w * s), round(h * s)), Image.Resampling.LANCZOS)
        im.save(tmp, "JPEG", quality=94)
    elif im.mode not in ("RGB", "L"):
        im.convert("RGB").save(tmp, "JPEG", quality=95)
    else:
        tmp.write_bytes(data)
    tmp.rename(dest)
    return image_id, None, im.size


def fetch_images(split: str, ids: list[str], img_dir: Path, max_side: int, workers: int) -> tuple[dict, dict]:
    img_dir.mkdir(parents=True, exist_ok=True)
    sizes, skipped = {}, {}
    t0 = time.time()
    with ThreadPoolExecutor(workers) as pool:
        futs = [pool.submit(download_image, split, i, img_dir / f"{i}.jpg", max_side) for i in ids]
        for n, fut in enumerate(as_completed(futs), 1):
            image_id, err, size = fut.result()
            if err:
                skipped[image_id] = err
            else:
                sizes[image_id] = size
            if n % 500 == 0 or n == len(ids):
                log(f"  {split}: {n}/{len(ids)} images ({time.time() - t0:.0f} s, {len(skipped)} skipped)")
    return sizes, skipped


def coco_json(images: list[str], sizes: dict, boxes: dict, categories: dict[str, int], split_dir: str) -> dict:
    out = {
        "info": {"description": f"Open Images V7 {PLATE} ({split_dir})", "annotations_license": ANNOTATION_LICENSE,
                 "date_created": datetime.now(timezone.utc).isoformat()},
        "licenses": [],
        "categories": [{"id": cid, "name": name} for name, cid in categories.items()],
        "images": [], "annotations": [],
    }
    ann_id = 1
    for num, image_id in enumerate(images, 1):
        w, h = sizes[image_id]
        out["images"].append({"id": num, "file_name": f"{image_id}.jpg", "width": w, "height": h})
        for b in boxes.get(image_id, []):
            x1, x2 = float(b["XMin"]) * w, float(b["XMax"]) * w
            y1, y2 = float(b["YMin"]) * h, float(b["YMax"]) * h
            if x2 - x1 < 1 or y2 - y1 < 1:
                continue
            out["annotations"].append({
                "id": ann_id, "image_id": num, "category_id": categories[b["_class"]],
                "bbox": [round(x1, 2), round(y1, 2), round(x2 - x1, 2), round(y2 - y1, 2)],
                "area": round((x2 - x1) * (y2 - y1), 2),
                "iscrowd": int(b["IsGroupOf"] == "1"),
                "oi": {k: int(b[k]) for k in ("IsOccluded", "IsTruncated", "IsDepiction", "IsInside") if b[k] in ("0", "1")},
            })
            ann_id += 1
    return out


def is_cc_by(license_url: str) -> bool:
    u = license_url.lower()
    return "creativecommons.org/licenses/by/" in u  # CC BY only: no -nc, -nd, -sa variants


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=Path("data/oi-plates"))
    ap.add_argument("--limit", type=int, default=0, help="at most this many images per split (0 = all)")
    ap.add_argument("--splits", default="train,validation,test")
    ap.add_argument("--classes", default=PLATE,
                    help="';'-separated Open Images class names (default: plates only; more classes make a pretraining set)")
    ap.add_argument("--max-side", type=int, default=1280, help="downscale images whose long side is larger (0 = keep)")
    ap.add_argument("--negatives", type=int, default=300, help="held-out no-plate test images with cars (0 = none)")
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    names = [n.strip() for n in args.classes.split(";") if n.strip()]
    mids = class_mids(names)
    categories = {name: i for i, name in enumerate(names, 1)}
    raw = args.out.parent / "raw"
    raw.mkdir(parents=True, exist_ok=True)
    key = hashlib.sha1(",".join(sorted(mids)).encode()).hexdigest()[:8]
    needles = tuple(f",{m}," for m in mids)
    want_car = "test" in args.splits and args.negatives > 0
    rng = random.Random(args.seed)

    manifest = {
        "created": datetime.now(timezone.utc).isoformat(), "classes": {m: n for m, n in mids.items()},
        "annotation_license": ANNOTATION_LICENSE,
        "image_license_note": "Open Images lists its images as CC BY 2.0 and asks users to verify each image; "
                              "every image's License/Author is in ATTRIBUTION.csv, non-CC-BY images are dropped.",
        "sources": SOURCES, "args": {k: str(v) for k, v in vars(args).items()}, "splits": {},
    }
    attribution = []

    for split in [s.strip() for s in args.splits.split(",")]:
        sdir = SPLIT_DIR[split]
        log(f"== {split}")
        box_needles = needles + ((f",{CAR},",) if split == "test" and want_car else ())
        rows = filtered_csv(SOURCES["boxes"][split], raw / f"{split}-boxes-{key}{'-car' if split == 'test' and want_car else ''}.csv",
                            lambda line: any(n in line for n in box_needles))
        boxes = defaultdict(list)
        car_images = set()
        for r in rows:
            if r["LabelName"] in mids:
                r["_class"] = mids[r["LabelName"]]
                boxes[r["ImageID"]].append(r)
            elif r["LabelName"] == CAR:
                car_images.add(r["ImageID"])
        positives = sorted(boxes)

        negatives = []
        if split == "test" and want_car:
            plate_mid = next((m for m, n in mids.items() if n == PLATE), None)
            labels = filtered_csv(SOURCES["test_imagelabels"], raw / "test-imagelabels-plate.csv",
                                  lambda line: f",{plate_mid}," in line) if plate_mid else []
            verified_no_plate = {r["ImageID"] for r in labels if r["Confidence"] == "0"}
            negatives = sorted((verified_no_plate & car_images) - set(boxes))
            manifest["negatives_pool"] = len(negatives)

        wanted = set(positives) | set(negatives)
        meta_rows = filtered_csv(SOURCES["images"][split], raw / f"{split}-images-{key}{'-car' if split == 'test' and want_car else ''}.csv",
                                 lambda line, w=wanted: line[:16] in w)
        meta = {r["ImageID"]: r for r in meta_rows}

        skipped = Counter()
        def usable(i):
            m = meta.get(i)
            if m is None:
                skipped["no image metadata"] += 1; return False
            if m.get("Rotation", "") not in ("", "0", "0.0"):
                skipped["needs rotation"] += 1; return False
            if not is_cc_by(m.get("License", "")):
                skipped[f"license {m.get('License')}"] += 1; return False
            return True
        positives = [i for i in positives if usable(i)]
        negatives = [i for i in negatives if usable(i)]
        if args.limit:
            positives = sorted(rng.sample(positives, min(args.limit, len(positives))))
        if negatives:
            n_neg = min(args.negatives, args.limit or args.negatives, len(negatives))
            negatives = sorted(rng.sample(negatives, n_neg))

        log(f"  {len(positives)} images with boxes ({sum(len(boxes[i]) for i in positives)}) to fetch")
        sizes, failed = fetch_images(split, positives, args.out / "images" / sdir, args.max_side, args.workers)
        for err in failed.values():
            skipped[err.split(":")[0]] += 1
        positives = [i for i in positives if i in sizes]
        (args.out / "annotations").mkdir(parents=True, exist_ok=True)
        coco = coco_json(positives, sizes, boxes, categories, sdir)
        (args.out / "annotations" / f"instances_{sdir}.json").write_text(json.dumps(coco))
        info = {"images": len(positives), "boxes": len(coco["annotations"]),
                "group_of_boxes": sum(a["iscrowd"] for a in coco["annotations"]), "skipped": dict(skipped)}

        for i in positives:
            attribution.append((sdir, i, meta[i]))

        if split == "test":
            # held-out 1: images whose plates are all small at the detector's input scale
            small = []
            for i in positives:
                w, h = sizes[i]
                s = LETTERBOX / max(w, h)
                widths = [(float(b["XMax"]) - float(b["XMin"])) * w * s for b in boxes[i] if b["IsGroupOf"] != "1"]
                if widths and max(widths) < SMALL_PX:
                    small.append(i)
            small_coco = coco_json(small, sizes, boxes, categories, "test")
            (args.out / "annotations" / "instances_test_small.json").write_text(json.dumps(small_coco))
            info["small_subset"] = {"images": len(small), "boxes": len(small_coco["annotations"]),
                                    "rule": f"every non-group plate < {SMALL_PX} px wide after letterboxing to {LETTERBOX}"}
            # held-out 2: verified plate-free images with cars
            if negatives:
                nsizes, nfailed = fetch_images(split, negatives, args.out / "images" / "test_neg", args.max_side, args.workers)
                negatives = [i for i in negatives if i in nsizes]
                neg = coco_json(negatives, nsizes, {}, categories, "test_neg")
                (args.out / "annotations" / "instances_test_neg.json").write_text(json.dumps(neg))
                info["negatives"] = {"images": len(negatives), "skipped": len(nfailed),
                                     "rule": "a Car box, and a human-verified 'no Vehicle registration plate' image label"}
                for i in negatives:
                    attribution.append(("test_neg", i, meta[i]))

        manifest["splits"][sdir] = info
        log(f"  {sdir}: {info}")

    # attribution manifest: every stored image, its license and author, as Open Images lists them
    cols = ["split", "image_id", "file", "license", "author", "author_profile_url", "title", "landing_url",
            "original_url", "original_md5"]
    with open(args.out / "ATTRIBUTION.csv", "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(cols)
        for sdir, i, m in sorted(attribution, key=lambda t: (t[0], t[1])):
            wr.writerow([sdir, i, f"images/{sdir}/{i}.jpg", m.get("License", ""), m.get("Author", ""),
                         m.get("AuthorProfileURL", ""), m.get("Title", ""), m.get("OriginalLandingURL", ""),
                         m.get("OriginalURL", ""), m.get("OriginalMD5", "")])
    tally = Counter((sdir, m.get("License", "")) for sdir, _, m in attribution)
    manifest["license_tally"] = {f"{s} {lic}": n for (s, lic), n in sorted(tally.items())}
    md = [
        "# Training data attribution", "",
        f"Images: Open Images V7, class(es) {', '.join(names)}. Each image is used under the license Open Images lists",
        "for it (all CC BY; any other license is dropped by fetch_data.py). Per-image author, title and source URL:",
        "`ATTRIBUTION.csv` next to this file.", "",
        f"Annotations: Open Images V7 bounding boxes, {ANNOTATION_LICENSE}. Converted to COCO JSON (no other change).",
        "",
        "Open Images states: \"The annotations are licensed by Google LLC under CC BY 4.0 license. The images are listed",
        "as having a CC BY 2.0 license. Note: while we tried to identify images that are licensed under a Creative",
        "Commons Attribution license, we make no representations or warranties regarding the license status of each",
        "image and you should verify the license for each image yourself.\"", "",
        "| split | license | images |", "|---|---|---|",
        *[f"| {s} | {lic} | {n} |" for (s, lic), n in sorted(tally.items())], "",
    ]
    (args.out / "ATTRIBUTION.md").write_text("\n".join(md))
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2))
    for stale in args.out.glob("*.pache"):  # the YOLO loader's file-list cache would hide a changed image set
        stale.unlink()
    log(f"done: {args.out}  ({len(attribution)} images in ATTRIBUTION.csv)")


if __name__ == "__main__":
    main()
