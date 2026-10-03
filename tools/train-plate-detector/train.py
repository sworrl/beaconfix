#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Train the plate detector with the MIT YOLO code (third_party/YOLO, checked out by setup.sh).

Same model, loss, data loader and validation as the repo's yolo/lazy.py, plus what a long run needs:
  - checkpoints: runs/train/<name>/checkpoints/last.ckpt (every epoch and every --ckpt-steps), best.ckpt (by val mAP50)
  - resume: --resume continues from last.ckpt (weights, optimizer, LR schedule, epoch, EMA weights and step count)
  - a CSV of every logged loss/metric (runs/train/<name>/csv/metrics.csv) and a plain progress line on stdout
  - --max-steps / --limit-val-batches for smoke tests

Anything after the options is passed to Hydra, e.g.  task.data.batch_size=32 task.epoch=200 weight=False
"""
from __future__ import annotations

import argparse
import math
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "third_party" / "YOLO"))


def compose_cfg(overrides: list[str]):
    from hydra import compose, initialize_config_dir

    with initialize_config_dir(config_dir=str(HERE / "third_party/YOLO/yolo/config"), version_base=None):
        return compose("config", overrides=[f"hydra.searchpath=[file://{HERE / 'conf'}]", *overrides])


def checkpoint_weights(ckpt_path, which: str = "ema"):
    """Weights of a train.py checkpoint keyed like YOLO.state_dict() ("model.<layer>.…"), and where they came from.
    "ema": the EMA shadow weights (what validation scores), else the raw training weights."""
    import torch

    ckpt = torch.load(ckpt_path, map_location="cpu", weights_only=False)
    sd = ckpt.get("state_dict", ckpt)
    if which == "ema":
        for key, state in (ckpt.get("callbacks") or {}).items():
            if "EMA" in key and state.get("ema"):
                return {k: v.cpu() for k, v in state["ema"].items()}, f"EMA weights (step {state.get('step')})"
        if any(k.startswith("ema.model.") for k in sd):
            return {k.removeprefix("ema."): v for k, v in sd.items() if k.startswith("ema.model.")}, \
                "EMA weights of the last validation"
    return {k.removeprefix("model."): v for k, v in sd.items() if k.startswith("model.model.")}, "raw (non-EMA) weights"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="plates-v9t")
    ap.add_argument("--resume", action="store_true", help="continue from runs/train/<name>/checkpoints/last.ckpt")
    ap.add_argument("--init", type=Path, help="start from this train.py checkpoint's EMA weights (new optimizer and "
                    "schedule; layers whose shape differs, e.g. the class head of a pretraining run, start fresh)")
    ap.add_argument("--max-steps", type=int, default=-1, help="stop after this many optimizer steps (-1: epochs)")
    ap.add_argument("--ckpt-steps", type=int, default=500, help="also save last.ckpt every this many steps")
    ap.add_argument("--val-every", type=int, default=1, help="validate every N epochs")
    ap.add_argument("--limit-val-batches", type=float, default=1.0)
    ap.add_argument("--precision", default=None, help="default: bf16-mixed on CUDA, 32-true on CPU")
    ap.add_argument("--devices", default="auto", help="'auto', a GPU count, or e.g. '[0,1]'")
    ap.add_argument("--accelerator", default="auto")
    ap.add_argument("--log-every", type=int, default=10)
    ap.add_argument("overrides", nargs="*", help="Hydra overrides")
    args = ap.parse_args()

    import torch
    from lightning import Callback, Trainer
    from lightning.pytorch.callbacks import ModelCheckpoint
    from lightning.pytorch.loggers import CSVLogger

    import plate_augment

    plate_augment.register()
    from yolo.tools.solver import TrainModel
    from yolo.utils.logging_utils import setup
    from yolo.utils.model_utils import EMA

    os.chdir(HERE)  # dataset paths in conf/ are relative to this directory
    base = [
        "task=train_plates", "dataset=oi_plates", "model=v9-t", f"name={args.name}",
        "weight=False",           # from scratch: the repo's COCO weights are not clean (see README)
        "use_wandb=False", "+quiet=True",
    ]
    if args.init and not args.resume:
        weights, source = checkpoint_weights(args.init)
        init = HERE / "weights" / f"init-{args.name}.pt"
        init.parent.mkdir(exist_ok=True)
        torch.save({k.removeprefix("model."): v for k, v in weights.items()}, init)  # keyed like YOLO.model
        print(f"init: {args.init} ({source}) -> {init}", flush=True)
        base = [o for o in base if not o.startswith("weight=")] + [f"weight={init}"]
    given = {o.split("=", 1)[0].lstrip("+~") for o in args.overrides}
    cfg = compose_cfg([o for o in base if o.split("=", 1)[0].lstrip("+") not in given] + args.overrides)

    callbacks, loggers, save_path = setup(cfg)
    save_path = Path(save_path)

    class ResumableEMA(EMA):
        """The repo's EMA, with its shadow weights and step count saved in (and restored from) the checkpoint."""

        def state_dict(self):
            return {"step": self.step, "ema": self.ema_state_dict}

        def load_state_dict(self, state):
            self.step = state["step"]
            self.ema_state_dict = state["ema"]

        def _to(self, device):
            if self.ema_state_dict is not None:
                for k, v in self.ema_state_dict.items():
                    if v.device != device:
                        self.ema_state_dict[k] = v.to(device)

        def on_validation_start(self, trainer, pl_module):
            self._to(pl_module.device)
            super().on_validation_start(trainer, pl_module)

        def on_train_batch_end(self, trainer, pl_module, *a, **kw):
            if self.ema_state_dict is None:
                self.ema_state_dict = {k: v.detach().clone() for k, v in pl_module.model.state_dict().items()}
            self._to(pl_module.device)
            super().on_train_batch_end(trainer, pl_module, *a, **kw)

    callbacks = [ResumableEMA(cfg.task.ema.decay) if type(c) is EMA else c for c in callbacks]

    class Progress(Callback):
        """A plain line every few steps and after each validation (the repo's rich bar is off: quiet)."""

        def __init__(self):
            self.t0 = time.time()

        def on_train_start(self, trainer, pl_module):
            if trainer.ckpt_path:
                # the repo's per-batch LR ramp interpolates from optimizer.max_lr; after a restore that must be the
                # restored LR, not the initial warm-up values
                for opt in trainer.optimizers:
                    opt.max_lr = [g["lr"] for g in opt.param_groups]
                print(f"resumed from {trainer.ckpt_path} at epoch {trainer.current_epoch}, step {trainer.global_step}",
                      flush=True)

        def on_train_batch_start(self, trainer, pl_module, batch, batch_idx):
            # a restore that Lightning treats as mid-epoch skips the module's on_train_epoch_start, which is where the
            # repo initialises its per-batch LR ramp; start the ramp here (flat at the restored LR) in that case
            for opt in trainer.optimizers:
                if not hasattr(opt, "batch_idx"):
                    opt.next_epoch(math.ceil(len(pl_module.train_loader) / trainer.world_size), trainer.current_epoch)

        def on_train_batch_end(self, trainer, pl_module, outputs, batch, batch_idx):
            if trainer.global_step % args.log_every or batch_idx % trainer.accumulate_grad_batches:
                return
            m = trainer.callback_metrics
            losses = "  ".join(f"{k.split('/')[-1].removesuffix('_step')}={float(v):.3f}"
                               for k, v in m.items() if k.startswith("Loss/") and k.endswith("_step"))
            lr = trainer.optimizers[0].param_groups[1]["lr"]
            print(f"epoch {trainer.current_epoch} step {trainer.global_step}  {losses}  lr={lr:.2e}  "
                  f"{time.time() - self.t0:.0f}s", flush=True)

        def on_validation_end(self, trainer, pl_module):
            if trainer.sanity_checking:
                return
            m = trainer.callback_metrics
            print(f"VAL epoch {trainer.current_epoch} step {trainer.global_step}  "
                  f"mAP50={float(m.get('map_50', 0)):.4f}  mAP50-95={float(m.get('map', 0)):.4f}", flush=True)

    ckpt_dir = save_path / "checkpoints"
    callbacks += [
        Progress(),
        ModelCheckpoint(dirpath=ckpt_dir, filename="best", monitor="map_50", mode="max", save_top_k=1,
                        enable_version_counter=False),
        ModelCheckpoint(dirpath=ckpt_dir, filename="step{step:07d}", save_last=True, save_top_k=1, auto_insert_metric_name=False,
                        every_n_train_steps=args.ckpt_steps, save_on_train_epoch_end=True,
                        enable_version_counter=False),
    ]
    loggers = [*loggers, CSVLogger(save_path, name="csv", version="")]

    ckpt = None
    if args.resume:
        last = ckpt_dir / "last.ckpt"
        if last.exists():
            ckpt = str(last)
        else:
            print(f"--resume: no {last}, starting fresh", flush=True)

    cuda = torch.cuda.is_available() and args.accelerator in ("auto", "gpu", "cuda")
    precision = args.precision or ("bf16-mixed" if cuda else "32-true")
    devices = args.devices
    if devices.isdigit():
        devices = int(devices)
    elif devices.startswith("["):
        devices = [int(x) for x in devices.strip("[]").split(",")]
    multi = (isinstance(devices, list) and len(devices) > 1) or (isinstance(devices, int) and devices > 1)

    trainer = Trainer(
        accelerator=args.accelerator,
        devices=devices,
        max_epochs=cfg.task.epoch,
        max_steps=args.max_steps,
        precision=precision,
        callbacks=callbacks,
        logger=loggers,
        log_every_n_steps=args.log_every,
        gradient_clip_val=10,
        gradient_clip_algorithm="norm",
        sync_batchnorm=multi,
        check_val_every_n_epoch=args.val_every,
        limit_val_batches=args.limit_val_batches,
        enable_progress_bar=False,
        enable_model_summary=False,
        default_root_dir=save_path,
    )
    model = TrainModel(cfg)
    trainer.fit(model, ckpt_path=ckpt)
    print(f"checkpoints: {ckpt_dir}", flush=True)


if __name__ == "__main__":
    main()
