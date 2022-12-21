#pragma once
#include "pch.h"
#include <map>

namespace Mirror
{
    class MirrorSurfaceData;

    struct DxgiFormatInfo {
        /// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
        /// Both the SRGB and linear formats should be UNORM.
        DXGI_FORMAT srgb, linear, typeless;

        /// THe bits per pixel, bits per channel, and the number of channels
        int bpp, bpc, channels;
    };

    bool GetFormatInfo(DXGI_FORMAT format, DxgiFormatInfo& out);

    class D3D11Mirror {
      public:
        D3D11Mirror();
        ~D3D11Mirror();

        void createSharedMirrorTexture(const XrSwapchain& swapchain, const ComPtr<ID3D11Texture2D>& tex);

        void createSharedMirrorTexture(const XrSwapchain& swapchain, HANDLE& handle);

        bool enabled();

        void flush();

        void addSpace(const XrSpace& space, const XrReferenceSpaceCreateInfo* createInfo);

        const XrReferenceSpaceCreateInfo* getSpaceInfo(const XrSpace& space);

        void Blend(const XrCompositionLayerProjectionView* view,
                   const XrCompositionLayerQuad* quad,
                   DXGI_FORMAT format);

        void copyPerspectiveTex(uint32_t width, uint32_t height, DXGI_FORMAT format, XrSwapchain swapchain);

        void copyToMIrror();

        void checkOBSRunning();

      private:
        void createMirrorSurface();

        void checkCopyTex(uint32_t width, uint32_t height, DXGI_FORMAT format);

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
}

