#include "common.h"
#include "settings.h"
#include "overlay.h"
#include "pin.h"
#include "output.h"

#include <memory>
#include <atomic>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shcore.lib")

#define APP_NAME    L"简截"
#define APP_VERSION L"1.0.4"

// 与 src/app.rc 里的 IDI_APP 保持一致
#define IDI_APP 101

static const UINT HOTKEY_ID       = 0xA1;
static const UINT WM_TRAY         = WM_APP + 1;
static const UINT WM_ACTIVATE_APP = WM_APP + 2;
static const wchar_t* WND_CLASS   = L"JianJieMsg";
static const wchar_t* RUN_KEY     = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static HINSTANCE g_hinst = nullptr;
static HWND g_msgwnd = nullptr;
static HICON g_icon = nullptr;
static HICON g_iconBig = nullptr;   // 32x32，给窗口类
static HICON g_iconSm = nullptr;    // 16x16，给窗口类标题栏/任务栏
static NOTIFYICONDATAW g_nid = {};
static Settings g_cfg;
static UINT g_hotMods = MOD_ALT, g_hotVk = 0x41;
static bool g_hotOk = false;
static std::wstring g_hotName = L"Alt+A";
static std::atomic<bool> g_watchStop{false};
static std::thread g_watch;

// ------------------------------------------------------------------ 图标
static HICON make_icon(int size) {
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, color);

    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        float S = (float)size;
        float inset = S * 0.055f;
        float r = (S - inset * 2) * 0.235f;
        Gdiplus::GraphicsPath p;
        float x = inset, y = inset, w = S - inset * 2, h = S - inset * 2;
        p.AddArc(x, y, r * 2, r * 2, 180, 90);
        p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
        p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
        p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
        p.CloseFigure();

        Gdiplus::SolidBrush green(Gdiplus::Color(255, 7, 193, 96));
        g.FillPath(&green, &p);

        // 白色取景框角标
        float sw = S * 0.075f;
        float m = S * 0.255f;
        float arm = S * 0.20f;
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), sw);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        float L = m, T = m, R = S - m, B = S - m;
        g.DrawLine(&pen, L, T + arm, L, T);
        g.DrawLine(&pen, L, T, L + arm, T);
        g.DrawLine(&pen, R - arm, T, R, T);
        g.DrawLine(&pen, R, T, R, T + arm);
        g.DrawLine(&pen, R, B - arm, R, B);
        g.DrawLine(&pen, R, B, R - arm, B);
        g.DrawLine(&pen, L + arm, B, L, B);
        g.DrawLine(&pen, L, B, L, B - arm);
    }

    SelectObject(mem, old);
    DeleteDC(mem);

    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

// 优先用 app.rc 里嵌入的 ico（与资源管理器显示的一致），失败再退回自绘
static HICON load_icon(int size) {
    HICON h = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                               size, size, LR_DEFAULTCOLOR);
    if (!h) {
        logf(L"嵌入图标缺失，改用自绘图标 size=%d", size);
        h = make_icon(size);
    }
    return h;
}

// ------------------------------------------------------------------ 自启动
static bool autostart_enabled() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD size = sizeof(buf);
    DWORD type = 0;
    bool on = RegQueryValueExW(k, APP_NAME, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS;
    RegCloseKey(k);
    return on;
}

static void set_autostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + exe_path() + L"\"";
        RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_NAME);
    }
    RegCloseKey(k);
}

// ------------------------------------------------------------------ 热键
static bool try_register(UINT mods, UINT vk) {
    UnregisterHotKey(g_msgwnd, HOTKEY_ID);
    if (RegisterHotKey(g_msgwnd, HOTKEY_ID, mods | MOD_NOREPEAT, vk)) {
        g_hotMods = mods;
        g_hotVk = vk;
        g_hotOk = true;
        return true;
    }
    g_hotOk = false;
    return false;
}

static bool register_preferred(const std::wstring& name) {
    UINT mods = 0, vk = 0;
    resolve_hotkey(name, mods, vk);
    if (try_register(mods, vk)) {
        g_hotName = name;
        logf(L"尝试热键 %s -> True", name.c_str());
        return true;
    }
    logf(L"尝试热键 %s -> False", name.c_str());
    for (int i = 0; i < N_HOTKEY_PRESETS; ++i) {
        if (name == HOTKEY_PRESETS[i].name) continue;
        if (try_register(HOTKEY_PRESETS[i].mods, HOTKEY_PRESETS[i].vk)) {
            g_hotName = HOTKEY_PRESETS[i].name;
            logf(L"回退热键 %s -> True", g_hotName.c_str());
            return true;
        }
    }
    logf(L"所有热键都注册失败");
    return false;
}

