"""Direct Vulkan capability queries and small, bounded hardware experiments."""
from __future__ import annotations

from array import array
import ctypes as C
import os
from pathlib import Path
import statistics
import time
import uuid

from vk_types import *


class VulkanError(RuntimeError):
    pass


def check(result, operation):
    if result != 0:
        raise VulkanError(f"{operation}: VkResult {result}")


def version(value):
    return f"{(value >> 22) & 127}.{(value >> 12) & 1023}.{value & 4095}"


def text(value):
    return bytes(value).split(b"\0", 1)[0].decode("utf-8", "replace")


def chain(*objects):
    for left, right in zip(objects, objects[1:]):
        left.pNext = address(right)
    return address(objects[0]) if objects else None


class Vulkan:
    def __init__(self, debug_utils=False):
        if os.name != "nt" or C.sizeof(P) != 8:
            raise VulkanError("This probe needs 64-bit Python on Windows 10/11.")
        try:
            self.dll = C.WinDLL("vulkan-1.dll", winmode=0x800)  # system32 only
        except OSError as exc:
            raise VulkanError("Vulkan loader unavailable. Install your GPU vendor's Vulkan-capable driver.") from exc
        self.functions = {}
        self.instance = H()
        loader_version = U(1 << 22)
        if hasattr(self.dll, "vkEnumerateInstanceVersion"):
            check(self.fn("vkEnumerateInstanceVersion", I, P)(C.byref(loader_version)), "loader version")
        if loader_version.value < ((1 << 22) | (1 << 12)):
            raise VulkanError("Vulkan 1.1 or later is required for this diagnostic.")
        self.api = min(loader_version.value, (1 << 22) | (2 << 12))
        app = info(ApplicationInfo, pApplicationName=b"Vega FSR Probe", applicationVersion=1,
                   pEngineName=b"Diagnostic", apiVersion=self.api)
        extension_names = (S * 1)(b"VK_EXT_debug_utils") if debug_utils else None
        ci = info(InstanceCreateInfo, pApplicationInfo=address(app),
                  enabledExtensionCount=1 if debug_utils else 0,
                  ppEnabledExtensionNames=C.cast(extension_names, P) if extension_names is not None else None)
        check(self.fn("vkCreateInstance", I, P, P, P)(C.byref(ci), None, C.byref(self.instance)), "vkCreateInstance")
        self.physical = []
        try:
            count = U()
            enum = self.fn("vkEnumeratePhysicalDevices", I, H, P, P)
            check(enum(self.instance, C.byref(count), None), "enumerate GPU count")
            devices = (H * count.value)()
            check(enum(self.instance, C.byref(count), devices), "enumerate GPUs")
            self.physical = list(devices)[:count.value]
        except BaseException:
            self.close()
            raise

    def fn(self, name, result, *args):
        if name not in self.functions:
            fn = getattr(self.dll, name)
            fn.restype = result
            fn.argtypes = list(args)
            self.functions[name] = fn
        return self.functions[name]

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if self.instance:
            self.fn("vkDestroyInstance", None, H, P)(self.instance, None)
            self.instance = H()

    def describe(self, physical, index):
        props = Properties()
        self.fn("vkGetPhysicalDeviceProperties", None, H, P)(physical, C.byref(props))
        ext_count = U()
        enum_ext = self.fn("vkEnumerateDeviceExtensionProperties", I, H, S, P, P)
        check(enum_ext(physical, None, C.byref(ext_count), None), "extension count")
        ext_array = (ExtensionProperties * ext_count.value)()
        check(enum_ext(physical, None, C.byref(ext_count), ext_array), "extensions")
        extensions = sorted(text(e.extensionName) for e in ext_array[:ext_count.value])
        ids, sub, driver = info(IDProperties), info(SubgroupProperties), info(DriverProperties)
        props_chain = [ids, sub]
        has_driver_props = props.apiVersion >= ((1 << 22) | (2 << 12)) or "VK_KHR_driver_properties" in extensions
        if has_driver_props:
            props_chain.append(driver)
        props2 = info(Properties2, pNext=chain(*props_chain))
        self.fn("vkGetPhysicalDeviceProperties2", None, H, P)(physical, C.byref(props2))
        storage, fp16, timeline = info(Storage16Features), info(Float16Features), info(TimelineFeatures)
        feature_chain = [storage]
        api12 = props.apiVersion >= ((1 << 22) | (2 << 12))
        has_fp16 = api12 or "VK_KHR_shader_float16_int8" in extensions
        has_timeline = api12 or "VK_KHR_timeline_semaphore" in extensions
        if has_fp16:
            feature_chain.append(fp16)
        if has_timeline:
            feature_chain.append(timeline)
        f2 = info(Features2, pNext=chain(*feature_chain))
        self.fn("vkGetPhysicalDeviceFeatures2", None, H, P)(physical, C.byref(f2))
        features = {name: bool(getattr(f2.features, name)) for name, _ in Features._fields_}
        for obj in feature_chain:
            features.update({name: bool(getattr(obj, name)) for name, _ in obj._fields_[2:]})
        count = U()
        get_queues = self.fn("vkGetPhysicalDeviceQueueFamilyProperties", None, H, P, P)
        get_queues(physical, C.byref(count), None)
        queues = (QueueProperties * count.value)()
        get_queues(physical, C.byref(count), queues)
        queue_info = [{"index": i, "count": q.queueCount, "graphics": bool(q.queueFlags & 1),
                       "compute": bool(q.queueFlags & 2), "transfer": bool(q.queueFlags & 7),
                       "timestamp_valid_bits": q.timestampValidBits} for i, q in enumerate(queues[:count.value])]
        memory = MemoryProperties()
        self.fn("vkGetPhysicalDeviceMemoryProperties", None, H, P)(physical, C.byref(memory))
        formats = {}
        for name, code in [("RGBA8_UNORM", 37), ("RGBA16_UNORM", 91), ("RGBA16_SFLOAT", 97),
                           ("RG16_SFLOAT", 83), ("R32_SFLOAT", 100)]:
            fmt = FormatProperties()
            self.fn("vkGetPhysicalDeviceFormatProperties", None, H, U, P)(physical, code, C.byref(fmt))
            flags = fmt.optimalTilingFeatures
            formats[name] = {"sampled_image": bool(flags & 1), "storage_image": bool(flags & 2),
                             "linear_filter": bool(flags & 0x1000), "transfer_src": bool(flags & 0x4000),
                             "transfer_dst": bool(flags & 0x8000), "optimal_tiling_flags": flags}
        selected_limits = ["maxImageDimension2D", "maxStorageBufferRange", "maxComputeSharedMemorySize",
                           "maxComputeWorkGroupInvocations", "maxPerStageDescriptorStorageImages",
                           "timestampPeriod"]
        limits = {name: getattr(props.limits, name) for name in selected_limits}
        limits["maxComputeWorkGroupCount"] = list(props.limits.maxComputeWorkGroupCount)
        limits["maxComputeWorkGroupSize"] = list(props.limits.maxComputeWorkGroupSize)
        return {
            "index": index, "uuid": str(uuid.UUID(bytes=bytes(ids.deviceUUID))),
            "name": text(props.deviceName), "vendor_id": props.vendorID, "device_id": props.deviceID,
            "device_type": {1: "integrated", 2: "discrete", 3: "virtual", 4: "cpu"}.get(props.deviceType, "other"),
            "api_version": version(props.apiVersion), "api_version_raw": props.apiVersion,
            "driver_name": text(driver.driverName) if has_driver_props else "Unknown",
            "driver_info": text(driver.driverInfo) if has_driver_props else "Unknown",
            "driver_version_raw": props.driverVersion, "luid_valid": bool(ids.deviceLUIDValid),
            "luid": bytes(ids.deviceLUID).hex() if ids.deviceLUIDValid else None,
            "subgroup_size": sub.subgroupSize, "subgroup_compute": bool(sub.supportedStages & 0x20),
            "subgroup_operations": sub.supportedOperations,
            "subgroup_operation_names": [name for bit, name in [(1, "basic"), (2, "vote"), (4, "arithmetic"),
                (8, "ballot"), (16, "shuffle"), (32, "shuffle_relative"), (64, "clustered"), (128, "quad")]
                if sub.supportedOperations & bit],
            "features": features, "formats": formats, "limits": limits, "queues": queue_info,
            "memory_heaps": [{"size_mib": h.size / 1048576, "device_local": bool(h.flags & 1)}
                             for h in memory.memoryHeaps[:memory.memoryHeapCount]],
            "memory_types": [{"flags": t.propertyFlags, "heap_index": t.heapIndex,
                "device_local": bool(t.propertyFlags & 1), "host_visible": bool(t.propertyFlags & 2),
                "host_coherent": bool(t.propertyFlags & 4), "host_cached": bool(t.propertyFlags & 8)}
                for t in memory.memoryTypes[:memory.memoryTypeCount]],
            "extensions": extensions,
        }

    def inventory(self):
        return [self.describe(p, i) for i, p in enumerate(self.physical)]


