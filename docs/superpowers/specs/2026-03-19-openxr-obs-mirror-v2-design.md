# OpenXR OBS Mirror v2 — Design Spec

**Date:** 2026-03-19
**Status:** Approved by user

---

## Overview

A ground-up rewrite of the OpenXR API layer that mirrors VR application rendering to OBS Studio via shared DXGI textures. The rewrite addresses the following shortcomings of v1:

- Race condition: triple-buffered output textures were allocated but never used — all reads/writes used slot 0
- No cross-process synchronization on shared textures
- Incomplete D3D12 support (missing proper cross-device fence signaling)
- Overly complex dispatch generator framework for six intercepted functions
- Duplicated `MirrorSurfaceData` struct with mismatched types between DLL and plugin
- `copyToMirror()` always writes to index 0 regardless of buffer state

---

## Requirements

- Support D3D11 and D3D12 VR applications
- Use D3D11 to share output images with OBS Studio
- Render both eye projection layers and quad layers to the shared image (projection first, quads on top, in submission order)
- All format conversion done via pixel shaders — no format-specific copy paths in C++
- Quad layers correctly positioned using FOV and pose matrices as seen from the eye viewpoint
- Named Win32 file mapping for IPC (control block + texture handles)
- IDXGIKeyedMutex for cross-process synchronization of output textures
- Triple buffering so the VR application pipeline is not blocked
- Output resolution derived from HMD recommended image rect; OBS handles crop/scale
- OBS plugin with configurable eye mode (left, right, stereo blend) and crop controls
- CMake build system for the layer DLL; OBS plugin built within the OBS Studio source tree

---

## Architecture

```
VR App (D3D11 or D3D12)
    │
    ▼ OpenXR calls
┌─────────────────────────────────────────────────────┐
│  OpenXR Layer DLL (XR_APILAYER_NOVENDOR_OBSMirror)  │
│                                                     │
│  xrCreateInstance    → store instance, resolve ptrs │
│  xrCreateSession     → detect binding, create Comp  │
│  xrDestroySession    → cleanup swapchains + Comp    │
│  xrEnumerateSwapchainImages → alloc staging textures│
│  xrReleaseSwapchainImage    → copy to staging tex   │
│  xrEndFrame          → drive compositor             │
│                                                     │
│  ┌─────────────────────────────────────────────┐   │
│  │  Compositor (owns mirror D3D11 device)      │   │
│  │  • Opens staging textures via NT handles    │   │
│  │  • Renders projection + quads via PS        │   │
│  │  • Triple-buffered output + KeyedMutex      │   │
│  └─────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
    │                          │
    │ Named file mapping       │ DXGI shared NT handles
    │ (OBSMirrorControlBlock)  │ (3 output textures)
    ▼                          ▼
┌────────────────────────────────────┐
│  OBS Plugin (obs-openxr-mirror)    │
│  • Opens file mapping              │
│  • Opens shared texture handles    │
│  • Acquires KeyedMutex to read     │
│  • Feeds frame to OBS source       │
└────────────────────────────────────┘
```

---

## Component 1: OpenXR Layer DLL

### Manual Dispatch (replacing dispatch generator)

`layer_entry.cpp` exports `xrNegotiateLoaderApiLayerInterface`. It stores the loader's `xrGetInstanceProcAddr` and builds a hand-written dispatch table for the six intercepted functions. All other OpenXR calls pass through via the stored proc addr. No code generation is required.

Additionally, `xrLocateSpace` must be resolved and cached (but not intercepted) — the Compositor calls it when computing world matrices for quad layers.

### Intercepted Functions

| Function | Responsibility |
|---|---|
| `xrCreateInstance` | Store instance handle, resolve proc addrs (including `xrLocateSpace`), log runtime name |
| `xrCreateSession` | Detect D3D11/D3D12 binding from `createInfo->next` chain, borrow device/queue refs, create Compositor |
| `xrDestroySession` | Destroy all tracked swapchains associated with this session (in any order), then tear down the Compositor |
| `xrEnumerateSwapchainImages` | On the second call (when `imageCapacityInput > 0` and `images != nullptr`): allocate staging texture for color swapchains |
| `xrReleaseSwapchainImage` | Copy the just-rendered swapchain image into its staging texture |
| `xrEndFrame` | Read eye mode from control block, walk `frameEndInfo->layers`, drive Compositor to render projection then quads, call `copyToOutput()` |

