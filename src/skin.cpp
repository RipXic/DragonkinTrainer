// skin.cpp - painted panel textures (2026-11-10).
// The panel skin is a folder of PNGs (<root>\skin\*.png + skin.json). They are drawn through the
// existing icon ABI - trinity::ui::DrawItemIcon(dl, sprite, pMin, pMax, tint) - so the hot-reload host table is unchanged:
//   "row"                    whole image stretched into the rect
//   "row#3:24,24"            3-slice: left/right caps (source px) keep their aspect, the middle stretches
//   "frame#9:345,160,60,522@0.8"  9-slice: corners scaled by the @ factor (default 1), edges + centre stretch
// PNGs are decoded with WIC, uploaded to a default-heap RGBA8 texture (SRV in the overlay heap from IconsInit's first
// slot on), and the copy is recorded on the overlay command list by IconsRecordUploads before the ImGui draws.
// Loading is lazy (first draw) and render-thread only. A reloaded client copy forwards to the host's functions.
#include "trinity/gui/icons.h"
#include "common.h"
#include "host_api.h"

#include <Windows.h>
#include <d3d12.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace trinity::ui {
namespace {
struct Tex { ID3D12Resource* tex = nullptr; ImTextureID id = 0; int w = 0, h = 0; bool ok = false; FILETIME mt{}; ULONGLONG checked = 0; };
struct Pending { ID3D12Resource* tex; ID3D12Resource* upload; D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; };

ID3D12Device* g_dev = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr;
unsigned g_inc = 0, g_next = 0, g_count = 0;
std::vector<Pending> g_pending;
std::vector<ID3D12Resource*> g_keep;
std::unordered_map<std::string, Tex> g_tex;
IWICImagingFactory* g_wic = nullptr;

bool decode_png(const std::wstring& path, std::vector<uint8_t>& px, UINT& w, UINT& h) {
    if (!g_wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE is fine: WIC works in either apartment
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) return false;
    }
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(g_wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192) {
        px.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    return ok;
}

bool upload(UINT w, UINT h, const uint8_t* px, Tex& out) {
    if (!g_dev || g_next >= g_count) return false;
    D3D12_HEAP_PROPERTIES def{}; def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = w; td.Height = h; td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* tex = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows = 0; UINT64 rowBytes = 0, size = 0;
    g_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &size);
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* ub = nullptr;
    if (FAILED(g_dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ub)))) { tex->Release(); return false; }
    uint8_t* m = nullptr; D3D12_RANGE none{ 0, 0 };
    if (FAILED(ub->Map(0, &none, (void**)&m))) { ub->Release(); tex->Release(); return false; }
    for (UINT r = 0; r < rows; ++r) memcpy(m + fp.Offset + (size_t)r * fp.Footprint.RowPitch, px + (size_t)r * w * 4, (size_t)w * 4);
    ub->Unmap(0, nullptr);
    g_pending.push_back({ tex, ub, fp });
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE c = g_heap->GetCPUDescriptorHandleForHeapStart(); c.ptr += (SIZE_T)g_next * g_inc;
    g_dev->CreateShaderResourceView(tex, &sd, c);
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_heap->GetGPUDescriptorHandleForHeapStart(); gh.ptr += (UINT64)g_next * g_inc;
    ++g_next;
    out.tex = tex; out.id = (ImTextureID)gh.ptr; out.w = (int)w; out.h = (int)h; out.ok = true;
    return true;
}

std::vector<ID3D12Resource*> g_old;     // replaced textures (the GPU may still read them this frame): freed at shutdown
FILETIME mtime_of(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{}; if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return a.ftLastWriteTime; return FILETIME{};
}
const Tex* get(const std::string& name) {
    if (name.find_first_of("\\/:.") != std::string::npos) return nullptr;
    std::string p = wb::g_cfg.root + "\\skin\\" + name + ".png";
    std::wstring wp(p.begin(), p.end());
    const ULONGLONG now = GetTickCount64();
    auto it = g_tex.find(name);
    if (it != g_tex.end()) {
        Tex& t = it->second;
        if (now - t.checked < 1500) return t.ok ? &t : nullptr;
        t.checked = now;                       // re-cut art (tools\make_skin.py) is picked up without a restart
        const FILETIME m = mtime_of(wp);
        if (CompareFileTime(&m, &t.mt) == 0) return t.ok ? &t : nullptr;
        if (t.tex) g_old.push_back(t.tex);
        t = Tex{};
    }
    Tex& t = g_tex[name];   // a miss is cached too (re-checked by file time)
    t.checked = now; t.mt = mtime_of(wp);
    std::vector<uint8_t> px; UINT w = 0, h = 0;
    if (decode_png(wp, px, w, h) && upload(w, h, px.data(), t)) return &t;
    wb::log("[skin] could not load %s", p.c_str());
    return nullptr;
}

