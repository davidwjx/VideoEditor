#!/usr/bin/env python3
"""DirectML probe for the M4 AI model gate.

The probe opens one ONNX graph at a time (read-only, no network), builds a CPU
session and a DirectML session with the M4 session options, feeds identical
deterministic inputs to both, and reports:

  * whether each session was created,
  * CPU and DirectML p50 latency,
  * the maximum relative difference between the two output sets.

It is intentionally a spike tool: it does not implement the tracking or
inpainting pipelines, it only proves that the graphs can be created and that
DirectML agrees with the CPU reference on the same inputs.

Usage (run from an isolated venv, never from the model directory):

  python -I tools/m4_spike/dml_probe.py --model C:/m4-spike/models/x.onnx
  python -I tools/m4_spike/dml_probe.py --model ... --shape image=1,3,512,512 \
      --shape mask=1,1,512,512 --runs 5 --json

The deterministic inputs are seeded uniform noise for float tensors, random
bytes/binary masks for uint8 tensors, and zeros for integer/bool tensors. Zeros
would make several of these graphs produce trivial outputs, which hides
numerical differences, hence the seeded-random default.
"""

from __future__ import annotations

import argparse
import json
import sys
import time

import numpy as np
import onnxruntime as ort

DTYPE_MAP = {
    "tensor(float)": np.float32,
    "tensor(float16)": np.float16,
    "tensor(double)": np.float64,
    "tensor(uint8)": np.uint8,
    "tensor(int8)": np.int8,
    "tensor(uint16)": np.uint16,
    "tensor(int16)": np.int16,
    "tensor(uint32)": np.uint32,
    "tensor(int32)": np.int32,
    "tensor(uint64)": np.uint64,
    "tensor(int64)": np.int64,
    "tensor(bool)": np.bool_,
}


def parse_shape_overrides(entries: list[str] | None) -> dict[str, list[int]]:
    overrides: dict[str, list[int]] = {}
    for entry in entries or []:
        if "=" not in entry:
            raise argparse.ArgumentTypeError(
                f"--shape expects name=d0,d1,... (got {entry!r})"
            )
        name, dims = entry.split("=", 1)
        overrides[name] = [int(v) for v in dims.split(",") if v != ""]
    return overrides


def make_session(
    path: str, provider: str, device: int = 0, log_severity: int = 3
) -> ort.InferenceSession:
    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    so.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    so.enable_mem_pattern = False
    so.enable_cpu_mem_arena = False
    so.log_severity_level = log_severity
    if provider == "dml":
        providers = [("DmlExecutionProvider", {"device_id": device})]
    else:
        providers = ["CPUExecutionProvider"]
    return ort.InferenceSession(path, so, providers=providers)


def feeds_for(
    session: ort.InferenceSession,
    shape_overrides: dict[str, list[int]],
    seed: int,
) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    feeds: dict[str, np.ndarray] = {}
    for inp in session.get_inputs():
        override = shape_overrides.get(inp.name)
        if override is not None:
            dims = list(override)
        else:
            dims = [d if isinstance(d, int) else 1 for d in inp.shape]
        dtype = DTYPE_MAP.get(inp.type)
        if dtype is None:
            raise ValueError(f"unsupported input type {inp.type} for {inp.name}")
        if dtype == np.float32 or dtype == np.float64:
            data = rng.random(dims, dtype=np.float64).astype(dtype)
        elif dtype == np.float16:
            data = rng.random(dims, dtype=np.float32).astype(np.float16)
        elif dtype == np.uint8:
            if "mask" in inp.name.lower():
                # A sparse binary mask exercises the inpainting code paths;
                # an all-zero mask would short-circuit most of the graph.
                data = ((rng.random(dims) < 0.1) * 255).astype(np.uint8)
            else:
                data = rng.integers(0, 256, size=dims, dtype=np.uint8)
        else:
            data = np.zeros(dims, dtype=dtype)
        feeds[inp.name] = np.ascontiguousarray(data)
    return feeds


def run(
    session: ort.InferenceSession, feeds: dict[str, np.ndarray], runs: int
) -> tuple[list[np.ndarray], list[float]]:
    times: list[float] = []
    outputs: list[np.ndarray] = []
    for _ in range(runs):
        t0 = time.perf_counter()
        outputs = session.run(None, feeds)
        times.append((time.perf_counter() - t0) * 1000.0)
    return outputs, times


def p50(values: list[float]) -> float:
    return float(np.median(values))


def relative_diff(reference: list[np.ndarray], candidate: list[np.ndarray]) -> dict:
    worst = 0.0
    abs_worst = 0.0
    mismatched_shape = False
    for a, b in zip(reference, candidate):
        a = np.asarray(a, dtype=np.float32)
        b = np.asarray(b, dtype=np.float32)
        if a.shape != b.shape:
            mismatched_shape = True
            continue
        denom = max(1e-6, float(np.max(np.abs(a))))
        abs_worst = max(abs_worst, float(np.max(np.abs(a - b))))
        worst = max(worst, float(np.max(np.abs(a - b))) / denom)
    return {
        "maxRelativeDiff": worst,
        "maxAbsDiff": abs_worst,
        "shapeMismatch": mismatched_shape,
    }


