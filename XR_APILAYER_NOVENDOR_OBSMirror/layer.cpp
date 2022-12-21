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

#include <directxmath.h> // Matrix math functions and objects
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <winrt/base.h>

#include <d3d11_1.h>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")

#define CHECK_DX(expression)                                                                                           \
    do {                                                                                                               \
        HRESULT res = (expression);                                                                                    \
        if (FAILED(res)) {                                                                                             \
            Log("DX Call failed with: 0x%08x\n", res);                                                                 \
            Log("CHECK_DX failed on: " #expression, " DirectX error - see log for details\n");                         \
        }                                                                                                              \
    } while (0);

namespace xr {

    //static inline std::string ToString(XrVersion version) {
    //    return ""; // fmt::format("{}.{}.{}", XR_VERSION_MAJOR(version), XR_VERSION_MINOR(version),
    //               // XR_VERSION_PATCH(version));
    //}

} // namespace xr

namespace {
    using namespace layer_OBSMirror;
    using namespace layer_OBSMirror::log;
    using namespace DirectX; // Matrix math

    void WaitForFence(ID3D12Fence* fence, UINT64 completionValue, HANDLE waitEvent) {
        if (fence->GetCompletedValue() < completionValue) {
            fence->SetEventOnCompletion(completionValue, waitEvent);
            WaitForSingleObject(waitEvent, 1000);
        }
    }

    struct DxgiFormatInfo {
        /// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
        /// Both the SRGB and linear formats should be UNORM.
        DXGI_FORMAT srgb, linear, typeless;

        /// THe bits per pixel, bits per channel, and the number of channels
        int bpp, bpc, channels;
    };

    bool GetFormatInfo(DXGI_FORMAT format, DxgiFormatInfo& out) {
#define DEF_FMT_BASE(typeless, linear, srgb, bpp, bpc, channels)                                                       \
    {                                                                                                                  \
        out = DxgiFormatInfo{srgb, linear, typeless, bpp, bpc, channels};                                              \
        return true;                                                                                                   \
    }

#define DEF_FMT_NOSRGB(name, bpp, bpc, channels)                                                                       \
    case name##_TYPELESS:                                                                                              \
    case name##_UNORM:                                                                                                 \
        DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

#define DEF_FMT(name, bpp, bpc, channels)                                                                              \
    case name##_TYPELESS:                                                                                              \
    case name##_UNORM:                                                                                                 \
    case name##_UNORM_SRGB:                                                                                            \
        DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, name##_UNORM_SRGB, bpp, bpc, channels)

#define DEF_FMT_UNORM(linear, bpp, bpc, channels)                                                                      \
    case linear:                                                                                                       \
        DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, linear, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

        // Note that this *should* have pretty much all the types we'll ever see in games
        // Filtering out the non-typeless and non-unorm/srgb types, this is all we're left with
        // (note that types that are only typeless and don't have unorm/srgb variants are dropped too)
        switch (format) {
            // The relatively traditional 8bpp 32-bit types
            DEF_FMT(DXGI_FORMAT_R8G8B8A8, 32, 8, 4)
            DEF_FMT(DXGI_FORMAT_B8G8R8A8, 32, 8, 4)
            DEF_FMT(DXGI_FORMAT_B8G8R8X8, 32, 8, 3)

            // Some larger linear-only types
            DEF_FMT_NOSRGB(DXGI_FORMAT_R16G16B16A16, 64, 16, 4)
            DEF_FMT_NOSRGB(DXGI_FORMAT_R10G10B10A2, 32, 10, 4)

            // A jumble of other weird types
            DEF_FMT_UNORM(DXGI_FORMAT_B5G6R5_UNORM, 16, 5, 3)
            DEF_FMT_UNORM(DXGI_FORMAT_B5G5R5A1_UNORM, 16, 5, 4)
            DEF_FMT_UNORM(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, 32, 10, 4)
            DEF_FMT_UNORM(DXGI_FORMAT_B4G4R4A4_UNORM, 16, 4, 4)
            DEF_FMT(DXGI_FORMAT_BC1, 64, 16, 4)

        default:
            // Unknown type
            return false;
        }

#undef DEF_FMT
#undef DEF_FMT_NOSRGB
#undef DEF_FMT_BASE
#undef DEF_FMT_UNORM
    }

    XMMATRIX d3dXrProjection(XrFovf fov, float clip_near, float clip_far) {
        const float left = clip_near * tanf(fov.angleLeft);
        const float right = clip_near * tanf(fov.angleRight);
        const float down = clip_near * tanf(fov.angleDown);
        const float up = clip_near * tanf(fov.angleUp);

        // Log("fov: L %f R %f U %f D %f N %f F %f\n",
        //     fov.angleLeft,
        //     fov.angleRight,
        //     fov.angleUp,
        //     fov.angleDown,
        //     clip_near,
        //     clip_far);

        return XMMatrixPerspectiveOffCenterRH(left, right, down, up, clip_near, clip_far);
    }

    struct quad_transform_buffer_t {
        XMFLOAT4X4 world;
        XMFLOAT4X4 viewproj;
    };

    constexpr char fs_shader_code[] = R"_(
Texture2D shaderTexture : register(t0);

SamplerState SampleType : register(s0);

struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

psIn vs_fs(uint vI : SV_VERTEXID)
{
	psIn output;
    output.tex = float2(vI&1,vI>>1);
    output.pos = float4((output.tex.x-0.5f)*2,-(output.tex.y-0.5f)*2,0,1);
	output.tex.y = 1.0f - output.tex.y;
	return output;
}

float4 ps_fs(psIn inputPS) : SV_TARGET
{
	float4 textureColor = shaderTexture.Sample(SampleType, inputPS.tex);
	return textureColor;
	//return float4(1,0,0,1);
})_";

    constexpr char quad_shader_code[] = R"_(
cbuffer TransformBuffer : register(b0) {
	float4x4 world;
	float4x4 viewproj;
};

Texture2D shaderTexture : register(t0);

SamplerState SampleType : register(s0);

struct vsIn {
	float4 pos  : POSITION;
	float2 tex  : TEXCOORD0;
};

struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

psIn vs_quad(vsIn input)
{
	psIn output;
	output.pos = mul(mul(input.pos, world), viewproj);
	output.tex = input.tex;
	return output;
}

float4 ps_quad(psIn inputPS) : SV_TARGET
{
	float4 textureColor = shaderTexture.Sample(SampleType, inputPS.tex);
	return textureColor;
})_";

    float quad_verts[] = {
        // coord x,y,z,w  tex x,y,
        -0.5, 0.5, 0, 1, 0, 0, -0.5, -0.5, 0, 1, 0, 1, 0.5, 0.5, 0, 1, 1, 0, 0.5, -0.5, 0, 1, 1, 1};

    uint16_t quad_inds[] = {2, 1, 0, 2, 3, 1};

    ID3DBlob* d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target) {
        DWORD flags =
            D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ID3DBlob *compiled, *errors;
        if (FAILED(D3DCompile(
                hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, target, flags, 0, &compiled, &errors)))
            Log("Error: D3DCompile failed %s", (char*)errors->GetBufferPointer());
        if (errors)
            errors->Release();

        return compiled;
    }

    HANDLE hMapFile;
    TCHAR szName[] = TEXT("OpenCompositeMirrorSurface");
    char szName_[] = "OpenCompositeMirrorSurface";

    struct MirrorSurfaceData {
        uint32_t frameNumber = 0;
        uint32_t eyeIndex = 0;
        HANDLE sharedHandle = nullptr;

        void setHandle(HANDLE h) {
            sharedHandle = h;
        }

        HANDLE getHandle() {
            return sharedHandle;
        }

        void reset() {
            sharedHandle = nullptr;
        }
    };

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

    class D3D11Mirror {
      public:
        D3D11Mirror() {
            HRESULT hr;
            D3D_FEATURE_LEVEL featureLevel[] = {
                D3D_FEATURE_LEVEL_11_1, 
                D3D_FEATURE_LEVEL_11_0
            };
            
            hr = D3D11CreateDevice(NULL,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   0,
#ifdef _DEBUG
                                   D3D11_CREATE_DEVICE_DEBUG |
#endif
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   0,
                                   0,
                                   D3D11_SDK_VERSION,
                                   _d3d11MirrorDevice.ReleaseAndGetAddressOf(),
                                   featureLevel,
                                   _d3d11MirrorContext.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {
                Log("init: D3D11CreateDevice failed\n");
                return;
            }

            Log("init: D3D11CreateDevice created\n");

            ID3DBlob* vShaderBlob = d3d_compile_shader(quad_shader_code, "vs_quad", "vs_5_0");
            ID3DBlob* pShaderBlob = d3d_compile_shader(quad_shader_code, "ps_quad", "ps_5_0");
            CHECK_DX(_d3d11MirrorDevice->CreateVertexShader(vShaderBlob->GetBufferPointer(),
                                                            vShaderBlob->GetBufferSize(),
                                                            nullptr,
                                                            _quadVShader.ReleaseAndGetAddressOf()));
            CHECK_DX(_d3d11MirrorDevice->CreatePixelShader(pShaderBlob->GetBufferPointer(),
                                                           pShaderBlob->GetBufferSize(),
                                                           nullptr,
                                                           _quadPShader.ReleaseAndGetAddressOf()));

            D3D11_INPUT_ELEMENT_DESC q_vert_desc[] = {
                {"POSITION",
                 0,
                 DXGI_FORMAT_R32G32B32A32_FLOAT,
                 0,
                 D3D11_APPEND_ALIGNED_ELEMENT,
                 D3D11_INPUT_PER_VERTEX_DATA,
                 0},
                {"TEXCOORD",
                 0,
                 DXGI_FORMAT_R32G32_FLOAT,
                 0,
                 D3D11_APPEND_ALIGNED_ELEMENT,
                 D3D11_INPUT_PER_VERTEX_DATA,
                 0},
            };
            CHECK_DX(_d3d11MirrorDevice->CreateInputLayout(q_vert_desc,
                                                           (UINT)_countof(q_vert_desc),
                                                           vShaderBlob->GetBufferPointer(),
                                                           vShaderBlob->GetBufferSize(),
                                                           _quadShaderLayout.ReleaseAndGetAddressOf()));

            D3D11_SUBRESOURCE_DATA qVertBufferData = {quad_verts};
            D3D11_SUBRESOURCE_DATA qIndBufferData = {quad_inds};
            CD3D11_BUFFER_DESC qVertBufferDesc(
                sizeof(quad_verts), D3D11_BIND_VERTEX_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
            CD3D11_BUFFER_DESC qIndBufferDesc(sizeof(quad_inds), D3D11_BIND_INDEX_BUFFER);
            CD3D11_BUFFER_DESC qConstBufferDesc(sizeof(quad_transform_buffer_t), D3D11_BIND_CONSTANT_BUFFER);
            CHECK_DX(_d3d11MirrorDevice->CreateBuffer(
                &qVertBufferDesc, &qVertBufferData, _quadVertexBuffer.ReleaseAndGetAddressOf()));
            CHECK_DX(_d3d11MirrorDevice->CreateBuffer(
                &qIndBufferDesc, &qIndBufferData, _quadIndexBuffer.ReleaseAndGetAddressOf()));
            CHECK_DX(_d3d11MirrorDevice->CreateBuffer(
                &qConstBufferDesc, nullptr, _quadConstantBuffer.ReleaseAndGetAddressOf()));

            // Create a texture sampler state description.
            D3D11_SAMPLER_DESC samplerDesc;
            samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            samplerDesc.MipLODBias = 0.0f;
            samplerDesc.MaxAnisotropy = 1;
            samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
            samplerDesc.BorderColor[0] = 1.0f;
            samplerDesc.BorderColor[1] = 1.0f;
            samplerDesc.BorderColor[2] = 1.0f;
            samplerDesc.BorderColor[3] = 1.0f;
            samplerDesc.MinLOD = -FLT_MAX;
            samplerDesc.MaxLOD = FLT_MAX;

            // Create the texture sampler state.
            CHECK_DX(_d3d11MirrorDevice->CreateSamplerState(&samplerDesc, _quadSampleState.ReleaseAndGetAddressOf()));

            D3D11_BLEND_DESC blendDesc;
            ZeroMemory(&blendDesc, sizeof(D3D11_BLEND_DESC));

            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

            CHECK_DX(_d3d11MirrorDevice->CreateBlendState(&blendDesc, _quadBlendState.ReleaseAndGetAddressOf()));

            _d3d11MirrorContext->VSSetConstantBuffers(0, 1, _quadConstantBuffer.GetAddressOf());
            _d3d11MirrorContext->VSSetShader(_quadVShader.Get(), nullptr, 0);
            _d3d11MirrorContext->PSSetShader(_quadPShader.Get(), nullptr, 0);
            _d3d11MirrorContext->PSSetSamplers(0, 1, _quadSampleState.GetAddressOf());

            UINT strides[4] = {sizeof(float) * 6, sizeof(float) * 6, sizeof(float) * 6, sizeof(float) * 6};
            UINT offsets[4] = {0, 0, 0, 0};
            _d3d11MirrorContext->IASetVertexBuffers(0, 1, _quadVertexBuffer.GetAddressOf(), strides, offsets);
            _d3d11MirrorContext->IASetIndexBuffer(_quadIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
            _d3d11MirrorContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            _d3d11MirrorContext->IASetInputLayout(_quadShaderLayout.Get());

            //_mappedQuadVertexBuffer = D3D11_MAPPED_SUBRESOURCE();
            CHECK_DX(_d3d11MirrorContext->Map(
                _quadVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &_mappedQuadVertexBuffer));

            createMirrorSurface();
            
        }

        ~D3D11Mirror() {
            if (_pMirrorSurfaceData) {
                Log("Unmapping file\n");
                _pMirrorSurfaceData->reset();
                UnmapViewOfFile(_pMirrorSurfaceData);
                _pMirrorSurfaceData = nullptr;
                CloseHandle(hMapFile);
            }

            _d3d11MirrorContext->Unmap(_quadVertexBuffer.Get(), 0);
        }

        void createSharedMirrorTexture(const XrSwapchain & swapchain, const ComPtr<ID3D11Texture2D>& tex) {
            IDXGIResource* pOtherResource(NULL);
            CHECK_DX(tex->QueryInterface(__uuidof(IDXGIResource), (void**)&pOtherResource));

            HANDLE sharedHandle;
            pOtherResource->GetSharedHandle(&sharedHandle);

            MirrorData& data = _mirrorData[swapchain];
            data = MirrorData();

            CHECK_DX(_d3d11MirrorDevice->OpenSharedResource(
                sharedHandle, __uuidof(IDXGIResource), reinterpret_cast<void**>(data._mirrorSharedResource.GetAddressOf())));

            CHECK_DX(data._mirrorSharedResource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                reinterpret_cast<void**>(data._mirrorTexture.GetAddressOf())));

            D3D11_TEXTURE2D_DESC srcDesc;
            data._mirrorTexture->GetDesc(&srcDesc);

            // Figure out what format we need to use
            DxgiFormatInfo info = {};
            if (!GetFormatInfo(srcDesc.Format, info)) {
                Log("Unknown DXGI texture format %d\n", srcDesc.Format);
            }

            bool useLinearFormat = info.bpc > 8;
            DXGI_FORMAT type = useLinearFormat ? info.linear : info.srgb;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
            viewDesc.Format = type;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
            viewDesc.Texture2D.MostDetailedMip = 0;

            CHECK_DX(_d3d11MirrorDevice->CreateShaderResourceView(
                data._mirrorTexture.Get(), &viewDesc, data._quadTextureView.GetAddressOf()));
        }

        void createSharedMirrorTexture(const XrSwapchain & swapchain, HANDLE& handle) {
            MirrorData& data = _mirrorData[swapchain];
            data = MirrorData();
            ComPtr<ID3D11Device1> pDevice = nullptr;

            CHECK_DX(_d3d11MirrorDevice->QueryInterface(IID_PPV_ARGS(&pDevice)));
            CHECK_DX(pDevice->OpenSharedResource1(handle, IID_PPV_ARGS(&data._mirrorTexture)));

            D3D11_TEXTURE2D_DESC srcDesc;
            data._mirrorTexture->GetDesc(&srcDesc);

            // Figure out what format we need to use
            DxgiFormatInfo info = {};
            if (!GetFormatInfo(srcDesc.Format, info)) {
                Log("Unknown DXGI texture format %d\n", srcDesc.Format);
            }

            bool useLinearFormat = info.bpc > 8;
            DXGI_FORMAT type = useLinearFormat ? info.linear : info.srgb;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
            viewDesc.Format = type;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
            viewDesc.Texture2D.MostDetailedMip = 0;

            CHECK_DX(_d3d11MirrorDevice->CreateShaderResourceView(
                data._mirrorTexture.Get(), &viewDesc, data._quadTextureView.GetAddressOf()));
        }

        bool enabled() {
            return _obsRunning;
        }

        void flush() {
            _d3d11MirrorContext->Flush();
            if (_targetView) {
                _d3d11MirrorContext->OMSetRenderTargets(1, _targetView.GetAddressOf(), nullptr);
                float clearRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                _d3d11MirrorContext->ClearRenderTargetView(_targetView.Get(), clearRGBA);
            }
        }

        void addSpace(const XrSpace& space, const XrReferenceSpaceCreateInfo* createInfo) {
            _spaceInfo[space] = *createInfo;
        }

        const XrReferenceSpaceCreateInfo* getSpaceInfo(const XrSpace& space) {
            auto it = _spaceInfo.find(space);
            if (it != _spaceInfo.end())
                return &it->second;
            else
                return nullptr;
        }

        void Blend(const XrCompositionLayerProjectionView* view,
                   const XrCompositionLayerQuad* quad,
                   DXGI_FORMAT format) {
            auto it = _mirrorData.find(quad->subImage.swapchain);
            if (it == _mirrorData.end())
                return;

            auto src = it->second._mirrorTexture;

            if (!src)
                return;

            checkCopyTex(view->subImage.imageRect.extent.width, view->subImage.imageRect.extent.height, format);

            D3D11_TEXTURE2D_DESC srcDesc;

            src->GetDesc(&srcDesc);
            if (_copyTexture == nullptr || _mirrorTexture == nullptr)
                return;

            // Figure out what format we need to use
            DxgiFormatInfo info = {};
            if (!GetFormatInfo(srcDesc.Format, info)) {
                Log("Unknown DXGI texture format %d\n", srcDesc.Format);
            }
            bool useLinearFormat = info.bpc > 8;
            DXGI_FORMAT type = useLinearFormat ? info.linear : info.srgb;
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc;
            viewDesc.Format = type;
            viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipLevels = 1;
            viewDesc.Texture2D.MostDetailedMip = 0;

            // D3D11_MAPPED_SUBRESOURCE& d3dMappedResource = it->second.d3dMappedResource;
            // D3D11_MAPPED_SUBRESOURCE d3dMappedResource;
            // CHECK_DX(_d3d11MirrorContext->Map(_quadVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &d3dMappedResource));

            float* pBuffer = (float*)_mappedQuadVertexBuffer.pData;
            memcpy(pBuffer, quad_verts, sizeof(quad_verts));

            const uint32_t row = 6;
            // Top left
            pBuffer[0 * row + 4] = (float)quad->subImage.imageRect.offset.x / (float)srcDesc.Width;
            pBuffer[0 * row + 5] = (float)quad->subImage.imageRect.offset.y / (float)srcDesc.Height;
            // Bottom left
            pBuffer[1 * row + 4] = (float)quad->subImage.imageRect.offset.x / (float)srcDesc.Width;
            pBuffer[1 * row + 5] = (float)(quad->subImage.imageRect.offset.y + quad->subImage.imageRect.extent.height) /
                                   (float)srcDesc.Height;
            // Top right
            pBuffer[2 * row + 4] = (float)(quad->subImage.imageRect.offset.x + quad->subImage.imageRect.extent.width) /
                                   (float)srcDesc.Width;
            pBuffer[2 * row + 5] = (float)(quad->subImage.imageRect.offset.y) / (float)srcDesc.Height;
            // Bottom right
            pBuffer[3 * row + 4] = (float)(quad->subImage.imageRect.offset.x + quad->subImage.imageRect.extent.width) /
                                   (float)srcDesc.Width;
            pBuffer[3 * row + 5] = (float)(quad->subImage.imageRect.offset.y + quad->subImage.imageRect.extent.height) /
                                   (float)srcDesc.Height;

            //_d3d11MirrorContext->Unmap(_quadVertexBuffer, 0);

            auto _quadTextureView = it->second._quadTextureView;

            _d3d11MirrorContext->PSSetShaderResources(0, 1, _quadTextureView.GetAddressOf());

            float blend_factor[4] = {1.f, 1.f, 1.f, 1.f};
            _d3d11MirrorContext->OMSetBlendState(_quadBlendState.Get(), blend_factor, 0xffffffff);

            const XrRect2Di& rect = view->subImage.imageRect;
            D3D11_VIEWPORT viewport = CD3D11_VIEWPORT(
                (float)rect.offset.x, (float)rect.offset.y, (float)rect.extent.width, (float)rect.extent.height);
            _d3d11MirrorContext->RSSetViewports(1, &viewport);
            D3D11_RECT rects[1];
            rects[0].top = rect.offset.y;
            rects[0].left = rect.offset.x;
            rects[0].bottom = rect.offset.y + rect.extent.height;
            rects[0].right = rect.offset.x + rect.extent.width;
            _d3d11MirrorContext->RSSetScissorRects(1, rects);

            // Set up for rendering
            _d3d11MirrorContext->OMSetRenderTargets(1, _targetView.GetAddressOf(), nullptr);

            // Set up camera matrices based on OpenXR's predicted viewpoint information
            XMMATRIX mat_projection = d3dXrProjection(view->fov, 0.05f, 100.0f);

            // XrPosef poseSpace = {{0.f, 0.f, 0.f, 1.f}, {0.0f, 0.0f, 0.0f}};
            XrPosef viewPose = view->pose;

            if (_spaceInfo.count(quad->space) &&
                _spaceInfo[quad->space].referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
                viewPose = _spaceInfo[quad->space].poseInReferenceSpace;
            }

            XMMATRIX mat_view =
                XMMatrixInverse(nullptr,
                                XMMatrixAffineTransformation(DirectX::g_XMOne,
                                                             DirectX::g_XMZero,
                                                             // XMLoadFloat4((XMFLOAT4*)&view->pose.orientation),
                                                             XMLoadFloat4((XMFLOAT4*)&viewPose.orientation),
                                                             // XMLoadFloat3((XMFLOAT3*)&view->pose.position)));
                                                             XMLoadFloat3((XMFLOAT3*)&viewPose.position)));

            // Log("Orientation: %f %f %f %f Position: %f %f %f FOV: %f %f %f %f\n",
            //    view->pose.orientation.x,
            //    view->pose.orientation.y,
            //    view->pose.orientation.z,
            //    view->pose.orientation.w,
            //    view->pose.position.x,
            //    view->pose.position.y,
            //    view->pose.position.z,
            //    view->fov.angleUp,
            //    view->fov.angleDown,
            //    view->fov.angleLeft,
            //    view->fov.angleRight);

            // Put camera matrices into the shader's constant buffer
            quad_transform_buffer_t transform_buffer;
            XMStoreFloat4x4(&transform_buffer.viewproj, XMMatrixTranspose(mat_view * mat_projection));

            // Log("Quad dim w: %f h: %f %d %d %d %d\n",
            //     quad->size.width,
            //     quad->size.height,
            //     quad->subImage.imageRect.offset.x,
            //     quad->subImage.imageRect.offset.y,
            //     quad->subImage.imageRect.extent.width,
            //     quad->subImage.imageRect.extent.height);

            float wScale = quad->size.width * (float)srcDesc.Width /
                           (float)(quad->subImage.imageRect.extent.width - quad->subImage.imageRect.offset.x);
            float hScale = quad->size.height * (float)srcDesc.Height /
                           (float)(quad->subImage.imageRect.extent.height - quad->subImage.imageRect.offset.y);

            XrPosef pose = {{1.f, 0.f, 0.f, 0.f}, {0.0f, 0.0f, -0.5f}};
            pose = quad->pose;
            if (pose.orientation.x == 0.f && pose.orientation.y == 0.f && pose.orientation.z == 0.f &&
                pose.orientation.w == 1.f) {
                pose.orientation = {1.f, 0.f, 0.f, 0.f};
            }

            XMVECTORF32 scalingVector = {quad->size.width, -1.0f * quad->size.height, 1.f, 1.f};
            XMVECTORF32 rotOrigin = {0.f, 0.f, 0.f, 1.f};
            XMVECTORF32 quatRot = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
            XMVECTORF32 trans = {pose.position.x, pose.position.y, pose.position.z, 1.f};

            XMMATRIX mat_model = XMMatrixAffineTransformation(scalingVector, rotOrigin, quatRot, trans);

            // Update the shader's constant buffer with the transform matrix info, and then draw the quad
            XMStoreFloat4x4(&transform_buffer.world, XMMatrixTranspose(mat_model));
            _d3d11MirrorContext->UpdateSubresource(_quadConstantBuffer.Get(), 0, nullptr, &transform_buffer, 0, 0);
            _d3d11MirrorContext->DrawIndexed((UINT)_countof(quad_inds), 0, 0);
        }

        void copyPerspectiveTex(uint32_t width, uint32_t height, DXGI_FORMAT format, XrSwapchain swapchain) {
            auto it = _mirrorData.find(swapchain);
            if (it == _mirrorData.end())
                return;

            checkCopyTex(width, height, format);
            if (_copyTexture) {
                _d3d11MirrorContext->CopyResource(_copyTexture.Get(), it->second._mirrorTexture.Get());
            }
        }

        void checkCopyTex(uint32_t width, uint32_t height, DXGI_FORMAT format) {
            if (_copyTexture) {
                D3D11_TEXTURE2D_DESC srcDesc;
                _copyTexture->GetDesc(&srcDesc);
                if (srcDesc.Width != width || srcDesc.Height != height) {
                    _copyTexture = nullptr;
                    _mirrorTexture = nullptr;
                }
            }
            if (_copyTexture == nullptr) {
                D3D11_TEXTURE2D_DESC desc;
                ZeroMemory(&desc, sizeof(desc));
                desc.Width = width;
                desc.Height = height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = format;
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

                Log("Creating mirror textures w %u h %u f %d\n", desc.Width, desc.Height, format);

                CHECK_DX(_d3d11MirrorDevice->CreateTexture2D(&desc, NULL, _copyTexture.ReleaseAndGetAddressOf()));
                CHECK_DX(_d3d11MirrorDevice->CreateTexture2D(&desc, NULL, _mirrorTexture.ReleaseAndGetAddressOf()));

                IDXGIResource* pOtherResource(NULL);
                CHECK_DX(_mirrorTexture->QueryInterface(__uuidof(IDXGIResource), (void**)&pOtherResource));

                HANDLE sharedHandle;
                pOtherResource->GetSharedHandle(&sharedHandle);
                _pMirrorSurfaceData->setHandle(sharedHandle);
                Log("Shared handle: 0x%p\n", sharedHandle);

                D3D11_TEXTURE2D_DESC color_desc;
                _copyTexture->GetDesc(&color_desc);

                Log("Texture description: %d x %d Format %d\n", color_desc.Width, color_desc.Height, color_desc.Format);

                // Create a view resource for the swapchain image target that we can use to set
                // up rendering.
                D3D11_RENDER_TARGET_VIEW_DESC targetDesc = {};
                targetDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                targetDesc.Format = color_desc.Format;
                targetDesc.Texture2D.MipSlice = 0;
                ID3D11RenderTargetView* rtv;
                CHECK_DX(_d3d11MirrorDevice->CreateRenderTargetView(_copyTexture.Get(), &targetDesc, &rtv));
                _targetView.Attach(rtv);
            }
        }

        void copyToMIrror() {
            if (_copyTexture && _mirrorTexture) {
                _d3d11MirrorContext->CopyResource(_mirrorTexture.Get(), _copyTexture.Get());
            }
        }

        void checkOBSRunning() {
            static uint32_t frameCounter = 10;
            static uint32_t lastFrameNum = 0;

            if (lastFrameNum == _pMirrorSurfaceData->frameNumber)
                frameCounter++;
            else
                frameCounter = 0;

            if (frameCounter > 10)
                _obsRunning = false;
            else
                _obsRunning = true;

            lastFrameNum = _pMirrorSurfaceData->frameNumber;
        }

      private:
        void createMirrorSurface() {
            Log("Mapping file %s.\n", szName_);
            hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE,      // use paging file
                                          NULL,                      // default security
                                          PAGE_READWRITE,            // read/write access
                                          0,                         // maximum object size (high-order DWORD)
                                          sizeof(MirrorSurfaceData), // maximum object size (low-order DWORD)
                                          szName_);                  // name of mapping object

            if (hMapFile == NULL) {
                Log("Could not create file mapping object (%d).\n", GetLastError());
                throw std::string("Could not create file mapping object");
            }
            _pMirrorSurfaceData = (MirrorSurfaceData*)MapViewOfFile(hMapFile,            // handle to map object
                                                                    FILE_MAP_ALL_ACCESS, // read/write permission
                                                                    0,
                                                                    0,
                                                                    sizeof(MirrorSurfaceData));

            if (_pMirrorSurfaceData == nullptr) {
                Log("Could not map view of file (%d).\n", GetLastError());
                CloseHandle(hMapFile);
                throw std::string("Could not map view of file");
            }
        }

        struct MirrorData {
            ComPtr<IDXGIResource> _mirrorSharedResource = nullptr;
            ComPtr<ID3D11Texture2D> _mirrorTexture = nullptr;
            ComPtr<ID3D11ShaderResourceView> _quadTextureView = nullptr;
        };

        ComPtr<ID3D11Device> _d3d11MirrorDevice = nullptr;
        ComPtr<ID3D11DeviceContext> _d3d11MirrorContext = nullptr;

        std::map<XrSwapchain, MirrorData> _mirrorData;
        MirrorSurfaceData* _pMirrorSurfaceData = nullptr;

        std::map<XrSpace, XrReferenceSpaceCreateInfo> _spaceInfo;

        ComPtr<ID3D11RenderTargetView> _targetView = nullptr;

        ComPtr<ID3D11VertexShader> _quadVShader = nullptr;
        ComPtr<ID3D11PixelShader> _quadPShader = nullptr;
        ComPtr<ID3D11InputLayout> _quadShaderLayout = nullptr;
        ComPtr<ID3D11Buffer> _quadConstantBuffer = nullptr;
        ComPtr<ID3D11Buffer> _quadVertexBuffer = nullptr;
        ComPtr<ID3D11Buffer> _quadIndexBuffer = nullptr;
        ComPtr<ID3D11SamplerState> _quadSampleState = nullptr;
        ComPtr<ID3D11BlendState> _quadBlendState = nullptr;

        D3D11_MAPPED_SUBRESOURCE _mappedQuadVertexBuffer{};

        ComPtr<ID3D11Texture2D> _copyTexture = nullptr;
        ComPtr<ID3D11Texture2D> _mirrorTexture = nullptr;

        bool _obsRunning = false;
    };

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
                DxgiFormatInfo formatInfo;
                GetFormatInfo((DXGI_FORMAT)swapchainState._createInfo.format, formatInfo);
                if (formatInfo.bpc <= 10 &&
                    swapchainState._createInfo.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) {
                    if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                        swapchainState._dx11SurfaceImages.resize(*imageCountOutput);
                        for (int i = 0; i < *imageCountOutput; ++i) {
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

                            _mirror->createSharedMirrorTexture(swapchain, swapchainState._dx11LastTexture);
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

                        for (int i = 0; i < *imageCountOutput; ++i) {
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

        XrResult xrReleaseSwapchainImage(XrSwapchain swapchain,
                                         const XrSwapchainImageReleaseInfo* releaseInfo) override {
            if (_mirror && _mirror->enabled() && isSwapchainHandled(swapchain)) {
                auto& swapchainState = _swapchains[swapchain];
                uint32_t idx = swapchainState._aquiredIndex;
                if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR &&
                    !swapchainState._dx11SurfaceImages.empty()) {
                    auto* textPtr = swapchainState._dx11SurfaceImages[idx].texture;
                    if (swapchainState._dx11LastTexture) {
                        _d3d11Context->CopyResource(swapchainState._dx11LastTexture.Get(), textPtr);
                    }
                }
                else if (_xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR &&
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
                    }
                }
            }
            const XrResult result = OpenXrApi::xrReleaseSwapchainImage(swapchain, releaseInfo);
            if (_mirror && _mirror->enabled() && isSwapchainHandled(swapchain) &&
                _xrGraphicsAPI == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR){
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
                    for (int nView = 0; nView < *viewCountOutput; nView++) {
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
                    _projectionViews[0].subImage.imageRect.offset.x = 0;
                    _projectionViews[0].subImage.imageRect.offset.y = 0;
                    _projectionViews[0].subImage.imageRect.extent.width = _xrViewsList[0].recommendedImageRectWidth;
                    _projectionViews[0].subImage.imageRect.extent.height = _xrViewsList[0].recommendedImageRectHeight;

                    //_projectionViews[0].pose.orientation = XrQuaternionf{0, 0, 0, 1};
                    //_projectionViews[0].pose.position = XrVector3f{0, 0.25, 0.25};

                    uint32_t count = frameEndInfo->layerCount;
                    for (uint32_t i = 0; i < count; ++i) {
                        const XrCompositionLayerBaseHeader* hdr = frameEndInfo->layers[i];
                        if (hdr->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                            const XrCompositionLayerProjection* projLayer =
                                reinterpret_cast<const XrCompositionLayerProjection*>(hdr);
                            if (projLayer->viewCount == 2) {
                                projView = &projLayer->views[0];
                                if (isSwapchainHandled(projView->subImage.swapchain)) {
                                    auto& swapchainState = _swapchains[projView->subImage.swapchain];
                                    if (swapchainState._dx11LastTexture || swapchainState._dx12LastTexture) {
                                        _mirror->copyPerspectiveTex(projView->subImage.imageRect.extent.width,
                                                                    projView->subImage.imageRect.extent.height,
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
                                if (swapchainState._dx11LastTexture || swapchainState._dx12LastTexture) {
                                    if (projView) {
                                        _mirror->Blend(
                                            projView, quadLayer, (DXGI_FORMAT)swapchainState._createInfo.format);
                                    }
                                }
                            }
                        }
                    }
                    _mirror->copyToMIrror();
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
