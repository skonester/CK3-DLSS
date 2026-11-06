from pathlib import Path
import sys
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from assessment import annotate_fsr_assessment, report_markdown
from fsr_benchmark import compare_images, jitter, make_inputs, validate_image
from vk_native import VulkanError


def analytic_image(width=64, height=48):
    data = np.ones((height, width, 4), dtype=np.float16)
    data[:, :, 0] = .15 + .7 * (np.arange(width)[None, :] + .5) / width
    data[:, :, 1] = .15 + .7 * (np.arange(height)[:, None] + .5) / height
    data[:, :, 2] = .2
    return data


class ImageValidationTests(unittest.TestCase):
    def test_analytic_gradient_passes(self):
        result = validate_image(analytic_image().tobytes(), 64, 48)
        self.assertTrue(result["finite"])
        self.assertLess(result["red_gradient_mae"], .001)

    def test_unwritten_poison_output_fails(self):
        image = analytic_image()
        image[5, 6, 0] = np.nan
        with self.assertRaisesRegex(VulkanError, "NaN"):
            validate_image(image.tobytes(), 64, 48)

    def test_wrong_gradient_fails_even_when_not_constant(self):
        image = analytic_image()
        image[:, :, 0] = image[:, ::-1, 0]
        with self.assertRaisesRegex(VulkanError, "gradient"):
            validate_image(image.tobytes(), 64, 48)

    def test_material_cross_gpu_corruption_fails(self):
        image = analytic_image()
        changed = image.copy()
        changed[10:30, 10:30, 0] += .25
        with self.assertRaisesRegex(VulkanError, "tolerance"):
            compare_images(image.tobytes(), changed.tobytes(), 64, 48)

    def test_identical_outputs_pass(self):
        data = analytic_image().tobytes()
        self.assertEqual(compare_images(data, data, 64, 48)["rgb_mae"], 0)

    def test_packed_inputs_have_expected_formats_and_size(self):
        width, height = 32, 24
        data = make_inputs(width, height, (.25, -.25))
        self.assertEqual(len(data), width * height * 16)
        color_bytes = width * height * 8
        depth = np.frombuffer(data[color_bytes:color_bytes + width * height * 4], dtype=np.float32)
        self.assertTrue(np.all(depth == .5))
        self.assertEqual(set(data[color_bytes + width * height * 4:]), {0})

    def test_jitter_sequence_is_bounded_and_varies(self):
        sequence = [jitter(i, 1280, 1920) for i in range(18)]
        self.assertTrue(all(-.5 <= x <= .5 and -.5 <= y <= .5 for x, y in sequence))
        self.assertEqual(len(set(sequence)), 18)
        self.assertEqual(jitter(18, 1280, 1920), sequence[0])


class RuntimeEvidenceTests(unittest.TestCase):
    def assessment(self):
        return {"gpu_uuid": "target", "conclusion": "candidate", "fsr": [
            {"status": "candidate", "runtime_tested": False} for _ in range(5)]}

    def evidence(self):
        return {"target_uuid": "target", "runtime_executed": True,
                "target_fsr": {"status": "passed", "output_validation": {"finite": True}}}

    def test_target_fsr_validation_promotes_only_that_version(self):
        a = annotate_fsr_assessment(self.assessment(), self.evidence())
        self.assertTrue(a["fsr"][2]["runtime_tested"])
        self.assertFalse(a["fsr"][1]["runtime_tested"])
        self.assertEqual(a["fsr"][2]["status"], "runtime_passed")

    def test_other_adapter_evidence_does_not_promote_target(self):
        evidence = self.evidence()
        evidence["target_uuid"] = "other"
        self.assertFalse(annotate_fsr_assessment(self.assessment(), evidence)["fsr"][2]["runtime_tested"])

    def test_context_creation_alone_does_not_claim_execution(self):
        evidence = self.evidence()
        evidence["runtime_executed"] = False
        self.assertFalse(annotate_fsr_assessment(self.assessment(), evidence)["fsr"][2]["runtime_tested"])

    def test_unvalidated_output_does_not_claim_a_pass(self):
        evidence = self.evidence()
        evidence["target_fsr"]["output_validation"]["finite"] = False
        self.assertFalse(annotate_fsr_assessment(self.assessment(), evidence)["fsr"][2]["runtime_tested"])


if __name__ == "__main__":
    unittest.main()
