"""Minimal 64-bit Vulkan ABI declarations, following the Khronos Vulkan headers.

Only the core/feature-query structures used by this diagnostic are declared.
The installed Vulkan loader/driver is used directly; no Python Vulkan package or
Vulkan SDK is needed at runtime.
"""
import ctypes as C

U = C.c_uint32
I = C.c_int32
Q = C.c_uint64
F = C.c_float
P = C.c_void_p
S = C.c_char_p
H = C.c_void_p  # All handles are 64 bits on the supported Windows x64 platform.


def ufields(names):
    return [(name, U) for name in names.split()]


def structure(name, fields, stype=None):
    cls = type(name, (C.Structure,), {"_fields_": fields})
    cls.stype = stype
    return cls


def tagged(name, fields, stype):
    return structure(name, [("sType", U), ("pNext", P)] + fields, stype)


def info(cls, **kwargs):
    obj = cls(**kwargs)
    if cls.stype is not None:
        obj.sType = cls.stype
    return obj


def address(obj):
    return C.cast(C.pointer(obj), P)


Features = structure("Features", ufields("""
robustBufferAccess fullDrawIndexUint32 imageCubeArray independentBlend geometryShader
tessellationShader sampleRateShading dualSrcBlend logicOp multiDrawIndirect
drawIndirectFirstInstance depthClamp depthBiasClamp fillModeNonSolid depthBounds
wideLines largePoints alphaToOne multiViewport samplerAnisotropy textureCompressionETC2
textureCompressionASTC_LDR textureCompressionBC occlusionQueryPrecise pipelineStatisticsQuery
vertexPipelineStoresAndAtomics fragmentStoresAndAtomics shaderTessellationAndGeometryPointSize
shaderImageGatherExtended shaderStorageImageExtendedFormats shaderStorageImageMultisample
shaderStorageImageReadWithoutFormat shaderStorageImageWriteWithoutFormat
shaderUniformBufferArrayDynamicIndexing shaderSampledImageArrayDynamicIndexing
shaderStorageBufferArrayDynamicIndexing shaderStorageImageArrayDynamicIndexing
shaderClipDistance shaderCullDistance shaderFloat64 shaderInt64 shaderInt16
shaderResourceResidency shaderResourceMinLod sparseBinding sparseResidencyBuffer
sparseResidencyImage2D sparseResidencyImage3D sparseResidency2Samples sparseResidency4Samples
sparseResidency8Samples sparseResidency16Samples sparseResidencyAliased variableMultisampleRate
inheritedQueries
"""))

