"""Actual FSR 3.1.3 Vulkan dispatches, with a measured image offload path.

The pinned official runtime is optional and isolated from CK3. See
AMD-FSR-LICENSE.txt and Setup-FSRRuntime.ps1.
"""
from __future__ import annotations

import ctypes as C
import hashlib
import math
from pathlib import Path
import statistics
import struct
import time
import zlib

import numpy as np

import fsr_types as FX
from vk_types import *
from vk_native import Device, VulkanError, check, percentile

RUNTIME_SHA256 = "04a455bb7c01af1adc630f3be41bd85689b6a8565199e25b09696240750f6d1e"
SDK_COMMIT = "54fbaafdc34716811751bea5032700e78f5a0f33"
PRESETS = {"1080p": (1280, 720, 1920, 1080), "1440p": (1706, 960, 2560, 1440),
           "720p": (854, 480, 1280, 720), "smoke": (160, 90, 240, 135)}


def ffx_check(code, operation):
    if code:
        raise VulkanError(f"{operation}: FidelityFX return code {code}")


def stats(values):
    return {"median_ms": statistics.median(values), "p95_ms": percentile(values, .95),
            "p99_ms": percentile(values, .99), "min_ms": min(values), "max_ms": max(values),
            "sample_ms": values}


def halton(index, base):
    value, fraction = 0.0, 1.0
    while index:
        fraction /= base
        value += fraction * (index % base)
        index //= base
    return value


def jitter(index, render_width, output_width):
    phases = int(8 * (output_width / render_width) ** 2)
    position = (index % phases) + 1
    return halton(position, 2) - .5, halton(position, 3) - .5


def make_inputs(width, height, offset):
    """A static analytic plane sampled with the declared subpixel jitter."""
    x = (np.arange(width, dtype=np.float32) + .5 + offset[0]) / width
    y = (np.arange(height, dtype=np.float32) + .5 + offset[1]) / height
    color = np.ones((height, width, 4), dtype=np.float16)
    color[:, :, 0] = .15 + .7 * x[None, :]
    color[:, :, 1] = .15 + .7 * y[:, None]
    checker = ((np.floor(x[None, :] * 12) + np.floor(y[:, None] * 8)) % 2)
    color[:, :, 2] = .2 + .5 * checker
    depth = np.full((height, width), .5, dtype=np.float32)
    motion = np.zeros((height, width, 2), dtype=np.float16)
    return color.tobytes() + depth.tobytes() + motion.tobytes()


