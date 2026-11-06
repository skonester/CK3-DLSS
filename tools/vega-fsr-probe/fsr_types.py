"""ctypes ABI for AMD FidelityFX SDK v1.1.3's FSR 3.1 Vulkan API.

Declarations follow the pinned AMD headers. See AMD-FSR-LICENSE.txt.
"""
import ctypes as C
from vk_types import U, Q, F, P, H, structure

Header = structure("FfxHeader", [("type", Q), ("pNext", P)])
Dimensions = structure("FfxDimensions", [("width", U), ("height", U)])
Coords = structure("FfxCoords", [("x", F), ("y", F)])
BackendVK = structure("FfxBackendVK", [("header", Header), ("device", H),
    ("physicalDevice", H), ("getDeviceProcAddr", P)])
GetVersions = structure("FfxGetVersions", [("header", Header), ("createDescType", Q),
    ("device", P), ("outputCount", P), ("versionIds", P), ("versionNames", P)])
OverrideVersion = structure("FfxOverrideVersion", [("header", Header), ("versionId", Q)])
CreateUpscale = structure("FfxCreateUpscale", [("header", Header), ("flags", U),
    ("maxRenderSize", Dimensions), ("maxUpscaleSize", Dimensions), ("message", P)])
ResourceDescription = structure("FfxResourceDescription", [(name, U) for name in
    "type format width height depth mipCount flags usage".split()])
Resource = structure("FfxResource", [("resource", P), ("description", ResourceDescription), ("state", U)])
DispatchUpscale = structure("FfxDispatchUpscale", [("header", Header), ("commandList", P)] + [
    (name, Resource) for name in "color depth motionVectors exposure reactive transparencyAndComposition output".split()
] + [("jitterOffset", Coords), ("motionVectorScale", Coords), ("renderSize", Dimensions),
    ("upscaleSize", Dimensions), ("enableSharpening", C.c_bool), ("sharpness", F),
    ("frameTimeDelta", F), ("preExposure", F), ("reset", C.c_bool), ("cameraNear", F),
    ("cameraFar", F), ("cameraFovAngleVertical", F), ("viewSpaceToMetersFactor", F), ("flags", U)])
