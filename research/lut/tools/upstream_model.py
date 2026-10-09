"""Loads the upstream sRGB model exactly as demo_eval.py builds it, for analysis tools. Research only.

``models_x.Classifier`` and ``Generator3DLUT_identity``/``Generator3DLUT_zero`` come from the pinned
checkout itself; ``trilinear`` is the pure-PyTorch ExtensionApi (the classifier does not call it).
``fuse`` is demo_eval.generate_LUT: pred[0]*LUT0 + pred[1]*LUT1 + pred[2]*LUT2 with no softmax.
reproduce_demo.py shows that this path gives the same fused LUT as running demo_eval.py itself.
"""

from __future__ import annotations

import contextlib
import os
import sys
from dataclasses import dataclass
from pathlib import Path

import lut_common as lc
import torch
import trilinear_torch as tt
from PIL import Image


@dataclass
class UpstreamModel:
    classifier: torch.nn.Module
    luts: list[torch.Tensor]  # three (3, 33, 33, 33) float32 basis LUTs, [c][b][g][r]
    models_x: object


@contextlib.contextmanager
def _upstream_import_context(checkout: Path):
    saved_cwd, saved_path = os.getcwd(), list(sys.path)
    saved = {name: sys.modules.pop(name, None) for name in ("models_x", "trilinear")}
    sys.modules["trilinear"] = tt.ExtensionApi()
    try:
        os.chdir(checkout)  # Generator3DLUT_identity opens IdentityLUT33.txt relative to the cwd
        sys.path.insert(0, str(checkout))
        yield
    finally:
        os.chdir(saved_cwd)
        sys.path[:] = saved_path
        for name, module in saved.items():
            sys.modules.pop(name, None)
            if module is not None:
                sys.modules[name] = module


def load(checkout: Path | None = None) -> UpstreamModel:
    checkout = checkout or lc.checkout_path()
    lc.verify_checkout(checkout)
    with _upstream_import_context(checkout):
        import models_x  # noqa: PLC0415  (the checkout's module, imported fresh)

        classifier = models_x.Classifier()
        classifier.load_state_dict(
            torch.load("pretrained_models/sRGB/classifier.pth", map_location="cpu", weights_only=True))
        classifier.eval()
        generators = [models_x.Generator3DLUT_identity(), models_x.Generator3DLUT_zero(), models_x.Generator3DLUT_zero()]
        states = torch.load("pretrained_models/sRGB/LUTs.pth", map_location="cpu", weights_only=True)
        for i, gen in enumerate(generators):
            gen.load_state_dict(states[str(i)])
            gen.eval()
    lc.assert_checkout_unchanged(checkout)
    return UpstreamModel(classifier, [g.LUT.detach().clone() for g in generators], models_x)


def load_image(path: Path) -> torch.Tensor:
    """demo_eval's sRGB input: PIL decode, torchvision to_tensor (uint8 / 255 in float32), batch of one."""
    import torchvision.transforms.functional as TF

    return TF.to_tensor(Image.open(path)).type(torch.FloatTensor).unsqueeze(0)


def fuse(model: UpstreamModel, img: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    with torch.no_grad():
        pred = model.classifier(img).squeeze()
        lut = pred[0] * model.luts[0] + pred[1] * model.luts[1] + pred[2] * model.luts[2]
    return pred, lut
