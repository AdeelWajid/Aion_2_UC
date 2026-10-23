// Overlay.cpp — D3D11 immediate-mode 2D overlay for the internal ESP.
#include "aion/Overlay.hpp"

// UpdateLayeredWindowIndirect (used for dirty-rect presents) needs Vista+ headers.
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <vector>

#include "aion/Logger.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace aion {

namespace {

struct Vertex {
    float x, y;      // NDC
    float u, v;      // texture coords
    float r, g, b, a;
    // Stroke feathering, meaningful only when uv.x < 0 (untextured). edge is the position
    // across the stroke, -1 at one side and +1 at the other; half is the stroke's half-width
    // in pixels, and 0 marks a fill, which is opaque everywhere and needs no feathering.
    float edge, half;
};

const char* kShaderSrc = R"(
struct VS_IN { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0;
               float2 edge : TEXCOORD1; };
struct PS_IN { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0;
               float2 edge : TEXCOORD1; };
PS_IN VSMain(VS_IN input) {
    PS_IN o;
    o.pos = float4(input.pos, 0.0, 1.0);
    o.uv = input.uv;
    o.col = input.col;
    o.edge = input.edge;
    return o;
}
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 PSMain(PS_IN input) : SV_TARGET {
    // Glyph quads sample the atlas; every other primitive is untextured (uv = -1).
    float cov = 1.0;
    if (input.uv.x < 0.0) {
        // A stroke carries its perpendicular position and its half-width, so the pixel its
        // own edge falls in can be blended by how much of it the stroke really covers. The
        // rasterizer alone decides whole pixels, which hairline-saws a long diagonal - and
        // every snapline is a long diagonal. For an axis-aligned 1px stroke this still
        // evaluates to exactly 1.0 on the covered row and 0.0 beside it, so box edges keep
        // the crisp hairline they have always had.
        if (input.edge.y > 0.0)
            cov = saturate(input.edge.y + 0.5 - abs(input.edge.x) * (input.edge.y + 0.5));
    } else {
        cov = tex.Sample(smp, input.uv).r;
    }
    float a = input.col.a * cov;
    // premultiplied alpha for UpdateLayeredWindow(ULW_ALPHA)
    return float4(input.col.rgb * a, a);
}
)";

constexpr int kAtlasCols = 16;
constexpr int kAtlasRows = 8;
constexpr int kFirstChar = 32;
constexpr int kLastChar = 127;

// Glyphs are rasterized at kSuper x their native size and box-filtered down. That
// yields ~17 coverage levels per pixel (real antialiasing) instead of the 2-4 levels
// GDI hinting produces at native size, which is what makes small text look like a
// bitmap font.
constexpr int kSuper = 4;
// Slack inside each atlas cell for ascenders/descenders and the 1px shadow.
constexpr int kCellPad = 4;

}  // namespace

// One texture holds both fonts (small glyphs first, large below) so a single
// draw call and a single sampler binding serve all text.
struct FontLayout {
    int cell = 0;
    int cols = kAtlasCols, rows = kAtlasRows;
    int y_origin = 0;   // first glyph row inside the shared atlas
    std::vector<float> adv;  // per-glyph advance width in pixels (proportional text)
    bool valid = false;
};

