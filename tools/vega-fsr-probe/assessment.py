"""Conservative preflight assessments; never turn a feature bit into an FSR pass."""
from __future__ import annotations

AMD_SDK = "https://gpuopen.com/amd-fsr-sdk/"
AMD_GUIDE = "https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/"
AMD_FSR1 = "https://gpuopen.com/fidelityfx-superresolution/"
AMD_FSR2 = "https://github.com/GPUOpen-Effects/FidelityFX-FSR2"
SOURCES = [
    {"title": "AMD FSR SDK: requirements and supported hardware", "url": AMD_SDK},
    {"title": "AMD FSR temporal upscaler integration guide", "url": AMD_GUIDE},
    {"title": "AMD FSR 1 overview", "url": AMD_FSR1},
    {"title": "AMD FSR 2 source and documentation", "url": AMD_FSR2},
]


def is_cezanne(gpu):
    return gpu["vendor_id"] == 0x1002 and gpu["device_id"] == 0x1638 and gpu["device_type"] == "integrated"


def default_target(devices):
    preferred = [d for d in devices if is_cezanne(d)]
    preferred += [d for d in devices if d["vendor_id"] == 0x1002 and d["device_type"] == "integrated"]
    return preferred[0]["uuid"] if preferred else None


def default_source(devices, target):
    preferred = [d for d in devices if d["uuid"] != target and d["vendor_id"] == 0x10de and d["device_type"] == "discrete"]
    return preferred[0]["uuid"] if preferred else None


def assess(gpu):
    features, formats = gpu["features"], gpu["formats"]
    compute = any(q["compute"] and q["count"] for q in gpu["queues"])
    rgba8 = formats.get("RGBA8_UNORM", {})
    unorm16 = formats.get("RGBA16_UNORM", {})
    float16 = formats.get("RGBA16_SFLOAT", {})
    api12 = gpu["api_version_raw"] >= ((1 << 22) | (2 << 12))
    checks = [
        {"name": "Compute queue", "pass": compute, "required": True, "detail": "A separate device needs a compute-capable queue."},
        {"name": "Vulkan 1.2 baseline", "pass": api12, "required": True, "detail": "Baseline for the current AMD FSR SDK Vulkan route."},
        {"name": "RGBA16 UNORM storage image", "pass": unorm16.get("storage_image", False), "required": True,
         "detail": "FSR 2.3.4 / 3.1.6 SDK requirement: VK_FORMAT_R16G16B16A16_UNORM storage-image support."},
        {"name": "Extended storage-image formats", "pass": features.get("shaderStorageImageExtendedFormats", False),
         "required": True, "detail": "Relevant to typed storage-image reads/writes; exact shader permutations still need execution."},
        {"name": "RGBA16 float sampled / storage", "pass": bool(float16.get("sampled_image") and float16.get("storage_image")),
         "required": True, "detail": "Working color format for the planned stage."},
        {"name": "Compute subgroups", "pass": gpu.get("subgroup_compute", False), "required": False,
         "detail": f"Wave size {gpu.get('subgroup_size', '?')}; operations: {', '.join(gpu.get('subgroup_operation_names', []))}."},
        {"name": "FP16 shader arithmetic", "pass": features.get("shaderFloat16", False), "required": False,
         "detail": "Optional optimization. AMD documents an FP32 fallback; absence alone does not rule out FSR."},
        {"name": "16-bit storage buffers", "pass": features.get("storageBuffer16BitAccess", False), "required": False,
         "detail": "Reported independently from FP16 arithmetic."},
        {"name": "Timeline semaphores", "pass": features.get("timelineSemaphore", False), "required": False,
         "detail": "Useful for scheduling. This diagnostic's GPU tests use ordinary fences."},
        {"name": "Win32 memory / semaphore extensions", "pass": all(e in gpu["extensions"] for e in
             ["VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"]), "required": False,
         "detail": "Advertised interop support; cross-vendor resource import has not been tested."},
        {"name": "Device LUID", "pass": gpu.get("luid_valid", False), "required": False,
         "detail": "Allows matching this physical GPU to a DXGI/D3D12 adapter."},
    ]
    missing = [c["name"] for c in checks if c["required"] and not c["pass"]]
    candidate = not missing
    candidate_status = "candidate" if candidate else "missing_prerequisites"
    reason = ("Inspected preflight checks pass. Full shader requirements, FSR context creation and image quality remain untested."
              if candidate else "Missing inspected prerequisites: " + ", ".join(missing))
    spatial_ok = compute and rgba8.get("sampled_image", False) and rgba8.get("storage_image", False)
    fsr = [
        {"name": "FSR 1 spatial upscaling", "status": "candidate" if spatial_ok else "missing_prerequisites",
         "runtime_tested": False, "detail": "Basic sampled/storage and compute checks only. No temporal inputs needed; FSR shader not executed.",
         "source": AMD_FSR1},
        {"name": "FSR 2 temporal upscaling", "status": candidate_status, "runtime_tested": False,
         "detail": reason + " Needs depth, motion, jitter and history.", "source": AMD_FSR2},
        {"name": "FSR 3.1 upscaling", "status": candidate_status, "runtime_tested": False,
         "detail": reason + " This is the proposed Vega stage; separate from frame generation.", "source": AMD_SDK},
        {"name": "FSR 3.1 frame generation", "status": "outside_published_range" if is_cezanne(gpu) else "not_assessed",
         "runtime_tested": False, "detail": "AMD lists RX 5000-class hardware or equivalent for frame generation. "
         "The 5700G Vega iGPU is not in that listed range. No frame-generation workload is tested.", "source": AMD_SDK},
        {"name": "FSR 4 ML upscaling", "status": "not_officially_supported" if is_cezanne(gpu) else "not_assessed",
         "runtime_tested": False, "detail": "5700G Vega is absent from AMD's FSR 4 supported hardware list. "
         "Newer supported APUs do not imply Vega support; an SDK fallback to FSR 3.1 is not FSR 4.", "source": AMD_SDK},
    ]
    return {"gpu_uuid": gpu["uuid"], "identity": "Cezanne / Vega integrated GPU (5700G family)" if is_cezanne(gpu) else gpu["name"],
            "preflight_status": candidate_status, "checks": checks, "fsr": fsr,
            "conclusion": "FSR 3.1 is a hardware candidate; runtime and net speedup unverified." if candidate
                          else "Resolve the missing preflight checks before testing this FSR 3.1 route.",
            "rules_checked_date": "2026-11-05"}


