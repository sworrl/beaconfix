# SPDX-License-Identifier: Apache-2.0
"""
Dash-cam augmentations for the YOLO repo's data pipeline.

The repo builds its transforms with eval(name)(prob) inside yolo.tools.data_loader, each called as
    image, boxes = t(image: PIL.Image, boxes: Tensor[N, 5] = (class, x1, y1, x2, y2) normalised to 0..1)
so register() puts these classes into that module's namespace and the task config can name them.
"""
from __future__ import annotations

import io
import math
import random

import numpy as np
import torch
from PIL import Image, ImageEnhance, ImageFilter

GREY = (114, 114, 114)


def _rand(a: float, b: float) -> float:
    return random.uniform(a, b)


class _Prob:
    def __init__(self, prob: float = 0.5):
        self.prob = float(prob)

    def __call__(self, image, boxes):
        if random.random() >= self.prob:
            return image, boxes
        return self.apply(image, boxes)

    def apply(self, image, boxes):
        raise NotImplementedError


class PlateMosaic(_Prob):
    """Four images, each scaled to a long side of 0.5..1 × base size, around a random centre of a 2·base canvas; then
    a window of 1..2 × base around that centre is cut out and resized to base. Plates end up at 1/4..1 of their
    letterboxed size (a mix of near and distant ones, more plates per sample)."""

    def __init__(self, prob: float = 0.5):
        super().__init__(prob)
        self.parent = None

    def set_parent(self, parent):
        self.parent = parent

    def apply(self, image, boxes):
        s = int(self.parent.base_size)
        data = [(image, boxes)] + self.parent.get_more_data(3)
        canvas = Image.new("RGB", (2 * s, 2 * s), GREY)
        cx, cy = int(_rand(0.6, 1.4) * s), int(_rand(0.6, 1.4) * s)
        out = []
        for k, (img, bx) in enumerate(data):
            w, h = img.size
            r = s * _rand(0.5, 1.0) / max(w, h)
            nw, nh = max(1, round(w * r)), max(1, round(h * r))
            img = img.resize((nw, nh), Image.Resampling.BILINEAR)
            x0 = cx - nw if k in (0, 2) else cx
            y0 = cy - nh if k in (0, 1) else cy
            canvas.paste(img, (x0, y0))
            bx = bx.clone()
            bx[:, [1, 3]] = (bx[:, [1, 3]] * nw + x0) / (2 * s)
            bx[:, [2, 4]] = (bx[:, [2, 4]] * nh + y0) / (2 * s)
            out.append(bx)
        boxes = torch.cat(out) if out else boxes
        c = int(s * _rand(1.0, 2.0))
        x0 = min(max(cx - c // 2, 0), 2 * s - c)
        y0 = min(max(cy - c // 2, 0), 2 * s - c)
        boxes = boxes.clone()
        boxes[:, [1, 3]] = (boxes[:, [1, 3]] * 2 * s - x0) / c
        boxes[:, [2, 4]] = (boxes[:, [2, 4]] * 2 * s - y0) / c
        out_img = canvas.crop((x0, y0, x0 + c, y0 + c)).resize((s, s), Image.Resampling.BILINEAR)
        return out_img, _clip(boxes)


class ZoomOut(_Prob):
    """Shrink the image to 35..90 % and paste it at a random spot of a grey canvas of the same size."""

    def apply(self, image, boxes):
        w, h = image.size
        r = _rand(0.35, 0.9)
        nw, nh = max(1, round(w * r)), max(1, round(h * r))
        x0, y0 = random.randint(0, w - nw), random.randint(0, h - nh)
        canvas = Image.new("RGB", (w, h), GREY)
        canvas.paste(image.resize((nw, nh), Image.Resampling.BILINEAR), (x0, y0))
        boxes = boxes.clone()
        boxes[:, [1, 3]] = (boxes[:, [1, 3]] * nw + x0) / w
        boxes[:, [2, 4]] = (boxes[:, [2, 4]] * nh + y0) / h
        return canvas, boxes


class ColorJitter(_Prob):
    def apply(self, image, boxes):
        image = image.convert("RGB")
        image = ImageEnhance.Brightness(image).enhance(_rand(0.6, 1.4))
        image = ImageEnhance.Contrast(image).enhance(_rand(0.6, 1.4))
        image = ImageEnhance.Color(image).enhance(_rand(0.5, 1.5))
        if random.random() < 0.5:  # hue shift
            hsv = np.array(image.convert("HSV"), dtype=np.int16)
            hsv[..., 0] = (hsv[..., 0] + random.randint(-10, 10)) % 256
            image = Image.fromarray(hsv.astype(np.uint8), "HSV").convert("RGB")
        return image, boxes


class LowLight(_Prob):
    """Night: darker (gain 0.15..0.6, gamma 1.2..2.2), a little colour cast, then signal-dependent sensor noise."""

    def apply(self, image, boxes):
        a = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.0
        a = np.power(a, _rand(1.2, 2.2)) * _rand(0.15, 0.6)
        a *= np.array([_rand(0.85, 1.1), _rand(0.9, 1.05), _rand(0.85, 1.15)], dtype=np.float32)
        sigma = _rand(0.01, 0.05)
        a = a + np.random.normal(0.0, 1.0, a.shape).astype(np.float32) * (sigma * np.sqrt(np.clip(a, 1e-4, 1)) + sigma / 4)
        return Image.fromarray((np.clip(a, 0, 1) * 255 + 0.5).astype(np.uint8)), boxes


class MotionBlur(_Prob):
    """Linear blur of 3..15 px at a random angle (mostly horizontal, as from a turning or overtaking car)."""

    def apply(self, image, boxes):
        a = np.asarray(image.convert("RGB"), dtype=np.float32)
        n = random.randint(3, 15)
        ang = math.radians(random.gauss(0, 25))
        acc = np.zeros_like(a)
        for i in range(n):
            t = i - (n - 1) / 2
            acc += np.roll(a, (round(t * math.sin(ang)), round(t * math.cos(ang))), axis=(0, 1))
        return Image.fromarray((acc / n + 0.5).astype(np.uint8)), boxes


class GaussianBlur(_Prob):
    def apply(self, image, boxes):
        return image.filter(ImageFilter.GaussianBlur(_rand(0.5, 2.0))), boxes


class JpegArtifacts(_Prob):
    def apply(self, image, boxes):
        buf = io.BytesIO()
        image.convert("RGB").save(buf, "JPEG", quality=random.randint(20, 70))
        buf.seek(0)
        return Image.open(buf).convert("RGB"), boxes


def _clip(boxes, min_size: float = 0.002):
    """Clip boxes to the image; drop the ones left thinner than ~1 px at 640 or mostly outside."""
    if boxes.numel() == 0:
        return boxes
    w0 = (boxes[:, 3] - boxes[:, 1]).clamp(min=1e-9)
    h0 = (boxes[:, 4] - boxes[:, 2]).clamp(min=1e-9)
    boxes = boxes.clone()
    boxes[:, 1:5] = boxes[:, 1:5].clamp(0, 1)
    w, h = boxes[:, 3] - boxes[:, 1], boxes[:, 4] - boxes[:, 2]
    keep = (w > min_size) & (h > min_size) & (w * h > 0.4 * w0 * h0)
    return boxes[keep]


class ClipBoxes:
    def __init__(self, prob: float = 1):
        pass

    def __call__(self, image, boxes):
        return image, _clip(boxes)


AUGMENTATIONS = [PlateMosaic, ZoomOut, ColorJitter, LowLight, MotionBlur, GaussianBlur, JpegArtifacts, ClipBoxes]


def register():
    import yolo.tools.data_loader as dl

    for cls in AUGMENTATIONS:
        setattr(dl, cls.__name__, cls)