**Note on `xrDestroySession`:** Applications may legally destroy a session without first destroying individual swapchains. The layer must iterate `_swapchains` and clean up all entries associated with the session before releasing the Compositor. This prevents resource leaks on abnormal application exit.

**Note on `xrEnumerateSwapchainImages`:** OpenXR applications call this function twice: first with `imageCapacityInput=0` to query the count, then again with the actual buffer. The staging texture must only be allocated on the second call when the image array is populated. On the first call (count query), pass through directly.

### Swapchain Tracking

Only color swapchains are tracked (`XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT`, format bpc ≤ 10). Each tracked swapchain holds:

- `XrSwapchainCreateInfo` (width, height, format, arraySize)
- Acquired image index
- **D3D11:** array of `XrSwapchainImageD3D11KHR` from the runtime
- **D3D12:** array of `XrSwapchainImageD3D12KHR` + per-image D3D12 fence + fence event + command allocator + command list
- A staging texture (details differ by API — see below) opened on the Compositor's mirror device

### D3D11 Staging Texture

Created on the **app's D3D11 device** with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX` (or `MISC_SHARED` if KeyedMutex is not needed on the staging path — a simple NT-handle share suffices here since staging→compositor is same-machine cross-device). An NT handle is obtained via `IDXGIResource1::CreateSharedHandle`. The Compositor opens it via `ID3D11Device1::OpenSharedResource1`.

On `xrReleaseSwapchainImage`: call `ID3D11DeviceContext::CopySubresourceRegion` on the app device from the swapchain image into the staging texture.

### D3D12 Staging Texture

**The D3D12 device is the creator; the D3D11 mirror device is the consumer.** This is the only valid direction for cross-API NT-handle sharing between D3D12 and D3D11.

Creation (on `xrEnumerateSwapchainImages`, second call):
1. Create a `D3D12_RESOURCE` on the app's D3D12 device with `D3D12_HEAP_FLAG_SHARED` and `D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET`
2. Call `ID3D12Device::CreateSharedHandle` to obtain an NT handle
3. Open it on the Compositor's D3D11 mirror device via `ID3D11Device1::OpenSharedResource1` — store as `ID3D11Texture2D`

On `xrReleaseSwapchainImage`:
1. Reset and record a D3D12 command list that calls `CopyResource` from the swapchain image into the D3D12 staging resource
2. Close and submit the command list to the app's command queue
3. Signal a per-image `ID3D12Fence` on the app's command queue
4. Store the fence + completion value — the Compositor will wait on this before sampling

**D3D12 fence wait in Compositor (CPU-side):**
Before compositing in `xrEndFrame`, the Compositor iterates all tracked D3D12 swapchains and for each calls `ID3D12Fence::SetEventOnCompletion` + `WaitForSingleObject` on the per-image fence event. This CPU-side wait blocks the compositor thread until the D3D12 copy is complete. The wait occurs in `xrEndFrame` before any `Blend()` calls, not inside `xrReleaseSwapchainImage`. Maximum wait timeout: 1000ms.

---

## Component 2: Compositor

### Device Setup

The Compositor owns a dedicated D3D11 device (`D3D_DRIVER_TYPE_HARDWARE`, `BGRA_SUPPORT`). Created once at `xrCreateSession`, torn down at `xrDestroySession`.

### Internal Resources

- `_compositorTexture` (`ID3D11Texture2D`) — render target for the current frame, `DXGI_FORMAT_B8G8R8A8_UNORM`, sized to output dimensions
- `_compositorRTV` (`ID3D11RenderTargetView`) — render target view for `_compositorTexture`, bound via `OMSetRenderTargets` before each frame's rendering
- `_outputTextures[3]` — triple-buffered output textures, `MISC_SHARED_NTHANDLE | MISC_SHARED_KEYEDMUTEX`
- `_writeIndex` — current write slot (0, 1, or 2)
- Vertex buffer, index buffer, constant buffers, sampler, blend state, shaders (compiled once at startup via `D3DCompile`)

### Shader Compilation