def validate_settings(mib, samples, fps):
    if mib not in (4, 16, 32, 64):
        raise ValueError("Payload must be 4, 16, 32 or 64 MiB per direction.")
    if samples not in (10, 20, 40):
        raise ValueError("Samples must be 10, 20 or 40.")
    if fps not in (30, 60, 120):
        raise ValueError("Target FPS must be 30, 60 or 120.")


def annotate_fsr_assessment(assessment, benchmark):
    target = benchmark.get("target_fsr", {})
    if (benchmark.get("target_uuid") == assessment["gpu_uuid"] and benchmark.get("runtime_executed")
            and target.get("status") == "passed" and target.get("output_validation", {}).get("finite")):
        assessment["fsr"][2].update(status="runtime_passed", runtime_tested=True,
            detail="Official FSR 3.1.3 context, dispatch and synthetic output validation passed on this GPU. "
                   "Other FSR versions, game integration and general image quality remain untested.")
        assessment["conclusion"] = "FSR 3.1.3 executed on the processing GPU; see the measured full-path comparison."
    return assessment


def fsr_markdown(result):
    lines = ["", "## Actual FSR comparison", "", f"Status: {result['status']}", "", result.get("detail", ""), ""]
    if result.get("error"):
        lines += ["Error: " + result["error"], "Phase: " + result.get("phase", "unknown"), ""]
    if result.get("runtime"):
        runtime = result["runtime"]
        lines += [f"Provider: **FSR {runtime['provider']['name']}**, explicitly selected; no fallback.",
                  f"Runtime SHA-256: `{runtime['sha256']}`", f"SDK commit: `{runtime['sdk_commit']}`", ""]
    if "input_size" in result:
        lines += [f"Input {result['input_size'][0]}x{result['input_size'][1]} → output "
                  f"{result['output_size'][0]}x{result['output_size'][1]}; "
                  f"{result['samples']} measured samples after {result['warmups']} warmups per path.", "",
                  "| Path | Median wall ms | p95 wall ms | Median FSR GPU ms |",
                  "| --- | --- | --- | --- |"]
        for key, label, gpu_key in [("source_fsr", "Rendering GPU alone", "gpu"),
                                   ("target_fsr", "Processing GPU alone", "gpu"),
                                   ("offload", "Full offload path", "amd_fsr_gpu")]:
            if key in result:
                row = result[key]
                lines.append(f"| {label} | {row['wall']['median_ms']:.4f} | {row['wall']['p95_ms']:.4f} "
                             f"| {row[gpu_key]['median_ms']:.4f} |")
    comparison = result.get("comparison")
    if comparison:
        lines += ["", f"**Verdict: {comparison['verdict']}.** Offload/direct wall-time ratio: "
                  f"{comparison['offload_to_direct_ratio']:.2f}x; offload minus direct: "
                  f"{comparison['offload_minus_direct_ms']:+.4f} ms.", ""]
    if "offload" in result:
        row = result["offload"]
        lines += [f"Input transport {row['input_bytes']:,} bytes; output transport {row['output_bytes']:,} bytes. "
                  f"Every returned image byte verified: {row['all_returned_bytes_verified']}.", ""]
    for key in ("cross_gpu_image_comparison", "offload_image_comparison"):
        if key in result:
            row = result[key]
            lines += [f"{key}: RGB MAE {row['rgb_mae']:.6f}; p99 absolute error {row['rgb_p99_abs_error']:.6f}. "
                      f"Tolerance checks passed: {row['passed']}."]
    if result.get("previews"):
        lines += ["", "Linear-to-sRGB preview PNGs (numerical validation uses float16 output):"]
        lines += [f"- [{name}](<{path.replace(chr(92), '/')}>)" for name, path in result["previews"].items()]
    return lines


