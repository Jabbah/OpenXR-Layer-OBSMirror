// MIT License
//
// Copyright(c) 2022 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"

#include "layer.h"
#include "log.h"
#include "util.h"
#include "dx11mirror.h"

#include <directxmath.h> // Matrix math functions and objects
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <winrt/base.h>
#include <d3d11_1.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")

namespace {
#define CHECK_DX(expression)                                                                                           \
    do {                                                                                                               \
        HRESULT res = (expression);                                                                                    \
        if (FAILED(res)) {                                                                                             \
            Log("DX Call failed with: 0x%08x\n", res);                                                                 \
            Log("CHECK_DX failed on: " #expression, " DirectX error - see log for details\n");                         \
        }                                                                                                              \
    } while (0);

    using namespace layer_OBSMirror;
    using namespace layer_OBSMirror::log;
    using namespace DirectX; // Matrix math
    using namespace Mirror;

    void WaitForFence(ID3D12Fence* fence, UINT64 completionValue, HANDLE waitEvent) {
        if (fence->GetCompletedValue() < completionValue) {
            fence->SetEventOnCompletion(completionValue, waitEvent);
            WaitForSingleObject(waitEvent, 1000);
        }
    }

    using namespace xr::math;

    std::vector<const char*> ParseExtensionString(char* names) {
        std::vector<const char*> list;
        while (*names != 0) {
            list.push_back(names);
            while (*(++names) != 0) {
                if (*names == ' ') {
                    *names++ = '\0';
                    break;
                }
            }
        }
        return list;
    }

    class OpenXrLayer : public layer_OBSMirror::OpenXrApi {
      public:
        OpenXrLayer() {
            _mirror = std::make_unique<D3D11Mirror>();
        }

        ~OpenXrLayer() override {
            while (_sessions.size()) {
                cleanupSession(_sessions.begin()->second);
                _sessions.erase(_sessions.begin());
            }
        }

        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledApiLayerNames[i], "ApiLayerName"));
            }
            for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledExtensionNames[i], "ExtensionName"));
            }

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // Dump the application name and OpenXR runtime information to help debugging customer issues.
            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(runtimeName.c_str(), "RuntimeName"));
            Log("Application: %s\n", GetApplicationName().c_str());
            Log("Using OpenXR runtime: %s\n", runtimeName.c_str());

            return XR_SUCCESS;
        }

        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            Log("xrCreateSession\n");
            if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSession",
                              TLPArg(instance, "Instance"),
                              TLArg((int)createInfo->systemId, "SystemId"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            Session newSession;
            bool handled = true;

            // if (isSystemHandled(createInfo->systemId)) {
            const XrBaseInStructure* const* pprev =
                reinterpret_cast<const XrBaseInStructure* const*>(&createInfo->next);
            const XrBaseInStructure* entry = reinterpret_cast<const XrBaseInStructure*>(createInfo->next);
            while (entry) {
                Log("Entry: %d\n", entry->type);
                if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                    _xrGraphicsAPI = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
                    const XrGraphicsBindingD3D11KHR* d3d11Bindings =
                        reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry);
                    _d3d11Device = d3d11Bindings->device;
                    _d3d11Device->GetImmediateContext(_d3d11Context.ReleaseAndGetAddressOf());

                    handled = true;
                    if (!_graphicsRequirementQueried) {
                        // return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
                    }
                } else if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                    _xrGraphicsAPI = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
                    const XrGraphicsBindingD3D12KHR* d3d12Bindings =
                        reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
                    _d3d12Device = d3d12Bindings->device;
                    _d3d12CommandQueue = d3d12Bindings->queue;
                } else {
                    _xrGraphicsAPI = XR_TYPE_UNKNOWN;
                }

                entry = entry->next;
            }


            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (handled) {
                if (XR_SUCCEEDED(result)) {
                    // On success, record the state.
                    newSession._xrSession = *session;
                    _sessions.insert_or_assign(*session, newSession);

                    // List off the views and store them locally for easy access
                    XrSystemId xr_system;
                    XrSystemGetInfo systemInfo{};
                    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
                    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
                    CHECK_XRCMD(xrGetSystem(instance, &systemInfo, &xr_system));

                    uint32_t viewCount = 0;
                    CHECK_XRCMD(xrEnumerateViewConfigurationViews(
                        instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));

                    _xrViewsList = std::vector<XrViewConfigurationView>(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});

                    CHECK_XRCMD(xrEnumerateViewConfigurationViews(instance,
                                                                  xr_system,
                                                                  XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                  viewCount,
                                                                  &viewCount,
                                                                  _xrViewsList.data()));

                    assert(viewCount == _xrViewsList.size());
                } else {
                }
            }

            if (XR_SUCCEEDED(result)) {
                TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLPArg(*session, "Session"));
            }

            return result;
        }

        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            Log("xrCreateSwapchain\n");
            if (createInfo->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSwapchain",
                              TLPArg(session, "Session"),
                              TLArg(createInfo->arraySize, "ArraySize"),
                              TLArg(createInfo->width, "Width"),
                              TLArg(createInfo->height, "Height"),
                              TLArg(createInfo->createFlags, "CreateFlags"),
                              TLArg(createInfo->format, "Format"),
                              TLArg(createInfo->faceCount, "FaceCount"),
                              TLArg(createInfo->mipCount, "MipCount"),
                              TLArg(createInfo->sampleCount, "SampleCount"),
                              TLArg(createInfo->usageFlags, "UsageFlags"));

            XrSwapchainCreateInfo chainCreateInfo = *createInfo;
            Swapchain newSwapchain;
            bool handled = false;

            if (isSessionHandled(session)) {
                auto& sessionState = _sessions[session];

                Log("Creating swapchain with dimensions=%ux%u, arraySize=%u, mipCount=%u, sampleCount=%u, format=%d, "
                    "usage=0x%x\n",
                    createInfo->width,
                    createInfo->height,
                    createInfo->arraySize,
                    createInfo->mipCount,
                    createInfo->sampleCount,
                    createInfo->format,
                    createInfo->usageFlags);

                handled = true;
            }

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, &chainCreateInfo, swapchain);
            if (handled) {
                if (XR_SUCCEEDED(result)) {
                    // On success, record the state.
                    newSwapchain._xrSwapchain = *swapchain;
                    newSwapchain._createInfo = chainCreateInfo;
                    newSwapchain._aquiredIndex = -1;
                    newSwapchain._releasedIndex = -1;
                    newSwapchain._dx11SurfaceImages.clear();
                    newSwapchain._dx12SurfaceImages.clear();
                    // newSwapchain._xrSession = session;
                    auto res = _swapchains.insert_or_assign(*swapchain, newSwapchain);
                    Log("%p %s\n", swapchain, res.second ? "inserted: " : "assigned: ");
                } else {
                    auto& sessionState = _sessions[session];
                }
            }

            TraceLoggingWrite(g_traceProvider, "xrCreateSwapchain", TLPArg(*swapchain, "Swapchain"));

            return result;
        }

        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLPArg(swapchain, "Swapchain"));

            Log("xrDestroySwapchain %d\n", swapchain);
            const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);
            if (XR_SUCCEEDED(result) && isSwapchainHandled(swapchain)) {
                auto& swapchainState = _swapchains[swapchain];
                cleanupSwapchain(swapchainState);
                _swapchains.erase(swapchain);
            }

            return result;
        }

        XrResult xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                            uint32_t imageCapacityInput,
                                            uint32_t* imageCountOutput,
                                            XrSwapchainImageBaseHeader* images) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateSwapchainImages",
                              TLPArg(swapchain, "Swapchain"),
                              TLArg(imageCapacityInput, "ImageCapacityInput"));
            Log("xrEnumerateSwapchainImages swapChain %p imageCapacityInput %d\n", swapchain, imageCapacityInput);
            if (!isSwapchainHandled(swapchain) || imageCapacityInput == 0) {
                const XrResult result =
                    OpenXrApi::xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
                TraceLoggingWrite(
                    g_traceProvider, "xrEnumerateSwapchainImages", TLArg(*imageCountOutput, "ImageCountOutput"));
                Log("Result %d\n", result);
                return result;
            }

            // Enumerate the actual D3D swapchain images.
            auto& swapchainState = _swapchains[swapchain];
            const XrResult result =
                OpenXrApi::xrEnumerateSwapchainImages(swapchain, imageCapacityInput, imageCountOutput, images);
            if (XR_SUCCEEDED(result) && _mirror) {
                Mirror::DxgiFormatInfo formatInfo;
                Mirror::GetFormatInfo((DXGI_FORMAT)swapchainState._createInfo.format, formatInfo);
                if (formatInfo.bpc <= 10 &&
                    swapchainState._createInfo.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) {
#ifdef _DEBUG
                    Log("Mirroring swapchain width %d height %d format %d usage %d sample %d array %d face %d mip %d\n",
                        swapchainState._createInfo.width,
                        swapchainState._createInfo.height,
                        swapchainState._createInfo.format,
                        swapchainState._createInfo.usageFlags,
                        swapchainState._createInfo.sampleCount,
                        swapchainState._createInfo.arraySize,
                        swapchainState._createInfo.faceCount,
                        swapchainState._createInfo.mipCount);
#endif
                    if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                        swapchainState._dx11SurfaceImages.resize(*imageCountOutput);
                        for (uint32_t i = 0; i < *imageCountOutput; ++i) {
                            swapchainState._dx11SurfaceImages[i] =
                                reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[i];
                        }
                        images =
                            reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainState._dx11SurfaceImages.data());
                        if (swapchainState._dx11LastTexture) {
                            D3D11_TEXTURE2D_DESC srcDesc;
                            swapchainState._dx11LastTexture->GetDesc(&srcDesc);
                            if (srcDesc.Width != swapchainState._createInfo.width ||
                                srcDesc.Height != swapchainState._createInfo.height ||
                                srcDesc.Format != (DXGI_FORMAT)swapchainState._createInfo.format) {
                                swapchainState._dx11LastTexture = nullptr;
                            }
                        }
                        if (swapchainState._dx11LastTexture == nullptr) {
                            D3D11_TEXTURE2D_DESC desc;
                            ZeroMemory(&desc, sizeof(desc));
                            desc.Width = swapchainState._createInfo.width;
                            desc.Height = swapchainState._createInfo.height;
                            desc.MipLevels = 1;
                            desc.ArraySize = 1;
                            desc.Format = (DXGI_FORMAT)swapchainState._createInfo.format;
                            desc.SampleDesc.Count = 1;
                            desc.SampleDesc.Quality = 0;
                            desc.Usage = D3D11_USAGE_DEFAULT;
                            desc.CPUAccessFlags = 0;
                            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

                            CHECK_DX(_d3d11Device->CreateTexture2D(
                                &desc, NULL, swapchainState._dx11LastTexture.ReleaseAndGetAddressOf()));

                            _mirror->createSharedMirrorTexture(swapchain, swapchainState._dx11LastTexture, desc.Format);
                        }
                    } else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                        for (auto event : swapchainState._frameFenceEvents) {
                            CloseHandle(event);
                        }

                        swapchainState._frameFenceEvents.clear();
                        swapchainState._frameFences.clear();
                        swapchainState._fenceValues.clear();
                        swapchainState._dx12SurfaceImages.resize(*imageCountOutput);
                        swapchainState._commandAllocators.resize(*imageCountOutput);
                        swapchainState._commandLists.resize(*imageCountOutput);
                        swapchainState._frameFenceEvents.resize(*imageCountOutput);
                        swapchainState._frameFences.resize(*imageCountOutput);
                        swapchainState._fenceValues.resize(*imageCountOutput);

                        for (uint32_t i = 0; i < *imageCountOutput; ++i) {
                            swapchainState._dx12SurfaceImages[i] =
                                reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[i];

                            swapchainState._frameFenceEvents[i] = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                            swapchainState._fenceValues[i] = 0;
                            _d3d12Device->CreateFence(
                                0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&swapchainState._frameFences[i]));

                            _d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                 IID_PPV_ARGS(&swapchainState._commandAllocators[i]));
                            _d3d12Device->CreateCommandList(0,
                                                            D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            swapchainState._commandAllocators[i].Get(),
                                                            nullptr,
                                                            IID_PPV_ARGS(&swapchainState._commandLists[i]));
                            swapchainState._commandLists[i]->Close();
                        }
                        images = reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainState._dx12SurfaceImages.data());
                        if (swapchainState._dx12LastTexture) {
                            D3D12_RESOURCE_DESC srcDesc = swapchainState._dx12LastTexture->GetDesc();
                            if (srcDesc.Width != swapchainState._createInfo.width ||
                                srcDesc.Height != swapchainState._createInfo.height ||
                                srcDesc.Format != (DXGI_FORMAT)swapchainState._createInfo.format) {
                                swapchainState._dx12LastTexture = nullptr;
                            }
                        }
                        if (swapchainState._dx12LastTexture == nullptr) {
                            D3D12_RESOURCE_DESC d3d12TextureDesc{};
                            d3d12TextureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                            d3d12TextureDesc.Alignment = 0;
                            d3d12TextureDesc.Width = swapchainState._createInfo.width;
                            d3d12TextureDesc.Height = swapchainState._createInfo.height;
                            d3d12TextureDesc.DepthOrArraySize = 1;
                            d3d12TextureDesc.MipLevels = 1;
                            d3d12TextureDesc.Format = (DXGI_FORMAT)swapchainState._createInfo.format;
                            d3d12TextureDesc.SampleDesc.Count = 1;
                            d3d12TextureDesc.SampleDesc.Quality = 0;
                            d3d12TextureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                            d3d12TextureDesc.Flags =
                                D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

                            D3D12_HEAP_PROPERTIES heapProperties;
                            heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
                            heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                            heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
                            heapProperties.CreationNodeMask = 0;
                            heapProperties.VisibleNodeMask = 0;

                            D3D12_CLEAR_VALUE clearValue{};
                            clearValue.Format = d3d12TextureDesc.Format;

                            CHECK_DX(
                                _d3d12Device->CreateCommittedResource(&heapProperties,
                                                                      D3D12_HEAP_FLAG_SHARED,
                                                                      &d3d12TextureDesc,
                                                                      D3D12_RESOURCE_STATE_COMMON,
                                                                      &clearValue,
                                                                      IID_PPV_ARGS(&swapchainState._dx12LastTexture)));

                            if (swapchainState._sharedHandle) {
                                CloseHandle(swapchainState._sharedHandle);
                                swapchainState._sharedHandle = NULL;
                            }

                            CHECK_DX(_d3d12Device->CreateSharedHandle(swapchainState._dx12LastTexture.Get(),
                                                                      nullptr,
                                                                      GENERIC_ALL,
                                                                      nullptr,
                                                                      &swapchainState._sharedHandle));

                            _mirror->createSharedMirrorTexture(swapchain, swapchainState._sharedHandle);
                            
                        }
                    }
                } 