Limits = structure("Limits", ufields("""
maxImageDimension1D maxImageDimension2D maxImageDimension3D maxImageDimensionCube
maxImageArrayLayers maxTexelBufferElements maxUniformBufferRange maxStorageBufferRange
maxPushConstantsSize maxMemoryAllocationCount maxSamplerAllocationCount
""") + [("bufferImageGranularity", Q), ("sparseAddressSpaceSize", Q)] + ufields("""
maxBoundDescriptorSets maxPerStageDescriptorSamplers maxPerStageDescriptorUniformBuffers
maxPerStageDescriptorStorageBuffers maxPerStageDescriptorSampledImages
maxPerStageDescriptorStorageImages maxPerStageDescriptorInputAttachments maxPerStageResources
maxDescriptorSetSamplers maxDescriptorSetUniformBuffers maxDescriptorSetUniformBuffersDynamic
maxDescriptorSetStorageBuffers maxDescriptorSetStorageBuffersDynamic maxDescriptorSetSampledImages
maxDescriptorSetStorageImages maxDescriptorSetInputAttachments maxVertexInputAttributes
maxVertexInputBindings maxVertexInputAttributeOffset maxVertexInputBindingStride
maxVertexOutputComponents maxTessellationGenerationLevel maxTessellationPatchSize
maxTessellationControlPerVertexInputComponents maxTessellationControlPerVertexOutputComponents
maxTessellationControlPerPatchOutputComponents maxTessellationControlTotalOutputComponents
maxTessellationEvaluationInputComponents maxTessellationEvaluationOutputComponents
maxGeometryShaderInvocations maxGeometryInputComponents maxGeometryOutputComponents
maxGeometryOutputVertices maxGeometryTotalOutputComponents maxFragmentInputComponents
maxFragmentOutputAttachments maxFragmentDualSrcAttachments maxFragmentCombinedOutputResources
maxComputeSharedMemorySize
""") + [("maxComputeWorkGroupCount", U * 3), ("maxComputeWorkGroupInvocations", U),
       ("maxComputeWorkGroupSize", U * 3)] + ufields("""
subPixelPrecisionBits subTexelPrecisionBits mipmapPrecisionBits maxDrawIndexedIndexValue
maxDrawIndirectCount
""") + [("maxSamplerLodBias", F), ("maxSamplerAnisotropy", F), ("maxViewports", U),
       ("maxViewportDimensions", U * 2), ("viewportBoundsRange", F * 2),
       ("viewportSubPixelBits", U), ("minMemoryMapAlignment", C.c_size_t),
       ("minTexelBufferOffsetAlignment", Q), ("minUniformBufferOffsetAlignment", Q),
       ("minStorageBufferOffsetAlignment", Q), ("minTexelOffset", I), ("maxTexelOffset", U),
       ("minTexelGatherOffset", I), ("maxTexelGatherOffset", U),
       ("minInterpolationOffset", F), ("maxInterpolationOffset", F)] + ufields("""
subPixelInterpolationOffsetBits maxFramebufferWidth maxFramebufferHeight maxFramebufferLayers
framebufferColorSampleCounts framebufferDepthSampleCounts framebufferStencilSampleCounts
framebufferNoAttachmentsSampleCounts maxColorAttachments sampledImageColorSampleCounts
sampledImageIntegerSampleCounts sampledImageDepthSampleCounts sampledImageStencilSampleCounts
storageImageSampleCounts maxSampleMaskWords timestampComputeAndGraphics
""") + [("timestampPeriod", F)] + ufields("""
maxClipDistances maxCullDistances maxCombinedClipAndCullDistances discreteQueuePriorities
""") + [("pointSizeRange", F * 2), ("lineWidthRange", F * 2),
       ("pointSizeGranularity", F), ("lineWidthGranularity", F), ("strictLines", U),
       ("standardSampleLocations", U), ("optimalBufferCopyOffsetAlignment", Q),
       ("optimalBufferCopyRowPitchAlignment", Q), ("nonCoherentAtomSize", Q)])

Properties = structure("Properties", ufields("apiVersion driverVersion vendorID deviceID deviceType") + [
    ("deviceName", C.c_char * 256), ("pipelineCacheUUID", C.c_ubyte * 16),
    ("limits", Limits), ("sparseProperties", U * 5)])
Features2 = tagged("Features2", [("features", Features)], 1000059000)
Properties2 = tagged("Properties2", [("properties", Properties)], 1000059001)
IDProperties = tagged("IDProperties", [("deviceUUID", C.c_ubyte * 16),
    ("driverUUID", C.c_ubyte * 16), ("deviceLUID", C.c_ubyte * 8),
    ("deviceNodeMask", U), ("deviceLUIDValid", U)], 1000071004)
SubgroupProperties = tagged("SubgroupProperties", ufields(
    "subgroupSize supportedStages supportedOperations quadOperationsInAllStages"), 1000094000)
DriverProperties = tagged("DriverProperties", [("driverID", U),
    ("driverName", C.c_char * 256), ("driverInfo", C.c_char * 256),
    ("conformanceVersion", C.c_ubyte * 4)], 1000196000)
Float16Features = tagged("Float16Features", ufields("shaderFloat16 shaderInt8"), 1000082000)
Storage16Features = tagged("Storage16Features", ufields("storageBuffer16BitAccess "
    "uniformAndStorageBuffer16BitAccess storagePushConstant16 storageInputOutput16"), 1000083000)