Shaders are compiled at startup via `D3DCompile` (requires `d3dcompiler_47.dll` or later on the system). If compilation fails, log a detailed error (including the `ID3DBlob` error message) and disable compositing. The layer must not crash if `d3dcompiler_47.dll` is absent — test for the DLL before attempting compilation and log a clear message if missing.

### Rendering Pipeline (per `xrEndFrame`)

1. Bind `_compositorRTV` via `OMSetRenderTargets`
2. Clear `_compositorTexture` to transparent black `{0, 0, 0, 0}`
3. For the `XrCompositionLayerProjection`: call `BlendProjection()` using `ps_projection`
4. For each `XrCompositionLayerQuad` in submission order: call `BlendQuad()` using `ps_quad` with alpha blending

### Pixel Shaders

**`vs_quad.hlsl`** — shared vertex shader. Transforms a unit quad by `world` and `viewproj` matrices. Passes through UV coordinates.

**`ps_projection.hlsl`** — samples `Texture2D` or `Texture2DArray` (array index selects eye). For FOV-match cases: full-screen blit. For FOV mismatch: orthographic scale to fit HMD FOV. Outputs opaque color. Format conversion is implicit via the SRV descriptor — no C++ branching on DXGI format.

**`ps_quad.hlsl`** — samples `Texture2D`, applies perspective projection from eye pose + FOV, world matrix from quad pose + space location (resolved via the cached `xrLocateSpace` proc addr). Alpha-blends on top of existing compositor content. For stereo blend mode, a smoothstep transition is applied at the configurable blend position.

### Triple Buffering + IDXGIKeyedMutex

Three output textures created with `MISC_SHARED_NTHANDLE | MISC_SHARED_KEYEDMUTEX`.

**Initial state:** After `CreateTexture2D`, `IDXGIKeyedMutex` key value is 0. This means only the writer can acquire a slot initially (using key=0). The reader (OBS) cannot acquire until the writer has released with key=1. This is the intended behavior — OBS will use the non-blocking `timeout=0` path and simply not render until the first frame arrives.

**Layer (writer) on `copyToOutput()`:**
1. Try `AcquireSync(_writeIndex, key=0, timeout=0)` — non-blocking
2. If unavailable, try `(_writeIndex + 1) % 3`, then `(_writeIndex + 2) % 3`
3. If all three fail, drop the frame — do not advance `_writeIndex`, do not update `lastWrittenSlot`
4. On success: `CopyResource(_outputTextures[acquiredSlot], _compositorTexture)`
5. `ReleaseSync(acquiredSlot, key=1)` — marks slot ready for OBS
6. Write `lastWrittenSlot = acquiredSlot` to control block (see memory ordering note below)
7. Set `_writeIndex = (acquiredSlot + 1) % 3`

**OBS plugin (reader) per frame:**
1. Read `lastWrittenSlot` from control block
2. `AcquireSync(lastWrittenSlot, key=1, timeout=0)` — non-blocking; if unavailable, render previous frame
3. `CopySubresourceRegion` into `texCrop`
4. `ReleaseSync(lastWrittenSlot, key=0)`

### Output Texture Sizing

- **Single eye:** `outputWidth = recommendedImageRectWidth`, `outputHeight = recommendedImageRectHeight`
- **Stereo blend:** `outputWidth = recommendedImageRectWidth * (1.0f + stereoOverlap)`, rounded up to the next even pixel; `outputHeight = recommendedImageRectHeight`

`stereoOverlap` is read from `OBSMirrorControlBlock::stereoOverlap` (a fraction 0.0–1.0 corresponding to the OBS slider 0–100%).

Output format is always `DXGI_FORMAT_B8G8R8A8_UNORM` (OBS preferred format).

---

## Component 3: IPC Protocol

### Shared Header

`obs_mirror_ipc.h` lives in a top-level `shared/` directory and is the single source of truth — do not edit the copy in `OBSPlugin/`. The layer DLL references it via `target_include_directories(... shared/)`. The `OBSPlugin/` copy is generated by `Setup-OBSPlugin.ps1` at setup time and exists only because the OBS plugin is built inside the OBS source tree where it cannot reach the repo's `shared/` directory via a relative path.

### OBSMirrorControlBlock