// ------------------------------------------------------------------ 托盘
static void tray_add() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_msgwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = g_icon;
    wcsncpy_s(g_nid.szTip, APP_NAME, _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    std::wstring cap = std::wstring(L"截图  ") + g_hotName;
    AppendMenuW(menu, MF_STRING, 1, cap.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU hk = CreatePopupMenu();
    for (int i = 0; i < N_HOTKEY_PRESETS; ++i) {
        UINT flags = MF_STRING;
        if (g_hotName == HOTKEY_PRESETS[i].name) flags |= MF_CHECKED;
        AppendMenuW(hk, flags, 100 + i, HOTKEY_PRESETS[i].name);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)hk, L"快捷键");

    AppendMenuW(menu, MF_STRING, 2, L"翻译服务…");
    AppendMenuW(menu, MF_STRING | (autostart_enabled() ? MF_CHECKED : 0), 3, L"开机自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"关于");
    AppendMenuW(menu, MF_STRING, 5, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_msgwnd);
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_msgwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 1) {
        overlay_open();
    } else if (cmd >= 100 && cmd < 100 + N_HOTKEY_PRESETS) {
        std::wstring name = HOTKEY_PRESETS[cmd - 100].name;
        if (register_preferred(name)) {
            g_cfg.hotkey = g_hotName;
            save_settings(g_cfg);
        }
    } else if (cmd == 2) {
        extern void show_settings_dialog(HWND owner);
        show_settings_dialog(g_msgwnd);
    } else if (cmd == 3) {
        set_autostart(!autostart_enabled());
    } else if (cmd == 4) {
        std::wstring msg = std::wstring(L"简截 ") + APP_VERSION + L"\n\n"
                           L"微信风格的独立截图工具（原生 C++ 版）。\n"
                           L"不依赖微信，占用小、启动快。\n\n"
                           L"当前快捷键：" + g_hotName;
        MessageBoxW(nullptr, msg.c_str(), L"关于简截", MB_OK | MB_ICONINFORMATION);
    } else if (cmd == 5) {
        PostMessageW(g_msgwnd, WM_CLOSE, 0, 0);
    }
}

// ------------------------------------------------------------------ 设置对话框
struct CfgDlg {
    HWND hwnd = nullptr;
    HWND base = nullptr, key = nullptr, model = nullptr;
    HWND save = nullptr, cancel = nullptr;
    bool done = false;
};

static void cfg_layout(CfgDlg* d) {
    int pad = S(18), lw = S(110), eh = S(28), rowGap = S(52);
    int w = S(560);
    int y = pad + S(10);
    CreateWindowExW(0, L"STATIC", L"接口地址", WS_CHILD | WS_VISIBLE | SS_LEFT,
                    pad, y + S(6), lw, S(20), d->hwnd, nullptr, g_hinst, nullptr);
    d->base = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              pad + lw, y, w - pad * 2 - lw, eh, d->hwnd, nullptr, g_hinst, nullptr);

    y += rowGap;
    CreateWindowExW(0, L"STATIC", L"API Key", WS_CHILD | WS_VISIBLE | SS_LEFT,
                    pad, y + S(6), lw, S(20), d->hwnd, nullptr, g_hinst, nullptr);
    d->key = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                             pad + lw, y, w - pad * 2 - lw, eh, d->hwnd, nullptr, g_hinst, nullptr);

    y += rowGap;
    CreateWindowExW(0, L"STATIC", L"模型名称", WS_CHILD | WS_VISIBLE | SS_LEFT,
                    pad, y + S(6), lw, S(20), d->hwnd, nullptr, g_hinst, nullptr);
    d->model = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               pad + lw, y, w - pad * 2 - lw, eh, d->hwnd, nullptr, g_hinst, nullptr);

    y += rowGap + S(4);
    CreateWindowExW(0, L"STATIC",
                    L"留空 API Key 则使用免费的在线翻译接口。",
                    WS_CHILD | WS_VISIBLE | SS_LEFT, pad, y, w - pad * 2, S(20),
                    d->hwnd, nullptr, g_hinst, nullptr);

    int by = y + S(40);
    d->save = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              w - pad - S(90) * 2 - S(10), by, S(90), S(30), d->hwnd, (HMENU)1, g_hinst, nullptr);
    d->cancel = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
                                w - pad - S(90), by, S(90), S(30), d->hwnd, (HMENU)2, g_hinst, nullptr);

    SetWindowTextW(d->base, g_cfg.llm_base.c_str());
    SetWindowTextW(d->key, g_cfg.llm_key.c_str());
    SetWindowTextW(d->model, g_cfg.llm_model.c_str());
}