struct D3D11OverlayImpl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HWND hwnd = nullptr;
    ID3D11Texture2D* offscreen = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    void* dib_bits = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* input_layout = nullptr;
    ID3D11Buffer* vbuf = nullptr;
    ID3D11Texture2D* font_tex = nullptr;
    ID3D11ShaderResourceView* font_srv = nullptr;
    int atlas_w = 0, atlas_h = 0;
    FontLayout font_small;
    FontLayout font_large;
    FontConfig font_cfg;
    OverlayStats stats;
    std::vector<unsigned char> atlas_cover;  // last rasterized atlas (debug dump)
    HDC screen_dc = nullptr;
    // Dirty region of the current frame, in window pixels (-1 == nothing drawn).
    float dirty_x0 = 0.f, dirty_y0 = 0.f, dirty_x1 = 0.f, dirty_y1 = 0.f;
    bool have_dirty = false;
    RECT last_dirty{};
    bool have_last_dirty = false;
    std::chrono::steady_clock::time_point last_present{};
    bool warned_ulw = false;
    int present_fps = 60;
    // Fast path: a composition swapchain that DWM blends on the GPU. No readback and
    // no UpdateLayeredWindow, so a frame costs a draw + Present (~microseconds)
    // instead of a 11 MB GPU->CPU copy plus a full-window DWM hand-off (~27 ms).
    bool use_dcomp = false;
    bool want_dcomp = true;  // host preference; DComp falls back automatically on failure
    bool vsync = false;  // Present(1,0) blocks up to 2 vblanks; we pace ourselves instead
    IDXGISwapChain1* swapchain = nullptr;
    IDCompositionDevice* dcomp_device = nullptr;
    IDCompositionTarget* dcomp_target = nullptr;
    IDCompositionVisual* dcomp_visual = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11BlendState* blend = nullptr;
    ID3D11RasterizerState* raster = nullptr;
    UINT vbuf_capacity = 0;
    int width = 0, height = 0;
    std::vector<Vertex> verts;
    // Second pass drawn between our flush and the present (the ImGui menu). Empty when no host is
    // attached, which is the normal case for the standalone overlay_test target.
    std::function<void()> pre_present;

    bool CreateShaders() {
        ID3DBlob* vsb = nullptr;
        ID3DBlob* psb = nullptr;
        ID3DBlob* err = nullptr;
        HRESULT hr = D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "overlay", nullptr, nullptr,
                                "VSMain", "vs_4_0", 0, 0, &vsb, &err);
        if (FAILED(hr)) {
            AION_ERROR("VS compile failed hr=0x%08lX: %s", (unsigned long)hr,
                       err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        hr = D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "overlay", nullptr, nullptr, "PSMain",
                        "ps_4_0", 0, 0, &psb, &err);
        if (FAILED(hr)) {
            AION_ERROR("PS compile failed hr=0x%08lX: %s", (unsigned long)hr,
                       err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        hr = device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
        if (FAILED(hr)) { AION_ERROR("CreateVertexShader hr=0x%08lX", (unsigned long)hr); return false; }
        hr = device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
        if (FAILED(hr)) { AION_ERROR("CreatePixelShader hr=0x%08lX", (unsigned long)hr); return false; }

        D3D11_INPUT_ELEMENT_DESC elems[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        device->CreateInputLayout(elems, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                  &input_layout);
        if (vsb) vsb->Release();
        if (psb) psb->Release();

        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = 1024 * 1024;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &vbuf);
        vbuf_capacity = (1024 * 1024) / sizeof(Vertex);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MipLODBias = 0.f;
        sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 0.f;
        sd.MinLOD = 0.f;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        HRESULT smp_hr = device->CreateSamplerState(&sd, &sampler);
        if (FAILED(smp_hr)) AION_ERROR("CreateSamplerState hr=0x%08lX", (unsigned long)smp_hr);

        D3D11_BLEND_DESC bld{};
        bld.RenderTarget[0].BlendEnable = TRUE;
        bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;          // premultiplied
        bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device->CreateBlendState(&bld, &blend);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        device->CreateRasterizerState(&rd, &raster);
        if (!(vs && ps && input_layout && vbuf && sampler && blend && raster)) {
            AION_ERROR("shader objects null: vs=%d ps=%d il=%d vb=%d smp=%d blend=%d ras=%d",
                       (int)(vs != nullptr), (int)(ps != nullptr), (int)(input_layout != nullptr),
                       (int)(vbuf != nullptr), (int)(sampler != nullptr), (int)(blend != nullptr),
                       (int)(raster != nullptr));
            return false;
        }
        return true;
    }

    // Rasterize one font at kSuper x the native size and box-filter it down to the
    // native cell size. Supersampling is what buys real antialiasing: GDI at native
    // size snaps stems to whole pixels (the "bitmap font" look), while averaging 16
    // sub-samples per pixel recovers the true outline coverage.
    bool RasterizeBlock(int cell, int em, int y_origin, unsigned char* dest, FontLayout* out) {
        const int ss = kSuper;
        const int scell = cell * ss;
        const int sw = kAtlasCols * scell, sh = kAtlasRows * scell;
        if (sw <= 0 || sh <= 0) return false;

        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = sw;
        bi.bmiHeader.biHeight = -sh;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp || !bits) { DeleteDC(dc); return false; }
        HGDIOBJ old = SelectObject(dc, bmp);
        std::memset(bits, 0, (std::size_t)sw * sh * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        const char* face = font_cfg.face.empty() ? "Segoe UI" : font_cfg.face.c_str();
        HFONT font = CreateFontA(-em * ss, 0, 0, 0, font_cfg.bold ? FW_SEMIBOLD : FW_NORMAL, FALSE,
                                 FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FF_DONTCARE, face);
        if (!font) {
            // Fall back to whatever the system picks so we never end up with no text.
            font = CreateFontA(-em * ss, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               FF_DONTCARE, nullptr);
        }
        if (!font) {
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
            return false;
        }
        HGDIOBJ oldf = SelectObject(dc, font);
        out->adv.assign(kLastChar - kFirstChar + 1, (float)em * 0.5f);
        for (int c = kFirstChar; c <= kLastChar; ++c) {
            const int idx = c - kFirstChar;
            const int gx = (idx % kAtlasCols) * scell;
            const int gy = (idx / kAtlasCols) * scell;
            RECT r{gx, gy, gx + scell, gy + scell};
            char s[2] = {(char)c, 0};
            DrawTextA(dc, s, 1, &r, DT_LEFT | DT_TOP | DT_NOPREFIX);
            // Proportional spacing: store the real advance (em-relative) per glyph.
            ABC abc{};
            if (GetCharABCWidthsA(dc, (UINT)c, (UINT)c, &abc))
                out->adv[(std::size_t)idx] = (float)(abc.abcA + abc.abcB + abc.abcC) / (float)ss;
        }
        SelectObject(dc, oldf);
        DeleteObject(font);

        // kSuper x kSuper box filter, coverage read from the blue channel.
        const unsigned char* src = (const unsigned char*)bits;
        const int dcells = cell;
        const int total = kAtlasRows * dcells;
        for (int y = 0; y < total; ++y) {
            unsigned char* drow = dest + (std::size_t)(y_origin + y) * atlas_w;
            for (int x = 0; x < kAtlasCols * dcells; ++x) {
                unsigned sum = 0;
                for (int dy = 0; dy < ss; ++dy) {
                    const unsigned char* srow = src + (std::size_t)(y * ss + dy) * sw * 4;
                    for (int dx = 0; dx < ss; ++dx) sum += srow[(std::size_t)(x * ss + dx) * 4];
                }
                drow[x] = (unsigned char)((sum + (ss * ss) / 2) / (ss * ss));
            }
        }
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        return true;
    }

    bool CreateFonts() {
        const int cell_small = (font_cfg.esp_px > 6 ? font_cfg.esp_px : 8) + kCellPad;
        const int cell_large = (font_cfg.menu_px > 6 ? font_cfg.menu_px : 8) + kCellPad;
        const int small_h = kAtlasRows * cell_small;
        const int large_h = kAtlasRows * cell_large;
        atlas_w = kAtlasCols * std::max(cell_small, cell_large);
        atlas_h = small_h + large_h;
        atlas_cover.assign((std::size_t)atlas_w * atlas_h, 0);

        struct Block { int cell; int em; int y; FontLayout* out; };
        Block blocks[2] = {{cell_small, font_cfg.esp_px, 0, &font_small},
                           {cell_large, font_cfg.menu_px, small_h, &font_large}};
        for (const Block& b : blocks) {
            if (!RasterizeBlock(b.cell, b.em, b.y, atlas_cover.data(), b.out)) return false;
            b.out->cell = b.cell;
            b.out->cols = kAtlasCols;
            b.out->rows = kAtlasRows;
            b.out->y_origin = b.y;
            b.out->valid = true;
        }

        // Coverage -> BGRA8. The shader samples .r as coverage, so R=B=G=coverage.
        std::vector<unsigned char> px((std::size_t)atlas_w * atlas_h * 4, 0);
        for (std::size_t n = 0; n < atlas_cover.size(); ++n) {
            px[n * 4 + 0] = px[n * 4 + 1] = px[n * 4 + 2] = atlas_cover[n];
            px[n * 4 + 3] = 255;
        }
        if (font_srv) { font_srv->Release(); font_srv = nullptr; }
        if (font_tex) { font_tex->Release(); font_tex = nullptr; }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = atlas_w; td.Height = atlas_h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = px.data();
        init.SysMemPitch = atlas_w * 4;
        const HRESULT hr = device->CreateTexture2D(&td, &init, &font_tex);
        if (FAILED(hr)) return false;
        if (FAILED(device->CreateShaderResourceView(font_tex, nullptr, &font_srv))) return false;
        AION_INFO("font atlas %dx%d face='%s' esp=%dpx menu=%dpx bold=%d shadow=%d",
                  atlas_w, atlas_h, font_cfg.face.c_str(), font_cfg.esp_px, font_cfg.menu_px,
                  (int)font_cfg.bold, (int)font_cfg.shadow);
        return true;
    }

    // --- dirty region of the current frame (window pixels) --------------------
    // Clamped to the target: the present walks this rectangle with a row copy, so a pass that marks
    // outside the surface (a second pass laid out against the window's client rect while the target
    // ended up a few pixels smaller) would write past the staging bitmap.
    void Mark(float x0, float y0, float x1, float y1) {
        if (x1 < x0) std::swap(x0, x1);
        if (y1 < y0) std::swap(y0, y1);
        if (width <= 0 || height <= 0) return;
        x0 = std::max(x0, 0.f);
        y0 = std::max(y0, 0.f);
        x1 = std::min(x1, (float)width);
        y1 = std::min(y1, (float)height);
        if (!(x1 > x0) || !(y1 > y0)) return;  // empty after clamping, or not a number
        if (!have_dirty) {
            dirty_x0 = x0; dirty_y0 = y0; dirty_x1 = x1; dirty_y1 = y1;
            have_dirty = true;
        } else {
            dirty_x0 = std::min(dirty_x0, x0);
            dirty_y0 = std::min(dirty_y0, y0);
            dirty_x1 = std::max(dirty_x1, x1);
            dirty_y1 = std::max(dirty_y1, y1);
        }
    }
    void ResetMark() { have_dirty = false; }
    RECT DirtyRect() const {
        RECT r{0, 0, 0, 0};
        if (!have_dirty || width <= 0 || height <= 0) return r;
        r.left = (LONG)std::floor(dirty_x0);
        r.top = (LONG)std::floor(dirty_y0);
        r.right = (LONG)std::ceil(dirty_x1) + 1;  // +1: a 1px stroke covers its own pixel
        r.bottom = (LONG)std::ceil(dirty_y1) + 1;
        r.left = std::max<LONG>(0, r.left);
        r.top = std::max<LONG>(0, r.top);
        r.right = std::min<LONG>((LONG)width, r.right);
        r.bottom = std::min<LONG>((LONG)height, r.bottom);
        if (r.right <= r.left || r.bottom <= r.top) return RECT{0, 0, 0, 0};
        return r;
    }

    // 24-bit bottom-up BMP of an 8-bit coverage buffer (font debugging).
    static bool WriteGrayBmp(const std::string& path, const unsigned char* data, int w, int h) {
        const int row = ((w * 3) + 3) & ~3;
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        unsigned char hdr[54] = {0};
        const unsigned img = (unsigned)row * (unsigned)h;
        const unsigned size = 54 + img, off = 54, ih = 40;
        const unsigned short planes = 1, bpp = 24;
        hdr[0] = 'B'; hdr[1] = 'M';
        std::memcpy(hdr + 2, &size, 4);
        std::memcpy(hdr + 10, &off, 4);
        std::memcpy(hdr + 14, &ih, 4);
        std::memcpy(hdr + 18, &w, 4);
        std::memcpy(hdr + 22, &h, 4);
        std::memcpy(hdr + 26, &planes, 2);
        std::memcpy(hdr + 28, &bpp, 2);
        std::memcpy(hdr + 34, &img, 4);
        std::fwrite(hdr, 1, 54, f);
        std::vector<unsigned char> line((std::size_t)row, 0);
        for (int y = h - 1; y >= 0; --y) {
            const unsigned char* s = data + (std::size_t)y * w;
            for (int x = 0; x < w; ++x) line[x * 3 + 0] = line[x * 3 + 1] = line[x * 3 + 2] = s[x];
            std::fwrite(line.data(), 1, (std::size_t)row, f);
        }
        std::fclose(f);
        return true;
    }

    bool SaveFontAtlasBmp(const std::string& path) {
        if (atlas_cover.empty() || atlas_w <= 0 || atlas_h <= 0) return false;
        return WriteGrayBmp(path, atlas_cover.data(), atlas_w, atlas_h);
    }

    void ReleaseTargets() {
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (offscreen) { offscreen->Release(); offscreen = nullptr; }
        if (staging) { staging->Release(); staging = nullptr; }
        if (dib) { DeleteObject(dib); dib = nullptr; dib_bits = nullptr; }
        if (mem_dc) { DeleteDC(mem_dc); mem_dc = nullptr; }
    }

    // Composition swapchain: DWM blends our surface with per-pixel alpha on the GPU.
    bool CreateSwapchain(int w, int h) {
        IDXGIDevice* dxgi_dev = nullptr;
        if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev))) return false;
        IDXGIAdapter* adapter = nullptr;
        dxgi_dev->GetAdapter(&adapter);
        IDXGIFactory2* factory = nullptr;
        if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
        bool ok = false;
        if (factory) {
            DXGI_SWAP_CHAIN_DESC1 d{};
            d.Width = (UINT)w;
            d.Height = (UINT)h;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            d.BufferCount = 2;
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
            d.Scaling = DXGI_SCALING_STRETCH;
            ok = SUCCEEDED(factory->CreateSwapChainForComposition(device, &d, nullptr, &swapchain));
        }
        if (ok) {
            // One frame in flight, maximum. The composition swapchain hands frames to DWM, and an
            // uncapped ESP produces them far faster than the compositor consumes them: with a
            // deeper queue the frame DWM shows can be several iterations old, which is felt as the
            // boxes trailing the camera. Depth one means the compositor always gets the newest
            // frame we have finished.
            IDXGIDevice1* d1 = nullptr;
            if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&d1)) && d1) {
                d1->SetMaximumFrameLatency(1);
                d1->Release();
            }
        }
        if (factory) factory->Release();
        if (adapter) adapter->Release();
        dxgi_dev->Release();
        return ok;
    }

    // Flip-model swapchains rotate their buffers, so the render-target view must be
    // re-acquired after every Present - holding a stale RTV draws into a buffer that
    // is never shown.
    bool AcquireBackbuffer() {
        if (rtv) { rtv->Release(); rtv = nullptr; }
        ID3D11Texture2D* bb = nullptr;
        if (!swapchain ||
            FAILED(swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)))
            return false;
        const HRESULT hr = device->CreateRenderTargetView(bb, nullptr, &rtv);
        bb->Release();
        return SUCCEEDED(hr);
    }

    bool EnsureSize(HWND hwnd) {
        RECT rc{};
        if (!GetClientRect(hwnd, &rc)) return false;
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return false;

        if (use_dcomp) {
            if (w == width && h == height && rtv) return true;
            if (rtv) { rtv->Release(); rtv = nullptr; }
            if (!swapchain) {
                if (!CreateSwapchain(w, h)) return false;
            } else if (FAILED(swapchain->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN,
                                                       0))) {
                return false;
            }
            if (!AcquireBackbuffer()) return false;
            width = w;
            height = h;
            return true;
        }

        if (w == width && h == height && offscreen && rtv && staging && dib) return true;
        ReleaseTargets();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &offscreen))) return false;
        if (FAILED(device->CreateRenderTargetView(offscreen, nullptr, &rtv))) return false;

        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging))) return false;

        mem_dc = CreateCompatibleDC(nullptr);
        if (!mem_dc) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
        if (!dib || !dib_bits) return false;
        SelectObject(mem_dc, dib);

        width = w;
        height = h;
        return true;
    }

    bool InitWindow(HWND h, const FontConfig& fonts) {
        hwnd = h;
        font_cfg = fonts;
        const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                          D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL fl{};
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, want, 3,
                                       D3D11_SDK_VERSION, &device, &fl, &ctx);
        if (FAILED(hr)) {
            AION_WARN("hardware D3D11 device failed (0x%08lX); trying WARP", (unsigned long)hr);
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, want, 3,
                                   D3D11_SDK_VERSION, &device, &fl, &ctx);
            if (FAILED(hr)) {
                AION_ERROR("D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr);
                return false;
            }
        }
        if (!CreateShaders()) { AION_ERROR("CreateShaders failed"); return false; }
        if (!CreateFonts()) AION_WARN("font atlas failed");

        // Prefer the composition path; fall back to a layered window when DComp is
        // unavailable (the host then has to add WS_EX_LAYERED, which is why it is
        // not set at window creation).
        IDXGIDevice* dxgi_dev = nullptr;
        if (want_dcomp &&
            SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_dev))) {
            IDCompositionDevice* cdev = nullptr;
            if (SUCCEEDED(DCompositionCreateDevice(dxgi_dev, __uuidof(IDCompositionDevice),
                                                   (void**)&cdev))) {
                dcomp_device = cdev;
                use_dcomp = true;
            }
            dxgi_dev->Release();
        }
        if (use_dcomp) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
            const bool ok =
                SUCCEEDED(dcomp_device->CreateTargetForHwnd(hwnd, TRUE, &dcomp_target)) &&
                SUCCEEDED(dcomp_device->CreateVisual(&dcomp_visual)) &&
                CreateSwapchain(cw > 0 ? cw : 1280, ch > 0 ? ch : 720) &&
                SUCCEEDED(dcomp_visual->SetContent(swapchain)) &&
                SUCCEEDED(dcomp_target->SetRoot(dcomp_visual)) &&
                SUCCEEDED(dcomp_device->Commit());
            if (!ok) {
                AION_WARN("DirectComposition setup failed; using UpdateLayeredWindow");
                ReleaseDcomp();
                use_dcomp = false;
            }
        }
        if (!use_dcomp) {
            const LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
            SetWindowLongPtrA(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        }
        if (!EnsureSize(hwnd)) { AION_ERROR("render target creation failed"); return false; }
        AION_INFO("overlay present path: %s", use_dcomp ? "DirectComposition (GPU alpha)"
                                                        : "UpdateLayeredWindow (CPU alpha)");
        return true;
    }

    void ReleaseDcomp() {
        if (dcomp_visual) { dcomp_visual->Release(); dcomp_visual = nullptr; }
        if (dcomp_target) { dcomp_target->Release(); dcomp_target = nullptr; }
        if (swapchain) { swapchain->Release(); swapchain = nullptr; }
        if (dcomp_device) { dcomp_device->Release(); dcomp_device = nullptr; }
    }
};