def report_markdown(report):
    lines = ["# Vega FSR preflight report", "", f"Captured: {report['captured_at']}", "",
             "This report separates advertised driver features, generic GPU checks, and actual synthetic FSR tests where present.", ""]
    if report.get("error"):
        lines += ["Error: " + report["error"], ""]
    for gpu in report.get("devices", []):
        lines += [f"## {gpu['name']}", "", f"- UUID: {gpu['uuid']}",
                  f"- PCI ID: {gpu['vendor_id']:04x}:{gpu['device_id']:04x}; {gpu['device_type']}",
                  f"- Vulkan: {gpu['api_version']}; driver: {gpu['driver_info']}", ""]
    if report.get("assessment"):
        a = report["assessment"]
        lines += ["## Selected processing GPU", "", a["identity"], "", a["conclusion"], "",
                  "| Preflight check | Result |", "| --- | --- |"]
        lines += [f"| {c['name']} | {'Present' if c['pass'] else 'Missing'} |" for c in a["checks"]]
        lines += ["", "## FSR assessment", ""]
        for fsr in a["fsr"]:
            lines += [f"- **{fsr['name']}**: {fsr['status']}. {fsr['detail']} "
                      f"Runtime executed: {'yes (synthetic FSR 3.1.3)' if fsr['runtime_tested'] else 'no'}."]
    tests = report.get("tests", {})
    if tests:
        lines += ["", "## Executed tests", ""]
        for name, result in tests.items():
            if name == "fsr":
                lines += fsr_markdown(result)
                continue
            lines += [f"### {name}", "", f"Status: {result['status']}", "", result.get("detail", ""), ""]
            if "median_ms" in result:
                lines += [f"- {result['bytes_each_direction'] / 1048576:g} MiB each direction; {result['samples']} samples.",
                          f"- Median {result['median_ms']:.3f} ms; p95 {result['p95_ms']:.3f} ms; p99 {result['p99_ms']:.3f} ms.",
                          f"- {result['median_budget_percent']:.1f}% of a {result['frame_budget_ms']:.2f} ms frame budget.",
                          f"- Verdict: {result['verdict']}. FSR dispatch time remains unmeasured.", ""]
    limits = ("Actual FSR 3.1.3 was exercised only on the stated synthetic scene and dimensions. "
              "No CK3/DLSS workload, concurrent game rendering, temporal game-quality evaluation, external "
              "shared-handle import or pipelined overlap was measured. Wall times include Python/CPU scheduling."
              if report.get("fsr_runtime_executed") else
              "No FSR algorithm, CK3 integration, external shared-handle import, image packing or DLSS 5 workload "
              "was tested. Buffer transport timings include Python/CPU scheduling.")
    lines += ["", "## Limits", "", limits + " These results do not predict CK3 FPS.", "", "## Sources", ""]
    lines += [f"- [{s['title']}]({s['url']})" for s in SOURCES]
    return "\n".join(lines) + "\n"