void quad(ImDrawList* dl, const Tex& t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, ImU32 tint) {
    if (x1 - x0 < 0.5f || y1 - y0 < 0.5f) return;
    dl->AddImage(t.id, ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(u0 / t.w, v0 / t.h), ImVec2(u1 / t.w, v1 / t.h), tint);
}
} // namespace

void IconsInit(ID3D12Device* device, ID3D12DescriptorHeap* heap, unsigned inc, unsigned firstSlot, unsigned slotCount) {
    g_dev = device; g_heap = heap; g_inc = inc; g_next = firstSlot; g_count = slotCount;
}
void IconsRecordUploads(ID3D12GraphicsCommandList* list) {
    if (!list || g_pending.empty()) return;
    for (const Pending& p : g_pending) {
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = p.tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = p.upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = p.fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = p.tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        g_keep.push_back(p.upload);
    }
    g_pending.clear();
}
void IconsShutdown() {
    for (auto& [n, t] : g_tex) if (t.tex) t.tex->Release();
    g_tex.clear();
    for (auto* o : g_old) if (o) o->Release();
    g_old.clear();
    for (auto* u : g_keep) if (u) u->Release();
    g_keep.clear();
    for (auto& p : g_pending) { if (p.upload) p.upload->Release(); if (p.tex) p.tex->Release(); }
    g_pending.clear();
    if (g_wic) { g_wic->Release(); g_wic = nullptr; }
    g_dev = nullptr; g_heap = nullptr;
}
bool IconsReady() { if (auto h = ::wb::host_icons()) return h->ready(); return g_dev != nullptr; }
ImVec2 IconSize(Icon, float height) { return ImVec2(height, height); }
void DrawIcon(ImDrawList*, Icon, ImVec2, ImVec2, ImU32) {}
float DrawIconH(ImDrawList*, Icon, float, float, float h, ImU32) { return h; }

bool DrawItemIcon(ImDrawList* dl, const char* sprite, ImVec2 a, ImVec2 b, ImU32 tint) {
    if (auto h = ::wb::host_icons()) return h->item(dl, sprite, a, b, tint);
    if (!dl || !sprite || !*sprite || !g_dev) return false;
    std::string s = sprite, name = s, spec;
    const size_t hash = s.find('#');
    if (hash != std::string::npos) { name = s.substr(0, hash); spec = s.substr(hash + 1); }
    const Tex* t = get(name);
    if (!t) return false;
    if (spec.empty()) { quad(dl, *t, a.x, a.y, b.x, b.y, 0, 0, (float)t->w, (float)t->h, tint); return true; }
    float scale = -1.f;
    const size_t at = spec.find('@');
    if (at != std::string::npos) { scale = (float)atof(spec.c_str() + at + 1); spec.resize(at); }
    const char kind = spec[0];
    float v[4] = { 0, 0, 0, 0 }; int n = 0;
    { const char* c = strchr(spec.c_str(), ':'); while (c && n < 4) { v[n++] = (float)atof(c + 1); c = strchr(c + 1, ','); } }
    const float W = (float)t->w, H = (float)t->h;
    if (kind == '3') {   // left cap, middle, right cap - caps keep their aspect at the rect's height
        const float k = scale > 0 ? scale : (b.y - a.y) / H;
        float l = v[0] * k, r = v[1] * k;
        const float room = b.x - a.x;
        if (l + r > room) { const float f = room / (l + r); l *= f; r *= f; }
        quad(dl, *t, a.x, a.y, a.x + l, b.y, 0, 0, v[0], H, tint);
        quad(dl, *t, a.x + l, a.y, b.x - r, b.y, v[0], 0, W - v[1], H, tint);
        quad(dl, *t, b.x - r, a.y, b.x, b.y, W - v[1], 0, W, H, tint);
        return true;
    }
    if (kind == '9') {
        const float k = scale > 0 ? scale : 1.f;
        float L = v[0] * k, T = v[1] * k, R = v[2] * k, B = v[3] * k;
        if (L + R > b.x - a.x) { const float f = (b.x - a.x) / (L + R); L *= f; R *= f; }
        if (T + B > b.y - a.y) { const float f = (b.y - a.y) / (T + B); T *= f; B *= f; }
        const float xs[4] = { a.x, a.x + L, b.x - R, b.x }, ys[4] = { a.y, a.y + T, b.y - B, b.y };
        const float us[4] = { 0, v[0], W - v[2], W }, vs[4] = { 0, v[1], H - v[3], H };
        for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i)
            quad(dl, *t, xs[i], ys[j], xs[i + 1], ys[j + 1], us[i], vs[j], us[i + 1], vs[j + 1], tint);
        return true;
    }
    quad(dl, *t, a.x, a.y, b.x, b.y, 0, 0, W, H, tint);
    return true;
}
} // namespace trinity::ui