// ------------------------------------------------------------------ wrapper ---
D3D11Overlay::D3D11Overlay() : impl_(new D3D11OverlayImpl()) {}
D3D11Overlay::~D3D11Overlay() {
    Shutdown();
    delete impl_;
}

bool D3D11Overlay::InitWindow(void* hwnd, const FontConfig& fonts) {
    return impl_->InitWindow(reinterpret_cast<HWND>(hwnd), fonts);
}
void D3D11Overlay::SetFontConfig(const FontConfig& fonts) {
    impl_->font_cfg = fonts;
    if (impl_->device && !impl_->CreateFonts()) AION_WARN("font atlas rebuild failed");
}
const FontConfig& D3D11Overlay::font_config() const { return impl_->font_cfg; }
const OverlayStats& D3D11Overlay::stats() const { return impl_->stats; }
void D3D11Overlay::set_present_fps(int fps) { impl_->present_fps = fps < 0 ? 0 : fps; }
void D3D11Overlay::set_vsync(bool on) { impl_->vsync = on; }
void D3D11Overlay::set_use_dcomp(bool on) { impl_->want_dcomp = on; }
bool D3D11Overlay::SaveFontAtlas(const std::string& path) { return impl_->SaveFontAtlasBmp(path); }
void D3D11Overlay::Shutdown() {
    auto* i = impl_;
    if (!i) return;
    if (i->rtv) i->rtv->Release();
    if (i->font_srv) i->font_srv->Release();
    if (i->font_tex) i->font_tex->Release();
    if (i->vbuf) i->vbuf->Release();
    if (i->blend) i->blend->Release();
    if (i->raster) i->raster->Release();
    if (i->sampler) i->sampler->Release();
    if (i->input_layout) i->input_layout->Release();
    if (i->ps) i->ps->Release();
    if (i->vs) i->vs->Release();
    if (i->staging) i->staging->Release();
    if (i->offscreen) i->offscreen->Release();
    if (i->dib) DeleteObject(i->dib);
    if (i->mem_dc) DeleteDC(i->mem_dc);
    if (i->screen_dc) { ReleaseDC(nullptr, i->screen_dc); i->screen_dc = nullptr; }
    i->ReleaseDcomp();
    if (i->ctx) i->ctx->Release();
    if (i->device) i->device->Release();
    std::memset(i, 0, sizeof(*i));
}

