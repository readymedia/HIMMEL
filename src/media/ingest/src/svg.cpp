// SPDX-License-Identifier: MIT
// Copyright (c) 2026 ReadyMedia.no and Himmel contributors
// SVG -> PNG with Windows' own Direct2D SVG renderer (Windows 10 1703+), so no
// SVG library is needed (D142). CPU only (a WIC bitmap target): it never
// touches the GPU the show runs on.
#include "himmel/ingest/svg.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1_3.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace himmel::ingest {

using Microsoft::WRL::ComPtr;

bool rasterize_svg(const fs::path& svg, const fs::path& png, int target_h, int max_w, std::string* err, int* out_w, int* out_h) {
    auto fail = [&](const char* m, HRESULT hr = S_OK) {
        if (err) {
            char b[160];
            std::snprintf(b, sizeof(b), "%s (0x%08lX)", m, static_cast<unsigned long>(hr));
            *err = b;
        }
        return false;
    };
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct CoGuard { bool on; ~CoGuard() { if (on) CoUninitialize(); } } guard{SUCCEEDED(co)};

    ComPtr<IStream> stream;
    HRESULT hr = SHCreateStreamOnFileEx(svg.wstring().c_str(), STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream);
    if (FAILED(hr)) return fail("cannot open the SVG", hr);
    ComPtr<ID2D1Factory> factory;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    if (FAILED(hr)) return fail("no Direct2D", hr);
    ComPtr<IWICImagingFactory> wic;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return fail("no WIC", hr);

    auto make_target = [&](UINT w, UINT h, ComPtr<IWICBitmap>& bmp, ComPtr<ID2D1DeviceContext5>& dc) -> HRESULT {
        HRESULT r = wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
        if (FAILED(r)) return r;
        ComPtr<ID2D1RenderTarget> rt;
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        r = factory->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt);
        if (FAILED(r)) return r;
        return rt.As(&dc);
    };

    // 1. Read the natural size (viewBox, else width / height).
    ComPtr<IWICBitmap> bmp;
    ComPtr<ID2D1DeviceContext5> dc;
    hr = make_target(4, 4, bmp, dc);
    if (FAILED(hr)) return fail("no Direct2D SVG support (needs Windows 10 1703+)", hr);
    ComPtr<ID2D1SvgDocument> doc;
    hr = dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(100, 100), &doc);
    if (FAILED(hr) || !doc) return fail("not a readable SVG", hr);
    ComPtr<ID2D1SvgElement> root;
    doc->GetRoot(&root);
    float vw = 0, vh = 0;
    if (root) {
        D2D1_SVG_VIEWBOX vb{};
        if (SUCCEEDED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) && vb.width > 0 &&
            vb.height > 0)
            vw = vb.width, vh = vb.height;
        if (vw <= 0) {
            D2D1_SVG_LENGTH lw{}, lh{};
            if (SUCCEEDED(root->GetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lw, sizeof(lw))) &&
                SUCCEEDED(root->GetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH, &lh, sizeof(lh))) &&
                lw.units == D2D1_SVG_LENGTH_UNITS_NUMBER && lh.units == D2D1_SVG_LENGTH_UNITS_NUMBER && lw.value > 0 && lh.value > 0)
                vw = lw.value, vh = lh.value;
        }
    }
    if (vw <= 0 || vh <= 0) vw = vh = 1;  // GUESS: no size at all -> square
    int h = std::max(4, target_h);
    int w = static_cast<int>(std::lround(h * (vw / vh)));
    if (w > max_w) {
        h = static_cast<int>(std::lround(h * static_cast<double>(max_w) / w));
        w = max_w;
    }
    w = std::max(4, w / 4 * 4), h = std::max(4, h / 4 * 4);

    // 2. Draw at that size.
    hr = make_target(static_cast<UINT>(w), static_cast<UINT>(h), bmp, dc);
    if (FAILED(hr)) return fail("cannot make the render target", hr);
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    doc.Reset();
    hr = dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(static_cast<float>(w), static_cast<float>(h)), &doc);
    if (FAILED(hr)) return fail("not a readable SVG", hr);
    if (root) {
        doc->GetRoot(&root);
        // Make the picture fill the viewport even when the SVG names a pixel size.
        root->SetAttributeValue(L"width", D2D1_SVG_LENGTH{static_cast<float>(w), D2D1_SVG_LENGTH_UNITS_NUMBER});
        root->SetAttributeValue(L"height", D2D1_SVG_LENGTH{static_cast<float>(h), D2D1_SVG_LENGTH_UNITS_NUMBER});
        D2D1_SVG_VIEWBOX vb{};
        if (FAILED(root->GetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb))) || vb.width <= 0) {
            vb = {0, 0, vw, vh};
            root->SetAttributeValue(L"viewBox", D2D1_SVG_ATTRIBUTE_POD_TYPE_VIEWBOX, &vb, sizeof(vb));
        }
    }
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->DrawSvgDocument(doc.Get());
    hr = dc->EndDraw();
    if (FAILED(hr)) return fail("drawing the SVG failed", hr);

    // 3. Save as PNG (straight alpha).
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    hr = conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return fail("pixel format", hr);
    std::error_code ec;
    fs::create_directories(png.parent_path(), ec);
    ComPtr<IWICStream> out;
    wic->CreateStream(&out);
    hr = out->InitializeFromFilename(png.wstring().c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return fail("cannot write the PNG", hr);
    ComPtr<IWICBitmapEncoder> enc;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(out.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    enc->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    hr = frame->WriteSource(conv.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) return fail("cannot write the PNG", hr);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return true;
}

}  // namespace himmel::ingest