class Buffer:
    def __init__(self, device, size, host, readback=False):
        self.owner, self.size = device, size
        self.buffer, self.memory, self.mapped = H(), H(), P()
        ci = info(BufferCreateInfo, size=size, usage=3 | (0 if host else 0x20))
        check(device.call("vkCreateBuffer", I, H, P, P, P)(device.handle, C.byref(ci), None, C.byref(self.buffer)), "create buffer")
        try:
            req = MemoryRequirements()
            device.call("vkGetBufferMemoryRequirements", None, H, H, P)(device.handle, self.buffer, C.byref(req))
            types = device.memory.memoryTypes
            candidates = [i for i in range(device.memory.memoryTypeCount) if req.memoryTypeBits & (1 << i)]
            if host:
                candidates = [i for i in candidates if types[i].propertyFlags & 2]
                # Reading uncached/write-combined staging memory can dominate
                # the whole test. Prefer HOST_CACHED for GPU -> CPU readback.
                candidates.sort(key=lambda i: (
                    bool(types[i].propertyFlags & (8 if readback else 4)),
                    bool(types[i].propertyFlags & 4)), reverse=True)
            else:
                candidates.sort(key=lambda i: bool(types[i].propertyFlags & 1), reverse=True)
            if not candidates:
                raise VulkanError("No suitable buffer memory type.")
            index = candidates[0]
            if not host and not (types[index].propertyFlags & 1):
                raise VulkanError("No device-local buffer memory type.")
            self.coherent = bool(types[index].propertyFlags & 4)
            self.memory_type = index
            self.memory_flags = types[index].propertyFlags
            ai = info(MemoryAllocateInfo, allocationSize=req.size, memoryTypeIndex=index)
            check(device.call("vkAllocateMemory", I, H, P, P, P)(device.handle, C.byref(ai), None, C.byref(self.memory)), "allocate buffer memory")
            check(device.call("vkBindBufferMemory", I, H, H, H, Q)(device.handle, self.buffer, self.memory, 0), "bind buffer memory")
            if host:
                check(device.call("vkMapMemory", I, H, H, Q, Q, U, P)(device.handle, self.memory, 0, req.size, 0,
                      C.byref(self.mapped)), "map staging memory")
        except BaseException:
            self.close()
            raise

    def flush(self):
        if not self.coherent:
            rng = info(MappedMemoryRange, memory=self.memory, size=0xffffffffffffffff)
            check(self.owner.call("vkFlushMappedMemoryRanges", I, H, U, P)(self.owner.handle, 1, C.byref(rng)), "flush staging memory")

    def invalidate(self):
        if not self.coherent:
            rng = info(MappedMemoryRange, memory=self.memory, size=0xffffffffffffffff)
            check(self.owner.call("vkInvalidateMappedMemoryRanges", I, H, U, P)(self.owner.handle, 1, C.byref(rng)), "invalidate staging memory")

    def close(self):
        d = self.owner
        if self.mapped:
            d.call("vkUnmapMemory", None, H, H)(d.handle, self.memory)
            self.mapped = P()
        if self.buffer:
            d.call("vkDestroyBuffer", None, H, H, P)(d.handle, self.buffer, None)
            self.buffer = H()
        if self.memory:
            d.call("vkFreeMemory", None, H, H, P)(d.handle, self.memory, None)
            self.memory = H()