bool D3D11Overlay::Ready() const { return impl_ && impl_->rtv && impl_->vs; }
int D3D11Overlay::Width() const { return impl_ ? impl_->width : 0; }
int D3D11Overlay::Height() const { return impl_ ? impl_->height : 0; }
bool D3D11Overlay::using_dcomp() const { return impl_ && impl_->use_dcomp; }

void* D3D11Overlay::d3d_device() const { return impl_ ? (void*)impl_->device : nullptr; }

void* D3D11Overlay::d3d_context() const { return impl_ ? (void*)impl_->ctx : nullptr; }

void D3D11Overlay::set_pre_present_hook(std::function<void()> fn) {
    if (impl_) impl_->pre_present = std::move(fn);
}

void D3D11Overlay::MarkDirty(float x0, float y0, float x1, float y1) {
    if (impl_) impl_->Mark(x0, y0, x1, y1);
}

void D3D11Overlay::BindRenderTarget() {
    auto* i = impl_;
    // AcquireBackbuffer() creates the next back buffer's view but never binds it, and Flush() binds
    // only when it has vertices, so the binding has to be made explicitly here.
    if (i && i->ctx && i->rtv) i->ctx->OMSetRenderTargets(1, &i->rtv, nullptr);
}

void D3D11Overlay::BeginFrame() {
    auto* i = impl_;
    if (i->hwnd) i->EnsureSize(i->hwnd);
    i->verts.clear();
    i->verts.reserve(1 << 16);
    i->ResetMark();
    if (i->ctx && i->rtv) {
        const float clear[4] = {0.f, 0.f, 0.f, 0.f};  // fully transparent
        i->ctx->ClearRenderTargetView(i->rtv, clear);
    }
}

