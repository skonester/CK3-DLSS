import asyncio
import copy
import ctypes
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import AsyncMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from assessment import assess, default_source, default_target, report_markdown, validate_settings
from runner import run_worker
from vk_native import percentile
from vk_types import Features, Limits, Properties, MemoryProperties


def gpu():
    return {"uuid": "amd", "name": "AMD Radeon Graphics", "vendor_id": 0x1002, "device_id": 0x1638,
            "device_type": "integrated", "api_version_raw": (1 << 22) | (3 << 12),
            "api_version": "1.3.0", "driver_info": "test", "luid_valid": True,
            "queues": [{"compute": True, "count": 1}], "features": {
                "shaderStorageImageExtendedFormats": True, "shaderFloat16": True,
                "storageBuffer16BitAccess": True, "timelineSemaphore": True},
            "formats": {name: {"sampled_image": True, "storage_image": True} for name in
                        ["RGBA8_UNORM", "RGBA16_UNORM", "RGBA16_SFLOAT"]},
            "subgroup_compute": True, "subgroup_size": 64, "subgroup_operation_names": ["basic", "ballot"],
            "extensions": ["VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"]}


class AssessmentTests(unittest.TestCase):
    def test_feature_scan_never_claims_an_executed_fsr_version(self):
        a = assess(gpu())
        self.assertEqual(a["preflight_status"], "candidate")
        self.assertTrue(all(not f["runtime_tested"] for f in a["fsr"]))
        self.assertEqual(a["fsr"][3]["status"], "outside_published_range")
        self.assertEqual(a["fsr"][4]["status"], "not_officially_supported")

    def test_missing_typed_storage_is_not_a_temporal_candidate(self):
        device = gpu()
        device["formats"]["RGBA16_UNORM"]["storage_image"] = False
        a = assess(device)
        self.assertEqual(a["fsr"][0]["status"], "candidate")
        self.assertEqual(a["fsr"][2]["status"], "missing_prerequisites")

    def test_fp32_fallback_is_not_rejected_for_missing_fp16(self):
        device = gpu()
        device["features"]["shaderFloat16"] = False
        self.assertEqual(assess(device)["preflight_status"], "candidate")

    def test_unrecognized_amd_gpu_is_not_called_a_5700g(self):
        device = gpu()
        device["device_id"] = 0xbeef
        a = assess(device)
        self.assertEqual(a["identity"], device["name"])
        self.assertEqual(a["fsr"][4]["status"], "not_assessed")

    def test_no_silent_discrete_target_fallback(self):
        device = gpu()
        device.update(vendor_id=0x10de, device_type="discrete", uuid="nvidia")
        self.assertIsNone(default_target([device]))
        self.assertEqual(default_source([gpu(), device], "amd"), "nvidia")

    def test_invalid_or_unbounded_workloads_rejected(self):
        for settings in [(1024, 20, 60), (16, 10000, 60), (16, 20, 0)]:
            with self.assertRaises(ValueError):
                validate_settings(*settings)

    def test_markdown_preserves_fsr_limitation_after_compute_pass(self):
        report = {"captured_at": "test", "devices": [gpu()], "assessment": assess(gpu()),
                  "tests": {"compute": {"status": "passed", "detail": "Generic compute only."}}}
        md = report_markdown(report)
        self.assertIn("Runtime executed: no", md)
        self.assertIn("No FSR algorithm", md)

    def test_interpolated_percentiles(self):
        self.assertAlmostEqual(percentile([40, 10, 30, 20], .95), 38.5)

    @unittest.skipUnless(ctypes.sizeof(ctypes.c_void_p) == 8, "64-bit ABI only")
    def test_layouts_used_by_driver_writes(self):
        self.assertEqual(ctypes.sizeof(Features), 220)
        self.assertEqual(ctypes.sizeof(Limits), 504)
        self.assertEqual(ctypes.sizeof(Properties), 824)
        self.assertEqual(ctypes.sizeof(MemoryProperties), 520)


class WorkerTests(unittest.IsolatedAsyncioTestCase):
    async def test_native_crash_becomes_a_reportable_error(self):
        process = type("Process", (), {"returncode": -1073741819,
            "communicate": AsyncMock(return_value=(b"", b"driver failed"))})()
        with patch("runner.asyncio.create_subprocess_exec", AsyncMock(return_value=process)):
            report = await run_worker(["--mode", "scan"])
        self.assertIn("without a valid report", report["error"])

    async def test_failed_test_report_survives_nonzero_exit(self):
        expected = {"tests": {"compute": {"status": "failed", "detail": "mismatch"}}}
        process = type("Process", (), {"returncode": 1,
            "communicate": AsyncMock(return_value=(json.dumps(expected).encode(), b""))})()
        with patch("runner.asyncio.create_subprocess_exec", AsyncMock(return_value=process)):
            self.assertEqual(await run_worker([]), expected)

    async def test_timeout_stops_worker(self):
        class Process:
            returncode = None
            killed = False
            async def communicate(self):
                if not self.killed:
                    await asyncio.sleep(1)
                return b"", b""
            def kill(self):
                self.killed = True
                self.returncode = 1
        process = Process()
        with patch("runner.asyncio.create_subprocess_exec", AsyncMock(return_value=process)):
            report = await run_worker([], timeout=.01)
        self.assertTrue(process.killed)
        self.assertIn("exceeded", report["error"])


if __name__ == "__main__":
    unittest.main()
