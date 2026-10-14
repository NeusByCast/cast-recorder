// Screenshots: GDI capture of a monitor, the whole virtual desktop, or one window (PrintWindow with
// PW_RENDERFULLCONTENT so DirectX/Chromium content is included), encoded with WIC as PNG or JPEG.
// Windows that set WDA_EXCLUDEFROMCAPTURE (our toasts) are left out of GDI screen captures by DWM.
#include "recorder.h"
#include <wincodec.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace cr {

namespace {
bool encodeWic(const std::wstring& path, const void* bgra, int w, int h, bool jpg, std::wstring& err) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    [&] {
        ComPtr<IWICImagingFactory> f;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        if (FAILED(hr)) { err = L"WIC unavailable"; return; }
        ComPtr<IWICStream> st;
        if (FAILED(hr = f->CreateStream(&st)) || FAILED(hr = st->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) { err = L"can't create the file"; return; }
        ComPtr<IWICBitmapEncoder> enc;
        if (FAILED(hr = f->CreateEncoder(jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc))) { err = L"no encoder"; return; }
        enc->Initialize(st.Get(), WICBitmapEncoderNoCache);
        ComPtr<IWICBitmapFrameEncode> fr; ComPtr<IPropertyBag2> props;
        if (FAILED(hr = enc->CreateNewFrame(&fr, &props))) { err = L"CreateNewFrame failed"; return; }
        if (jpg && props) {
            PROPBAG2 opt{}; opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT v; VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.92f;
            props->Write(1, &opt, &v);
        }
        fr->Initialize(props.Get());
        fr->SetSize((UINT)w, (UINT)h);
        WICPixelFormatGUID fmt = jpg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
        fr->SetPixelFormat(&fmt);
        if (jpg || fmt != GUID_WICPixelFormat32bppBGRA) {
            // convert BGRA -> what the encoder accepted
            ComPtr<IWICBitmap> bmp;
            if (FAILED(hr = f->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGRA, (UINT)w * 4, (UINT)w * h * 4, (BYTE*)bgra, &bmp))) { err = L"bitmap failed"; return; }
            ComPtr<IWICFormatConverter> cv;
            f->CreateFormatConverter(&cv);
            if (FAILED(hr = cv->Initialize(bmp.Get(), fmt, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) { err = L"convert failed"; return; }
            hr = fr->WriteSource(cv.Get(), nullptr);
        } else {
            hr = fr->WritePixels((UINT)h, (UINT)w * 4, (UINT)w * h * 4, (BYTE*)bgra);
        }
        if (FAILED(hr) || FAILED(hr = fr->Commit()) || FAILED(hr = enc->Commit())) { err = L"encode failed"; return; }
        ok = true;
    }();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}
}  // namespace

bool saveScreenshot(HWND window, HMONITOR monitor, bool allDisplays, const std::wstring& path, bool jpg, std::wstring& err, int* outW, int* outH) {
    RECT r{};
    if (window) {
        if (!IsWindow(window)) { err = L"window closed"; return false; }
        if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) GetWindowRect(window, &r);
    } else if (allDisplays) {
        r = RECT{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
        r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN); r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else {
        MONITORINFO mi{sizeof mi};
        HMONITOR m = monitor ? monitor : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (!GetMonitorInfoW(m, &mi)) { err = L"monitor gone"; return false; }
        r = mi.rcMonitor;
    }
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) { err = L"nothing to capture"; return false; }
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { ReleaseDC(nullptr, screen); err = L"out of memory"; return false; }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    bool got = false;
    if (window) {
        RECT wr{}; GetWindowRect(window, &wr);  // PrintWindow draws the whole window rect; crop to the visible frame
        const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        HDC m2 = CreateCompatibleDC(screen);
        HBITMAP b2 = CreateCompatibleBitmap(screen, ww, wh);
        HGDIOBJ o2 = SelectObject(m2, b2);
        got = PrintWindow(window, m2, 2 /* PW_RENDERFULLCONTENT */) != 0;
        if (got) BitBlt(mem, 0, 0, w, h, m2, r.left - wr.left, r.top - wr.top, SRCCOPY);
        SelectObject(m2, o2); DeleteObject(b2); DeleteDC(m2);
    }
    if (!got) got = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != 0;
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    bool ok = false;
    if (got) {
        uint8_t* p = (uint8_t*)bits;
        for (size_t i = 0; i < (size_t)w * h; ++i) p[i * 4 + 3] = 255;  // GDI leaves alpha undefined
        ok = encodeWic(path, bits, w, h, jpg, err);
    } else err = L"screen capture failed";
    DeleteObject(dib);
    if (ok) { if (outW) *outW = w; if (outH) *outH = h; }
    return ok;
}

}  // namespace cr
