"""JSON worker/CLI. Native driver calls run outside the Flet UI process."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import platform
import sys
from pathlib import Path

from assessment import (SOURCES, assess, annotate_fsr_assessment, default_source, default_target,
                        report_markdown, validate_settings)


def run(mode="scan", target=None, source=None, mib=16, samples=20, fps=60, resolution="1080p", artifact_dir=None):
    from vk_native import Device, Vulkan, roundtrip
    validate_settings(mib, samples, fps)
    report = {"schema_version": 1, "captured_at": datetime.now(timezone.utc).isoformat(),
              "platform": platform.platform(), "python": platform.python_version(),
              "mode": mode, "devices": [], "tests": {}, "sources": SOURCES,
              "fsr_runtime_executed": False}
    try:
        with Vulkan(debug_utils=mode == "fsr") as vk:
            devices = vk.inventory()
            report["devices"] = devices
            target = target or default_target(devices)
            source = default_source(devices, target) if source is None else None if source == "none" else source
            selected = next((g for g in devices if g["uuid"] == target), None)
            other = next((g for g in devices if g["uuid"] == source), None)
            report.update(target_uuid=target, source_uuid=source)
            if selected is None:
                report["selection_message"] = "No AMD integrated GPU auto-selected. Select a processing GPU explicitly."
                if mode in ("test", "fsr"):
                    report["error"] = "Requested processing GPU was not found. Refresh the hardware list."
                return report
            report["assessment"] = assess(selected)
            if mode == "fsr":
                from fsr_benchmark import benchmark
                if other is None:
                    report["tests"]["fsr"] = {"status": "failed", "runtime_executed": False,
                        "detail": "Select a second physical GPU for the actual FSR comparison."}
                else:
                    result = benchmark(vk, selected, other, resolution, samples, fps, artifact_dir)
                    report["tests"]["fsr"] = result
                    report["fsr_runtime_executed"] = result["runtime_executed"]
                    annotate_fsr_assessment(report["assessment"], result)
            if mode == "test":
                try:
                    with Device(vk, selected) as device:
                        report["tests"]["device"] = {"status": "passed", "gpu_uuid": selected["uuid"],
                            "detail": "Created a logical device and compute queue on " + selected["name"],
                            "enabled_extensions": device.enabled_extensions,
                            "entry_points": device.functions_available()}
                        report["tests"]["compute"] = device.compute_smoke()
                except Exception as exc:
                    key = "compute" if "device" in report["tests"] else "device"
                    report["tests"][key] = {"status": "failed", "detail": str(exc)}
                if other is None:
                    report["tests"]["transfer"] = {"status": "skipped", "detail": "Select a second physical GPU for the transfer test."}
                elif other["uuid"] == selected["uuid"]:
                    report["tests"]["transfer"] = {"status": "failed", "detail": "Transfer test requires two different physical GPUs."}
                else:
                    try:
                        report["tests"]["transfer"] = roundtrip(vk, other, selected, mib, samples, fps)
                    except Exception as exc:
                        report["tests"]["transfer"] = {"status": "failed", "detail": str(exc)}
    except Exception as exc:
        report["error"] = str(exc)
    return report


def main():
    parser = argparse.ArgumentParser(description="Vega FSR preflight: native Vulkan scan and optional generic GPU tests.")
    parser.add_argument("--mode", choices=["scan", "test", "fsr"], default="scan")
    parser.add_argument("--target", help="Processing GPU UUID (defaults to AMD integrated).")
    parser.add_argument("--source", help="Rendering GPU UUID (defaults to NVIDIA discrete).")
    parser.add_argument("--mib", type=int, choices=[4, 16, 32, 64], default=16)
    parser.add_argument("--samples", type=int, choices=[10, 20, 40], default=20)
    parser.add_argument("--fps", type=int, choices=[30, 60, 120], default=60)
    parser.add_argument("--resolution", choices=["720p", "1080p", "1440p", "smoke"], default="1080p")
    parser.add_argument("--artifacts", type=Path, help="Save synthetic FSR output previews in this local directory.")
    parser.add_argument("--output", type=Path, help="Also save JSON and Markdown locally.")
    args = parser.parse_args()
    artifacts = args.artifacts or (args.output.with_suffix("") / "previews" if args.output and args.mode == "fsr" else None)
    report = run(args.mode, args.target, args.source, args.mib, args.samples, args.fps, args.resolution, artifacts)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
        args.output.with_suffix(".md").write_text(report_markdown(report), encoding="utf-8")
    print(json.dumps(report))
    failed = report.get("error") or any(t["status"] == "failed" for t in report["tests"].values())
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
