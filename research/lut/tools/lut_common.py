"""Shared helpers for the learned-appearance research tools (research/lut). Research only.

Nothing here is shipped or imported by Lucent. The tools reproduce Image-Adaptive-3DLUT
(blueprint Appendix D2) from a pinned, unmodified checkout under research/third_party.

Hash conventions used in every results file:
  - file:  SHA-256 of the file's bytes.
  - array: SHA-256 of the C-contiguous, little-endian bytes of the array, recorded together with
           its dtype string (numpy ``dtype.str``, e.g. ``<f4``) and shape. Tensors are hashed
           through ``tensor.detach().cpu().numpy()``.
"""

from __future__ import annotations

import hashlib
import importlib.machinery
import importlib.util
import json
import os
import platform
import struct
import subprocess
import sys
import types
from pathlib import Path
from typing import Any

# Importing upstream modules from the checkout must not write __pycache__ into it: the checkout is
# verified to be byte-identical to the pinned commit before and after every run.
sys.dont_write_bytecode = True

LUT_ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = LUT_ROOT.parents[1]
UPSTREAM_URL = "https://github.com/HuiZeng/Image-Adaptive-3DLUT"
PINNED_COMMIT = "b491f6df64a588864739a157db271e5c848e1805"
BUILD_DIR = LUT_ROOT / "build"
RESULTS_DIR = LUT_ROOT / "results"
EXTENSION_LIB_DIR = BUILD_DIR / "trilinear_ext" / "lib"
EXTENSION_BUILD_LOG = BUILD_DIR / "trilinear_ext" / "build.log"

# binsize as written in the two upstream model files (dim = LUT size, 33 for the sRGB model).
BINSIZE_MODELS_X = lambda dim: 1.000001 / (dim - 1)  # noqa: E731  models_x.py, PyTorch 1.x path
BINSIZE_MODELS_LEGACY = lambda dim: 1.0001 / (dim - 1)  # noqa: E731  models.py, PyTorch 0.4.1 path


def checkout_path() -> Path:
    override = os.environ.get("LUCENT_LUT_CHECKOUT")
    if override:
        return Path(override).resolve()
    return REPO_ROOT / "research" / "third_party" / "Image-Adaptive-3DLUT"


def git(checkout: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(checkout), *args], check=True, capture_output=True, text=True
    ).stdout.strip()


def verify_checkout(checkout: Path) -> dict[str, Any]:
    """Fails unless the checkout is exactly the pinned commit with no local or ignored files."""
    if not (checkout / ".git").exists():
        raise SystemExit(
            f"No checkout at {checkout}. Clone {UPSTREAM_URL} there and check out {PINNED_COMMIT} "
            "(research/lut/README.md, 'Setup')."
        )
    head = git(checkout, "rev-parse", "HEAD")
    if head != PINNED_COMMIT:
        raise SystemExit(f"{checkout} is at {head}, expected {PINNED_COMMIT}.")
    status = git(checkout, "status", "--porcelain", "--ignored")
    if status:
        raise SystemExit(f"{checkout} has local changes or untracked files:\n{status}")
    return {
        "repository": UPSTREAM_URL,
        "commit": head,
        "commit_date": git(checkout, "log", "-1", "--format=%cI"),
        "clean": True,
    }


def assert_checkout_unchanged(checkout: Path) -> None:
    status = git(checkout, "status", "--porcelain", "--ignored")
    if status:
        raise RuntimeError(f"The run modified the pinned checkout:\n{status}")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def array_record(a: Any) -> dict[str, Any]:
    import numpy as np

    if hasattr(a, "detach"):
        a = a.detach().cpu().numpy()
    a = np.ascontiguousarray(a)
    if a.dtype.byteorder == ">" or (a.dtype.byteorder == "=" and sys.byteorder != "little"):
        a = a.astype(a.dtype.newbyteorder("<"))
    return {"sha256": sha256_bytes(a.tobytes()), "dtype": a.dtype.str, "shape": list(a.shape)}


def state_dict_record(state_dict: dict[str, Any]) -> dict[str, Any]:
    """Per-tensor records plus one digest over the sorted (name, dtype, shape, bytes) sequence."""
    import numpy as np

    combined = hashlib.sha256()
    tensors: dict[str, Any] = {}
    for name in sorted(state_dict):
        arr = np.ascontiguousarray(state_dict[name].detach().cpu().numpy())
        rec = array_record(arr)
        tensors[name] = rec
        combined.update(f"{name}|{rec['dtype']}|{rec['shape']}\n".encode())
        combined.update(arr.tobytes())
    return {"sha256": combined.hexdigest(), "tensors": tensors}


