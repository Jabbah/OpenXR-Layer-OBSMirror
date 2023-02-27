#pragma once
#include "pch.h"
#include <map>

namespace Mirror
{
    struct MirrorSurfaceData;

    struct DxgiFormatInfo {
        /// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
        /// Both the SRGB and linear formats should be UNORM.
        DXGI_FORMAT srgb, linear, typeless;

        /// THe bits per pixel, bits per channel, and the number of channels
        int bpp, bpc, channels;
    };

    bool GetFormatInfo(const DXGI_FORMAT format, DxgiFormatInfo& out);

    class D3D11Mirror {
      public:
        D3D11Mirror();
        ~D3D11Mirror();

        void createSharedMirrorTexture(const XrSwapchain& swapchain, const ComPtr<ID3D11Texture2D>& tex, const DXGI_FORMAT format);

        void createSharedMirrorTexture(const XrSwapchain& swapchain, const HANDLE& handle);

        bool enabled() const;

        void flush();

        void addSpace(const XrSpace space, const XrReferenceSpaceCreateInfo* createInfo);

        void removeSpace(const XrSpace space);

        const XrReferenceSpaceCreateInfo* getSpaceInfo(const XrSpace space) const;

        void Blend(const XrCompositionLayerProjectionView* view,
                   const XrCompositionLayerQuad* quad,
                   const DXGI_FORMAT format,
                   const XrSpace space,
                   const XrTime displayTime);

        void copyPerspectiveTex(const XrRect2Di& imgRect, const DXGI_FORMAT format, const XrSwapchain& swapchain);

        void copyToMirror();

        void checkOBSRunning();

        uint32_t getEyeIndex() const;

      private:
        void createMirrorSurface();

        void checkCopyTex(const uint32_t width, const uint32_t height, const DXGI_FORMAT format);

        struct SourceData {
            ComPtr<IDXGIResource> _sharedResource = nullptr;
            ComPtr<ID3D11Texture2D> _texture = nullptr;
            ComPtr<ID3D11ShaderResourceView> _quadTextureView = nullptr;
        };

        ComPtr<ID3D11Device> _d3d11MirrorDevice = nullptr;
        ComPtr<ID3D11DeviceContext> _d3d11MirrorContext = nullptr;

        std::map<XrSwapchain, SourceData> _sourceData;
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

        ComPtr<ID3D11Texture2D> _compositorTexture = nullptr;
        std::vector<ComPtr<ID3D11Texture2D>> _mirrorTextures;

        uint32_t _frameCounter = 0;
        bool _obsRunning = false;
    };
}