#ifdef _DEBUG
                else {
                    Log("Not mirroring swapchain width %d height %d format %d usage %d sample %d array %d face %d mip "
                        "%d",
                        swapchainState._createInfo.width,
                        swapchainState._createInfo.height,
                        swapchainState._createInfo.format,
                        swapchainState._createInfo.usageFlags,
                        swapchainState._createInfo.sampleCount,
                        swapchainState._createInfo.arraySize,
                        swapchainState._createInfo.faceCount,
                        swapchainState._createInfo.mipCount);
                }
#endif
            } else {
                if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR)
                    swapchainState._dx11SurfaceImages.clear();
                else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR)
                    swapchainState._dx12SurfaceImages.clear();
            }

            return result;
        }

        XrResult xrAcquireSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageAcquireInfo* acquireInfo,
                                         uint32_t* index) override {
            if (acquireInfo && acquireInfo->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            const XrResult result = OpenXrApi::xrAcquireSwapchainImage(swapchain, acquireInfo, index);

            if (XR_SUCCEEDED(result) && isSwapchainHandled(swapchain)) {
                _swapchains[swapchain]._aquiredIndex = *index;
            }

            return result;
        }

        XrResult updateSwapChainImages(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* releaseInfo, bool doXRcall) {
            if (_mirror && _mirror->enabled() && isSwapchainHandled(swapchain)) {
                auto& swapchainState = _swapchains[swapchain];
                uint32_t idx = swapchainState._aquiredIndex;
                if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR &&
                    !swapchainState._dx11SurfaceImages.empty()) {
                    auto* textPtr = swapchainState._dx11SurfaceImages[idx].texture;
                    if (swapchainState._dx11LastTexture) {
                        _d3d11Context->CopyResource(swapchainState._dx11LastTexture.Get(), textPtr);
                        swapchainState._releasedIndex = swapchainState._aquiredIndex;
                    }
                } else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR &&
                           !swapchainState._dx12SurfaceImages.empty()) {
                    auto* textPtr = swapchainState._dx12SurfaceImages[idx].texture;
                    if (swapchainState._dx12LastTexture) {
                        WaitForFence(swapchainState._frameFences[idx].Get(),
                                     swapchainState._fenceValues[idx],
                                     swapchainState._frameFenceEvents[idx]);
                        swapchainState._commandAllocators[idx]->Reset();
                        swapchainState._commandLists[idx]->Reset(swapchainState._commandAllocators[idx].Get(), nullptr);
                        swapchainState._commandLists[idx]->CopyResource(swapchainState._dx12LastTexture.Get(), textPtr);
                        swapchainState._commandLists[idx]->Close();
                        ID3D12CommandList* set[] = {swapchainState._commandLists[idx].Get()};
                        _d3d12CommandQueue->ExecuteCommandLists(1, set);
                        swapchainState._releasedIndex = swapchainState._aquiredIndex;
                    }
                }
            }
            XrResult result;
            if (doXRcall)
                result = OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);

            if (_mirror && _mirror->enabled() && isSwapchainHandled(swapchain) &&
                _xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                auto& swapchainState = _swapchains[swapchain];
                uint32_t idx = swapchainState._aquiredIndex;
                if (!swapchainState._dx12SurfaceImages.empty()) {
                    const auto fenceValue = _currentFenceValue;
                    _d3d12CommandQueue->Signal(swapchainState._frameFences[idx].Get(), fenceValue);
                    swapchainState._fenceValues[idx] = fenceValue;
                    ++_currentFenceValue;
                }
            }
            return result;
        }

        XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageReleaseInfo* releaseInfo) override {
            return updateSwapChainImages(swapchain, releaseInfo, true);
        }

        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            XrResult res =
                OpenXrApi::xrLocateViews(session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

            if (_mirror && _mirror->enabled() && XR_SUCCEEDED(res)) {
                auto siPtr = _mirror->getSpaceInfo(viewLocateInfo->space);
                if (siPtr && siPtr->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                    if (_projectionViews.size() != *viewCountOutput)
                        _projectionViews.resize(*viewCountOutput, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
                    for (uint32_t nView = 0; nView < *viewCountOutput; nView++) {
                        _projectionViews[nView].fov = views[nView].fov;

                        XrPosef pose = views[nView].pose;

                        // Make sure we at least have halfway-sane values if the runtime isn't providing them. In
                        // particular if the runtime gives us an invalid orientation, that'd otherwise cause
                        // XR_ERROR_POSE_INVALID errors later.
                        if ((viewState->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
                            pose.orientation = XrQuaternionf{0, 0, 0, 1};
                        }
                        if ((viewState->viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
                            pose.position = XrVector3f{0, 1.5, 0};
                        }

                        _projectionViews[nView].pose = pose;
                    }
                }
            }

            return res;
        }

        XrResult xrCreateReferenceSpace(XrSession session,
                                        const XrReferenceSpaceCreateInfo* createInfo,
                                        XrSpace* space) {
            XrResult res = OpenXrApi::xrCreateReferenceSpace(session, createInfo, space);
            if (_mirror && XR_SUCCEEDED(res)) {
                _mirror->addSpace(*space, createInfo);
            }
            return res;
        }

        XrResult xrDestroySpace(XrSpace space) {
            XrResult res = OpenXrApi::xrDestroySpace(space);
            if (_mirror && XR_SUCCEEDED(res)) {
                _mirror->removeSpace(space);
            }
            return res;
        }

        XrResult xrBeginFrame(XrSession session, const XrFrameBeginInfo* frameBeginInfo) override {
            if (_mirror)
                _mirror->flush();
            return OpenXrApi::xrBeginFrame(session, frameBeginInfo);
        }

        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            if (_mirror) {
                _mirror->checkOBSRunning();

                if (_mirror->enabled() && isSessionHandled(session) && !_projectionViews.empty() &&
                    !_xrViewsList.empty()) {

                    auto& sessionState = _sessions[session];
                    const XrCompositionLayerProjectionView* projView = &_projectionViews[0];
                    const XrCompositionLayerProjection* projLayer = nullptr;

                    _projectionViews[0].subImage.imageRect.offset.x = 0;
                    _projectionViews[0].subImage.imageRect.offset.y = 0;
                    _projectionViews[0].subImage.imageRect.extent.width = _xrViewsList[0].recommendedImageRectWidth;
                    _projectionViews[0].subImage.imageRect.extent.height = _xrViewsList[0].recommendedImageRectHeight;

                    uint32_t count = frameEndInfo->layerCount;
                    for (uint32_t i = 0; i < count; ++i) {
                        const XrCompositionLayerBaseHeader* hdr = frameEndInfo->layers[i];
                        if (hdr->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                            projLayer = reinterpret_cast<const XrCompositionLayerProjection*>(hdr);
                            if (projLayer->viewCount == 2) {
                                projView = &projLayer->views[_mirror->getEyeIndex()];
                                if (isSwapchainHandled(projView->subImage.swapchain)) {
                                    auto& swapchainState = _swapchains[projView->subImage.swapchain];
                                    if (swapchainState._dx11LastTexture || swapchainState._dx12LastTexture) {
                                        _mirror->copyPerspectiveTex(projView->subImage.imageRect,
                                                                    (DXGI_FORMAT)swapchainState._createInfo.format,
                                                                    projView->subImage.swapchain);
                                    }
                                }
                            }
                        } else if (hdr->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                            const XrCompositionLayerQuad* quadLayer =
                                reinterpret_cast<const XrCompositionLayerQuad*>(hdr);
                            if (isSwapchainHandled(quadLayer->subImage.swapchain)) {
                                auto& swapchainState = _swapchains[quadLayer->subImage.swapchain];
                                if (swapchainState._aquiredIndex != swapchainState._releasedIndex) {
                                    // Probably missed an update to swap chain whilst waiting for OBS plugin
                                    // Swapchains don't need to be updated every frame so just copy the last one aquired
                                    updateSwapChainImages(quadLayer->subImage.swapchain, nullptr, false);
                                }
                                if (swapchainState._dx11LastTexture || swapchainState._dx12LastTexture) {
                                    if (projView) {
                                        _mirror->Blend(projView,
                                                       quadLayer,
                                                       (DXGI_FORMAT)swapchainState._createInfo.format,
                                                       projLayer ? projLayer->space : nullptr,
                                                       frameEndInfo->displayTime);
                                    }
                                }
                            }
                        }
                    }
                    _mirror->copyToMirror();
                }
            }

            return OpenXrApi::xrEndFrame(session, frameEndInfo);
        }

      private:
        // State associated with an OpenXR session.
        struct Session {
            XrSession _xrSession{XR_NULL_HANDLE};
        };

        struct Swapchain {
            ~Swapchain() {
                for (auto event : _frameFenceEvents) {
                    CloseHandle(event);
                }
                if (_sharedHandle)
                    CloseHandle(_sharedHandle);
            }
            XrSwapchain _xrSwapchain{XR_NULL_HANDLE};
            XrSwapchainCreateInfo _createInfo;
            std::vector<XrSwapchainImageD3D11KHR> _dx11SurfaceImages;
            std::vector<XrSwapchainImageD3D12KHR> _dx12SurfaceImages;
            uint32_t _aquiredIndex = -1;
            uint32_t _releasedIndex = -1;
            ComPtr<ID3D11Texture2D> _dx11LastTexture = nullptr;
            ComPtr<ID3D12Resource> _dx12LastTexture = nullptr;
            std::vector<ComPtr<ID3D12GraphicsCommandList>> _commandLists;
            std::vector<ComPtr<ID3D12CommandAllocator>> _commandAllocators;
            std::vector<HANDLE> _frameFenceEvents;
            std::vector<ComPtr<ID3D12Fence>> _frameFences;
            std::vector<UINT64> _fenceValues;
            HANDLE _sharedHandle = NULL;
        };

        void cleanupSession(Session& sessionState) {
        }

        void cleanupSwapchain(Swapchain& swapchainState) {
        }

        bool isSystemHandled(XrSystemId systemId) const {
            return systemId == _systemId;
        }

        bool isSessionHandled(XrSession session) const {
            return _sessions.find(session) != _sessions.cend();
        }

        bool isSwapchainHandled(XrSwapchain swapchain) const {
            return _swapchains.find(swapchain) != _swapchains.cend();
        }

        std::unique_ptr<D3D11Mirror> _mirror;

        UINT64 _currentFenceValue;

        XrStructureType _xrGraphicsAPI = XR_TYPE_UNKNOWN;

        ID3D11Device* _d3d11Device = nullptr;
        ComPtr<ID3D11DeviceContext> _d3d11Context = nullptr;

        ID3D12Device* _d3d12Device = nullptr;
        ID3D12CommandQueue* _d3d12CommandQueue = nullptr;
        
        XrSystemId _systemId{XR_NULL_SYSTEM_ID};
        bool _graphicsRequirementQueried{false};

        std::vector<XrViewConfigurationView> _xrViewsList{};
        std::vector<XrCompositionLayerProjectionView> _projectionViews{};

        std::map<XrSession, Session> _sessions;
        std::map<XrSwapchain, Swapchain> _swapchains;

    };

    std::unique_ptr<OpenXrLayer> g_instance = nullptr;

} // namespace

namespace layer_OBSMirror {
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

    void ResetInstance() {
        g_instance.reset();
    }

} // namespace layer_OBSMirror

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(layer_OBSMirror::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