TimelineFeatures = tagged("TimelineFeatures", [("timelineSemaphore", U)], 1000207000)
Extent3D = structure("Extent3D", ufields("width height depth"))
QueueProperties = structure("QueueProperties", ufields("queueFlags queueCount timestampValidBits") + [
    ("minImageTransferGranularity", Extent3D)])
MemoryType = structure("MemoryType", ufields("propertyFlags heapIndex"))
MemoryHeap = structure("MemoryHeap", [("size", Q), ("flags", U)])
MemoryProperties = structure("MemoryProperties", [("memoryTypeCount", U),
    ("memoryTypes", MemoryType * 32), ("memoryHeapCount", U), ("memoryHeaps", MemoryHeap * 16)])
ExtensionProperties = structure("ExtensionProperties", [("extensionName", C.c_char * 256), ("specVersion", U)])
FormatProperties = structure("FormatProperties", ufields("linearTilingFeatures optimalTilingFeatures bufferFeatures"))

ApplicationInfo = tagged("ApplicationInfo", [("pApplicationName", S), ("applicationVersion", U),
    ("pEngineName", S), ("engineVersion", U), ("apiVersion", U)], 0)
InstanceCreateInfo = tagged("InstanceCreateInfo", [("flags", U), ("pApplicationInfo", P),
    ("enabledLayerCount", U), ("ppEnabledLayerNames", P), ("enabledExtensionCount", U),
    ("ppEnabledExtensionNames", P)], 1)
DeviceQueueCreateInfo = tagged("DeviceQueueCreateInfo", [("flags", U), ("queueFamilyIndex", U),
    ("queueCount", U), ("pQueuePriorities", P)], 2)
DeviceCreateInfo = tagged("DeviceCreateInfo", [("flags", U), ("queueCreateInfoCount", U),
    ("pQueueCreateInfos", P), ("enabledLayerCount", U), ("ppEnabledLayerNames", P),
    ("enabledExtensionCount", U), ("ppEnabledExtensionNames", P), ("pEnabledFeatures", P)], 3)
BufferCreateInfo = tagged("BufferCreateInfo", [("flags", U), ("size", Q), ("usage", U),
    ("sharingMode", U), ("queueFamilyIndexCount", U), ("pQueueFamilyIndices", P)], 12)
MemoryRequirements = structure("MemoryRequirements", [("size", Q), ("alignment", Q), ("memoryTypeBits", U)])
MemoryAllocateInfo = tagged("MemoryAllocateInfo", [("allocationSize", Q), ("memoryTypeIndex", U)], 5)
MappedMemoryRange = tagged("MappedMemoryRange", [("memory", H), ("offset", Q), ("size", Q)], 6)
CommandPoolCreateInfo = tagged("CommandPoolCreateInfo", [("flags", U), ("queueFamilyIndex", U)], 39)
CommandBufferAllocateInfo = tagged("CommandBufferAllocateInfo", [("commandPool", H),
    ("level", U), ("commandBufferCount", U)], 40)
CommandBufferBeginInfo = tagged("CommandBufferBeginInfo", [("flags", U), ("pInheritanceInfo", P)], 42)
SubmitInfo = tagged("SubmitInfo", [("waitSemaphoreCount", U), ("pWaitSemaphores", P),
    ("pWaitDstStageMask", P), ("commandBufferCount", U), ("pCommandBuffers", P),
    ("signalSemaphoreCount", U), ("pSignalSemaphores", P)], 4)
FenceCreateInfo = tagged("FenceCreateInfo", [("flags", U)], 8)
BufferCopy = structure("BufferCopy", [("srcOffset", Q), ("dstOffset", Q), ("size", Q)])
BufferMemoryBarrier = tagged("BufferMemoryBarrier", [("srcAccessMask", U), ("dstAccessMask", U),
    ("srcQueueFamilyIndex", U), ("dstQueueFamilyIndex", U), ("buffer", H),
    ("offset", Q), ("size", Q)], 44)
ShaderModuleCreateInfo = tagged("ShaderModuleCreateInfo", [("flags", U),
    ("codeSize", C.c_size_t), ("pCode", P)], 16)