static LRESULT CALLBACK cfg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* d = (CfgDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        d = (CfgDlg*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
        d->hwnd = h;
        cfg_layout(d);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1 && d) {
            wchar_t buf[1024];
            GetWindowTextW(d->base, buf, 1024);  g_cfg.llm_base = buf;
            GetWindowTextW(d->key, buf, 1024);   g_cfg.llm_key = buf;
            GetWindowTextW(d->model, buf, 1024); g_cfg.llm_model = buf;
            save_settings(g_cfg);
            DestroyWindow(h);
        } else if (id == 2) {
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (d) d->done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void show_settings_dialog(HWND owner) {
    static bool cls_done = false;
    if (!cls_done) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = cfg_proc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"JianJieSettings";
        wc.hIcon = g_iconBig;
        wc.hIconSm = g_iconSm;
        RegisterClassExW(&wc);
        cls_done = true;
    }

    CfgDlg d;
    int w = S(560), h = S(330);
    RECT orc = {0, 0, 0, 0};
    if (owner && GetWindowRect(owner, &orc)) {
        // 托盘窗口没有尺寸，忽略
    }
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"JianJieSettings", L"翻译服务设置",
                               WS_CAPTION | WS_SYSMENU | WS_POPUP,
                               x, y, w, h, owner, nullptr, g_hinst, &d);
    logf(L"设置对话框 hwnd=%p err=%lu", dlg, dlg ? 0UL : GetLastError());
    if (!dlg) return;

    // 统一字体
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(dlg, [](HWND c, LPARAM f) -> BOOL {
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        return TRUE;
    }, (LPARAM)font);

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetActiveWindow(dlg);
    if (owner) EnableWindow(owner, FALSE);

    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) EnableWindow(owner, TRUE);
    if (IsWindow(dlg)) DestroyWindow(dlg);
}

// ------------------------------------------------------------------ 单实例
static bool watch_activate() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"JianJie_Activate");
    if (!ev) return false;
    SetEvent(ev);
    CloseHandle(ev);
    return true;
}

static void start_watcher() {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, L"JianJie_Activate");
    if (!ev) return;
    g_watch = std::thread([ev]() {
        while (!g_watchStop.load()) {
            DWORD r = WaitForSingleObject(ev, 300);
            if (r == WAIT_OBJECT_0) {
                PostMessageW(g_msgwnd, WM_ACTIVATE_APP, 0, 0);
            }
        }
        CloseHandle(ev);
    });
}

// ------------------------------------------------------------------ 消息窗口
static LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) {
            logf(L"收到热键");
            overlay_open();
        }
        return 0;

    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            show_tray_menu();
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            overlay_open();
        }
        return 0;

    case WM_ACTIVATE_APP:
        overlay_open();
        return 0;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    static UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbar_created) {
        tray_add();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------ 入口
static void init_dpi() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL(WINAPI * SetCtx)(void*);
        auto fn = (SetCtx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (fn && fn((void*)-4)) {   // PER_MONITOR_AWARE_V2
            // ok
        } else {
            SetProcessDPIAware();
        }
    }
    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    if (g_scale < 1.0) g_scale = 1.0;
    logf(L"DPI scale = %.3f", g_scale);
}

// 兜底：真崩了至少把异常码和地址写进日志，方便下次定位
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        logf(L"程序异常 code=0x%08X addr=%p",
             (unsigned)ep->ExceptionRecord->ExceptionCode,
             ep->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int) {
    g_hinst = hinst;
    SetUnhandledExceptionFilter(crash_filter);

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

    // 单实例
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"JianJie_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (watch_activate()) {
            CloseHandle(mutex);
            Gdiplus::GdiplusShutdown(token);
            return 0;
        }
        MessageBoxW(nullptr, L"简截已经在运行了，请查看系统托盘。", APP_NAME, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        Gdiplus::GdiplusShutdown(token);
        return 0;
    }

    init_dpi();
    g_cfg = load_settings();
    logf(L"启动 %s，exe=%s", APP_VERSION, exe_path().c_str());

    // 注册消息窗口类
    g_iconBig = load_icon(GetSystemMetrics(SM_CXICON));
    g_iconSm = load_icon(GetSystemMetrics(SM_CXSMICON));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = msg_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = WND_CLASS;
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSm;
    RegisterClassExW(&wc);

    g_msgwnd = CreateWindowExW(0, WND_CLASS, APP_NAME, 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, hinst, nullptr);
    if (!g_msgwnd) return 1;

    g_icon = g_iconSm ? g_iconSm : make_icon(GetSystemMetrics(SM_CXSMICON));
    tray_add();

    register_preferred(g_cfg.hotkey);
    start_watcher();

    // 调试/快捷入口：简截.exe --settings 直接打开翻译服务设置
    if (wcsstr(GetCommandLineW(), L"--settings")) {
        show_settings_dialog(g_msgwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 收尾
    g_watchStop.store(true);
    if (g_watch.joinable()) g_watch.join();
    tray_remove();
    pin_close_all();
    overlay_destroy();
    if (g_icon) DestroyIcon(g_icon);
    if (mutex) CloseHandle(mutex);
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