namespace {
inline void Push(std::vector<Vertex>& v, float x, float y, float u, float w, float r, float g,
                 float b, float a, float edge = 0.f, float half = 0.f) {
    v.push_back({x, y, u, w, r, g, b, a, edge, half});
}
}  // namespace

void D3D11Overlay::Line(float x0, float y0, float x1, float y1, Color c, float t) {
    auto* i = impl_;
    if (!i->width || !i->height) return;
    const float W = (float)i->width, H = (float)i->height;
    auto nx = [&](float x) { return x / W * 2.f - 1.f; };
    auto ny = [&](float y) { return 1.f - y / H * 2.f; };
    // Endpoints are taken as given. Callers that need a hard edge (boxes, panel frames)
    // already work in whole pixels, and flooring here as well would make every stroke that
    // is not axis-aligned - every snapline - step along in whole-pixel jumps while it swings
    // with the camera, on top of the staircase its hard edges already had.
    const float width = t < 1.f ? 1.f : std::floor(t + 0.5f);
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) return;
    const float ux = -dy / len, uy = dx / len;          // unit normal
    const float half = width * 0.5f;
    const float centre = ((int)width & 1) ? 0.5f : 0.f;  // pull onto pixel centre
    const float ox = ux * centre, oy = uy * centre;
    // The quad runs half a pixel wider on each side than the stroke itself; the shader
    // fades that outer band, and that fade is the antialiasing.
    const float px = ux * (half + 0.5f), py = uy * (half + 0.5f);
    const float ax = x0 + ox, ay = y0 + oy, bx = x1 + ox, by = y1 + oy;
    i->Mark(std::min(ax - px, bx - px), std::min(ay - py, by - py),
            std::max(ax + px, bx + px), std::max(ay + py, by + py));
    Push(i->verts, nx(ax + px), ny(ay + py), -1, -1, c.R, c.G, c.B, c.A, 1.f, half);
    Push(i->verts, nx(ax - px), ny(ay - py), -1, -1, c.R, c.G, c.B, c.A, -1.f, half);
    Push(i->verts, nx(bx + px), ny(by + py), -1, -1, c.R, c.G, c.B, c.A, 1.f, half);
    Push(i->verts, nx(bx - px), ny(by - py), -1, -1, c.R, c.G, c.B, c.A, -1.f, half);
    Push(i->verts, nx(bx + px), ny(by + py), -1, -1, c.R, c.G, c.B, c.A, 1.f, half);
    Push(i->verts, nx(ax - px), ny(ay - py), -1, -1, c.R, c.G, c.B, c.A, -1.f, half);
}

