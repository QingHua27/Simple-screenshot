#include "output.h"
#include <shlobj.h>
#include <objidl.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

std::wstring default_save_dir() {
    std::wstring dir;
    PWSTR pics = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) && pics) {
        dir = pics;
        CoTaskMemFree(pics);
    }
    if (dir.empty()) {
        wchar_t buf[MAX_PATH] = {0};
        GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        dir = std::wstring(buf) + L"\\Pictures";
    }
    dir += L"\\简截";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// ---------------------------------------------------------------- 剪贴板
static HGLOBAL bitmap_to_dib(Gdiplus::Bitmap* bmp, BITMAPINFOHEADER& outHdr) {
    int w = (int)bmp->GetWidth();
    int h = (int)bmp->GetHeight();

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;      // 正数：自下而上，符合 CF_DIB 约定
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = (DWORD)w * h * 4;

    HDC dc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(dc);
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        DeleteDC(mem);
        ReleaseDC(nullptr, dc);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(mem, hbm);

    Gdiplus::Graphics g(mem);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.DrawImage(bmp, 0, 0, w, h);
    g.Flush();

    size_t bytes = (size_t)w * h * 4;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (hMem) {
        void* dst = GlobalLock(hMem);
        outHdr = bi.bmiHeader;
        memcpy(dst, &bi.bmiHeader, sizeof(BITMAPINFOHEADER));
        memcpy((BYTE*)dst + sizeof(BITMAPINFOHEADER), bits, bytes);
        GlobalUnlock(hMem);
    }

    SelectObject(mem, old);
    DeleteObject(hbm);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    return hMem;
}

static bool png_encoder_clsid(CLSID& clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            clsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

static HGLOBAL bitmap_to_png(Gdiplus::Bitmap* bmp, DWORD& size) {
    CLSID clsid;
    if (!png_encoder_clsid(clsid)) return nullptr;
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, FALSE, &stream) != S_OK) return nullptr;
    if (bmp->Save(stream, &clsid, nullptr) != Gdiplus::Ok) {
        stream->Release();
        return nullptr;
    }
    HGLOBAL h = nullptr;
    GetHGlobalFromStream(stream, &h);
    size = (DWORD)GlobalSize(h);
    stream->Release();
    return h;
}

bool copy_bitmap_to_clipboard(Gdiplus::Bitmap* bmp) {
    if (!bmp) return false;
    BITMAPINFOHEADER hdr = {};
    HGLOBAL dib = bitmap_to_dib(bmp, hdr);
    if (!dib) return false;

    if (!OpenClipboard(nullptr)) {
        GlobalFree(dib);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_DIB, dib);

    DWORD pngSize = 0;
    HGLOBAL png = bitmap_to_png(bmp, pngSize);
    if (png) {
        UINT fmt = RegisterClipboardFormatW(L"PNG");
        if (fmt) SetClipboardData(fmt, png);
    }
    CloseClipboard();
    return true;
}

// ---------------------------------------------------------------- 保存
static bool encode_to_file(Gdiplus::Bitmap* bmp, const std::wstring& path) {
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = path.substr(dot + 1);
    for (auto& c : ext) c = towlower(c);

    CLSID clsid;
    if (ext == L"jpg" || ext == L"jpeg") {
        UINT num = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        std::vector<BYTE> buf(size);
        auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
        Gdiplus::GetImageEncoders(num, size, codecs);
        bool found = false;
        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0) {
                clsid = codecs[i].Clsid;
                found = true;
                break;
            }
        }
        if (!found) return false;

        // 白底，避免透明区域变黑
        Gdiplus::Bitmap flat((INT)bmp->GetWidth(), (INT)bmp->GetHeight(), PixelFormat24bppRGB);
        {
            Gdiplus::Graphics g(&flat);
            Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
            g.FillRectangle(&white, 0, 0, (INT)flat.GetWidth(), (INT)flat.GetHeight());
            g.DrawImage(bmp, 0, 0, (INT)flat.GetWidth(), (INT)flat.GetHeight());
        }
        ULONG quality = 92;
        Gdiplus::EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &quality;
        return flat.Save(path.c_str(), &clsid, &params) == Gdiplus::Ok;
    }

    if (!png_encoder_clsid(clsid)) return false;
    return bmp->Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

static std::wstring timestamp_name() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[128];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"简截_%04d%02d%02d_%02d%02d%02d.png",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool save_bitmap_dialog(HWND owner, Gdiplus::Bitmap* bmp, const std::wstring& preferDir) {
    if (!bmp) return false;
    std::wstring dir = preferDir.empty() ? default_save_dir() : preferDir;
    std::wstring full = dir + L"\\" + timestamp_name();

    wchar_t file[MAX_PATH * 2] = {0};
    wcsncpy_s(file, full.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0JPEG 图片 (*.jpg)\0*.jpg\0所有文件\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = L"保存截图";
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetSaveFileNameW(&ofn)) {
        DWORD e = CommDlgExtendedError();
        logf(L"保存对话框取消/失败 err=%lu", (unsigned long)e);
        return false;
    }
    bool ok = encode_to_file(bmp, file);
    logf(L"写盘 %ls -> %d", file, (int)ok);
    return ok;
}
