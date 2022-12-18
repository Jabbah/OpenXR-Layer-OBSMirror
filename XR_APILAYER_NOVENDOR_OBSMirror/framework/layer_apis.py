# The list of OpenXR functions our layer will override.
override_functions = [
    "xrCreateSession",
    "xrCreateSwapchain",
    "xrDestroySwapchain",
    "xrEnumerateSwapchainImages",
    "xrAcquireSwapchainImage",
    "xrReleaseSwapchainImage",
    "xrLocateViews",
    "xrBeginFrame",
    "xrEndFrame",
    "xrCreateReferenceSpace"
]

# The list of OpenXR functions our layer will use from the runtime.
# Might repeat entries from override_functions above.
requested_functions = [
    "xrGetInstanceProperties",
    "xrGetSystemProperties",
    "xrGetSystem",
    "xrEnumerateViewConfigurationViews"
]

# The list of OpenXR extensions our layer will either override or use.
extensions = []
