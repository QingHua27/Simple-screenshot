#include "capture.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

// 截图时忽略的窗口类（桌面壳层等）
static const wchar_t* SKIP_CLASSES[] = {
    L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
    L"Windows.UI.Core.CoreWindow", L"ApplicationManager_DesktopShellWindow",
    L"MultitaskingViewFrame", L"ForegroundStaging", L"TaskListThumbnailWnd",
    L"Windows.Internal.Shell.TabProxyWindow", L"MsgrIMEWindowClass",
    L"SysShadow", L"ConsoleWindowClass", L"JianJieOverlay", L"JianJiePin",
    L"JianJieSettings", L"JianJieTip",
};

static bool class_is_skipped(const wchar_t* cls) {
    for (auto* c : SKIP_CLASSES) {
        if (wcscmp(cls, c) == 0) return true;
    }
    return false;
}

bool capture_desktop(Shot& s) {
    free_shot(s);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = vw;
    bi.bmiHeader.biHeight = -vh;     // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    BOOL ok = BitBlt(hdcMem, 0, 0, vw, vh, hdcScreen, vx, vy, SRCCOPY | CAPTUREBLT);
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    if (!ok) {
        DeleteObject(hbm);
        return false;
    }

    // 压暗副本：等价于盖一层 alpha=122 的黑色
    void* dimBits = nullptr;
    HBITMAP hbmDim = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &dimBits, nullptr, 0);
    if (!hbmDim) {
        DeleteObject(hbm);
        return false;
    }
    {
        const BYTE* src = (const BYTE*)bits;
        BYTE* dst = (BYTE*)dimBits;
        size_t n = (size_t)vw * vh;
        for (size_t i = 0; i < n; ++i) {
            // px * (1 - 122/255)
            dst[i * 4 + 0] = (BYTE)(src[i * 4 + 0] * 133 / 255);
            dst[i * 4 + 1] = (BYTE)(src[i * 4 + 1] * 133 / 255);
            dst[i * 4 + 2] = (BYTE)(src[i * 4 + 2] * 133 / 255);
            dst[i * 4 + 3] = 255;
        }
    }

    s.vx = vx; s.vy = vy; s.vw = vw; s.vh = vh;
    s.hbm = hbm; s.hbmDim = hbmDim; s.bits = bits;
    // 直接包裹 DIB 内存，零拷贝
    s.bmp = new Gdiplus::Bitmap(vw, vh, vw * 4, PixelFormat32bppRGB, (BYTE*)bits);
    s.bmpDim = new Gdiplus::Bitmap(vw, vh, vw * 4, PixelFormat32bppRGB, (BYTE*)dimBits);
    return true;
}

void free_shot(Shot& s) {
    delete s.bmp;      s.bmp = nullptr;
    delete s.bmpDim;   s.bmpDim = nullptr;
    if (s.hbm) { DeleteObject(s.hbm); s.hbm = nullptr; }
    if (s.hbmDim) { DeleteObject(s.hbmDim); s.hbmDim = nullptr; }
    s.bits = nullptr;
    s.vw = s.vh = 0;
}

// ------------------------------------------------------------------ 窗口探测
struct WinHit {
    HWND hwnd;
    RECT rect;
    long long area;
};

static BOOL CALLBACK enum_proc(HWND h, LPARAM lp) {
    auto* best = (WinHit*)lp;

    if (!IsWindowVisible(h)) return TRUE;
    if (IsIconic(h)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 256);
    if (class_is_skipped(cls)) return TRUE;

    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    if (ex & WS_EX_NOACTIVATE) return TRUE;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return TRUE;
    }

    RECT r = {};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) {
        if (!GetWindowRect(h, &r)) return TRUE;
    }
    int w = r.right - r.left;
    int hh = r.bottom - r.top;
    if (w < 8 || hh < 8) return TRUE;

    long long area = (long long)w * hh;
    if (area < best->area || best->hwnd == nullptr) {
        best->hwnd = h;
        best->rect = r;
        best->area = area;
    }
    return TRUE;
}

bool window_rect_at(int sx, int sy, RECT& out) {
    WinHit best = {nullptr, {}, LLONG_MAX};
    POINT pt = {sx, sy};

    // 从该点所属的顶层窗口开始向上找，再退化为全桌遍历
    HWND h = WindowFromPoint(pt);
    if (h) {
        HWND root = GetAncestor(h, GA_ROOT);
        if (root) {
            RECT r = {};
            if (SUCCEEDED(DwmGetWindowAttribute(root, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
                GetWindowRect(root, &r)) {
                if (ptInRect(r, sx, sy)) {
                    wchar_t cls[256] = {0};
                    GetClassNameW(root, cls, 256);
                    DWORD pid = 0;
                    GetWindowThreadProcessId(root, &pid);
                    if (!class_is_skipped(cls) && pid != GetCurrentProcessId()) {
                        out = r;
                        return true;
                    }
                }
            }
        }
    }

    EnumWindows(enum_proc, (LPARAM)&best);
    if (best.hwnd && ptInRect(best.rect, sx, sy)) {
        out = best.rect;
        return true;
    }
    return false;
}