class Device:
    def __init__(self, vk, gpu, fsr=False):
        self.vk, self.gpu = vk, gpu
        self.handle, self.queue, self.pool, self.command, self.fence = H(), H(), H(), H(), H()
        self.cleanups, self.buffers = [], []
        compute_queues = [q for q in gpu["queues"] if q["compute"] and q["count"]]
        if not compute_queues:
            raise VulkanError("Selected adapter has no compute queue.")
        queue_index = compute_queues[0]["index"]
        priority = F(1)
        qci = info(DeviceQueueCreateInfo, queueFamilyIndex=queue_index, queueCount=1, pQueuePriorities=address(priority))
        extension_names = ["VK_KHR_external_memory_win32", "VK_KHR_external_semaphore_win32"]
        fsr_features = []
        core_features = None
        if fsr:
            if gpu["api_version_raw"] < ((1 << 22) | (2 << 12)):
                raise VulkanError("The FSR benchmark requires Vulkan 1.2 or newer.")
            # Enable the advertised shader features used by the pinned FSR
            # runtime, rather than querying supported bits on a featureless device.
            core_features = Features()
            vk.fn("vkGetPhysicalDeviceFeatures", None, H, P)(vk.physical[gpu["index"]], C.byref(core_features))
            fsr_features = [info(Float16Features), info(Storage16Features), info(SubgroupExtendedFeatures)]
            if "VK_EXT_subgroup_size_control" in gpu["extensions"]:
                fsr_features.append(info(SubgroupControlFeatures))
                extension_names.append("VK_EXT_subgroup_size_control")
            query = info(Features2, pNext=chain(*fsr_features))
            vk.fn("vkGetPhysicalDeviceFeatures2", None, H, P)(vk.physical[gpu["index"]], C.byref(query))
            extension_names += ["VK_KHR_shader_float16_int8", "VK_KHR_16bit_storage",
                                "VK_KHR_shader_subgroup_extended_types", "VK_KHR_get_memory_requirements2",
                                "VK_KHR_dedicated_allocation", "VK_KHR_maintenance1"]
        enabled = [e for e in extension_names if e in gpu["extensions"]]
        ext_names = (S * len(enabled))(*(e.encode() for e in enabled))
        ci = info(DeviceCreateInfo, queueCreateInfoCount=1, pQueueCreateInfos=address(qci),
                  enabledExtensionCount=len(enabled), ppEnabledExtensionNames=C.cast(ext_names, P),
                  pNext=chain(*fsr_features), pEnabledFeatures=address(core_features) if core_features is not None else None)
        check(self.call("vkCreateDevice", I, H, P, P, P)(vk.physical[gpu["index"]], C.byref(ci), None,
              C.byref(self.handle)), "vkCreateDevice on " + gpu["name"])
        self.enabled_extensions = enabled
        try:
            self.call("vkGetDeviceQueue", None, H, U, U, P)(self.handle, queue_index, 0, C.byref(self.queue))
            pci = info(CommandPoolCreateInfo, flags=2, queueFamilyIndex=queue_index)
            check(self.call("vkCreateCommandPool", I, H, P, P, P)(self.handle, C.byref(pci), None, C.byref(self.pool)), "create command pool")
            ai = info(CommandBufferAllocateInfo, commandPool=self.pool, commandBufferCount=1)
            check(self.call("vkAllocateCommandBuffers", I, H, P, P)(self.handle, C.byref(ai), C.byref(self.command)), "allocate command buffer")
            fci = info(FenceCreateInfo)
            check(self.call("vkCreateFence", I, H, P, P, P)(self.handle, C.byref(fci), None, C.byref(self.fence)), "create fence")
            self.memory = MemoryProperties()
            self.call("vkGetPhysicalDeviceMemoryProperties", None, H, P)(vk.physical[gpu["index"]], C.byref(self.memory))
        except BaseException:
            self.close()
            raise

    def call(self, *args):
        return self.vk.fn(*args)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self):
        if not self.handle:
            return
        self.call("vkDeviceWaitIdle", I, H)(self.handle)
        for fn, handle in reversed(self.cleanups):
            self.call(fn, None, H, H, P)(self.handle, handle, None)
        for buf in reversed(self.buffers):
            buf.close()
        if self.fence:
            self.call("vkDestroyFence", None, H, H, P)(self.handle, self.fence, None)
        if self.pool:
            self.call("vkDestroyCommandPool", None, H, H, P)(self.handle, self.pool, None)
        self.call("vkDestroyDevice", None, H, P)(self.handle, None)
        self.handle = H()

    def buffer(self, size, host=False, readback=False):
        buf = Buffer(self, size, host, readback)
        self.buffers.append(buf)
        return buf

    def functions_available(self):
        gdpa = self.call("vkGetDeviceProcAddr", P, H, S)
        names = ["vkCmdDispatch", "vkCmdCopyBuffer", "vkCreateComputePipelines",
                 "vkGetMemoryWin32HandleKHR", "vkGetMemoryWin32HandlePropertiesKHR",
                 "vkImportSemaphoreWin32HandleKHR", "vkGetSemaphoreWin32HandleKHR"]
        return {name: bool(gdpa(self.handle, name.encode())) for name in names}

    def execute(self, record):
        check(self.call("vkResetFences", I, H, U, P)(self.handle, 1, C.byref(self.fence)), "reset fence")
        check(self.call("vkResetCommandBuffer", I, H, U)(self.command, 0), "reset command buffer")
        ci = info(CommandBufferBeginInfo, flags=1)
        check(self.call("vkBeginCommandBuffer", I, H, P)(self.command, C.byref(ci)), "begin command buffer")
        record(self.command)
        check(self.call("vkEndCommandBuffer", I, H)(self.command), "end command buffer")
        submit = info(SubmitInfo, commandBufferCount=1, pCommandBuffers=address(self.command))
        check(self.call("vkQueueSubmit", I, H, U, P, H)(self.queue, 1, C.byref(submit), self.fence), "queue submit")
        check(self.call("vkWaitForFences", I, H, U, P, U, Q)(self.handle, 1, C.byref(self.fence), 1,
              5_000_000_000), "wait for GPU (5 second limit)")

    def barrier(self, command, buf):
        # Conservative barriers: this is a correctness/serial staging baseline.
        b = info(BufferMemoryBarrier, srcAccessMask=0x18000, dstAccessMask=0x18000,
                 srcQueueFamilyIndex=0xffffffff, dstQueueFamilyIndex=0xffffffff,
                 buffer=buf.buffer, size=buf.size)
        self.call("vkCmdPipelineBarrier", None, H, U, U, U, U, P, U, P, U, P)(
            command, 0x14000, 0x14000, 0, 0, None, 1, C.byref(b), 0, None)

    def copy(self, source, target):
        if source.size != target.size:
            raise ValueError("Copy sizes must match.")
        def record(cmd):
            self.barrier(cmd, source)
            self.barrier(cmd, target)
            region = BufferCopy(size=source.size)
            self.call("vkCmdCopyBuffer", None, H, H, H, U, P)(cmd, source.buffer, target.buffer, 1, C.byref(region))
            self.barrier(cmd, target)
        self.execute(record)

    def create(self, name, cls, destroy, **kwargs):
        ci, out = info(cls, **kwargs), H()
        check(self.call(name, I, H, P, P, P)(self.handle, C.byref(ci), None, C.byref(out)), name)
        self.cleanups.append((destroy, out))
        return out

    def compute_smoke(self):
        count = 65536
        pattern = array("I", (((i * 17) ^ 0x13579bdf) & 0xffffffff for i in range(count)))
        expected = array("I", (((v ^ 0x5a39c7e1) + i) & 0xffffffff for i, v in enumerate(pattern))).tobytes()
        payload = pattern.tobytes()
        upload, local, readback = self.buffer(len(payload), True), self.buffer(len(payload)), self.buffer(len(payload), True, True)
        C.memmove(upload.mapped, payload, len(payload))
        upload.flush()
        self.copy(upload, local)
        code = (Path(__file__).parent / "shaders" / "compute_check.spv").read_bytes()
        if len(code) % 4 or code[:4] != b"\x03\x02\x23\x07":
            raise VulkanError("Invalid compute_check.spv. Rebuild from the included GLSL source.")
        words = (U * (len(code) // 4)).from_buffer_copy(code)
        shader = self.create("vkCreateShaderModule", ShaderModuleCreateInfo, "vkDestroyShaderModule",
                             codeSize=len(code), pCode=C.cast(words, P))
        binding = DescriptorSetLayoutBinding(binding=0, descriptorType=7, descriptorCount=1, stageFlags=0x20)
        layout = self.create("vkCreateDescriptorSetLayout", DescriptorSetLayoutCreateInfo, "vkDestroyDescriptorSetLayout",
                             bindingCount=1, pBindings=address(binding))
        pipeline_layout = self.create("vkCreatePipelineLayout", PipelineLayoutCreateInfo, "vkDestroyPipelineLayout",
                                      setLayoutCount=1, pSetLayouts=address(layout))
        stage = info(PipelineShaderStageCreateInfo, stage=0x20, module=shader, pName=b"main")
        ci = info(ComputePipelineCreateInfo, stage=stage, layout=pipeline_layout, basePipelineIndex=-1)
        pipeline = H()
        check(self.call("vkCreateComputePipelines", I, H, H, U, P, P, P)(self.handle, None, 1, C.byref(ci), None,
              C.byref(pipeline)), "create compute pipeline")
        self.cleanups.append(("vkDestroyPipeline", pipeline))
        pool_size = DescriptorPoolSize(type=7, descriptorCount=1)
        pool = self.create("vkCreateDescriptorPool", DescriptorPoolCreateInfo, "vkDestroyDescriptorPool",
                           maxSets=1, poolSizeCount=1, pPoolSizes=address(pool_size))
        ai = info(DescriptorSetAllocateInfo, descriptorPool=pool, descriptorSetCount=1, pSetLayouts=address(layout))
        ds = H()
        check(self.call("vkAllocateDescriptorSets", I, H, P, P)(self.handle, C.byref(ai), C.byref(ds)), "allocate descriptor set")
        bi = DescriptorBufferInfo(buffer=local.buffer, range=local.size)
        write = info(WriteDescriptorSet, dstSet=ds, descriptorCount=1, descriptorType=7, pBufferInfo=address(bi))
        self.call("vkUpdateDescriptorSets", None, H, U, P, U, P)(self.handle, 1, C.byref(write), 0, None)
        def record(cmd):
            self.barrier(cmd, local)
            self.call("vkCmdBindPipeline", None, H, U, H)(cmd, 1, pipeline)
            self.call("vkCmdBindDescriptorSets", None, H, U, H, U, U, P, U, P)(cmd, 1, pipeline_layout, 0, 1,
                       C.byref(ds), 0, None)
            self.call("vkCmdDispatch", None, H, U, U, U)(cmd, count // 64, 1, 1)
            self.barrier(cmd, local)
        start = time.perf_counter()
        self.execute(record)
        elapsed = (time.perf_counter() - start) * 1000
        self.copy(local, readback)
        readback.invalidate()
        if C.string_at(readback.mapped, local.size) != expected:
            raise VulkanError("Compute output mismatch: the GPU result differs from the CPU oracle.")
        return {"status": "passed", "words_verified": count, "submission_wall_ms": elapsed,
                "detail": "65,536 uint32 results matched the CPU oracle. Generic compute only; no FSR shader."}


def percentile(values, fraction):
    ordered = sorted(values)
    at = (len(ordered) - 1) * fraction
    lower = int(at)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (at - lower)


def roundtrip(vk, source_gpu, target_gpu, mib=16, samples=20, fps=60):
    """Serial host-staged buffer transport; no external-memory/P2P or FSR claim."""
    if source_gpu["uuid"] == target_gpu["uuid"]:
        raise ValueError("Choose two different physical GPUs.")
    size = mib * 1048576
    with Device(vk, source_gpu) as source, Device(vk, target_gpu) as target:
        su, sr, sl = source.buffer(size, True), source.buffer(size, True, True), source.buffer(size)
        tu, tr, tl = target.buffer(size, True), target.buffer(size, True, True), target.buffer(size)
        # Pattern is prepared outside timing; vary every sample to catch stale data.
        payload = bytearray(bytes(range(256)) * (size // 256))
        values = []
        for iteration in range(samples + 3):
            payload[:4] = iteration.to_bytes(4, "little")
            initial = bytes(payload)
            C.memmove(su.mapped, initial, size)
            su.flush()
            source.copy(su, sl)
            start = time.perf_counter()
            source.copy(sl, sr)
            sr.invalidate()
            C.memmove(tu.mapped, sr.mapped, size)
            tu.flush()
            target.copy(tu, tl)
            target.copy(tl, tr)
            tr.invalidate()
            C.memmove(su.mapped, tr.mapped, size)
            su.flush()
            source.copy(su, sl)
            elapsed = (time.perf_counter() - start) * 1000
            # Final GPU readback/oracle excluded from timing. Every byte checked.
            source.copy(sl, sr)
            sr.invalidate()
            if C.string_at(sr.mapped, size) != initial:
                raise VulkanError(f"Cross-GPU output mismatch at sample {iteration}.")
            if iteration >= 3:
                values.append(elapsed)
        median = statistics.median(values)
        p95 = percentile(values, .95)
        budget = 1000 / fps
        return {"status": "passed", "method": "serial_host_staging", "source_uuid": source_gpu["uuid"],
                "target_uuid": target_gpu["uuid"], "source_name": source_gpu["name"], "target_name": target_gpu["name"],
                "bytes_each_direction": size, "warmups": 3, "samples": samples, "all_bytes_verified": True,
                "readback_memory_flags": {"source": sr.memory_flags, "target": tr.memory_flags},
                "median_ms": median, "p95_ms": p95, "p99_ms": percentile(values, .99),
                "min_ms": min(values), "max_ms": max(values), "sample_ms": values,
                "payload_gb_per_s": (size * 2 / 1e9) / (median / 1000),
                "target_fps": fps, "frame_budget_ms": budget,
                "median_budget_percent": median / budget * 100,
                "verdict": "staging_exceeds_budget" if p95 >= budget else "staging_fits_budget_fsr_unmeasured",
                "detail": "Wall time includes four GPU copies/submissions, two CPU staging copies, cache maintenance and waits. "
                          "Initial seed and final validation excluded. No FSR dispatch, image packing, engine integration, "
                          "cross-vendor shared handles or pipelined overlap measured."}