def error_info(exc: BaseException) -> dict:
    info = {"type": type(exc).__name__, "message": str(exc)}
    winerror = getattr(exc, "winerror", None)
    if winerror is not None:
        info["winerror"] = winerror
    return info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, help="path to a .onnx file")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--device", type=int, default=0)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--log-severity",
        type=int,
        default=3,
        help="ORT log severity: 0=verbose, 1=info, 2=warning, 3=error",
    )
    parser.add_argument(
        "--shape",
        action="append",
        default=[],
        help="override an input shape, e.g. image=1,3,512,512 (repeatable)",
    )
    parser.add_argument("--json", action="store_true", help="emit one JSON object")
    args = parser.parse_args()

    result: dict = {
        "model": args.model,
        "runs": args.runs,
        "device": args.device,
        "providersAvailable": ort.get_available_providers(),
        "ortVersion": ort.__version__,
        "numpyVersion": np.__version__,
        "cpu": {},
        "dml": {},
    }
    shape_overrides = parse_shape_overrides(args.shape)

    # CPU reference.
    cpu_out = None
    try:
        cpu = make_session(args.model, "cpu", log_severity=args.log_severity)
        result["cpu"]["session"] = "ok"
        result["cpu"]["providers"] = cpu.get_providers()
        result["cpu"]["inputs"] = [
            {"name": i.name, "type": i.type, "shape": i.shape} for i in cpu.get_inputs()
        ]
        result["cpu"]["outputs"] = [
            {"name": o.name, "type": o.type, "shape": o.shape}
            for o in cpu.get_outputs()
        ]
        feeds = feeds_for(cpu, shape_overrides, args.seed)
        for _ in range(args.warmup):
            cpu.run(None, feeds)
        cpu_out, cpu_t = run(cpu, feeds, args.runs)
        result["cpu"]["p50Ms"] = p50(cpu_t)
        result["cpu"]["minMs"] = min(cpu_t)
        result["cpu"]["maxMs"] = max(cpu_t)
    except Exception as exc:  # noqa: BLE001 - the probe records failures verbatim
        result["cpu"]["session"] = "failed"
        result["cpu"]["error"] = error_info(exc)
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            print(f"CPU FAILED: {error_info(exc)}")
        return 1

    # DirectML.
    dml_out = None
    try:
        dml = make_session(
            args.model, "dml", args.device, log_severity=args.log_severity
        )
        result["dml"]["session"] = "ok"
        result["dml"]["providers"] = dml.get_providers()
        # ORT silently drops the DML EP when the requested device is unusable
        # (for example the Microsoft Basic Render Driver) and returns a CPU-only
        # session. A missing DML provider is a DML failure, not a success.
        if "DmlExecutionProvider" not in result["dml"]["providers"]:
            raise RuntimeError(
                "DML provider not active after session creation; "
                f"providers={result['dml']['providers']}"
            )
        # Reuse the CPU session's inputs so both providers see the same tensors;
        # the DML session must declare the same input names.
        dml_feeds = {
            inp.name: feeds[inp.name]
            for inp in dml.get_inputs()
            if inp.name in feeds
        }
        missing = [i.name for i in dml.get_inputs() if i.name not in feeds]
        if missing:
            raise RuntimeError(f"DML session inputs missing from CPU feed: {missing}")
        for _ in range(args.warmup):
            dml.run(None, dml_feeds)
        dml_out, dml_t = run(dml, dml_feeds, args.runs)
        result["dml"]["p50Ms"] = p50(dml_t)
        result["dml"]["minMs"] = min(dml_t)
        result["dml"]["maxMs"] = max(dml_t)
    except Exception as exc:  # noqa: BLE001 - record HRESULT/session/run errors
        result["dml"]["session"] = "failed"
        result["dml"]["error"] = error_info(exc)
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            print(f"CPU provider={result['cpu']['providers']} "
                  f"p50={result['cpu']['p50Ms']:.2f}ms")
            print(f"DML FAILED: {error_info(exc)}")
        return 2

    result["parity"] = relative_diff(cpu_out, dml_out)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(f"CPU provider={result['cpu']['providers']} "
              f"p50={result['cpu']['p50Ms']:.2f}ms")
        print(f"DML provider={result['dml']['providers']} "
              f"p50={result['dml']['p50Ms']:.2f}ms")
        print(f"max_relative_output_diff={result['parity']['maxRelativeDiff']:.6f}")
        print(f"max_abs_output_diff={result['parity']['maxAbsDiff']:.6f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
