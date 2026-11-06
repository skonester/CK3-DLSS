"""Flet desktop preflight for an independent Vega FSR stage."""
from __future__ import annotations

import asyncio
from datetime import datetime
import json
import os
from pathlib import Path

import flet as ft

from assessment import SOURCES, assess, annotate_fsr_assessment, report_markdown
from runner import run_worker

ROOT = Path(__file__).resolve().parent
GREEN, AMBER, RED, MUTED = "#6ee7b7", "#f5c875", "#fca5a5", "#a3b3c9"
STATUS_LABELS = {
    "candidate": "Candidate · runtime untested", "missing_prerequisites": "Missing prerequisites",
    "outside_published_range": "Outside published range", "not_officially_supported": "No official Vega support",
    "not_assessed": "Not assessed", "passed": "Passed", "failed": "Failed", "skipped": "Skipped",
    "runtime_passed": "FSR 3.1.3 synthetic test passed",
}


def badge(label, color=AMBER):
    return ft.Container(ft.Text(label, size=12, color=color), padding=ft.Padding.symmetric(horizontal=10, vertical=5),
                        bgcolor="#202d40", border_radius=6)


def panel(title, controls):
    return ft.Container(ft.Column([ft.Text(title, size=19, weight=ft.FontWeight.W_600), *controls], spacing=12),
                        padding=20, bgcolor="#131e30", border_radius=12)