def float32_bits(x: float) -> str:
    """Exact float32 bit pattern, e.g. '0x3f800000' for 1.0."""
    return "0x%08x" % struct.unpack("<I", struct.pack("<f", x))[0]


def environment() -> dict[str, Any]:
    import numpy as np
    import PIL
    import torch
    import torchvision
    from PIL import features

    return {
        "python": sys.version.split()[0],
        "python_build": platform.python_compiler(),
        "os": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "logical_cpus": os.cpu_count(),
        "torch": torch.__version__,
        "torch_git": torch.version.git_version,
        "torch_cuda_available": torch.cuda.is_available(),
        "torch_cpu_capability": torch.backends.cpu.get_cpu_capability(),
        "torch_num_threads": torch.get_num_threads(),
        "torch_mkldnn": torch.backends.mkldnn.is_available(),
        "torchvision": torchvision.__version__,
        "numpy": np.__version__,
        "pillow": PIL.__version__,
        "libjpeg_turbo": features.version("libjpeg_turbo"),
    }


def write_json(path: Path, obj: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(obj, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def load_extension(lib_dir: Path = EXTENSION_LIB_DIR) -> types.ModuleType | None:
    """Loads the built upstream CPU extension (research/lut/tools/build_trilinear_ext.ps1).

    It is loaded under its real module name (``trilinear``, required by its PyInit symbol) and
    removed from ``sys.modules`` again, so that tests can install other backends under that name.
    Returns None when it has not been built.
    """
    import torch  # noqa: F401  (registers torch's DLL directory before the extension loads)

    candidates = sorted(lib_dir.glob("trilinear*.pyd")) + sorted(lib_dir.glob("trilinear*.so"))
    if not candidates:
        return None
    path = candidates[0]
    saved = sys.modules.pop("trilinear", None)
    try:
        loader = importlib.machinery.ExtensionFileLoader("trilinear", str(path))
        spec = importlib.util.spec_from_file_location("trilinear", str(path), loader=loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
    finally:
        sys.modules.pop("trilinear", None)
        if saved is not None:
            sys.modules["trilinear"] = saved
    module.__lucent_path__ = str(path)  # type: ignore[attr-defined]
    return module


def extension_build_record(module: types.ModuleType | None) -> dict[str, Any]:
    """What was built and how: the module hash and the compiler and linker lines from build.log."""
    if module is None:
        return {"built": False}
    path = Path(module.__lucent_path__)  # type: ignore[attr-defined]
    record: dict[str, Any] = {
        "built": True,
        "module": path.name,
        "module_sha256": sha256_file(path),
        "source": "trilinear_cpp/src/trilinear.cpp at the pinned commit, unmodified, through the "
        "repository's trilinear_cpp/setup.py (CppExtension, no CUDA)",
    }
    if EXTENSION_BUILD_LOG.exists():
        lines = EXTENSION_BUILD_LOG.read_text(encoding="utf-8", errors="replace").splitlines()
        compile_line = next((ln for ln in lines if ln.lstrip().startswith(("[1/1] cl ", "cl "))), "")
        flags = [t for t in compile_line.split() if t.startswith("/") and not t.startswith("/Fo")]
        record["compile_flags"] = [f for f in flags if not f.startswith("/showIncludes")]
        record["fp_model"] = next((f for f in flags if f.startswith("/fp:")), "/fp:precise (MSVC default)")
        record["arch"] = next((f for f in flags if f.startswith("/arch:")), "none (x64 SSE2 baseline)")
        record["warnings"] = sorted({ln.split(": warning ")[1].split(":")[0] for ln in lines if ": warning " in ln})
        msvc = next((ln for ln in lines if "\\MSVC\\" in ln), "")
        if "\\MSVC\\" in msvc:
            record["msvc_toolset"] = msvc.split("\\MSVC\\")[1].split("\\")[0]
    return record


class XyzOnlyStub(types.ModuleType):
    """Stands in for a module that upstream imports at module level but uses only on its XYZ path.

    demo_eval.py imports cv2 and torchvision_x_functional (which itself needs OpenCV and SciPy)
    unconditionally, but the sRGB branch never touches them. Any attribute access other than
    import-system dunders raises, so a run that completes proves the sRGB path did not use them.
    """

    def __getattr__(self, name: str) -> Any:
        if name.startswith("__") and name.endswith("__"):
            raise AttributeError(name)
        raise RuntimeError(f"{self.__name__}.{name} was used, but the sRGB path must not need it")


def install_xyz_stubs() -> list[str]:
    names = ["cv2", "torchvision_x_functional"]
    for name in names:
        sys.modules[name] = XyzOnlyStub(name)
    return names