void D3D11Overlay::Rect(float x0, float y0, float x1, float y1, Color c, float t) {
    Line(x0, y0, x1, y0, c, t);
    Line(x1, y0, x1, y1, c, t);
    Line(x1, y1, x0, y1, c, t);
    Line(x0, y1, x0, y0, c, t);
}

void D3D11Overlay::FillRect(float x0, float y0, float x1, float y1, Color c) {
    auto* i = impl_;
    const float W = (float)i->width, H = (float)i->height;
    auto nx = [&](float x) { return x / W * 2.f - 1.f; };
    auto ny = [&](float y) { return 1.f - y / H * 2.f; };
    i->Mark(x0, y0, x1, y1);
    Push(i->verts, nx(x0), ny(y0), -1, -1, c.R, c.G, c.B, c.A);
    Push(i->verts, nx(x1), ny(y0), -1, -1, c.R, c.G, c.B, c.A);
    Push(i->verts, nx(x0), ny(y1), -1, -1, c.R, c.G, c.B, c.A);
    Push(i->verts, nx(x1), ny(y0), -1, -1, c.R, c.G, c.B, c.A);
    Push(i->verts, nx(x1), ny(y1), -1, -1, c.R, c.G, c.B, c.A);
    Push(i->verts, nx(x0), ny(y1), -1, -1, c.R, c.G, c.B, c.A);
}

namespace {
// One pass of a string. Glyph origins are rounded to whole pixels (so every quad
// covers whole atlas texels and stays crisp) while advances stay fractional, which
// keeps proportional spacing accurate.
void DrawTextPass(D3D11OverlayImpl* i, float x, float y, const std::string& s, Color c,
                  float scale, const FontLayout& fl) {
    if (i->width <= 0 || i->height <= 0 || !fl.valid || fl.cell <= 0) return;
    const float W = (float)i->width, H = (float)i->height;
    const float cw = fl.cell * scale, ch = fl.cell * scale;
    const float aw = (float)i->atlas_w, ah = (float)i->atlas_h;
    // Quad spans exactly one cell in texels, so with LINEAR sampling the pixel
    // centres land on texel centres - no bleed from the neighbouring glyph.
    const float du = fl.cell / aw, dv = fl.cell / ah;
    auto nx = [&](float px) { return px / W * 2.f - 1.f; };
    auto ny = [&](float py) { return 1.f - py / H * 2.f; };
    x = std::floor(x);
    y = std::floor(y);
    const float y1 = y + ch;
    for (char ch_ : s) {
        const int code = (unsigned char)ch_;
        if (code < kFirstChar || code > kLastChar) {
            x += std::floor(cw * 0.5f + 0.5f);
            continue;
        }
        const int idx = code - kFirstChar;
        const float u0 = (idx % fl.cols) * fl.cell / aw;
        const float v0 = (fl.y_origin + (idx / fl.cols) * fl.cell) / ah;
        const float u1 = u0 + du, v1 = v0 + dv;
        const float gx = std::floor(x + 0.5f);
        i->Mark(gx, y, gx + cw, y1);
        Push(i->verts, nx(gx), ny(y), u0, v0, c.R, c.G, c.B, c.A);
        Push(i->verts, nx(gx + cw), ny(y), u1, v0, c.R, c.G, c.B, c.A);
        Push(i->verts, nx(gx), ny(y1), u0, v1, c.R, c.G, c.B, c.A);
        Push(i->verts, nx(gx + cw), ny(y), u1, v0, c.R, c.G, c.B, c.A);
        Push(i->verts, nx(gx + cw), ny(y1), u1, v1, c.R, c.G, c.B, c.A);
        Push(i->verts, nx(gx), ny(y1), u0, v1, c.R, c.G, c.B, c.A);
        const float a = fl.adv.empty() ? cw * 0.52f : fl.adv[(std::size_t)idx] * scale;
        x += a > 0.f ? a : cw * 0.52f;
    }
}
}  // namespace