async def main(page: ft.Page):
    page.title = "Vega FSR Preflight"
    page.theme_mode = ft.ThemeMode.DARK
    page.theme = ft.Theme(color_scheme_seed="#6ee7b7", font_family="Segoe UI")
    page.bgcolor = "#0b1220"
    page.padding = 24
    page.window.width, page.window.height = 1120, 900
    page.window.min_width, page.window.min_height = 860, 650

    report = None
    current_task = None
    busy = False
    status = ft.Text("Scanning the installed Vulkan drivers…", color=MUTED)
    progress = ft.ProgressBar(visible=True, color=GREEN)
    identity = ft.Column(spacing=8)
    fsr_results = ft.Column(spacing=12)
    fsr_description = ft.Text(color=AMBER, size=13)
    hardware = ft.Column(spacing=10)
    measured = ft.Column([ft.Text("Run GPU checks to create a compute device and measure staged transfers.", color=MUTED)])
    details = ft.Column(spacing=8)
    export_message = ft.Text("Reports stay on this computer.", size=12, color=MUTED, selectable=True)
    target = ft.Dropdown(label="Processing GPU", options=[], expand=True)
    source = ft.Dropdown(label="Rendering GPU / transfer partner", options=[], expand=True)
    payload = ft.Dropdown(label="MiB each direction", value="16", width=190,
                          options=[ft.DropdownOption(str(v)) for v in (4, 16, 32, 64)])
    samples = ft.Dropdown(label="Measured samples", value="20", width=190,
                          options=[ft.DropdownOption(str(v)) for v in (10, 20, 40)])
    fps = ft.Dropdown(label="Target FPS", value="60", width=150,
                      options=[ft.DropdownOption(str(v)) for v in (30, 60, 120)])
    resolution = ft.Dropdown(label="FSR output resolution", value="1080p", width=215,
                             options=[ft.DropdownOption(v) for v in ("720p", "1080p", "1440p")])

    def chosen_gpu():
        return next((d for d in (report or {}).get("devices", []) if d["uuid"] == target.value), None)

    def set_busy(value):
        nonlocal busy
        busy = value
        for control in (refresh_button, target, source, payload, samples, fps, resolution):
            control.disabled = value
        run_button.disabled = value or chosen_gpu() is None or bool(source.value and source.value == target.value)
        fsr_button.disabled = value or chosen_gpu() is None or not source.value or source.value == target.value
        export_button.disabled = value or report is None
        cancel_button.visible = value
        progress.visible = value

    def show_tests():
        measured.controls.clear()
        tests = (report or {}).get("tests", {})
        if not tests:
            measured.controls.append(ft.Text("Not run. Feature availability does not prove shader execution or transfer speed.", color=MUTED))
            return
        for key, title in (("device", "Separate processing device"), ("compute", "Compute correctness")):
            result = tests.get(key)
            if result:
                color = GREEN if result["status"] == "passed" else RED
                measured.controls += [ft.Row([ft.Text(title, weight=ft.FontWeight.W_600),
                    badge(STATUS_LABELS[result["status"]], color)], wrap=True),
                    ft.Text(result["detail"], size=13, color=MUTED)]
        transfer = tests.get("transfer")
        if transfer:
            color = GREEN if transfer["status"] == "passed" else RED if transfer["status"] == "failed" else AMBER
            measured.controls += [ft.Divider(), ft.Row([ft.Text("Rendering GPU ↔ processing GPU · serial host staging", weight=ft.FontWeight.W_600),
                                  badge(STATUS_LABELS[transfer["status"]], color)], wrap=True)]
            if "median_ms" in transfer:
                metrics = [("MEDIAN", f"{transfer['median_ms']:.2f} ms"), ("P95", f"{transfer['p95_ms']:.2f} ms"),
                           ("P99", f"{transfer['p99_ms']:.2f} ms"),
                           ("FRAME BUDGET", f"{transfer['frame_budget_ms']:.2f} ms")]
                measured.controls += [ft.Row([ft.Container(ft.Column([ft.Text(label, size=11, color=MUTED),
                    ft.Text(value, size=25, weight=ft.FontWeight.W_600)]), width=180) for label, value in metrics], wrap=True),
                    ft.Text(f"{transfer['bytes_each_direction'] / 1048576:g} MiB each direction · "
                            f"{transfer['samples']} samples after 3 warmups · every byte verified", color=MUTED, size=13)]
                over = transfer["p95_ms"] >= transfer["frame_budget_ms"]
                measured.controls += [ft.ProgressBar(value=min(1, transfer["p95_ms"] / transfer["frame_budget_ms"]),
                    color=RED if over else AMBER), ft.Text(
                    "Staging alone exceeds the target frame budget at p95." if over else
                    "Staging fits the frame budget; FSR time and the game's existing work still need to fit.",
                    color=RED if over else AMBER)]
            measured.controls.append(ft.Text(transfer["detail"], size=12, color=MUTED))
        fsr = tests.get("fsr")
        if fsr:
            measured.controls += [ft.Row([ft.Text("Actual FSR 3.1 comparison", size=18, weight=ft.FontWeight.W_600),
                badge(STATUS_LABELS[fsr["status"]], GREEN if fsr["status"] == "passed" else RED)], wrap=True)]
            if fsr.get("error"):
                measured.controls.append(ft.Text(fsr["error"], color=RED, selectable=True))
            if "input_size" in fsr:
                iw, ih = fsr["input_size"]
                ow, oh = fsr["output_size"]
                measured.controls.append(ft.Text(f"{iw} × {ih} → {ow} × {oh} · "
                    f"{fsr['samples']} samples after {fsr['warmups']} warmups · sharpening off", color=MUTED))
                rows = []
                for key, label, gpu_key in [("source_fsr", "Rendering GPU alone", "gpu"),
                                           ("target_fsr", "Processing GPU alone", "gpu"),
                                           ("offload", "Complete offload", "amd_fsr_gpu")]:
                    if key in fsr:
                        r = fsr[key]
                        rows.append(ft.DataRow(cells=[ft.DataCell(ft.Text(label, width=210)),
                            ft.DataCell(ft.Text(f"{r['wall']['median_ms']:.2f} ms", width=110)),
                            ft.DataCell(ft.Text(f"{r['wall']['p95_ms']:.2f} ms", width=110)),
                            ft.DataCell(ft.Text(f"{r[gpu_key]['median_ms']:.2f} ms", width=110))]))
                if rows:
                    measured.controls.append(ft.DataTable(columns=[ft.DataColumn(ft.Text(t)) for t in
                        ["Path", "Median wall", "p95 wall", "FSR GPU median"]], rows=rows, column_spacing=24))
            comparison = fsr.get("comparison")
            if comparison:
                faster = comparison["offload_faster"]
                text = (f"Offloading is {comparison['offload_to_direct_ratio']:.1f}× the direct wall time "
                        f"({comparison['offload_minus_direct_ms']:+.2f} ms). ")
                text += ("It was faster in this synthetic serial test." if faster else
                         "Keeping FSR on the rendering GPU was faster in this test.")
                measured.controls.append(ft.Text(text, color=GREEN if faster else AMBER,
                                                  weight=ft.FontWeight.W_600))
            if fsr.get("offload_image_comparison", {}).get("passed"):
                measured.controls.append(ft.Text("Output checks passed: finite images, analytic gradients, "
                    "cross-GPU error tolerance, and every byte of each returned image.", color=GREEN, size=13))
            measured.controls.append(ft.Text(fsr.get("detail", ""), size=12, color=MUTED))

    def show_selection():
        identity.controls.clear()
        fsr_results.controls.clear()
        hardware.controls.clear()
        details.controls.clear()
        gpu = chosen_gpu()
        if gpu is None:
            identity.controls.append(ft.Text("Select a processing GPU. No fallback adapter is selected silently.", color=AMBER))
            return
        a = annotate_fsr_assessment(assess(gpu), report.get("tests", {}).get("fsr", {}))
        report["assessment"] = a
        report["target_uuid"], report["source_uuid"] = target.value, source.value or None
        runtime_tested = a["fsr"][2]["runtime_tested"]
        fsr_description.value = ("FSR 3.1.3 ran on this processing GPU with the synthetic test inputs. "
            "Other entries remain preflight assessments; this does not establish CK3 support." if runtime_tested else
            "FSR is a shader/runtime integration, not a driver feature bit. Candidate means inspected "
            "prerequisites pass; run the FSR benchmark to exercise the pinned 3.1.3 runtime.")
        identity.controls += [ft.Text(a["identity"], size=23, weight=ft.FontWeight.W_600),
            ft.Text(f"{gpu['name']} · PCI {gpu['vendor_id']:04X}:{gpu['device_id']:04X} · {gpu['device_type']}", color=MUTED),
            ft.Row([badge("Vulkan " + gpu["api_version"], GREEN), badge(f"Wave {gpu['subgroup_size']}", GREEN),
                    badge("FP16 available" if gpu["features"].get("shaderFloat16") else "FP32 fallback")], wrap=True),
            ft.Text("Driver: " + gpu["driver_info"], size=13, color=MUTED),
            ft.Text(a["conclusion"], color=AMBER)]
        for item in a["fsr"]:
            fsr_results.controls.append(ft.Container(ft.Column([
                ft.Row([ft.Text(item["name"], size=15, weight=ft.FontWeight.W_600, expand=True),
                        badge(STATUS_LABELS[item["status"]], GREEN if item["runtime_tested"] else AMBER)]),
                ft.Text(item["detail"], size=13, color=MUTED),
            ], spacing=7), padding=12, bgcolor="#19263b", border_radius=8))
        for c in a["checks"]:
            color = GREEN if c["pass"] else RED if c["required"] else AMBER
            hardware.controls.append(ft.Column([
                ft.Row([ft.Text(c["name"], expand=True), badge("Present" if c["pass"] else "Missing", color)]),
                ft.Text(c["detail"], size=12, color=MUTED)], spacing=3))
        info_lines = [f"Device UUID: {gpu['uuid']}", f"Driver: {gpu['driver_name']} / {gpu['driver_info']}",
                      f"Shared compute memory: {gpu['limits']['maxComputeSharedMemorySize'] / 1024:g} KiB",
                      f"Max compute group: {gpu['limits']['maxComputeWorkGroupSize']}",
                      f"Max compute invocations: {gpu['limits']['maxComputeWorkGroupInvocations']}"]
        info_lines += [f"Queue {q['index']}: count={q['count']}, compute={q['compute']}, graphics={q['graphics']}" for q in gpu["queues"]]
        info_lines += [f"{name}: sampled={f['sampled_image']}, storage={f['storage_image']}, linear filter={f['linear_filter']}"
                       for name, f in gpu["formats"].items()]
        info_lines += ["", "Core and queried feature flags:"]
        info_lines += [f"{name}: {value}" for name, value in sorted(gpu["features"].items())]
        if report.get("tests", {}).get("device", {}).get("entry_points"):
            info_lines += ["", "Entry points on the tested device (availability, not an interop test):"]
            info_lines += [f"{name}: {value}" for name, value in report["tests"]["device"]["entry_points"].items()]
        info_lines += ["", "Advertised device extensions:", *gpu["extensions"]]
        details.controls.append(ft.Text("\n".join(info_lines), font_family="Consolas", size=12, selectable=True))

    async def perform(mode):
        nonlocal report, current_task
        if busy:
            return
        current_task = asyncio.current_task()
        if report:
            report["tests"] = {}
            report["fsr_runtime_executed"] = False
            report.pop("error", None)
        show_tests()
        set_busy(True)
        status.value = ("Scanning Vulkan hardware…" if mode == "scan" else
            "Running actual FSR 3.1.3 on both GPUs and the complete offload path…" if mode == "fsr" else
            "Running compute and transfer checks in an isolated worker…")
        status.color = MUTED
        page.update()
        args = ["--mode", mode]
        if mode in ("test", "fsr"):
            args += ["--target", target.value, "--mib", payload.value, "--samples", samples.value, "--fps", fps.value]
            args += ["--source", source.value or "none"]
            if mode == "fsr":
                artifacts = ROOT / "reports" / ("fsr-previews-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
                args += ["--resolution", resolution.value, "--artifacts", str(artifacts)]
        try:
            result = await run_worker(args, timeout=180 if mode == "fsr" else 90)
            if "devices" in result:
                report = result
                options = [ft.DropdownOption(d["uuid"], f"{d['name']} ({d['device_type']})") for d in report["devices"]]
                target.options = options
                source.options = [ft.DropdownOption("", "No transfer partner"),
                                  *[ft.DropdownOption(d["uuid"], d["name"]) for d in report["devices"]]]
                target.value, source.value = report.get("target_uuid"), report.get("source_uuid") or ""
            elif report:
                report["error"] = result.get("error", "Worker failed.")
            error = result.get("error") or result.get("tests", {}).get("fsr", {}).get("error")
            failed = any(t["status"] == "failed" for t in result.get("tests", {}).values())
            status.value = error or ("Some GPU checks failed. See the measured results." if failed else
                           "FSR 3.1.3 comparison complete. Results apply to the stated synthetic workload." if mode == "fsr" else
                           "Hardware scanned. FSR algorithms remain untested." if mode == "scan" else
                           "GPU checks complete. FSR algorithms remain untested.")
            status.color = RED if error or failed else GREEN
            show_selection()
            show_tests()
        except asyncio.CancelledError:
            status.value, status.color = "Check cancelled. The GPU worker was stopped.", AMBER
        except Exception as exc:
            status.value, status.color = str(exc), RED
        finally:
            current_task = None
            set_busy(False)
            page.update()

    async def refresh(_=None):
        await perform("scan")

    async def run_tests(_):
        await perform("test")

    async def run_fsr(_):
        await perform("fsr")

    def cancel(_):
        if current_task:
            current_task.cancel()

    def selection_changed(_):
        if report:
            report["tests"] = {}
            report["fsr_runtime_executed"] = False
            report.pop("error", None)
            show_selection()
            show_tests()
        same = bool(source.value and source.value == target.value)
        status.value = "Choose different GPUs for the two stages." if same else "Selection changed. Run GPU checks for this pair."
        status.color = AMBER
        set_busy(False)
        page.update()

    def export(_):
        if not report:
            return
        try:
            folder = ROOT / "reports"
            folder.mkdir(exist_ok=True)
            name = "vega-fsr-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f")
            json_path = folder / (name + ".json")
            json_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
            json_path.with_suffix(".md").write_text(report_markdown(report), encoding="utf-8")
            export_message.value, export_message.color = "Saved JSON + Markdown: " + str(json_path), GREEN
        except OSError as exc:
            export_message.value, export_message.color = "Could not save report: " + str(exc), RED
        page.update()

    refresh_button = ft.Button("Refresh hardware", on_click=refresh)
    run_button = ft.Button("Run GPU checks", icon=ft.Icons.PLAY_ARROW, on_click=run_tests, disabled=True)
    fsr_button = ft.Button("Benchmark FSR 3.1", icon=ft.Icons.SPEED, on_click=run_fsr, disabled=True)
    export_button = ft.Button("Export report", on_click=export, disabled=True)
    cancel_button = ft.TextButton("Cancel", on_click=cancel, visible=False)
    target.on_select = source.on_select = selection_changed
    payload.on_select = samples.on_select = fps.on_select = resolution.on_select = selection_changed
    page.on_disconnect = cancel
    page.add(ft.Column([
        ft.Text("VEGA / FSR PREFLIGHT", size=12, weight=ft.FontWeight.W_600, color=GREEN),
        ft.Text("Can Vega handle a separate FSR stage?", size=30, weight=ft.FontWeight.W_600),
        ft.Text("Inspect the driver, benchmark FSR 3.1.3 on both GPUs, and measure the complete offload path.", color=MUTED),
        ft.Row([refresh_button, run_button, fsr_button, export_button, cancel_button], wrap=True), status, progress,
        panel("Choose the two GPUs", [ft.Row([target, source]), ft.Row([payload, samples, fps, resolution], wrap=True),
            ft.Text("GPU checks use the MiB setting. The FSR benchmark uses actual image sizes at the selected output resolution.",
                    size=12, color=MUTED)]),
        panel("Processing GPU", [identity]), panel("Measured GPU checks", [measured]),
        panel("FSR compatibility assessment", [fsr_description, fsr_results]),
        panel("Feature preflight", [hardware]),
        ft.ExpansionTile(title="All features, formats, queues and entry points", controls=[details], controls_padding=16),
        panel("What this establishes", [ft.Text("GPU checks test the building blocks. The FSR benchmark adds actual 3.1.3 "
            "dispatches and image transfers on a synthetic scene. Neither mode establishes CK3 quality, "
            "concurrent game performance, or a CK3 FPS improvement.", color=MUTED),
            ft.Row([ft.TextButton("AMD hardware requirements", url=SOURCES[0]["url"]),
                    ft.TextButton("AMD integration guide", url=SOURCES[1]["url"])], wrap=True), export_message]),
    ], spacing=16, scroll=ft.ScrollMode.AUTO, expand=True))
    await refresh()


if __name__ == "__main__":
    # An optional project-local desktop client avoids re-downloading on this machine.
    local_client = ROOT / ".venv" / "flet-client" / "flet"
    if (local_client / "flet.exe").is_file():
        os.environ["FLET_VIEW_PATH"] = str(local_client)
    ft.run(main)