DescriptorSetLayoutBinding = structure("DescriptorSetLayoutBinding", ufields(
    "binding descriptorType descriptorCount stageFlags") + [("pImmutableSamplers", P)])
DescriptorSetLayoutCreateInfo = tagged("DescriptorSetLayoutCreateInfo", [("flags", U),
    ("bindingCount", U), ("pBindings", P)], 32)
PipelineLayoutCreateInfo = tagged("PipelineLayoutCreateInfo", [("flags", U),
    ("setLayoutCount", U), ("pSetLayouts", P), ("pushConstantRangeCount", U),
    ("pPushConstantRanges", P)], 30)
PipelineShaderStageCreateInfo = tagged("PipelineShaderStageCreateInfo", [("flags", U),
    ("stage", U), ("module", H), ("pName", S), ("pSpecializationInfo", P)], 18)
ComputePipelineCreateInfo = tagged("ComputePipelineCreateInfo", [("flags", U),
    ("stage", PipelineShaderStageCreateInfo), ("layout", H), ("basePipelineHandle", H),
    ("basePipelineIndex", I)], 29)
DescriptorPoolSize = structure("DescriptorPoolSize", ufields("type descriptorCount"))
DescriptorPoolCreateInfo = tagged("DescriptorPoolCreateInfo", [("flags", U), ("maxSets", U),
    ("poolSizeCount", U), ("pPoolSizes", P)], 33)
DescriptorSetAllocateInfo = tagged("DescriptorSetAllocateInfo", [("descriptorPool", H),
    ("descriptorSetCount", U), ("pSetLayouts", P)], 34)
DescriptorBufferInfo = structure("DescriptorBufferInfo", [("buffer", H), ("offset", Q), ("range", Q)])
WriteDescriptorSet = tagged("WriteDescriptorSet", [("dstSet", H), ("dstBinding", U),
    ("dstArrayElement", U), ("descriptorCount", U), ("descriptorType", U),
    ("pImageInfo", P), ("pBufferInfo", P), ("pTexelBufferView", P)], 35)

# Additional declarations used only by the actual FSR/image benchmark.
SubgroupExtendedFeatures = tagged("SubgroupExtendedFeatures", [("shaderSubgroupExtendedTypes", U)], 1000175000)
SubgroupControlFeatures = tagged("SubgroupControlFeatures", ufields("subgroupSizeControl computeFullSubgroups"), 1000225002)
ImageCreateInfo = tagged("ImageCreateInfo", [("flags", U), ("imageType", U), ("format", U),
    ("extent", Extent3D), ("mipLevels", U), ("arrayLayers", U), ("samples", U),
    ("tiling", U), ("usage", U), ("sharingMode", U), ("queueFamilyIndexCount", U),
    ("pQueueFamilyIndices", P), ("initialLayout", U)], 14)
ImageSubresourceRange = structure("ImageSubresourceRange", ufields("aspectMask baseMipLevel levelCount baseArrayLayer layerCount"))
ImageSubresourceLayers = structure("ImageSubresourceLayers", ufields("aspectMask mipLevel baseArrayLayer layerCount"))
Offset3D = structure("Offset3D", [("x", I), ("y", I), ("z", I)])
ImageMemoryBarrier = tagged("ImageMemoryBarrier", [("srcAccessMask", U), ("dstAccessMask", U),
    ("oldLayout", U), ("newLayout", U), ("srcQueueFamilyIndex", U), ("dstQueueFamilyIndex", U),
    ("image", H), ("subresourceRange", ImageSubresourceRange)], 45)
BufferImageCopy = structure("BufferImageCopy", [("bufferOffset", Q), ("bufferRowLength", U),
    ("bufferImageHeight", U), ("imageSubresource", ImageSubresourceLayers),
    ("imageOffset", Offset3D), ("imageExtent", Extent3D)])
QueryPoolCreateInfo = tagged("QueryPoolCreateInfo", ufields("flags queryType queryCount pipelineStatistics"), 11)