void D3D11Overlay::TextSized(float x, float y, const std::string& s, Color c, float scale,
                             FontSize fs) {
    auto* i = impl_;
    if (!i->font_srv || s.empty()) return;
    if (scale <= 0.f) scale = 1.f;
    const FontLayout& fl =
        (fs == FontSize::Small && i->font_small.valid) ? i->font_small : i->font_large;
    if (!fl.valid) return;
    // 1px black drop shadow first, so labels stay readable over bright scenes.
    if (i->font_cfg.shadow && c.A > 0.02f) {
        Color sh = c;
        sh.R = sh.G = sh.B = 0.f;
        sh.A = c.A * 0.7f;
        DrawTextPass(i, x + 1.f, y + 1.f, s, sh, scale, fl);
    }
    DrawTextPass(i, x, y, s, c, scale, fl);
}

float D3D11Overlay::TextHeight(FontSize fs) const {
    const FontLayout& fl =
        (fs == FontSize::Small && impl_->font_small.valid) ? impl_->font_small
                                                            : impl_->font_large;
    return (float)fl.cell;
}

float D3D11Overlay::TextWidth(const std::string& s, float scale, FontSize fs) const {
    if (s.empty()) return 0.f;
    if (scale <= 0.f) scale = 1.f;
    const FontLayout& fl =
        (fs == FontSize::Small && impl_->font_small.valid) ? impl_->font_small
                                                           : impl_->font_large;
    if (!fl.valid || fl.cell <= 0) return 0.f;
    const float cw = (float)fl.cell * scale;
    // The same accumulator DrawTextPass uses, except the last glyph counts as its cell instead of
    // its advance: the quads do cover that cell, and that - not the advance sum - is the ink a
    // centred label has to be offset by.
    float w = 0.f, last = 0.f;
    for (char c : s) {
        const int code = (unsigned char)c;
        float a;
        if (code < kFirstChar || code > kLastChar) {
            a = std::floor(cw * 0.5f + 0.5f);
        } else {
            const float adv = fl.adv.empty() ? cw * 0.52f
                                             : fl.adv[(std::size_t)(code - kFirstChar)] * scale;
            a = adv > 0.f ? adv : cw * 0.52f;
        }
        w += a;
        last = a;
    }
    return w - last + cw;
}

void D3D11Overlay::Text(float x, float y, const std::string& s, Color c, float scale) {
    TextSized(x, y, s, c, scale, FontSize::Large);
}

void D3D11Overlay::Flush() {
    auto* i = impl_;
    i->stats.verts = (int)i->verts.size();
    if (!i->ctx || !i->rtv || !i->vs || i->verts.empty()) {
        i->verts.clear();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if ((UINT)i->verts.size() > i->vbuf_capacity) {
        i->vbuf_capacity = (UINT)i->verts.size() * 2;
        if (i->vbuf) i->vbuf->Release();
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = i->vbuf_capacity * sizeof(Vertex);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(i->device->CreateBuffer(&bd, nullptr, &i->vbuf))) return;
    }
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(i->ctx->Map(i->vbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    std::memcpy(ms.pData, i->verts.data(), i->verts.size() * sizeof(Vertex));
    i->ctx->Unmap(i->vbuf, 0);

    UINT stride = sizeof(Vertex), offset = 0;
    i->ctx->OMSetRenderTargets(1, &i->rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)i->width, (float)i->height, 0, 1};
    i->ctx->RSSetViewports(1, &vp);
    i->ctx->IASetInputLayout(i->input_layout);
    i->ctx->IASetVertexBuffers(0, 1, &i->vbuf, &stride, &offset);
    i->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    i->ctx->VSSetShader(i->vs, nullptr, 0);
    i->ctx->PSSetShader(i->ps, nullptr, 0);
    i->ctx->PSSetShaderResources(0, 1, &i->font_srv);
    i->ctx->PSSetSamplers(0, 1, &i->sampler);
    float blend_factor[4] = {0, 0, 0, 0};
    i->ctx->OMSetBlendState(i->blend, blend_factor, 0xffffffff);
    i->ctx->RSSetState(i->raster);
    i->ctx->Draw((UINT)i->verts.size(), 0);
    i->verts.clear();
    i->stats.flush_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - t0).count();
}