```cpp
struct OBSMirrorControlBlock {
    // Written by layer, read by OBS
    uint64_t outputHandles[3];          // NT HANDLE values cast to uint64_t
    uint32_t outputWidth;               // compositor output width in pixels
    uint32_t outputHeight;              // compositor output height in pixels
    std::atomic<uint32_t> lastWrittenSlot;  // index (0-2) of most recently completed frame
    uint32_t frameCounter;              // reserved for future use; not currently read

    // Written by OBS, read by layer
    uint32_t eyeMode;                   // 0=left, 1=right, 2=stereo blend
    float    stereoOverlap;             // fraction of eye width for stereo output (0.0-1.0)
    float    blendWidth;                // stereo blend transition width (0.0-1.0)
    float    blendPosition;             // stereo blend centre position (0.0-1.0)
    uint32_t obsFrameCounter;           // incremented each render by OBS
};
```

**Memory ordering note:** `lastWrittenSlot` is `std::atomic<uint32_t>` to guarantee tear-free reads across processes. The layer writes it with `store(slot, std::memory_order_release)` after `ReleaseSync`. OBS reads it with `load(std::memory_order_acquire)` before `AcquireSync`. All other fields are written by one side only and are not accessed simultaneously, so plain `uint32_t` / `float` suffices for them.

**Platform note:** Placing `std::atomic<uint32_t>` inside a struct mapped via `MapViewOfFile` relies on the MSVC/Windows-specific guarantee that `std::atomic<uint32_t>` is layout-identical to `uint32_t` in mapped shared memory. The C++ standard does not formally guarantee this across processes. This struct must only be compiled with MSVC (or a compatible ABI) targeting x86/x64 Windows. Do not port this struct to other compilers without replacing `std::atomic` with `volatile uint32_t` and `InterlockedExchange`/`InterlockedCompareExchange` intrinsics.

Named mapping: `L"OpenXROBSMirrorSurface"` (unchanged for compatibility).

### Lifecycle

- Layer creates mapping at `xrCreateSession`, destroys at `xrDestroySession`
- OBS opens mapping on init, writes its configuration fields immediately
- Layer reads OBS-written fields (`eyeMode`, `stereoOverlap`, `blendWidth`, `blendPosition`) at the start of each `xrEndFrame`
- No mutex needed on other control block fields — they are each owned by one side; stale reads are benign

### OBS Detection

If `obsFrameCounter` has not changed for >10 consecutive frames, the layer skips compositing to avoid wasting GPU time. Threshold defined as `constexpr uint32_t kOBSTimeoutFrames = 10` in the layer source.

---

## Component 4: OBS Plugin

### Build

The `shared/obs_mirror_ipc.h` header is copied into `OBSPlugin/` as part of the `Setup-OBSPlugin.ps1` script (which copies the entire `OBSPlugin/` directory plus the shared header). The plugin source lives under `OBSPlugin/` in this repo and is built by copying into `obs-studio/plugins/obs-openxr-mirror/` and adding `add_subdirectory(obs-openxr-mirror)` to OBS's root `CMakeLists.txt`. The helper script `scripts/Setup-OBSPlugin.ps1` automates this copy to a configurable OBS source path.

### Initialization

