#include "pch.h"
#include "dx11mirror.h"
#include "log.h"
#include "util.h"

#include <directxmath.h> // Matrix math functions and objects
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
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
} // namespace

namespace Mirror {
    using namespace layer_OBSMirror::log;
    using namespace DirectX; // Matrix math


    bool GetFormatInfo(const DXGI_FORMAT format, DxgiFormatInfo& out) {
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

        return XMMatrixPerspectiveOffCenterRH(left, right, down, up, clip_near, clip_far);
    }

    struct quad_transform_buffer_t {
        XMFLOAT4X4 world;
        XMFLOAT4X4 viewproj;
    };


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
        -0.5,  0.5, 0, 1,   0, 0, 
        -0.5, -0.5, 0, 1,   0, 1, 
         0.5,  0.5, 0, 1,   1, 0, 
         0.5, -0.5, 0, 1,   1, 1};

    uint16_t quad_inds[] = {2, 1, 0, 
                            2, 3, 1};

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

    D3D11Mirror::D3D11Mirror() {
        HRESULT hr;
        D3D_FEATURE_LEVEL featureLevel[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};

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

        CHECK_DX(_d3d11MirrorContext->Map(
            _quadVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &_mappedQuadVertexBuffer));

        createMirrorSurface();
    }

    D3D11Mirror::~D3D11Mirror() {
        if (_pMirrorSurfaceData) {
            Log("Unmapping file\n");
            _pMirrorSurfaceData->reset();
            UnmapViewOfFile(_pMirrorSurfaceData);
            _pMirrorSurfaceData = nullptr;
            CloseHandle(hMapFile);
        }

        _d3d11MirrorContext->Unmap(_quadVertexBuffer.Get(), 0);
    }

    void D3D11Mirror::createSharedMirrorTexture(const XrSwapchain& swapchain, const ComPtr<ID3D11Texture2D>& tex) {
        ComPtr<IDXGIResource> pOtherResource = nullptr;
        CHECK_DX(tex->QueryInterface(IID_PPV_ARGS(&pOtherResource)));

        HANDLE sharedHandle;
        pOtherResource->GetSharedHandle(&sharedHandle);

        MirrorData& data = _mirrorData[swapchain];
        data = MirrorData();

        CHECK_DX(_d3d11MirrorDevice->OpenSharedResource(sharedHandle,
                                                        IID_PPV_ARGS(&data._mirrorSharedResource)));

        CHECK_DX(data._mirrorSharedResource->QueryInterface(IID_PPV_ARGS(&data._mirrorTexture)));

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

    void D3D11Mirror::createSharedMirrorTexture(const XrSwapchain& swapchain, const HANDLE& handle) {
        MirrorData& data = _mirrorData[swapchain];
        data = MirrorData();
        ComPtr<ID3D11Device1> pDevice = nullptr;

        CHECK_DX(_d3d11MirrorDevice->QueryInterface(IID_PPV_ARGS(&pDevice)));
        CHECK_DX(pDevice->OpenSharedResource1(handle, IID_PPV_ARGS(&data._mirrorTexture)));

        pDevice.Reset();

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

    bool D3D11Mirror::enabled() const {
        return _obsRunning;
    }

    void D3D11Mirror::flush() {
        _d3d11MirrorContext->Flush();
        if (_targetView) {
            _d3d11MirrorContext->OMSetRenderTargets(1, _targetView.GetAddressOf(), nullptr);
            float clearRGBA[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            _d3d11MirrorContext->ClearRenderTargetView(_targetView.Get(), clearRGBA);
        }
    }

    void D3D11Mirror::addSpace(const XrSpace& space, const XrReferenceSpaceCreateInfo* createInfo) {
        _spaceInfo[space] = *createInfo;
    }

    const XrReferenceSpaceCreateInfo* D3D11Mirror::getSpaceInfo(const XrSpace& space) const {
        auto it = _spaceInfo.find(space);
        if (it != _spaceInfo.end())
            return &it->second;
        else
            return nullptr;
    }

    void D3D11Mirror::Blend(const XrCompositionLayerProjectionView* view,
                            const XrCompositionLayerQuad* quad,
                            const DXGI_FORMAT format) {
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

        XrPosef viewPose = view->pose;

        if (_spaceInfo.count(quad->space) &&
            _spaceInfo[quad->space].referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW) {
            viewPose = _spaceInfo[quad->space].poseInReferenceSpace;
        }

        XMMATRIX mat_view =
            XMMatrixInverse(nullptr,
                            XMMatrixAffineTransformation(DirectX::g_XMOne,
                                                            DirectX::g_XMZero,
                                                            XMLoadFloat4((XMFLOAT4*)&viewPose.orientation),
                                                            XMLoadFloat3((XMFLOAT3*)&viewPose.position)));

        // Put camera matrices into the shader's constant buffer
        quad_transform_buffer_t transform_buffer;
        XMStoreFloat4x4(&transform_buffer.viewproj, XMMatrixTranspose(mat_view * mat_projection));

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

    void D3D11Mirror::copyPerspectiveTex(const XrRect2Di & imgRect, 
                                         const DXGI_FORMAT format, 
                                         const XrSwapchain & swapchain) {
        auto it = _mirrorData.find(swapchain);
        if (it == _mirrorData.end())
            return;

        checkCopyTex(imgRect.extent.width, imgRect.extent.height, format);
        if (_copyTexture) {
            D3D11_BOX sourceRegion;
            sourceRegion.left = imgRect.offset.x;
            sourceRegion.right = imgRect.offset.x + imgRect.extent.width;
            sourceRegion.top = imgRect.offset.y;
            sourceRegion.bottom = imgRect.offset.y + imgRect.extent.height;
            sourceRegion.front = 0;
            sourceRegion.back = 1;
            _d3d11MirrorContext->CopySubresourceRegion(
                _copyTexture.Get(), 0, 0, 0, 0, it->second._mirrorTexture.Get(), 0, &sourceRegion);
        }
    }

    void D3D11Mirror::checkCopyTex(const uint32_t width, 
                                   const uint32_t height, 
                                   const DXGI_FORMAT format) {
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

            ComPtr<IDXGIResource> pOtherResource = nullptr;
            CHECK_DX(_mirrorTexture->QueryInterface(IID_PPV_ARGS(&pOtherResource)));

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

    void D3D11Mirror::copyToMIrror() {
        if (_copyTexture && _mirrorTexture) {
            _d3d11MirrorContext->CopyResource(_mirrorTexture.Get(), _copyTexture.Get());
        }
    }

    void D3D11Mirror::checkOBSRunning() {
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

    uint32_t D3D11Mirror::getEyeIndex() const {
        return _pMirrorSurfaceData->eyeIndex;
    }

    void D3D11Mirror::createMirrorSurface() {
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
} // Mirror namespace