#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Creates .venv/ (git-ignored) and checks out the MIT YOLO code at a pinned commit into third_party/YOLO/.
# Nothing is installed system-wide.
#
#   ./setup.sh            # CUDA build of PyTorch (cu128: needed for an RTX 50xx / Blackwell, also fine on older GPUs)
#   ./setup.sh --cpu      # CPU-only PyTorch (small; what the smoke test used)
set -euo pipefail
cd "$(dirname "$0")"

YOLO_REPO=https://github.com/MultimediaTechLab/YOLO
YOLO_COMMIT=c4cb5f6f56102eceeaa7d75e23e1125cd0373eaf   # "Release/v1.0 (#225)", 2025-12-30, MIT
TORCH_INDEX=https://download.pytorch.org/whl/cu128
[[ "${1:-}" == "--cpu" ]] && TORCH_INDEX=https://download.pytorch.org/whl/cpu

if [[ ! -d third_party/YOLO/.git ]]; then
    git clone --quiet "$YOLO_REPO" third_party/YOLO
fi
git -C third_party/YOLO fetch --quiet origin "$YOLO_COMMIT" 2>/dev/null || true
git -C third_party/YOLO checkout --quiet "$YOLO_COMMIT"
grep -q "^MIT License" third_party/YOLO/LICENSE || { echo "third_party/YOLO is not MIT-licensed any more: stop" >&2; exit 1; }

if command -v uv >/dev/null; then
    [[ -d .venv ]] || uv venv --quiet --python 3.12 .venv
    PIP=(uv pip install --quiet --python .venv/bin/python)
else
    [[ -d .venv ]] || python3 -m venv .venv
    .venv/bin/python -m pip install --quiet --upgrade pip
    PIP=(.venv/bin/python -m pip install --quiet)
fi

"${PIP[@]}" --index-url "$TORCH_INDEX" "torch==2.9.1" "torchvision==0.24.1"
"${PIP[@]}" -r requirements.txt
# the YOLO package itself, without its requirements.txt (that one pulls opencv-python with GUI libs and wandb's
# server bits we don't use; requirements.txt here lists what the code actually imports)
"${PIP[@]}" --no-deps -e third_party/YOLO

.venv/bin/python - <<'EOF'
import torch, onnxruntime, yolo
print(f"torch {torch.__version__}  cuda={torch.cuda.is_available()}  onnxruntime {onnxruntime.__version__}  yolo at {yolo.__path__[0]}")
EOF
