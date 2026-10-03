# Clean plate-detector retraining

A retraining pipeline for the Android ALPR's plate **detector** with provenance BeaconFix can ship under its
Apache-2.0 terms: MIT training code, CC BY training data with a per-image attribution manifest, and no
pretrained weights of unclear origin.

The model it replaces, open-image-models `yolo-v9-t-{640,416}-license-plates-end2end.onnx`, was trained with the
GPL-3.0 WongKinYiu YOLOv9 code (its maintainer calls the weights' license a "grey zone") on scraped data that may
include AGPL openalpr benchmark images. docs/LICENSING.md therefore treats it as a GPL-3.0 model pack.

| part | what | license |
|---|---|---|
| training code | [MultimediaTechLab/YOLO](https://github.com/MultimediaTechLab/YOLO) at `c4cb5f6f` (Release v1.0, 2025-12-30), YOLOv9-t, cloned by `setup.sh` into `third_party/YOLO/` | MIT |
| training data | [Open Images V7](https://storage.googleapis.com/openimages/web/factsfigures_v7.html), class "Vehicle registration plate" (`/m/01jfm_`) | images: "listed as having a CC BY 2.0 license"; annotations: CC BY 4.0, Google LLC |
| initial weights | **none: the model is trained from scratch** (optionally after a stage-1 pretraining on more Open Images classes, same licenses) | n/a |
| this directory | `fetch_data.py`, `train.py`, `plate_augment.py`, `export_onnx.py`, `eval.py`, configs | Apache-2.0 |
| tools in `.venv` | PyTorch, Lightning, Hydra, pycocotools, faster-coco-eval, ONNX, ONNX Runtime, wandb client (imported by the YOLO code, not used) | BSD / MIT / Apache-2.0; none of these ship in the app |

**Why not the YOLO repo's `v9-t.pt`:** those COCO checkpoints (release `v1.0-alpha`) carry no license statement and
are converted from the original YOLOv9 training, so they bring back the grey zone. COCO's Flickr images also include
non-commercial licenses. `train.py` sets `weight=False` by default.

Open Images says: *"The annotations are licensed by Google LLC under CC BY 4.0 license. The images are listed as
having a CC BY 2.0 license. Note: while we tried to identify images that are licensed under a Creative Commons
Attribution license, we make no representations or warranties regarding the license status of each image and you
should verify the license for each image yourself."* For that reason `fetch_data.py` copies every image's own
`License`, `Author`, `AuthorProfileURL`, `Title` and Flickr URLs into `ATTRIBUTION.csv`, and drops any image whose
listed license is not CC BY. As of 2026-10, all 8,157 plate images are listed as CC BY 2.0.

## The data

| split | images | boxes | notes |
|---|---|---|---|
| train | 5,368 | 7,852 | 7 images dropped because they need rotation (`Rotation` ≠ 0, see below); 3 group-of boxes |
| val | 724 | 987 | used for validation and the best checkpoint during training |
| test | 2,065 | 2,843 | used only by `eval.py` |
| test_small (held out) | about 10 % of test | | every plate in the image is < 32 px wide once letterboxed to 640, i.e. dash-cam distance |
| test_neg (held out) | 300 | 0 | test images with a Car box **and** a human-verified "no Vehicle registration plate" image label, used to measure false alarms per image |

The held-out sets are my choice for a dash cam: distant plates are where a detector fails first, and false alarms
on plate-free street scenes cost OCR time and fill the app with junk. Both come from the test split, so they never
overlap the training data. `eval.py` also scores any other COCO-format set placed under `annotations/`, such as
your own dash-cam frames.

Format: COCO JSON (`annotations/instances_{train,val,test}.json`, boxes in stored-image pixels, group-of boxes as
`iscrowd: 1`) plus `images/<split>/<ImageID>.jpg`. The YOLO repo reads this layout directly. Images are downscaled
to a long side of at most 1280 px (`--max-side`), which is still 2× the network input. The full set is about
8.5k images and 2.5 GB.

## Full run on a 24–32 GB GPU (RTX 5090)

```sh
cd tools/train-plate-detector
./setup.sh                                   # .venv with torch 2.9.1+cu128 (Blackwell sm_120 OK); ~5 GB
.venv/bin/python fetch_data.py               # all plate images: ~15-20 min, ~2.5 GB
./train.sh full                              # 640 px, batch 64, 300 epochs, from scratch
./train.sh tile                              # 416 px model, fine-tuned from the 640 best.ckpt, 60 epochs
.venv/bin/python export_onnx.py runs/train/plates-v9t/checkpoints/best.ckpt \
    -o outputs/beaconfix-plates-v9t-640-end2end.onnx
.venv/bin/python export_onnx.py runs/train/plates-v9t-416/checkpoints/best.ckpt --size 416 \
    -o outputs/beaconfix-plates-v9t-416-end2end.onnx
# compare with the current models (both pinned in Models.kt)
curl -L -o models/current-end2end.onnx \
  https://github.com/ankandrew/open-image-models/releases/download/assets/yolo-v9-t-640-license-plates-end2end.onnx
curl -L -o models/current-416-end2end.onnx \
  https://github.com/ankandrew/open-image-models/releases/download/assets/yolo-v9-t-416-license-plates-end2end.onnx
.venv/bin/python eval.py models/current-end2end.onnx outputs/beaconfix-plates-v9t-640-end2end.onnx \
    models/current-416-end2end.onnx outputs/beaconfix-plates-v9t-416-end2end.onnx --json outputs/eval.json
```

Check the SHA-256 of the downloaded current models against Models.kt (`c3c1026c…` for 640, `0469d81f…` for 416).

**Recommended: stage-1 pretraining.** 5.4k training images is little for a from-scratch detector. With no clean
ImageNet or COCO weights available, the clean substitute is a pretraining pass on more Open Images street classes
(same licenses, same manifest):

```sh
.venv/bin/python fetch_data.py --out data/oi-vehicles --limit 120000 --negatives 0 --classes \
  "Vehicle registration plate;Car;Truck;Bus;Van;Taxi;Ambulance;Motorcycle;Bicycle;Wheel;Traffic sign;Person"
./train.sh pretrain                                           # conf/dataset/oi_vehicles.yaml, 100 epochs
INIT=runs/train/pretrain-v9t/checkpoints/best.ckpt ./train.sh full
```

`--init` takes the EMA weights; the 12-class head does not fit the 1-class model and starts fresh, with a warning.
Open Images labels only the classes verified for each image, so stage 1 sees some unboxed plates as background.
That is acceptable for pretraining, and stage 2 trains on exhaustively boxed plate images. The stage-1 data is
about 120k images and 30 GB, and its train image list streams 640 MB once. **Ship `data/oi-vehicles/ATTRIBUTION.csv`
too if you use it:** those images shaped the weights.

**Expected time** (estimate: no 5090 was available here; see "What was verified"):
- `fetch_data.py`: 15–20 min. About 6 min of that streams the 2.3 GB train box CSV and the 640 MB image list
  once, cached in `data/raw/`; the rest downloads ~8.5k images (about 25 img/s with 16 threads, measured).
- `train.sh full`: v9-t at 640 is about 8 GFLOPs per image forward. A 5090 does forward+backward of a batch of 64
  in well under 100 ms, so **the data loader is the bottleneck**. Per image, PIL JPEG decode, the augmentations and
  the LANCZOS letterbox measured **127 ms on one core** of the development box: a niced process on a 6-core desktop
  at load 15–20, so an idle modern core should do about 40–60 ms. With 16 workers that is roughly 130–400 img/s.
  One epoch (5,368 train + 724 val images) then takes about 0.3–0.8 min, and **300 epochs about 2–4 hours**. Raise
  `WORKERS` (default 16) up to the node's core count; it scales nearly linearly.
- `train.sh tile`: 60 epochs at 416, about 30–60 min (still decode-bound).
- `train.sh pretrain` (optional): 120k images × 100 epochs = 12M samples, about 10–25 hours with 16 workers, less
  with more. Use `TRAIN_ARGS="--val-every 5"` to validate less often.
- VRAM: batch 64 at 640 with bf16 is far below 24 GB for v9-t. Lower `BATCH` if another job shares the card;
  `equivalent_batch_size` stays 64 through gradient accumulation.

### Resume, logs, checkpoints

- `RESUME=1 ./train.sh full` (or `pretrain` / `tile`) continues from `runs/train/<name>/checkpoints/last.ckpt`:
  weights, optimizer, LR schedule, epoch, and the EMA weights with their step count. `last.ckpt` is written at
  every epoch end and every 500 optimizer steps. `best.ckpt` is the best validation mAP50.
- stdout prints a line every 10 steps (losses, LR) and one per validation (`VAL … mAP50=… mAP50-95=…`). Every
  logged value is also in `runs/train/<name>/csv/metrics.csv`.
- Extra Hydra overrides go after the mode, e.g. `./train.sh full task.epoch=400`. Extra `train.py` options go in
  `TRAIN_ARGS`. Multi-GPU: `TRAIN_ARGS="--devices 2"` (DDP with synced BatchNorm, untested here).

## Training setup

`conf/task/train_plates.yaml`: YOLOv9-t (`model=v9-t` from the YOLO repo, with the auxiliary PGI branch during
training), 640 px, one class, SGD with lr 0.01, 3 warm-up epochs then linear decay, EMA, and the repo's
loss (BCE + CIoU box + DFL, aux weight 0.25). Validation uses NMS IoU 0.45 and conf 0.001, the same as the exported
graph.

Augmentation (`plate_augment.py`, plugged into the repo's loader), in order:

| transform | p | for |
|---|---|---|
| PlateMosaic | 0.5 | 4 images around a random centre, then a 1–2× window of that resized to 640: more plates per sample at ¼–1× scale |
| RandomCrop (repo) | 0.25 | half-size crop: big, close plates |
| ZoomOut | 0.25 | shrink to 35–90 % onto grey: distant plates |
| HorizontalFlip (repo) | 0.5 | |
| ColorJitter | 0.8 | brightness, contrast, saturation, hue |
| LowLight | 0.25 | night: gain 0.15–0.6, gamma, colour cast, signal-dependent sensor noise |
| MotionBlur | 0.25 | 3–15 px linear blur, mostly horizontal |
| GaussianBlur | 0.1 | focus, dirty windscreen |
| JpegArtifacts | 0.3 | quality 20–70 |
| ClipBoxes | 1 | clip to the image, drop slivers (< 40 % visible, or < ~1 px) |

## The exported model: same I/O as the current one

`PlateDetector` (android/…/alpr/vision/Models.kt) feeds `float32 [1, 3, S, S]`, RGB, CHW, 0..1, letterboxed to the
centre with grey 114 (S = 640 for `DETECTOR`, 416 for `DETECTOR_TILE`). It reads output 0 as rows of 7 floats,
`batch, x1, y1, x2, y2, class, score`, in input pixels, and keeps `score >= minScore` (0.4). The current
open-image-models graph is WongKinYiu's `End2End` export: input `images`, output `output0` `[N, 7]`, and ONNX
`NonMaxSuppression` with at most 100 boxes, IoU 0.45, score 0.001.

`export_onnx.py` produces exactly that. The MIT repo has no end2end export, so the script adds the NMS itself:

- the main branch only (the training-only auxiliary branch is dropped);
- BatchNorm folded into the convs and each RepConv's 3×3 + 1×1 merged into one 3×3, so the graph is a plain conv net
  (checked: max |Δ| ≈ 1e-4 px against the unfused model);
- box decode in the graph (anchor ± predicted distances × stride → xyxy), sigmoid scores;
- an `onnx.helper` tail: `NonMaxSuppression` (per class; 100 / 0.45 / 0.001), then `GatherND` for boxes and scores,
  and `Concat` into `output0 [N, 7]`; opset 17, the same as the current model; ONNX Runtime Android 1.22 runs it;
- metadata props describing the contract, the data licenses and which weights were used (EMA by default).

After writing, the script reloads the file in onnxruntime, checks the I/O shapes, and compares the rows with the
PyTorch model plus torchvision NMS. No change to the Android decode is needed.

## Publishing as a BeaconFix download

1. Train, export both sizes, and run `eval.py` against the current models (table above). Ship only if it is at
   least as good on `test` and `test_small`, with no more false alarms on `test_neg`, or if you accept the gap for
   the clean license.
2. Make a GitHub release on the BeaconFix repo (e.g. tag `alpr-detector-v1`) with these assets:
   `beaconfix-plates-v9t-640-end2end.onnx`, `beaconfix-plates-v9t-416-end2end.onnx`, `ATTRIBUTION.csv` and
   `ATTRIBUTION.md` from `data/oi-plates/` (plus `data/oi-vehicles/ATTRIBUTION.csv` if stage 1 was used),
   `third_party/YOLO/LICENSE` as `LICENSE-YOLO-MIT.txt`, and `eval.json`.
   License text for the release notes: *"Model weights by the BeaconFix authors, trained with the MIT-licensed
   MultimediaTechLab/YOLO code on Open Images V7 data: images CC BY 2.0 (per-image authors and licences in
   ATTRIBUTION.csv), annotations CC BY 4.0 Google LLC. Redistribution must keep these attributions."*
3. Pin it: `sha256sum outputs/*.onnx` and `stat -c %s outputs/*.onnx`, then in
   `android/app/src/main/java/org/sworrl/beaconfix/alpr/vision/Models.kt` replace `DETECTOR` and `DETECTOR_TILE`:
   ```kotlin
   val DETECTOR = Spec("beaconfix-plates-v9t-640-end2end.onnx",
       "https://github.com/sworrl/beaconfix/releases/download/alpr-detector-v1/beaconfix-plates-v9t-640-end2end.onnx",
       "<sha256>", <bytes>)
   ```
   Make the same change in `android/app/alpr_models.gradle.kts`: its `specs` list bundles the models into the APK
   at build time, with the same file name, URL and SHA-256. A new file name avoids clashing with a cached copy of
   the old model in `noBackupFilesDir`.
4. In docs/LICENSING.md, replace the open-image-models row with the new model (MIT code + CC BY data,
   attribution shipped), and show `ATTRIBUTION.md` in the app's open-source licenses screen.

## Smoke test (what ran on the development box)

All on CPU (no GPU used), `nice 19`, ≤ 4 threads, on a 6-core desktop running other work (load 15–20), with
`fetch_data.py --limit 200`.

**1. Augmented smoke run** (`./train.sh smoke`: the full augmentation, 320 px, batch 4, 300 steps = 6 epochs over
200 images): 24 min, about 0.75 GB RSS. It ran end to end: data cache, sanity validation, 6 validations, `best`,
`last` and step checkpoints, CSV log. Epoch-mean losses (Box / DFL / BCE): 4.09 / 3.29 / 3.98 → 3.22 / 2.54 / 3.28
→ … → 5.45 / 3.12 / 5.02; val mAP50 0.000 → 0.0007. This loss is normalised by the sum of the assignment scores,
which grows as boxes start to overlap their targets, so it is not monotone early in a from-scratch run. 300 noisy
steps prove the plumbing, not learning; that is run 3. A first attempt at 640 px (batch 4, 2 loader workers,
~4.5 s/step) used 1.9 GB and was SIGTERMed by the desktop's `earlyoom`. The smoke mode is now 320 px, without
worker processes, and with `oom_score_adj` 1000, so it is the first thing killed.

**2. Resume** (`RESUME=1 STEPS=340 ./train.sh smoke`): `resumed from …/last.ckpt at epoch 6, step 300`. It
continued at the restored LR (9.5e-3, not a new warm-up) with the EMA state restored, to step 340, and wrote
new checkpoints. (Found and fixed on the way: a restore that Lightning treats as mid-epoch skipped the YOLO repo's
LR-ramp initialisation.)

**3. Learning check** (`SMOKE_OVERFIT=1 ./train.sh smoke`: memorise 16 training images, train = val, flips only,
300 steps): 9.5 min. The losses fall steadily and the mAP rises:

| epoch (4 steps each) | 0 | 10 | 20 | 30 | 40 | 50 | 60 | 70 | 74 |
|---|---|---|---|---|---|---|---|---|---|
| Box loss | 6.61 | 4.80 | 3.64 | 2.99 | 2.57 | 2.32 | 1.75 | 1.24 | 1.24 |
| DFL loss | 5.14 | 4.49 | 3.31 | 2.67 | 2.21 | 1.94 | 1.65 | 1.54 | 1.49 |
| BCE loss | 5.78 | 4.42 | 3.33 | 2.80 | 1.85 | 1.54 | 1.13 | 0.84 | 0.77 |
| val mAP50 (same 16 images) | | 0.000 (ep 9) | 0.000 | 0.050 | 0.043 | 0.167 | 0.343 | 0.590 (ep 69) | |

**4. Export** (`export_onnx.py`, about 30–40 s and 0.7 GB each): `images [1,3,640,640]` → `output0 [N,7]`
(7,900,276 bytes; the current model is 7,835,770) and `images [1,3,416,416]` → `output0 [N,7]` (7,803,252 bytes).
On a real letterboxed val image the ONNX rows equal PyTorch + torchvision NMS: scores within 7e-8, boxes within
6e-5 px. At 416 one box near the 100-box cap differs, an NMS tie among an untrained model's near-equal scores.

**5. Eval** (`eval.py`, Android preprocessing, 2 threads), on the 200-image subsets:

| model | set | mAP50 | mAP50-95 | AP_small | P@0.4 | R@0.4 | FP/img@0.4 | ms/img |
|---|---|---|---|---|---|---|---|---|
| current 640 (open-image-models) | test | 0.927 | 0.701 | 0.280 | 0.983 | 0.857 | 0.020 | 91 |
| current 640 | test_small | 0.846 | 0.478 | 0.362 | 1.000 | 0.704 | 0.000 | 87 |
| current 640 | test_neg | | | | | | 0.120 | 109 |
| current 416 | test | 0.895 | 0.669 | 0.194 | 0.974 | 0.860 | 0.030 | 45 |
| current 416 | test_small | 0.668 | 0.361 | 0.252 | 0.857 | 0.667 | 0.150 | 47 |
| current 416 | test_neg | | | | | | 0.240 | 43 |
| smoke 640 / 416 (run 1) | all | 0.000 | 0.000 | | | 0.000 | 0.000 | 128 / 36 |
| memorised (run 3), exported at 320 | the 16 images | 0.452 | 0.288 | | 0.667 | 0.316 | 0.188 | 16 |
| memorised (run 3), exported at 640 | the 16 images | 0.007 | 0.002 | | 0.032 | 0.053 | 1.875 | 59 |
| current 640 | the 16 images | 0.994 | 0.741 | | 0.947 | 0.947 | 0.062 | 59 |

The memorised model, exported and run through the app's letterbox and box mapping, finds its plates at the scale
it was trained on. That confirms the decode and coordinate round trip. At 640 it fails because it never saw that
scale; the real run trains at 640. Timings vary with the desktop's load. Measured in the same run, the new 640
graph is as fast as the current one (59 ms each), and the 416 graph is a little faster (36–41 vs 45 ms), since
RepConv is fused.

## What was verified and what was not

Verified on the development box (CPU only, 2026-10-03):
- `setup.sh --cpu`: venv in 79 s (1.1 GB with CPU torch); YOLO checked out at the pinned commit; MIT license checked.
- `fetch_data.py --limit 200`: 7.5 min, most of it streaming the full train box CSV (2.26 GB) and image list
  (638 MB) once. The full-split counts in "The data" come from those streams. 800 images (233 MB) were written:
  200 each of train, val, test and test_neg, with `ATTRIBUTION.csv`, all CC BY 2.0.
- the augmentations: checked visually (boxes follow every transform) and statistically (median plate 16 px wide
  at 640, 23 % ≥ 40 px, 7 % of samples without a box).
- `train.py`: the augmented smoke run, resume, and the memorisation run (losses fall, mAP 0 → 0.59); see above.
- `export_onnx.py` at 640 and 416: graph check passes, fusion error ≤ 1.2e-4 px, onnxruntime I/O `images [1,3,S,S]`
  → `output0 [N,7]` like the current model, rows equal to PyTorch + torchvision NMS on a real letterboxed image.
- `eval.py`: ran on the current 640/416 models and the smoke exports.

Not verified:
- **The full run.** Its time, VRAM and accuracy are estimates (above), and the final mAP vs the current model is
  unknown until it runs. bf16 on Blackwell, `--devices` > 1 (DDP), and 640 px training batches on a GPU never ran
  here. The smoke runs used 320 px on CPU because the box had little free RAM. The model is fully convolutional, so
  only the input size differs, and export and eval did run at 640 and 416.
- **Accuracy from scratch.** 5.4k training images may not reach the current model's level without the stage-1
  pretraining. That trade-off is the price of having no tainted weights.
- **Open Images rotation:** the docs don't say whether boxes are relative to the rotated or the stored image, so the
  few images with `Rotation` ≠ 0 (7 of 5,368 in train, none in val/test) and EXIF-rotated images are dropped, not guessed.
- **Image licenses:** taken from Open Images' list; Google gives no warranty. Re-checking a sample of Flickr pages
  by hand is the remaining due diligence (`landing_url` in ATTRIBUTION.csv).
- **Fair comparison:** open-image-models does not publish its training set. If it included Open Images test images,
  its scores here are inflated. Its high mAP50 on this test subset (0.93) suggests it may have.
- **test_neg labels are noisy.** I looked at the current model's 12 strongest "false alarms" there. 3 are real
  registration plates (Open Images' "verified no plate" label is wrong), 6 are dealer/model-name plates or empty
  plate holders, and 2 are odometers. Read FP/image on test_neg as an upper bound and compare models on it
  relatively, not absolutely.
- **The app itself:** no Android build was run with a new model; the contract was matched against Models.kt and
  the current graphs.

## Files

| file | |
|---|---|
| `setup.sh` | venv + pinned YOLO checkout (`--cpu` for a CPU-only torch) |
| `requirements.txt` | pinned versions of everything except torch (set in setup.sh) |
| `fetch_data.py` | Open Images download, COCO conversion, held-out sets, `ATTRIBUTION.csv` / `.md`, `manifest.json`; `--limit N` for subsets |
| `conf/dataset/oi_plates.yaml`, `conf/dataset/oi_vehicles.yaml` | dataset configs (Hydra, added to the YOLO repo's search path) |
| `conf/task/train_plates.yaml` | the training task: v9-t, 640, plates, augmentation, optimizer, EMA, validation NMS |
| `plate_augment.py` | dash-cam augmentations |
| `train.py`, `train.sh` | training with resume, checkpoints, CSV log, `--init`; modes `pretrain`, `full`, `tile`, `smoke` |
| `export_onnx.py` | end2end ONNX in BeaconFix's I/O contract, `--size 640/416` |
| `eval.py` | mAP50 / mAP50-95 / AP_small, P/R and false alarms at the app's 0.4, ms per image, for any end2end ONNX |

Everything generated (`.venv/`, `third_party/`, `data/`, `runs/`, `weights/`, `models/`, `outputs/`, `*.onnx`) is
git-ignored.