void D3D11Overlay::Present() {
    auto* i = impl_;
    i->stats.presented = false;

    // Fast path: hand the rendered surface straight to DWM. No readback, no copy,
    // no UpdateLayeredWindow - Present(1,0) also paces us to the refresh rate.
    if (i->use_dcomp) {
        if (!i->swapchain) return;
        const auto t0 = std::chrono::steady_clock::now();
        const HRESULT hr = i->swapchain->Present(i->vsync ? 1 : 0, 0);
        // Present() here is a wait for the compositor to take the previous frame, i.e. the display's
        // cadence. It is reported as `wait_ms`, not as hand-off cost: a paced frame is not this
        // program being slow, and counting it as such made the debug HUD read over budget on every
        // frame that was merely running at the panel's rate.
        i->stats.wait_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
        i->stats.present_ms = 0.0;
        i->stats.presented = SUCCEEDED(hr);
        i->AcquireBackbuffer();
        i->stats.dirty_w = i->width;
        i->stats.dirty_h = i->height;
        return;
    }
    i->stats.wait_ms = 0.0;

    if (!i->ctx || !i->offscreen || !i->staging || !i->hwnd || !i->mem_dc || !i->dib_bits)
        return;

    // Rate cap. The GPU readback and the DWM hand-off are the expensive parts and
    // the picture does not change while we skip, so throttling is free visually.
    const auto now = std::chrono::steady_clock::now();
    if (i->present_fps > 0 && i->last_present.time_since_epoch().count() != 0) {
        const double since = std::chrono::duration<double, std::milli>(now - i->last_present)
                                 .count();
        if (since < 1000.0 / i->present_fps) return;
    }
    i->last_present = now;

    // Repaint region = this frame's content UNION the last presented frame's: the
    // area the previous frame drew must receive the now-cleared (transparent) pixels.
    RECT un = i->DirtyRect();
    if (i->have_last_dirty) {
        if (un.right <= un.left || un.bottom <= un.top) {
            un = i->last_dirty;
        } else {
            un.left = std::min(un.left, i->last_dirty.left);
            un.top = std::min(un.top, i->last_dirty.top);
            un.right = std::max(un.right, i->last_dirty.right);
            un.bottom = std::max(un.bottom, i->last_dirty.bottom);
        }
    }
    if (un.right <= un.left || un.bottom <= un.top) return;  // nothing on screen

    const auto t0 = std::chrono::steady_clock::now();
    const int w = i->width, h = i->height;
    // Copy only the changed box on the GPU (the full-frame copy was the biggest
    // single cost in the old path).
    const D3D11_BOX box{(UINT)un.left, (UINT)un.top, 0, (UINT)un.right, (UINT)un.bottom, 1};
    i->ctx->CopySubresourceRegion(i->staging, 0, box.left, box.top, 0, i->offscreen, 0, &box);

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(i->ctx->Map(i->staging, 0, D3D11_MAP_READ, 0, &ms))) return;
    unsigned char* dst = static_cast<unsigned char*>(i->dib_bits);
    const unsigned char* src = static_cast<const unsigned char*>(ms.pData);
    const int row_bytes = (un.right - un.left) * 4;
    for (int y = un.top; y < un.bottom; ++y) {
        std::memcpy(dst + (std::size_t)y * w * 4 + (std::size_t)un.left * 4,
                    src + (std::size_t)y * ms.RowPitch + (std::size_t)un.left * 4,
                    (std::size_t)row_bytes);
    }
    i->ctx->Unmap(i->staging, 0);

    if (!i->screen_dc) i->screen_dc = GetDC(nullptr);
    POINT srcpt{0, 0};
    SIZE size{w, h};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    // The plain UpdateLayeredWindow has no dirty-rect parameter; the Indirect form
    // does, which is what lets DWM skip recompositing the whole 2560x1440 surface.
    UPDATELAYEREDWINDOWINFO info{};
    info.cbSize = sizeof(info);
    info.hdcDst = i->screen_dc;
    info.pptDst = nullptr;
    info.psize = &size;
    info.hdcSrc = i->mem_dc;
    info.pptSrc = &srcpt;
    info.crKey = 0;
    info.pblend = &bf;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &un;
    if (!UpdateLayeredWindowIndirect(i->hwnd, &info)) {
        const DWORD err = GetLastError();
        if (!i->warned_ulw) {
            i->warned_ulw = true;
            AION_WARN("UpdateLayeredWindowIndirect failed err=%lu; falling back to full update",
                      err);
        }
        UpdateLayeredWindow(i->hwnd, i->screen_dc, nullptr, &size, i->mem_dc, &srcpt, 0, &bf,
                            ULW_ALPHA);
    }
    i->last_dirty = un;
    i->have_last_dirty = true;
    i->stats.presented = true;
    i->stats.dirty_w = un.right - un.left;
    i->stats.dirty_h = un.bottom - un.top;
    i->stats.present_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
}

void D3D11Overlay::EndFrame() {
    Flush();
    // The second pass (the menu) draws into the surface just flushed, so both are presented
    // together - one hand-off, not two.
    if (impl_ && impl_->pre_present) impl_->pre_present();
    Present();
}

bool D3D11Overlay::SaveBackbufferToBmp(const std::string& path) {
    auto* i = impl_;
    if (!i->device || !i->ctx) return false;
    // Capture the surface that is actually on screen: the composition back buffer,
    // or the offscreen target on the UpdateLayeredWindow fallback path.
    ID3D11Texture2D* src = nullptr;
    if (i->use_dcomp) {
        if (!i->swapchain ||
            FAILED(i->swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&src)))
            return false;
    } else {
        if (!i->offscreen) return false;
        src = i->offscreen;
        src->AddRef();
    }
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(i->device->CreateTexture2D(&sd, nullptr, &staging))) {
        src->Release();
        return false;
    }
    i->ctx->CopyResource(staging, src);
    src->Release();
    D3D11_MAPPED_SUBRESOURCE ms{};
    bool ok = false;
    if (SUCCEEDED(i->ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
        const int w = d.Width, h = d.Height;
        const int row = ((w * 3) + 3) & ~3;
        const int size = row * h;
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f) {
            unsigned char fh[14] = {'B', 'M'};
            int total = 14 + 40 + size;
            std::memcpy(fh + 2, &total, 4);
            int off = 54;
            std::memcpy(fh + 10, &off, 4);
            std::fwrite(fh, 1, 14, f);
            unsigned char ih[40] = {};
            const unsigned int dib_size = 40;
            std::memcpy(ih + 0, &dib_size, 4);   // biSize
            std::memcpy(ih + 4, &w, 4);          // biWidth
            std::memcpy(ih + 8, &h, 4);          // biHeight
            ih[12] = 1;                          // biPlanes
            ih[14] = 24;                         // biBitCount
            std::memcpy(ih + 20, &size, 4);      // biSizeImage
            std::fwrite(ih, 1, 40, f);
            std::vector<unsigned char> linebuf(row, 0);
            for (int y = h - 1; y >= 0; --y) {
                const unsigned char* src = (const unsigned char*)ms.pData + (size_t)y * ms.RowPitch;
                for (int x = 0; x < w; ++x) {
                    linebuf[x * 3 + 0] = src[x * 4 + 0];
                    linebuf[x * 3 + 1] = src[x * 4 + 1];
                    linebuf[x * 3 + 2] = src[x * 4 + 2];
                }
                std::fwrite(linebuf.data(), 1, row, f);
            }
            std::fclose(f);
            ok = true;
        }
        i->ctx->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
}

}  // namespace aion
