#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# YOLOv9-t, plates only (conf/task/train_plates.yaml). Each mode resumes by itself if run again with RESUME=1.
#
#   ./train.sh pretrain [hydra…]  optional stage 1: 12 Open Images street classes (data/oi-vehicles), 640 px, from scratch
#   ./train.sh full     [hydra…]  the plate model, 640 px (DETECTOR); starts from stage 1 if INIT=<ckpt> is given
#   ./train.sh tile     [hydra…]  the 416 px model (DETECTOR_TILE), fine-tuned from the 640 model's best.ckpt
#   ./train.sh smoke    [hydra…]  pipeline check: CPU, nice 19, 320 px, batch 4, 300 steps (data from fetch_data.py --limit)
#                                 SMOKE_OVERFIT=1: memorise 16 images, train = val (a "does it learn" check)
#
#   RESUME=1 ./train.sh full      continue runs/train/plates-v9t from checkpoints/last.ckpt after a stop or crash
#   INIT=runs/train/pretrain-v9t/checkpoints/best.ckpt ./train.sh full
#   WORKERS=24 BATCH=64 ./train.sh full;   TRAIN_ARGS="--val-every 5" ./train.sh pretrain
set -euo pipefail
cd "$(dirname "$0")"
PY=.venv/bin/python
mode=${1:-}; shift || true
extra=(${TRAIN_ARGS:-})
[[ -n "${RESUME:-}" ]] && extra+=(--resume)
[[ -n "${INIT:-}" ]] && extra+=(--init "$INIT")
gpu=(task.data.batch_size="${BATCH:-64}" cpu_num="${WORKERS:-16}")
case "$mode" in
  pretrain)
    exec $PY train.py --name pretrain-v9t "${extra[@]}" -- "${gpu[@]}" dataset=oi_vehicles task.epoch=100 "$@" ;;
  full)
    exec $PY train.py --name plates-v9t "${extra[@]}" -- "${gpu[@]}" task.epoch=300 "$@" ;;
  tile)
    [[ -n "${INIT:-}" || -n "${RESUME:-}" ]] || extra+=(--init runs/train/plates-v9t/checkpoints/best.ckpt)
    exec $PY train.py --name plates-v9t-416 "${extra[@]}" -- "${gpu[@]}" "image_size=[416,416]" task.epoch=60 \
        task.scheduler.warmup.epochs=1 task.optimizer.args.lr=0.002 "$@" ;;
  smoke)
    # the box this was written on is someone's busy desktop with ~2-6 GB free: ≤ 4 CPU threads, lowest priority, no GPU,
    # no loader worker processes, 320 px (the net is fully convolutional: export/eval still use 640/416), ~1 GB RSS,
    # and first in line for earlyoom / the OOM killer
    export CUDA_VISIBLE_DEVICES="" OMP_NUM_THREADS=${OMP_NUM_THREADS:-4}
    echo 1000 > /proc/self/oom_score_adj 2>/dev/null || true
    name=smoke
    if [[ -n "${SMOKE_OVERFIT:-}" ]]; then
        # learning check: memorise 16 training images (train = val), no augmentation but flips. The losses must
        # fall and mAP on those images must rise; a from-scratch run on more data needs far more than 300 steps
        name=smoke-overfit
        $PY - <<'PY'
import json, os, pathlib
d = pathlib.Path("data/oi-plates"); src = json.loads((d / "annotations/instances_train.json").read_text())
by = {}
for a in src["annotations"]:
    by.setdefault(a["image_id"], []).append(a)
big = [im for im in src["images"] if any(a["bbox"][2] > 0.06 * im["width"] for a in by.get(im["id"], []))][:16]
ids = {im["id"] for im in big}
(d / "annotations/instances_tiny.json").write_text(json.dumps(dict(src, images=big,
    annotations=[a for a in src["annotations"] if a["image_id"] in ids])))
(d / "images/tiny").mkdir(exist_ok=True)
for im in big:
    link = d / "images/tiny" / im["file_name"]
    if not link.exists():
        os.symlink(os.path.abspath(d / "images/train" / im["file_name"]), link)
(d / "tiny.pache").unlink(missing_ok=True)
print(f"tiny set: {len(big)} images, {sum(len(by[i]) for i in ids)} plates")
PY
        set -- dataset.train=tiny dataset.validation=tiny \
            "~task.data.data_augment.PlateMosaic" "~task.data.data_augment.RandomCrop" "~task.data.data_augment.ZoomOut" \
            "~task.data.data_augment.ColorJitter" "~task.data.data_augment.LowLight" "~task.data.data_augment.MotionBlur" \
            "~task.data.data_augment.GaussianBlur" "~task.data.data_augment.JpegArtifacts" "$@"
        extra+=(--log-every 4 --val-every 10)
    fi
    exec nice -n 19 $PY train.py --name $name --accelerator cpu --precision 32-true \
        --max-steps "${STEPS:-300}" --ckpt-steps 50 --log-every 10 "${extra[@]}" -- \
        "image_size=[${SMOKE_SIZE:-320},${SMOKE_SIZE:-320}]" task.data.batch_size=4 task.data.equivalent_batch_size=4 \
        task.epoch=100 task.data.pin_memory=False task.validation.data.batch_size=4 task.validation.data.pin_memory=False \
        task.scheduler.warmup.epochs=1 cpu_num=0 "$@" ;;
  *)
    sed -n '4,15p' "$0"; exit 2 ;;
esac