def validate_image(data, width, height):
    image = np.frombuffer(data, dtype=np.float16).reshape(height, width, 4).astype(np.float32)
    rgb = image[:, :, :3]
    if not np.isfinite(image).all():
        raise VulkanError("FSR output contains NaN/Inf or retains the poison clear.")
    # The red/green channels are independent analytic gradients, not another
    # implementation of the FSR algorithm. Ignore the image's filter borders.
    margin = min(16, width // 8, height // 8)
    center = image[margin:-margin, margin:-margin]
    xx = (.15 + .7 * (np.arange(width, dtype=np.float32) + .5) / width)[None, :]
    yy = (.15 + .7 * (np.arange(height, dtype=np.float32) + .5) / height)[:, None]
    red_error = float(np.mean(np.abs(center[:, :, 0] - xx[:, margin:-margin])))
    green_error = float(np.mean(np.abs(center[:, :, 1] - yy[margin:-margin, :])))
    if max(red_error, green_error) > .015 or float(rgb.std()) < .08:
        raise VulkanError(f"FSR output failed the analytic-gradient check: red={red_error:.5f}, green={green_error:.5f}.")
    return {"finite": True, "red_gradient_mae": red_error, "green_gradient_mae": green_error,
            "rgb_min": float(rgb.min()), "rgb_max": float(rgb.max()),
            "rgb_std": float(rgb.std()), "sha256": hashlib.sha256(data).hexdigest()}


def compare_images(left, right, width, height):
    a = np.frombuffer(left, dtype=np.float16).reshape(height, width, 4).astype(np.float32)[:, :, :3]
    b = np.frombuffer(right, dtype=np.float16).reshape(height, width, 4).astype(np.float32)[:, :, :3]
    delta = np.abs(a - b)
    mean, p99 = float(delta.mean()), float(np.percentile(delta, 99))
    if not np.isfinite(delta).all() or mean > .005 or p99 > .025:
        raise VulkanError(f"GPU outputs differ beyond tolerance: RGB MAE={mean:.6f}, p99={p99:.6f}.")
    return {"passed": True, "rgb_mae": mean, "rgb_p99_abs_error": p99,
            "rgb_max_abs_error": float(delta.max()), "mae_tolerance": .005, "p99_tolerance": .025}


def save_png(path, data, width, height):
    # A display preview of linear data; numerical comparisons use float16 bytes.
    rgba = np.frombuffer(data, dtype=np.float16).reshape(height, width, 4).astype(np.float32)
    rgb = np.clip(rgba[:, :, :3], 0, 1)
    srgb = np.where(rgb <= .0031308, rgb * 12.92, 1.055 * np.power(rgb, 1 / 2.4) - .055)
    pixels = np.round(np.clip(srgb, 0, 1) * 255).astype(np.uint8)
    rows = b"".join(b"\0" + row.tobytes() for row in pixels)
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


class Runtime:
    def __init__(self):
        self.path = Path(__file__).parent / "runtime" / "fsr31" / "amd_fidelityfx_vk.dll"
        if not self.path.is_file():
            raise VulkanError("Official FSR runtime missing. Run Setup-FSRRuntime.ps1 in the probe folder.")
        if hashlib.sha256(self.path.read_bytes()).hexdigest() != RUNTIME_SHA256:
            raise VulkanError("FSR runtime hash mismatch. Re-run Setup-FSRRuntime.ps1 for the pinned official build.")
        self.dll = C.CDLL(str(self.path.resolve()), winmode=0x1100)
        for name, arguments in [("ffxQuery", [P, P]), ("ffxCreateContext", [P, P, P]),
                                ("ffxDestroyContext", [P, P]), ("ffxDispatch", [P, P])]:
            function = getattr(self.dll, name)
            function.restype, function.argtypes = U, arguments
        count = Q()
        query = FX.GetVersions(header=FX.Header(4), createDescType=0x10000, outputCount=address(count))
        ffx_check(self.dll.ffxQuery(None, C.byref(query)), "enumerate FSR version count")
        if not 0 < count.value <= 32:
            raise VulkanError("Invalid FSR provider count.")
        ids, names = (Q * count.value)(), (S * count.value)()
        query.versionIds, query.versionNames = C.cast(ids, P), C.cast(names, P)
        ffx_check(self.dll.ffxQuery(None, C.byref(query)), "enumerate FSR versions")
        self.versions = [{"id": ids[i], "name": names[i].decode()} for i in range(count.value)]
        selected = next((v for v in self.versions if v["name"] == "3.1.3"), None)
        if selected is None:
            raise VulkanError("Pinned FSR 3.1.3 provider not reported; no fallback is permitted.")
        self.version = selected


class Image:
    def __init__(self, device, width, height, fmt, ffx_fmt, bytes_per_pixel, output=False):
        self.device, self.width, self.height = device, width, height
        self.ffx_fmt, self.size = ffx_fmt, width * height * bytes_per_pixel
        self.handle, self.memory = H(), H()
        self.layout, self.home_layout = 0, 1 if output else 5
        ci = info(ImageCreateInfo, imageType=1, format=fmt, extent=Extent3D(width, height, 1),
                  mipLevels=1, arrayLayers=1, samples=1, usage=15)
        check(device.call("vkCreateImage", I, H, P, P, P)(device.handle, C.byref(ci), None, C.byref(self.handle)), "create FSR image")
        try:
            requirements = MemoryRequirements()
            device.call("vkGetImageMemoryRequirements", None, H, H, P)(device.handle, self.handle, C.byref(requirements))
            index = next((i for i in range(device.memory.memoryTypeCount) if
                requirements.memoryTypeBits & (1 << i) and device.memory.memoryTypes[i].propertyFlags & 1), None)
            if index is None:
                raise VulkanError("No device-local image memory type.")
            ai = info(MemoryAllocateInfo, allocationSize=requirements.size, memoryTypeIndex=index)
            check(device.call("vkAllocateMemory", I, H, P, P, P)(device.handle, C.byref(ai), None, C.byref(self.memory)), "allocate image")
            check(device.call("vkBindImageMemory", I, H, H, H, Q)(device.handle, self.handle, self.memory, 0), "bind image")
            device.execute(lambda command: self.transition(command, self.home_layout))
            if output:
                def poison(command):
                    color = (F * 4)(*([float("nan")] * 4))
                    sub = ImageSubresourceRange(aspectMask=1, levelCount=1, layerCount=1)
                    device.call("vkCmdClearColorImage", None, H, H, U, P, U, P)(
                        command, self.handle, self.layout, color, 1, C.byref(sub))
                    self.transition(command, self.home_layout)
                device.execute(poison)
        except BaseException:
            self.close()
            raise

    def transition(self, command, layout):
        barrier = info(ImageMemoryBarrier, srcAccessMask=0 if self.layout == 0 else 0x18000,
            dstAccessMask=0x18000, oldLayout=self.layout, newLayout=layout,
            srcQueueFamilyIndex=0xffffffff, dstQueueFamilyIndex=0xffffffff, image=self.handle,
            subresourceRange=ImageSubresourceRange(aspectMask=1, levelCount=1, layerCount=1))
        self.device.call("vkCmdPipelineBarrier", None, H, U, U, U, U, P, U, P, U, P)(
            command, 1 if self.layout == 0 else 0x10000, 0x10000, 0, 0, None, 0, None, 1, C.byref(barrier))
        self.layout = layout

    def copy(self, command, buffer, offset=0, upload=False):
        self.transition(command, 7 if upload else 6)
        region = BufferImageCopy(bufferOffset=offset,
            imageSubresource=ImageSubresourceLayers(aspectMask=1, layerCount=1),
            imageExtent=Extent3D(self.width, self.height, 1))
        if upload:
            self.device.call("vkCmdCopyBufferToImage", None, H, H, H, U, U, P)(
                command, buffer.buffer, self.handle, 7, 1, C.byref(region))
        else:
            self.device.call("vkCmdCopyImageToBuffer", None, H, H, U, H, U, P)(
                command, self.handle, 6, buffer.buffer, 1, C.byref(region))
        self.transition(command, self.home_layout)

    def resource(self, output=False):
        desc = FX.ResourceDescription(type=2, format=self.ffx_fmt, width=self.width,
            height=self.height, depth=1, mipCount=1, usage=2 if output else 0)
        return FX.Resource(resource=self.handle, description=desc, state=2 if output else 4)

    def close(self):
        d = self.device
        if self.handle:
            d.call("vkDestroyImage", None, H, H, P)(d.handle, self.handle, None)
            self.handle = H()
        if self.memory:
            d.call("vkFreeMemory", None, H, H, P)(d.handle, self.memory, None)
            self.memory = H()


class Context:
    def __init__(self, runtime, device, dimensions):
        self.runtime, self.device, self.handle = runtime, device, H()
        gdpa = device.call("vkGetDeviceProcAddr", P, H, S)
        for required in (b"vkGetBufferMemoryRequirements2KHR", b"vkCmdBeginDebugUtilsLabelEXT", b"vkCmdEndDebugUtilsLabelEXT"):
            if not gdpa(device.handle, required):
                raise VulkanError("Pinned SDK entry point unavailable: " + required.decode()
                                  + ". Enable the SDK's KHR memory and EXT_debug_utils requirements first.")
        self.messages = []
        self.callback = C.CFUNCTYPE(None, U, C.c_wchar_p)(
            lambda kind, message: self.messages.append({"kind": kind, "message": str(message)}))
        self.override = FX.OverrideVersion(header=FX.Header(5), versionId=runtime.version["id"])
        self.backend = FX.BackendVK(header=FX.Header(3, address(self.override)), device=device.handle,
            physicalDevice=device.vk.physical[device.gpu["index"]],
            getDeviceProcAddr=C.cast(device.call("vkGetDeviceProcAddr", P, H, S), P))
        iw, ih, ow, oh = dimensions
        self.description = FX.CreateUpscale(header=FX.Header(0x10000, address(self.backend)),
            flags=32 | 128, maxRenderSize=FX.Dimensions(iw, ih), maxUpscaleSize=FX.Dimensions(ow, oh),
            message=C.cast(self.callback, P))
        start = time.perf_counter()
        ffx_check(runtime.dll.ffxCreateContext(C.byref(self.handle), C.byref(self.description), None), "create FSR 3.1.3 context")
        self.create_wall_ms = (time.perf_counter() - start) * 1000

    def close(self):
        if self.handle:
            ffx_check(self.runtime.dll.ffxDestroyContext(C.byref(self.handle), None), "destroy FSR context")
            self.handle = H()


class Session:
    def __init__(self, runtime, device, dimensions):
        self.runtime, self.device, self.dimensions = runtime, device, dimensions
        self.images, self.context, self.query = [], None, H()
        self.completed = 0
        iw, ih, ow, oh = dimensions
        try:
            for params in [(iw, ih, 97, 4, 8, False), (iw, ih, 100, 28, 4, False),
                           (iw, ih, 83, 18, 4, False), (ow, oh, 97, 4, 8, True)]:
                self.images.append(Image(device, *params))
            self.input_size = sum(im.size for im in self.images[:3])
            self.output_size = self.images[3].size
            self.upload = device.buffer(self.input_size, True)
            self.readback = device.buffer(self.output_size, True, True)
            self.context = Context(runtime, device, dimensions)
            self.query = device.create("vkCreateQueryPool", QueryPoolCreateInfo, "vkDestroyQueryPool",
                                       queryType=2, queryCount=2)
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.context:
            self.context.close()
            self.context = None
        for image in reversed(self.images):
            image.close()
        self.images.clear()

    def reset_context(self):
        self.context.close()
        self.context = Context(self.runtime, self.device, self.dimensions)

    def record_upload(self, command):
        self.device.barrier(command, self.upload)
        offset = 0
        for image in self.images[:3]:
            image.copy(command, self.upload, offset, True)
            offset += image.size

    def prepare(self, index):
        iw, ih, ow, _ = self.dimensions
        values = make_inputs(iw, ih, jitter(index, iw, ow))
        C.memmove(self.upload.mapped, values, len(values))
        self.upload.flush()
        self.device.execute(self.record_upload)

    def record_dispatch(self, command, index, fps, jitter_value=None, frame_delta=None, reset=None, motion_scale=None):
        iw, ih, ow, oh = self.dimensions
        dx, dy = jitter(index, iw, ow) if jitter_value is None else jitter_value
        desc = FX.DispatchUpscale(header=FX.Header(0x10001), commandList=command,
            color=self.images[0].resource(), depth=self.images[1].resource(),
            motionVectors=self.images[2].resource(), output=self.images[3].resource(True),
            jitterOffset=FX.Coords(dx, dy), motionVectorScale=FX.Coords(*(motion_scale if motion_scale is not None else (iw, ih))),
            renderSize=FX.Dimensions(iw, ih), upscaleSize=FX.Dimensions(ow, oh),
            frameTimeDelta=1000 / fps if frame_delta is None else frame_delta,
            preExposure=1, reset=index == 0 if reset is None else reset,
            cameraNear=.1, cameraFar=100, cameraFovAngleVertical=math.pi / 3, viewSpaceToMetersFactor=1)
        d = self.device
        d.call("vkCmdResetQueryPool", None, H, H, U, U)(command, self.query, 0, 2)
        d.call("vkCmdWriteTimestamp", None, H, U, H, U)(command, 1, self.query, 0)
        ffx_check(self.runtime.dll.ffxDispatch(C.byref(self.context.handle), C.byref(desc)), "dispatch FSR 3.1.3")
        d.call("vkCmdWriteTimestamp", None, H, U, H, U)(command, 0x2000, self.query, 1)

    def gpu_time(self):
        errors = [m["message"] for m in self.context.messages if m["kind"] == 0]
        if errors:
            raise VulkanError("FSR runtime validation: " + " | ".join(errors))
        result = (Q * 2)()
        d = self.device
        check(d.call("vkGetQueryPoolResults", I, H, H, U, U, C.c_size_t, P, Q, U)(
            d.handle, self.query, 0, 2, C.sizeof(result), result, 8, 3), "read FSR timestamps")
        bits = next(q["timestamp_valid_bits"] for q in d.gpu["queues"] if q["compute"] and q["count"])
        if not bits:
            raise VulkanError("Compute queue has no valid GPU timestamp bits.")
        ticks = (result[1] - result[0]) & ((1 << bits) - 1)
        return ticks * d.gpu["limits"]["timestampPeriod"] / 1e6

    def read_output(self):
        def record(command):
            self.images[3].copy(command, self.readback)
            self.device.barrier(command, self.readback)
        self.device.execute(record)
        self.readback.invalidate()
        return C.string_at(self.readback.mapped, self.output_size)


def standalone(session, samples, fps):
    wall, gpu = [], []
    for index in range(samples + 5):
        session.prepare(index)
        start = time.perf_counter()
        session.device.execute(lambda command: session.record_dispatch(command, index, fps))
        elapsed = (time.perf_counter() - start) * 1000
        session.completed += 1
        duration = session.gpu_time()
        if index >= 5:
            wall.append(elapsed)
            gpu.append(duration)
    result = session.read_output()
    _, _, width, height = session.dimensions
    return {"status": "passed", "gpu_name": session.device.gpu["name"], "wall": stats(wall), "gpu": stats(gpu),
            "context_create_wall_ms": session.context.create_wall_ms,
            "runtime_messages": session.context.messages,
            "output_validation": validate_image(result, width, height)}, result


def offload(source, target, samples, fps):
    d = source.device
    packed = d.buffer(source.input_size, True, True)
    returned = d.buffer(target.output_size, True)
    wall, gpu = [], []
    target.reset_context()
    def pack(command):
        offset = 0
        for image in source.images[:3]:
            image.copy(command, packed, offset)
            offset += image.size
        d.barrier(command, packed)
    def receive(command):
        d.barrier(command, returned)
        source.images[3].copy(command, returned, upload=True)
    for index in range(samples + 5):
        source.prepare(index)  # models game input images already on NVIDIA
        start = time.perf_counter()
        d.execute(pack)
        packed.invalidate()
        C.memmove(target.upload.mapped, packed.mapped, source.input_size)
        target.upload.flush()
        def process(command):
            target.record_upload(command)
            target.record_dispatch(command, index, fps)
            target.images[3].copy(command, target.readback)
            target.device.barrier(command, target.readback)
        target.device.execute(process)
        target.completed += 1
        target.readback.invalidate()
        C.memmove(returned.mapped, target.readback.mapped, target.output_size)
        returned.flush()
        d.execute(receive)
        elapsed = (time.perf_counter() - start) * 1000
        duration = target.gpu_time()
        # Validate the complete return to NVIDIA after the timing endpoint.
        actual = source.read_output()
        if actual != C.string_at(target.readback.mapped, target.output_size):
            raise VulkanError(f"Offloaded image failed full-byte return validation at sample {index}.")
        if index >= 5:
            wall.append(elapsed)
            gpu.append(duration)
    _, _, width, height = source.dimensions
    return {"status": "passed", "wall": stats(wall), "amd_fsr_gpu": stats(gpu),
            "input_bytes": source.input_size, "output_bytes": target.output_size,
            "all_returned_bytes_verified": True, "runtime_messages": target.context.messages,
            "output_validation": validate_image(actual, width, height)}, actual


def benchmark(vk, target_gpu, source_gpu, resolution="1080p", samples=20, fps=60, artifact_dir=None):
    if resolution not in PRESETS or samples not in (10, 20, 40) or fps not in (30, 60, 120):
        raise ValueError("Invalid FSR benchmark settings.")
    if source_gpu is None or source_gpu["uuid"] == target_gpu["uuid"]:
        raise ValueError("FSR comparison requires two different physical GPUs.")
    dimensions = PRESETS[resolution]
    result = {"status": "failed", "runtime_executed": False, "resolution": resolution,
              "input_size": list(dimensions[:2]), "output_size": list(dimensions[2:]),
              "samples": samples, "warmups": 5, "target_fps": fps,
              "source_uuid": source_gpu["uuid"], "target_uuid": target_gpu["uuid"],
              "detail": "Actual pinned FSR 3.1.3 with auto exposure, sharpening off, a jittered static analytic plane, "
                        "constant depth and zero unjittered motion. Source-input preparation and output validation "
                        "are outside timing. Offload wall time includes actual image packing/copies, CPU staging, "
                        "AMD FSR, return image upload, and waits. No CK3/DLSS workload or overlap is measured."}
    source_session = target_session = None
    try:
        runtime = Runtime()
        result["runtime"] = {"provider": runtime.version, "available_providers": runtime.versions,
                             "sha256": RUNTIME_SHA256, "sdk_commit": SDK_COMMIT}
        result["phase"] = "create logical devices"
        with Device(vk, source_gpu, fsr=True) as source_device, Device(vk, target_gpu, fsr=True) as target_device:
            try:
                result["phase"] = "create source FSR session"
                source_session = Session(runtime, source_device, dimensions)
                result["phase"] = "create target FSR session"
                target_session = Session(runtime, target_device, dimensions)
                result["phase"] = "source FSR dispatches"
                result["source_fsr"], reference = standalone(source_session, samples, fps)
                result["phase"] = "target FSR dispatches"
                result["target_fsr"], target_image = standalone(target_session, samples, fps)
                width, height = dimensions[2:]
                result["cross_gpu_image_comparison"] = compare_images(reference, target_image, width, height)
                result["phase"] = "complete offload dispatches"
                result["offload"], offloaded_image = offload(source_session, target_session, samples, fps)
                result["offload_image_comparison"] = compare_images(reference, offloaded_image, width, height)
                direct = result["source_fsr"]["wall"]["median_ms"]
                split = result["offload"]["wall"]["median_ms"]
                result["comparison"] = {"direct_median_ms": direct, "offload_median_ms": split,
                    "offload_minus_direct_ms": split - direct, "offload_to_direct_ratio": split / direct,
                    "offload_faster": split < direct,
                    "verdict": "offload_slower" if split >= direct else "offload_faster_in_synthetic_test"}
                if artifact_dir:
                    folder = Path(artifact_dir)
                    folder.mkdir(parents=True, exist_ok=True)
                    for name, payload in [("nvidia-fsr", reference), ("vega-fsr", target_image), ("offloaded-fsr", offloaded_image)]:
                        path = folder / (name + ".png")
                        save_png(path, payload, width, height)
                        result.setdefault("previews", {})[name] = str(path.resolve())
                result["status"] = "passed"
                result["phase"] = "complete"
            finally:
                for session in (target_session, source_session):
                    if session:
                        session.close()
    except Exception as exc:
        result["error"] = str(exc)
        import traceback
        result["error_traceback"] = traceback.format_exc()
    finally:
        result["runtime_executed"] = any(session and session.completed for session in (source_session, target_session))
    return result