On source show/activate:
1. Open named file mapping via `OpenFileMappingW` — retry on next tick if absent (VR app not running)
2. Read `outputHandles[0..2]`, `outputWidth`, `outputHeight` from control block
3. Create D3D11 device, open all three output textures via `ID3D11Device1::OpenSharedResource1` (NT handle path — must match the `MISC_SHARED_NTHANDLE` creation in the layer)
4. Create `texCrop` (non-shared, `DXGI_FORMAT_B8G8R8A8_UNORM`, linear) for the cropped region
5. Register `texCrop` with OBS via `gs_texture_open_shared` using its legacy shared handle (since OBS's `gs_texture_open_shared` takes a 32-bit handle, `texCrop` is created with `MISC_SHARED` not `MISC_SHARED_NTHANDLE`)
6. Write `eyeMode`, `stereoOverlap`, `blendWidth`, `blendPosition` into control block

Re-initialization is triggered if `outputHandles[0]` changes value (layer restarted or resolution changed).

### Per-Frame Render

The plugin maintains a `_lastAcquiredSlot` state variable (initialized to `UINT32_MAX` as a sentinel meaning "no frame yet drawn"). On each render:

1. Increment `obsFrameCounter`
2. Read `lastWrittenSlot` via `load(std::memory_order_acquire)`
3. `AcquireSync(lastWrittenSlot, key=1, timeout=0)` — non-blocking
4. If `AcquireSync` succeeded: `CopySubresourceRegion` from `mirror_textures[lastWrittenSlot]` into `texCrop`; `ReleaseSync(lastWrittenSlot, key=0)`; set `_lastAcquiredSlot = lastWrittenSlot`
5. If `AcquireSync` failed and `_lastAcquiredSlot == UINT32_MAX`: return early (no frame available yet)
6. `Flush()`
7. Draw via `obs_source_draw` (draws whichever `texCrop` was last successfully populated)

### OBS Source Properties

| Control | Type | Description |
|---|---|---|
| Eye Capture | List | Left / Right / Both |
| Stereo Overlap | Float slider | 0–100% (maps to `stereoOverlap` 0.0–1.0) |
| Stereo Blend Width | Float slider | 0–100% |
| Stereo Blend Position | Float slider | 0–100% |
| Crop Top / Bottom / Left / Right | Float sliders | 0–100% |
| Preset | List | Loaded from `win_openxrmirror-presets.ini` |
| Reinitialize | Button | Force deinit + init |

---

## Component 5: Build System

### Repository Layout

```
OpenXR-Mirror-v2/
├── CMakeLists.txt                        # root: builds layer DLL
├── shared/
│   └── obs_mirror_ipc.h                  # single source of truth for IPC struct
├── XR_APILAYER_NOVENDOR_OBSMirror/
│   ├── CMakeLists.txt
│   ├── layer_entry.cpp                   # xrNegotiateLoaderApiLayerInterface, dispatch table
│   ├── layer.cpp / layer.h               # OpenXR intercept logic
│   ├── compositor.cpp / compositor.h     # D3D11 mirror device, rendering, triple buffer
│   ├── swapchain.cpp / swapchain.h       # per-swapchain staging texture management
│   └── shaders/
│       ├── vs_quad.hlsl
│       ├── ps_projection.hlsl
│       └── ps_quad.hlsl
├── OBSPlugin/
│   ├── CMakeLists.txt                    # for inclusion in obs-studio/plugins/
│   ├── obs-openxr-mirror.cpp
│   └── obs_mirror_ipc.h                  # copied here by Setup-OBSPlugin.ps1
├── external/
│   └── OpenXR-SDK/                       # submodule (headers only)
├── scripts/
│   ├── Install-Layer.ps1
│   ├── Uninstall-Layer.ps1
│   └── Setup-OBSPlugin.ps1               # copies OBSPlugin/ + shared header into OBS source tree
└── docs/
    └── superpowers/specs/
```

### Dependencies

- OpenXR-SDK submodule (headers only: `openxr/openxr.h`, `openxr/openxr_platform.h`)
- Windows SDK (D3D11, D3D12, DXGI — found via CMake's `find_package`)
- No NuGet: `fmt` replaced with `std::format` (C++20), WIL replaced with WRL `ComPtr`
- Shaders compiled at runtime via `D3DCompile` (requires `d3dcompiler_47.dll` on the system)

---

## Key Improvements Over v1

| Issue in v1 | Fix in v2 |
|---|---|
| Triple buffer allocated but unused (always index 0) | Rotating `_writeIndex` with non-blocking `AcquireSync`; frame drop if all slots busy |
| No cross-process synchronization | `IDXGIKeyedMutex` on all three output textures; `std::atomic` for `lastWrittenSlot` |
| D3D12 missing cross-device fence | Per-image D3D12 fence; CPU-side `WaitForSingleObject` in `xrEndFrame` before compositing |
| Format-specific copy paths in C++ | All conversion via SRV format descriptor in pixel shaders |
| Duplicated struct with mismatched types | Single `shared/obs_mirror_ipc.h` canonical header |
| Complex dispatch generator for 6 functions | Hand-written dispatch table in `layer_entry.cpp` |
| NuGet dependencies (fmt, WIL) | `std::format` + raw WRL, CMake-only build |
| Session destroy without swapchain cleanup | `xrDestroySession` explicitly iterates and cleans all associated swapchains |
