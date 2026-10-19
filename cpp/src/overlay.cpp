#include "overlay.h"
#include "ocr.h"
#include "translate.h"
#include "pin.h"
#include "output.h"
#include <imm.h>
#include <memory>
#include <mutex>

#pragma comment(lib, "imm32.lib")

using namespace Gdiplus;

// ------------------------------------------------------------------ 常量
#define ITEM_W     36
#define TB_HEIGHT  44
#define TB_PAD     6
#define ICON_BOX   22
#define HANDLE_SZ  7
#define HANDLE_HIT 12
#define MIN_SEL    8

static const wchar_t* OV_CLASS = L"JianJieOverlay";
static const UINT WM_JJ_OCR = WM_APP + 22;
static const UINT WM_JJ_TRANS = WM_APP + 23;

static Color C_BG(242, 31, 31, 31);
static Color C_BORDER(20, 255, 255, 255);
static Color C_HOVER(28, 255, 255, 255);
static Color C_ACTIVE(46, 7, 193, 96);
static Color C_ICON(255, 242, 242, 242);
static Color C_GREEN(255, 7, 193, 96);
static Color C_LABEL_BG(170, 0, 0, 0);

static std::shared_ptr<Overlay> g_ov;
static HBITMAP g_backBuf = nullptr;
static HDC g_backDC = nullptr;
static int g_bw = 0, g_bh = 0;

struct JobMsg {
    bool ok = false;
    std::wstring text;
    std::wstring err;
    TranslateResult tr;
    bool translated = false;
};

Overlay::~Overlay() {
    free_shot(shot);
    delete mosaic;
    mosaic = nullptr;
}

// ------------------------------------------------------------------ 小工具
static void force_foreground(HWND h) {
    ShowWindow(h, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD cur = GetCurrentThreadId();
    DWORD other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = false;
    if (other && other != cur) attached = !!AttachThreadInput(other, cur, TRUE);
    SetForegroundWindow(h);
    SetFocus(h);
    if (attached) AttachThreadInput(other, cur, FALSE);
}

static RectF handle_rect(const RECT& s, int which, float sz) {
    float cx = 0, cy = 0;
    float l = (float)s.left, t = (float)s.top, r = (float)s.right, b = (float)s.bottom;
    switch (which) {
    case 0: cx = l; cy = t; break;                 // nw
    case 1: cx = (l + r) / 2; cy = t; break;       // n
    case 2: cx = r; cy = t; break;                 // ne
    case 3: cx = r; cy = (t + b) / 2; break;       // e
    case 4: cx = r; cy = b; break;                 // se
    case 5: cx = (l + r) / 2; cy = b; break;       // s
    case 6: cx = l; cy = b; break;                 // sw
    default: cx = l; cy = (t + b) / 2; break;      // w
    }
    return RectF(cx - sz / 2, cy - sz / 2, sz, sz);
}

static LPCWSTR HANDLE_CURSORS[8] = {
    IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
    IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE,
};

static int handle_at(const Overlay* o, POINT pt) {
    if (!o->hasSel) return -1;
    float sz = SF((float)HANDLE_SZ);
    float hit = SF((float)HANDLE_HIT);
    for (int i = 0; i < 8; ++i) {
        RectF r = handle_rect(o->sel, i, sz);
        RectF big(r.X - hit / 2 + sz / 2, r.Y - hit / 2 + sz / 2, hit, hit);
        if (ptInRectF(big, (float)pt.x, (float)pt.y)) return i;
    }
    return -1;
}

static Bitmap* ensure_mosaic(Overlay* o) {
    if (!o->hasSel) return nullptr;
    if (o->sel.right - o->sel.left < 2 || o->sel.bottom - o->sel.top < 2) return nullptr;
    if (o->mosaic && EqualRect(&o->mosaicKey, &o->sel)) return o->mosaic;

    delete o->mosaic;
    o->mosaic = nullptr;
    o->mosaicKey = o->sel;

    int w = o->sel.right - o->sel.left;
    int h = o->sel.bottom - o->sel.top;
    Bitmap* crop = o->shot.bmp->Clone(o->sel.left, o->sel.top, w, h, PixelFormat32bppRGB);
    if (!crop) return nullptr;
    o->mosaic = make_mosaic(crop);
    delete crop;
    return o->mosaic;
}

// ------------------------------------------------------------------ 图标
static void setup_icon_pen(Pen& pen, Color c, float w) {
    pen.SetColor(c);
    pen.SetWidth(w);
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
}

static void i_rect(Graphics& g, const RectF& a, Color c) {
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.8f));
    float dx = SF(2.5f), dy = SF(4.5f);
    g.DrawRectangle(&pen, a.X + dx, a.Y + dy, a.Width - dx * 2, a.Height - dy * 2);
}

static void i_ellipse(Graphics& g, const RectF& a, Color c) {
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.8f));
    float dx = SF(2.5f), dy = SF(4.5f);
    g.DrawEllipse(&pen, a.X + dx, a.Y + dy, a.Width - dx * 2, a.Height - dy * 2);
}

static void i_arrow(Graphics& g, const RectF& a, Color c) {
    float x0 = a.X + SF(3.5f), y0 = a.Y + a.Height - SF(3.5f);
    float x1 = a.X + a.Width - SF(4.0f), y1 = a.Y + SF(4.0f);
    float ang = atan2f(y1 - y0, x1 - x0);
    float head = SF(6.4f);
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.9f));
    g.DrawLine(&pen, PointF(x0, y0), PointF(x1 - head * 0.6f * cosf(ang),
                                            y1 - head * 0.6f * sinf(ang)));
    SolidBrush br(c);
    PointF tri[3] = {
        PointF(x1, y1),
        PointF(x1 - head * cosf(ang - 0.42f), y1 - head * sinf(ang - 0.42f)),
        PointF(x1 - head * cosf(ang + 0.42f), y1 - head * sinf(ang + 0.42f)),
    };
    g.FillPolygon(&br, tri, 3);
}

static void i_pen(Graphics& g, const RectF& a, Color c) {
    GraphicsPath path;
    path.AddBezier(PointF(a.X + SF(3.0f), a.Y + a.Height - SF(4.0f)),
                   PointF(a.X + SF(6.0f), a.Y + SF(2.0f)),
                   PointF(a.X + SF(11.0f), a.Y + a.Height - SF(1.0f)),
                   PointF(a.X + SF(13.0f), a.Y + SF(6.0f)));
    path.AddBezier(PointF(a.X + SF(13.0f), a.Y + SF(6.0f)),
                   PointF(a.X + SF(15.0f), a.Y + SF(3.0f)),
                   PointF(a.X + SF(17.5f), a.Y + SF(5.0f)),
                   PointF(a.X + a.Width - SF(2.5f), a.Y + SF(3.0f)));
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.9f));
    g.DrawPath(&pen, &path);
}

static void i_mosaic(Graphics& g, const RectF& a, Color c) {
    SolidBrush br(c);
    float cell = SF(5.0f), gap = SF(1.2f);
    float x0 = a.X + SF(2.5f), y0 = a.Y + SF(2.5f);
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if ((row + col) % 2 == 0) continue;
            g.FillRectangle(&br, x0 + col * (cell + gap), y0 + row * (cell + gap), cell, cell);
        }
    }
}

static void i_text(Graphics& g, const RectF& a, Color c) {
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(2.0f));
    float cx = a.X + a.Width / 2;
    g.DrawLine(&pen, PointF(a.X + SF(5.0f), a.Y + SF(6.0f)),
               PointF(a.X + a.Width - SF(5.0f), a.Y + SF(6.0f)));
    g.DrawLine(&pen, PointF(cx, a.Y + SF(6.0f)), PointF(cx, a.Y + a.Height - SF(4.5f)));
}

static void i_number(Graphics& g, const RectF& a, Color c) {
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.7f));
    float d = SF(3.5f);
    g.DrawEllipse(&pen, a.X + d, a.Y + d, a.Width - d * 2, a.Height - d * 2);
    float cx = a.X + a.Width / 2, cy = a.Y + a.Height / 2;
    Pen pen2(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen2, c, SF(1.5f));
    g.DrawLine(&pen2, PointF(cx - SF(1.6f), cy - SF(2.6f)), PointF(cx, cy - SF(4.2f)));
    g.DrawLine(&pen2, PointF(cx, cy - SF(4.2f)), PointF(cx, cy + SF(4.2f)));
}

static void i_undo(Graphics& g, const RectF& a, Color c) {
    float l = a.X + SF(4.0f), t = a.Y + SF(5.0f);
    float w = a.Width - SF(8.0f), h = a.Height - SF(10.0f);
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.8f));
    g.DrawArc(&pen, l, t, w, h, 40.0f, 270.0f);
    SolidBrush br(c);
    float cy = t + h / 2;
    PointF tri[3] = {PointF(l - SF(0.5f), cy - SF(3.0f)),
                     PointF(l + SF(5.0f), cy - SF(3.0f)),
                     PointF(l + SF(2.2f), cy + SF(2.0f))};
    g.FillPolygon(&br, tri, 3);
}

static void i_save(Graphics& g, const RectF& a, Color c) {
    float cx = a.X + a.Width / 2;
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.8f));
    g.DrawLine(&pen, PointF(cx, a.Y + SF(3.5f)), PointF(cx, a.Y + a.Height - SF(8.0f)));
    SolidBrush br(c);
    PointF tri[3] = {PointF(cx, a.Y + a.Height - SF(5.0f)),
                     PointF(cx - SF(4.0f), a.Y + a.Height - SF(10.0f)),
                     PointF(cx + SF(4.0f), a.Y + a.Height - SF(10.0f))};
    g.FillPolygon(&br, tri, 3);
    g.DrawLine(&pen, PointF(a.X + SF(4.0f), a.Y + a.Height - SF(3.0f)),
               PointF(a.X + a.Width - SF(4.0f), a.Y + a.Height - SF(3.0f)));
}

static void i_copy(Graphics& g, const RectF& a, Color c) {
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.6f));
    float r = SF(2.0f);
    GraphicsPath p1, p2;
    float w1 = SF(11.0f), h1 = SF(11.0f);
    auto round_rect = [&](GraphicsPath& p, float x, float y, float w, float h) {
        p.AddArc(x, y, r * 2, r * 2, 180, 90);
        p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
        p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
        p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
        p.CloseFigure();
    };
    round_rect(p1, a.X + SF(3.0f), a.Y + SF(3.0f), w1, h1);
    round_rect(p2, a.X + SF(7.0f), a.Y + SF(7.5f), w1, h1);
    g.DrawPath(&pen, &p1);
    g.DrawPath(&pen, &p2);
}

static void i_pin(Graphics& g, const RectF& a, Color c) {
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.6f));
    float r = SF(2.0f);
    GraphicsPath p;
    float x = a.X + SF(3.0f), y = a.Y + SF(5.5f), w = SF(13.0f), h = SF(11.0f);
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
    g.DrawPath(&pen, &p);

    Pen pen2(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen2, c, SF(1.5f));
    g.DrawLine(&pen2, PointF(a.X + SF(5.5f), a.Y + SF(13.0f)),
               PointF(a.X + SF(9.5f), a.Y + SF(8.5f)));
    SolidBrush br(c);
    float rr = SF(2.6f);
    g.FillEllipse(&br, a.X + SF(15.0f) - rr, a.Y + SF(4.5f) - rr, rr * 2, rr * 2);
}

static void i_cancel(Graphics& g, const RectF& a, Color c) {
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.9f));
    g.DrawLine(&pen, PointF(a.X + SF(5.5f), a.Y + SF(5.5f)),
               PointF(a.X + a.Width - SF(5.5f), a.Y + a.Height - SF(5.5f)));
    g.DrawLine(&pen, PointF(a.X + a.Width - SF(5.5f), a.Y + SF(5.5f)),
               PointF(a.X + SF(5.5f), a.Y + a.Height - SF(5.5f)));
}

static void i_ocr(Graphics& g, const RectF& a, Color c) {
    float L = a.X + SF(3.0f), T = a.Y + SF(3.0f);
    float R = a.X + a.Width - SF(3.0f), B = a.Y + a.Height - SF(3.0f);
    float k = SF(4.6f);
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.7f));
    g.DrawLine(&pen, PointF(L, T + k), PointF(L, T));
    g.DrawLine(&pen, PointF(L, T), PointF(L + k, T));
    g.DrawLine(&pen, PointF(R - k, T), PointF(R, T));
    g.DrawLine(&pen, PointF(R, T), PointF(R, T + k));
    g.DrawLine(&pen, PointF(R, B - k), PointF(R, B));
    g.DrawLine(&pen, PointF(R, B), PointF(R - k, B));
    g.DrawLine(&pen, PointF(L + k, B), PointF(L, B));
    g.DrawLine(&pen, PointF(L, B), PointF(L, B - k));

    float cx = a.X + a.Width / 2, cy = a.Y + a.Height / 2;
    Pen pen2(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen2, c, SF(1.6f));
    g.DrawLine(&pen2, PointF(cx - SF(4.2f), cy - SF(3.6f)), PointF(cx + SF(4.2f), cy - SF(3.6f)));
    g.DrawLine(&pen2, PointF(cx, cy - SF(3.6f)), PointF(cx, cy + SF(4.0f)));
}

static void i_trans(Graphics& g, const RectF& a, Color c) {
    float cx = a.X + SF(6.6f), top = a.Y + SF(5.2f);
    Pen pen(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen, c, SF(1.45f));
    SolidBrush br(c);
    g.FillEllipse(&br, cx - SF(1.0f), top - SF(1.0f), SF(2.0f), SF(2.0f));
    g.DrawLine(&pen, PointF(cx - SF(4.0f), top + SF(3.2f)), PointF(cx + SF(4.0f), top + SF(3.2f)));
    g.DrawLine(&pen, PointF(cx - SF(3.4f), top + SF(6.0f)), PointF(cx + SF(3.4f), top + SF(11.2f)));
    g.DrawLine(&pen, PointF(cx + SF(3.4f), top + SF(6.0f)), PointF(cx - SF(3.4f), top + SF(11.2f)));

    float bx = a.X + a.Width - SF(6.4f), btop = a.Y + SF(5.6f);
    Pen pen2(Color(255, 0, 0, 0), 1);
    setup_icon_pen(pen2, c, SF(1.55f));
    g.DrawLine(&pen2, PointF(bx - SF(3.6f), btop + SF(10.4f)), PointF(bx, btop));
    g.DrawLine(&pen2, PointF(bx, btop), PointF(bx + SF(3.6f), btop + SF(10.4f)));
    g.DrawLine(&pen2, PointF(bx - SF(2.0f), btop + SF(6.4f)), PointF(bx + SF(2.0f), btop + SF(6.4f)));
}

// ------------------------------------------------------------------ 工具条
struct ToolDef { int id; const wchar_t* tip; };

static const ToolDef TOOL_DEFS[] = {
    {T_RECT,    L"矩形  R"},
    {T_ELLIPSE, L"椭圆  E"},
    {T_ARROW,   L"箭头  A"},
    {T_PEN,     L"画笔  P"},
    {T_MOSAIC,  L"马赛克  M"},
    {T_TEXT,    L"文字  T"},
    {T_NUMBER,  L"序号  N"},
    {T_SEP,     L""},
    {T_OCR,     L"提取文字  O"},
    {T_TRANS,   L"翻译  F"},
    {T_COLOR,   L"标注颜色  C"},
    {T_SEP,     L""},
    {T_UNDO,    L"撤销  Ctrl+Z"},
    {T_SAVE,    L"保存  Ctrl+S"},
    {T_COPY,    L"复制  Enter"},
    {T_PIN,     L"贴图  Ctrl+T"},
    {T_CANCEL,  L"取消  Esc"},
};
static const int N_TOOLS = (int)(sizeof(TOOL_DEFS) / sizeof(TOOL_DEFS[0]));

static void layout_toolbar(Overlay* o) {
    o->items.clear();
    float x = SF((float)TB_PAD);
    float h = SF((float)TB_HEIGHT);
    float iw = SF((float)ITEM_W);
    for (int i = 0; i < N_TOOLS; ++i) {
        ToolItem it;
        it.id = TOOL_DEFS[i].id;
        if (it.id == T_SEP) {
            it.rect = RectF(x + SF(2.0f), SF(10.0f), 1.0f, h - SF(20.0f));
            x += SF(5.0f);
        } else {
            it.rect = RectF(x, (h - iw) / 2 + SF(4.0f), iw, iw - SF(4.0f));
            x += iw;
        }
        o->items.push_back(it);
    }
}

static float toolbar_width() {
    float w = SF((float)TB_PAD) * 2;
    for (int i = 0; i < N_TOOLS; ++i) {
        w += (TOOL_DEFS[i].id == T_SEP) ? SF(5.0f) : SF((float)ITEM_W);
    }
    return w;
}

static bool rect_overlap(const RectF& a, const RectF& b) {
    return a.X < b.X + b.Width && b.X < a.X + a.Width &&
           a.Y < b.Y + b.Height && b.Y < a.Y + a.Height;
}

static void place_toolbar(Overlay* o) {
    float w = toolbar_width();
    float h = SF((float)TB_HEIGHT);
    float cx = (o->sel.left + o->sel.right) / 2.0f;
    float x = cx - w / 2;
    float y = (float)o->sel.bottom + SF(8.0f);
    if (y + h > (float)o->shot.vh - 2) y = (float)o->sel.top - h - SF(8.0f);
    x = std::max(4.0f, std::min(x, (float)o->shot.vw - w - 4.0f));
    y = std::max(4.0f, std::min(y, (float)o->shot.vh - h - 4.0f));

    // 结果面板开着的时候别压上去：先试翻到选区另一侧
    if (o->panel != PM_HIDDEN && rect_overlap(RectF(x, y, w, h), o->panelRect)) {
        bool below = (y >= (float)o->sel.bottom);
        float y2 = below ? (float)o->sel.top - h - SF(8.0f)
                         : (float)o->sel.bottom + SF(8.0f);
        y2 = std::max(4.0f, std::min(y2, (float)o->shot.vh - h - 4.0f));
        if (!rect_overlap(RectF(x, y2, w, h), o->panelRect)) y = y2;
    }

    o->tbX = x;
    o->tbY = y;
}

static int toolbar_hit(Overlay* o, POINT pt) {
    if (!o->tbVisible) return -1;
    float lx = (float)pt.x - o->tbX;
    float ly = (float)pt.y - o->tbY;
    for (size_t i = 0; i < o->items.size(); ++i) {
        if (o->items[i].id == T_SEP) continue;
        RectF r = o->items[i].rect;
        r.X -= SF(2.0f); r.Y -= SF(4.0f);
        r.Width += SF(4.0f); r.Height += SF(8.0f);
        if (ptInRectF(r, lx, ly)) return (int)i;
    }
    return -1;
}

static void draw_toolbar(Graphics& g, Overlay* o) {
    if (!o->tbVisible) return;
    float w = toolbar_width();
    float h = SF((float)TB_HEIGHT);
    GraphicsState st = g.Save();
    g.TranslateTransform(o->tbX, o->tbY);

    {
        GraphicsPath path;
        float r = SF(9.0f);
        path.AddArc(0.5f, 0.5f, r * 2, r * 2, 180, 90);
        path.AddArc(w - r * 2 - 0.5f, 0.5f, r * 2, r * 2, 270, 90);
        path.AddArc(w - r * 2 - 0.5f, h - r * 2 - 0.5f, r * 2, r * 2, 0, 90);
        path.AddArc(0.5f, h - r * 2 - 0.5f, r * 2, r * 2, 90, 90);
        path.CloseFigure();
        SolidBrush bg(C_BG);
        g.FillPath(&bg, &path);
        Pen border(C_BORDER, 1.0f);
        g.DrawPath(&border, &path);
    }

    for (size_t i = 0; i < o->items.size(); ++i) {
        auto& it = o->items[i];
        if (it.id == T_SEP) {
            Pen pen(Color(26, 255, 255, 255), 1.0f);
            g.DrawLine(&pen, it.rect.X, it.rect.Y + SF(2.0f), it.rect.X, it.rect.Y + it.rect.Height - SF(2.0f));
            continue;
        }

        RectF cell = it.rect;
        if ((int)i == o->hoverItem) {
            SolidBrush br(C_HOVER);
            RectF r(cell.X - SF(1.0f), cell.Y - SF(2.0f), cell.Width + SF(2.0f), cell.Height + SF(4.0f));
            GraphicsPath p;
            float rad = SF(6.0f);
            p.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
            p.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
            p.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 0, 90);
            p.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90, 90);
            p.CloseFigure();
            g.FillPath(&br, &p);
        }

        Color ic = C_ICON;
        if (o->tool == it.id) {
            SolidBrush br(C_ACTIVE);
            RectF r(cell.X - SF(1.0f), cell.Y - SF(2.0f), cell.Width + SF(2.0f), cell.Height + SF(4.0f));
            GraphicsPath p;
            float rad = SF(6.0f);
            p.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
            p.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
            p.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 0, 90);
            p.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90, 90);
            p.CloseFigure();
            g.FillPath(&br, &p);
            ic = C_GREEN;
        }
        if ((int)i == o->hoverItem) {
            if (it.id == T_UNDO || it.id == T_SAVE || it.id == T_COPY ||
                it.id == T_PIN || it.id == T_CANCEL || it.id == T_OCR || it.id == T_TRANS) {
                ic = Color(255, 255, 255, 255);
            }
        }

        float box = SF((float)ICON_BOX);
        RectF a(cell.X + cell.Width / 2 - box / 2, cell.Y + cell.Height / 2 - box / 2, box, box);

        switch (it.id) {
        case T_RECT:    i_rect(g, a, ic); break;
        case T_ELLIPSE: i_ellipse(g, a, ic); break;
        case T_ARROW:   i_arrow(g, a, ic); break;
        case T_PEN:     i_pen(g, a, ic); break;
        case T_MOSAIC:  i_mosaic(g, a, ic); break;
        case T_TEXT:    i_text(g, a, ic); break;
        case T_NUMBER:  i_number(g, a, ic); break;
        case T_OCR:     i_ocr(g, a, ic); break;
        case T_TRANS:   i_trans(g, a, ic); break;
        case T_UNDO:    i_undo(g, a, ic); break;
        case T_SAVE:    i_save(g, a, ic); break;
        case T_COPY:    i_copy(g, a, ic); break;
        case T_PIN:     i_pin(g, a, ic); break;
        case T_CANCEL:  i_cancel(g, a, ic); break;
        case T_COLOR: {
            Pen pen(Color(190, 255, 255, 255), SF(1.3f));
            SolidBrush br(PALETTE[o->colorIdx]);
            float d = SF(5.5f);
            g.DrawEllipse(&pen, a.X + d, a.Y + d, a.Width - d * 2, a.Height - d * 2);
            g.FillEllipse(&br, a.X + d + SF(1.2f), a.Y + d + SF(1.2f),
                          a.Width - d * 2 - SF(2.4f), a.Height - d * 2 - SF(2.4f));
            break;
        }
        default: break;
        }
    }

    // 悬浮提示
    if (o->hoverItem >= 0 && o->hoverItem < (int)o->items.size()) {
        const wchar_t* tip = TOOL_DEFS[o->hoverItem].tip;
        if (tip && tip[0]) {
            Font font(FONT_UI(), SF(12.0f), FontStyleRegular, UnitPixel);
            RectF bounds;
            g.MeasureString(tip, -1, &font, PointF(0, 0), &bounds);
            float tw = bounds.Width + SF(12.0f);
            float th = bounds.Height + SF(4.0f);
            float tx = o->items[o->hoverItem].rect.X + o->items[o->hoverItem].rect.Width / 2 - tw / 2;
            float ty = -th - SF(8.0f);
            if (o->tbY + ty < 2) ty = h + SF(8.0f);   // 顶部放不下就翻到下面
            tx = std::max(-o->tbX + 2.0f, std::min(tx, (float)o->shot.vw - o->tbX - tw - 2.0f));
            SolidBrush bg(Color(235, 20, 20, 20));
            g.FillRectangle(&bg, tx, ty, tw, th);
            SolidBrush fg(Color(255, 235, 235, 235));
            StringFormat fmt;
            fmt.SetAlignment(StringAlignmentCenter);
            fmt.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(tip, -1, &font, RectF(tx, ty, tw, th), &fmt, &fg);
        }
    }

    g.Restore(st);
}

// ------------------------------------------------------------------ 面板
static float panel_width() { return SF(340.0f); }

static void place_panel(Overlay* o) {
    const float w = panel_width();
    const float selH = (float)(o->sel.bottom - o->sel.top);
    const float h = std::max(SF(200.0f), std::min(selH, SF(360.0f)));
    const float GAP = SF(10.0f), M = SF(6.0f);
    const float cx = (o->sel.left + o->sel.right) / 2.0f;

    const RectF tb(o->tbX, o->tbY, toolbar_width(), SF((float)TB_HEIGHT));
    const bool tbOn = o->tbVisible;

    // 候选位置按优先级排列：
    //   0 选区右侧  1 选区左侧  2 选区下方  3 选区上方
    //   4 右侧但下移到工具条之下  5 右侧但上移到工具条之上
    const RectF cand[] = {
        RectF((float)o->sel.right + GAP,     (float)o->sel.top,          w, h),
        RectF((float)o->sel.left - w - GAP,  (float)o->sel.top,          w, h),
        RectF(cx - w / 2,                    (float)o->sel.bottom + GAP, w, h),
        RectF(cx - w / 2,                    (float)o->sel.top - h - GAP, w, h),
        RectF((float)o->sel.right + GAP,     tb.Y + tb.Height + GAP,     w, h),
        RectF((float)o->sel.right + GAP,     tb.Y - h - GAP,             w, h),
    };
    const int N_CAND = (int)(sizeof(cand) / sizeof(cand[0]));

    int best = 0;
    float bestScore = 1e30f;
    RectF bestRect = cand[0];
    for (int i = 0; i < N_CAND; ++i) {
        RectF r = cand[i];

        // 侧边候选只是被工具条压住一点点时，先尝试水平挪到工具条外侧，
        // 别为了几像素的重叠把整块面板挤到下面去
        if (tbOn && i < 2 && rect_overlap(r, tb)) {
            float rightX = tb.X + tb.Width + GAP;
            float leftX = tb.X - w - GAP;
            bool fitR = (rightX + w <= (float)o->shot.vw - M);
            bool fitL = (leftX >= M);
            if (fitR && fitL) {
                r.X = (fabsf(rightX - r.X) <= fabsf(leftX - r.X)) ? rightX : leftX;
            } else if (fitR) {
                r.X = rightX;
            } else if (fitL) {
                r.X = leftX;
            }
        }

        // 出界量（像素），优先保证整块面板都在屏幕里
        float out = 0.0f;
        if (r.X < M) out += M - r.X;
        if (r.Y < M) out += M - r.Y;
        if (r.X + w > (float)o->shot.vw - M) out += r.X + w - ((float)o->shot.vw - M);
        if (r.Y + h > (float)o->shot.vh - M) out += r.Y + h - ((float)o->shot.vh - M);

        float score = out * 1000.0f + (float)i;          // 出界远大于优先级差异
        if (tbOn && rect_overlap(r, tb)) score += 5000.0f;  // 压住工具条重罚
        if (score < bestScore) { bestScore = score; best = i; bestRect = r; }
    }

    RectF r = bestRect;
    r.X = std::max(M, std::min(r.X, (float)o->shot.vw - w - M));
    r.Y = std::max(M, std::min(r.Y, (float)o->shot.vh - h - M));
    o->panelRect = r;

    if (tbOn) {
        logf(L"布局 面板(%d,%d %dx%d) 工具条(%d,%d %dx%d) 候选#%d 重叠=%d",
             (int)r.X, (int)r.Y, (int)r.Width, (int)r.Height,
             (int)tb.X, (int)tb.Y, (int)tb.Width, (int)tb.Height,
             best, (int)rect_overlap(r, tb));
    }
}

static void panel_buttons(const Overlay* o, RectF out[3], const wchar_t* labels[3], int& count) {
    const RectF& r = o->panelRect;
    float pad = SF(10.0f);
    float bh = SF(26.0f);
    float bw = (r.Width - pad * 4) / 3;
    float y = r.Y + r.Height - pad - bh;
    for (int i = 0; i < 3; ++i) {
        out[i] = RectF(r.X + pad + i * (bw + pad), y, bw, bh);
    }
    count = 3;
    labels[0] = L"复制";
    labels[1] = (o->panel == PM_TEXT && !o->panelOriginal.empty())
                    ? (o->showOriginal ? L"译文" : L"原文")
                    : L"翻译";
    labels[2] = L"关闭";
}

static void panel_body_rect(const Overlay* o, RectF& out) {
    const RectF& r = o->panelRect;
    float pad = SF(10.0f);
    float top = r.Y + SF(38.0f);
    float bottom = r.Y + r.Height - pad - SF(26.0f) - SF(10.0f);
    out = RectF(r.X + pad, top, r.Width - pad * 2, bottom - top);
}

static const wchar_t* panel_current_text(const Overlay* o) {
    if (o->showOriginal && !o->panelOriginal.empty()) return o->panelOriginal.c_str();
    return o->panelBody.c_str();
}

static void draw_panel(Graphics& g, Overlay* o) {
    if (o->panel == PM_HIDDEN) return;
    const RectF& r = o->panelRect;

    GraphicsPath path;
    float rad = SF(10.0f);
    path.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
    path.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
    path.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90, 90);
    path.CloseFigure();

    SolidBrush bg(Color(245, 32, 32, 32));
    g.FillPath(&bg, &path);
    Pen border(Color(38, 255, 255, 255), 1.0f);
    g.DrawPath(&border, &path);

    // 标题
    Font tf(FONT_UI(), SF(13.0f), FontStyleBold, UnitPixel);
    SolidBrush white(Color(255, 240, 240, 240));
    g.DrawString(o->panelTitle.c_str(), -1, &tf, PointF(r.X + SF(12.0f), r.Y + SF(9.0f)), nullptr, &white);

    // 正文
    RectF body;
    panel_body_rect(o, body);
    GraphicsState st = g.Save();
    g.SetClip(body);
    if (o->panel == PM_LOADING) {
        Font f(FONT_UI(), SF(13.0f), FontStyleRegular, UnitPixel);
        SolidBrush dim(Color(255, 150, 150, 150));
        g.DrawString(L"正在处理…", -1, &f, PointF(body.X, body.Y), nullptr, &dim);
    } else if (o->panel == PM_ERROR) {
        Font f(FONT_UI(), SF(13.0f), FontStyleRegular, UnitPixel);
        SolidBrush red(Color(255, 240, 120, 120));
        g.DrawString(o->panelBody.c_str(), -1, &f,
                     RectF(body.X, body.Y, body.Width, body.Height + SF(400.0f)), nullptr, &red);
    } else {
        Font f(FONT_UI(), SF(14.0f), FontStyleRegular, UnitPixel);
        SolidBrush fg(Color(255, 228, 228, 228));
        RectF layout(body.X, body.Y - o->panelScroll, body.Width, body.Height + SF(4000.0f));
        g.DrawString(panel_current_text(o), -1, &f, layout, nullptr, &fg);
    }
    g.Restore(st);

    // 滚动条
    if (o->panel == PM_TEXT) {
        Font f(FONT_UI(), SF(14.0f), FontStyleRegular, UnitPixel);
        RectF measure;
        StringFormat fmt;
        g.MeasureString(panel_current_text(o), -1, &f,
                        RectF(0, 0, body.Width, SF(20000.0f)), &fmt, &measure);
        if (measure.Height > body.Height) {
            float trackH = body.Height;
            float thumbH = std::max(SF(24.0f), trackH * body.Height / measure.Height);
            float maxScroll = measure.Height - body.Height;
            float t = maxScroll > 0 ? o->panelScroll / maxScroll : 0;
            float ty = body.Y + (trackH - thumbH) * t;
            SolidBrush thumb(Color(70, 255, 255, 255));
            g.FillRectangle(&thumb, RectF(r.X + r.Width - SF(5.0f), ty, SF(3.0f), thumbH));
        }
    }

    // 按钮
    RectF btns[3];
    const wchar_t* labels[3];
    int n = 0;
    panel_buttons(o, btns, labels, n);
    for (int i = 0; i < n; ++i) {
        bool hover = (o->panelBtnHover == i);
        bool primary = (i == 1);
        Color fill = primary ? Color(255, 7, 193, 96)
                             : (hover ? Color(255, 58, 58, 58) : Color(255, 46, 46, 46));
        if (primary && !hover) fill = Color(255, 7, 160, 80);
        SolidBrush br(fill);
        GraphicsPath bp;
        float rr = SF(6.0f);
        bp.AddArc(btns[i].X, btns[i].Y, rr * 2, rr * 2, 180, 90);
        bp.AddArc(btns[i].X + btns[i].Width - rr * 2, btns[i].Y, rr * 2, rr * 2, 270, 90);
        bp.AddArc(btns[i].X + btns[i].Width - rr * 2, btns[i].Y + btns[i].Height - rr * 2, rr * 2, rr * 2, 0, 90);
        bp.AddArc(btns[i].X, btns[i].Y + btns[i].Height - rr * 2, rr * 2, rr * 2, 90, 90);
        bp.CloseFigure();
        g.FillPath(&br, &bp);

        Font bf(FONT_UI(), SF(12.5f), FontStyleRegular, UnitPixel);
        SolidBrush txt(Color(255, 255, 255, 255));
        StringFormat cf;
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(labels[i], -1, &bf, btns[i], &cf, &txt);
    }
}

static int panel_button_at(Overlay* o, POINT pt) {
    if (o->panel == PM_HIDDEN) return -1;
    if (!ptInRectF(o->panelRect, (float)pt.x, (float)pt.y)) return -1;
    RectF btns[3];
    const wchar_t* labels[3];
    int n = 0;
    panel_buttons(o, btns, labels, n);
    for (int i = 0; i < n; ++i) {
        if (ptInRectF(btns[i], (float)pt.x, (float)pt.y)) return i;
    }
    return -2;   // 面板内但不在按钮上
}

// ------------------------------------------------------------------ 绘制
static void draw_size_label(Graphics& g, const RECT& r, bool inside) {
    wchar_t text[64];
    _snwprintf_s(text, _countof(text), _TRUNCATE, L"%d × %d", r.right - r.left, r.bottom - r.top);
    Font font(FONT_UI(), SF(12.0f), FontStyleRegular, UnitPixel);
    RectF bounds;
    g.MeasureString(text, -1, &font, PointF(0, 0), &bounds);
    float w = bounds.Width + SF(14.0f);
    float h = bounds.Height + SF(6.0f);

    float x, y;
    if (inside) {
        x = (float)r.left + SF(4.0f);
        y = (float)r.top + SF(4.0f);
    } else {
        x = (float)r.left;
        y = (float)r.top - h - SF(5.0f);
        if (y < 2) { x = (float)r.left + SF(2.0f); y = (float)r.top + SF(2.0f); }
    }

    SolidBrush bg(C_LABEL_BG);
    g.FillRectangle(&bg, x, y, w, h);
    SolidBrush fg(Color(255, 255, 255, 255));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &font, RectF(x, y, w, h), &fmt, &fg);
}

static void draw_magnifier(Graphics& g, Overlay* o, POINT pos) {
    float R = SF(50.0f);
    float zoom = 7.0f;
    float cx = (float)pos.x + R + SF(26.0f);
    float cy = (float)pos.y + R + SF(26.0f);
    if (cx + R > (float)o->shot.vw - 2) cx = (float)pos.x - R - SF(26.0f);
    if (cy + R > (float)o->shot.vh - SF(46.0f)) cy = (float)pos.y - R - SF(26.0f);
    cx = std::max(R + 2, std::min(cx, (float)o->shot.vw - R - 2));
    cy = std::max(R + 2, std::min(cy, (float)o->shot.vh - R - 2));

    float span = 2 * R / zoom;
    float sx = (float)pos.x - span / 2;
    float sy = (float)pos.y - span / 2;

    GraphicsPath clip;
    clip.AddEllipse(cx - R, cy - R, R * 2, R * 2);
    GraphicsState st = g.Save();
    g.SetClip(&clip);
    g.SetInterpolationMode(InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(o->shot.bmp, RectF(cx - R, cy - R, R * 2, R * 2), sx, sy, span, span, UnitPixel);
    g.Restore(st);

    Pen shadow(Color(105, 0, 0, 0), SF(3.2f));
    g.DrawEllipse(&shadow, cx - R - 1, cy - R - 1, R * 2 + 2, R * 2 + 2);
    Pen ring(Color(235, 255, 255, 255), SF(1.6f));
    g.DrawEllipse(&ring, cx - R, cy - R, R * 2, R * 2);
    Pen cross(Color(210, 7, 193, 96), 1.0f);
    g.DrawLine(&cross, cx - R, cy, cx + R, cy);
    g.DrawLine(&cross, cx, cy - R, cx, cy + R);
    SolidBrush dot(Color(180, 255, 255, 255));
    g.FillEllipse(&dot, cx - SF(1.6f), cy - SF(1.6f), SF(3.2f), SF(3.2f));

    // 像素信息
    int px = pos.x, py = pos.y;
    BYTE rr = 0, gg = 0, bb = 0;
    if (px >= 0 && py >= 0 && px < o->shot.vw && py < o->shot.vh) {
        const BYTE* p = (const BYTE*)o->shot.bits + ((size_t)py * o->shot.vw + px) * 4;
        bb = p[0]; gg = p[1]; rr = p[2];
    }
    wchar_t text[128];
    _snwprintf_s(text, _countof(text), _TRUNCATE, L"RGB(%d, %d, %d)   (%d, %d)", rr, gg, bb, px, py);
    Font font(FONT_MONO(), SF(11.0f), FontStyleRegular, UnitPixel);
    RectF bounds;
    g.MeasureString(text, -1, &font, PointF(0, 0), &bounds);
    float w = bounds.Width + SF(12.0f);
    float h = bounds.Height + SF(5.0f);
    float x = std::max(2.0f, std::min(cx - w / 2, (float)o->shot.vw - w - 2));
    float y = cy + R + SF(6.0f);
    if (y + h > (float)o->shot.vh - 2) y = cy - R - h - SF(6.0f);
    SolidBrush bg(C_LABEL_BG);
    g.FillRectangle(&bg, x, y, w, h);
    SolidBrush fg(Color(255, 232, 232, 232));
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(text, -1, &font, RectF(x, y, w, h), &fmt, &fg);
}

static void draw_editing(Graphics& g, Overlay* o);

static void paint(Overlay* o) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(o->hwnd, &ps);

    int w = o->shot.vw, h = o->shot.vh;
    if (!g_backDC || g_bw != w || g_bh != h) {
        if (g_backBuf) DeleteObject(g_backBuf);
        if (g_backDC) DeleteDC(g_backDC);
        HDC screen = GetDC(nullptr);
        g_backDC = CreateCompatibleDC(screen);
        g_backBuf = CreateCompatibleBitmap(screen, w, h);
        SelectObject(g_backDC, g_backBuf);
        ReleaseDC(nullptr, screen);
        g_bw = w; g_bh = h;
    }

    HDC src = CreateCompatibleDC(hdc);
    HGDIOBJ oldSrc = SelectObject(src, o->shot.hbmDim);
    BitBlt(g_backDC, 0, 0, w, h, src, 0, 0, SRCCOPY);

    if (o->hasSel) {
        SelectObject(src, o->shot.hbm);
        int sw = o->sel.right - o->sel.left;
        int sh = o->sel.bottom - o->sel.top;
        if (sw > 0 && sh > 0) {
            BitBlt(g_backDC, o->sel.left, o->sel.top, sw, sh, src, o->sel.left, o->sel.top, SRCCOPY);
        }
    }
    SelectObject(src, oldSrc);
    DeleteDC(src);

    {
        Graphics g(g_backDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);

        if (o->hasSel) {
            int sw = o->sel.right - o->sel.left;
            int sh = o->sel.bottom - o->sel.top;
            if (sw > 0 && sh > 0) {
                GraphicsState st = g.Save();
                g.SetClip(Rect(o->sel.left, o->sel.top, sw, sh));
                g.TranslateTransform((REAL)o->sel.left, (REAL)o->sel.top);
                Bitmap* mos = o->mosaic;
                for (auto& a : o->anns) a.draw(g, mos);
                if (o->hasCur) o->cur.draw(g, mos);
                draw_editing(g, o);
                g.Restore(st);
            }

            // 边框
            Pen border(C_GREEN, 1.0f);
            g.DrawRectangle(&border, (float)o->sel.left + 0.5f, (float)o->sel.top + 0.5f,
                            (float)sw - 1.0f, (float)sh - 1.0f);

            // 控制点
            if (o->mode == 0 && o->tool == T_NONE) {
                Pen pen(C_GREEN, 1.0f);
                SolidBrush white(Color(255, 255, 255, 255));
                float sz = SF((float)HANDLE_SZ);
                for (int i = 0; i < 8; ++i) {
                    RectF r = handle_rect(o->sel, i, sz);
                    g.FillRectangle(&white, r);
                    g.DrawRectangle(&pen, r);
                }
            }
            draw_size_label(g, o->sel, false);
        } else {
            if (o->hasHover) {
                Pen pen(C_GREEN, SF(2.0f));
                g.DrawRectangle(&pen, (float)o->hoverWin.left + 1, (float)o->hoverWin.top + 1,
                                (float)(o->hoverWin.right - o->hoverWin.left) - 2,
                                (float)(o->hoverWin.bottom - o->hoverWin.top) - 2);
                draw_size_label(g, o->hoverWin, true);
            }
        }

        if (o->magValid && (o->mode == 1 || o->mode == 2)) {
            draw_magnifier(g, o, o->magPos);
        }

        draw_panel(g, o);
        draw_toolbar(g, o);
    }

    BitBlt(hdc, 0, 0, w, h, g_backDC, 0, 0, SRCCOPY);
    EndPaint(o->hwnd, &ps);
}

// ------------------------------------------------------------------ 文字输入（内联）
static void commit_text(Overlay* o);
static void cancel_text(Overlay* o);

static bool ime_composing(HWND h) {
    HIMC imc = ImmGetContext(h);
    if (!imc) return false;
    LONG n = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(h, imc);
    return n > 0;
}

static std::wstring ime_string(HWND h, DWORD which) {
    std::wstring out;
    HIMC imc = ImmGetContext(h);
    if (!imc) return out;
    LONG n = ImmGetCompositionStringW(imc, which, nullptr, 0);
    if (n > 0) {
        out.resize((size_t)n / sizeof(wchar_t));
        ImmGetCompositionStringW(imc, which, &out[0], n);
    }
    ImmReleaseContext(h, imc);
    return out;
}

static void edit_attach_ime(Overlay* o) {
    if (!o->himc) o->himc = ImmCreateContext();
    if (o->himc) ImmAssociateContext(o->hwnd, o->himc);
}

static void edit_detach_ime(Overlay* o) {
    ImmAssociateContext(o->hwnd, nullptr);
    o->composing.clear();
}

// 输入法（TSF）在消息队列之前就把按键吃掉了，键盘钩子也抓不到。
// 用 GetAsyncKeyState 的「自上次查询以来按下过」位轮询物理键，与输入法无关。
static const UINT_PTR TIMER_EDIT = 77;

static void edit_poll_start(HWND h) {
    GetAsyncKeyState(VK_RETURN);
    GetAsyncKeyState(VK_ESCAPE);
    GetAsyncKeyState(VK_BACK);
    SetTimer(h, TIMER_EDIT, 25, nullptr);
}

static void edit_poll_stop(HWND h) {
    KillTimer(h, TIMER_EDIT);
}

static void begin_text(Overlay* o, POINT pt) {
    commit_text(o);
    o->editing = true;
    o->editText.clear();
    o->composing.clear();
    o->editPos = PointF((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
    edit_attach_ime(o);
    edit_poll_start(o->hwnd);
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void commit_text(Overlay* o) {
    if (!o->editing) return;
    o->editing = false;
    std::wstring t = o->editText + o->composing;
    o->editText.clear();
    edit_detach_ime(o);
    edit_poll_stop(o->hwnd);

    size_t b = t.find_first_not_of(L" \t\r\n");
    size_t e = t.find_last_not_of(L" \t\r\n");
    if (b != std::wstring::npos) {
        Ann a;
        a.kind = AK_TEXT;
        a.color = PALETTE[o->colorIdx];
        a.pos = o->editPos;
        a.text = t.substr(b, e - b + 1);
        o->anns.push_back(a);
    }
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void cancel_text(Overlay* o) {
    if (!o->editing) return;
    o->editing = false;
    o->editText.clear();
    edit_detach_ime(o);
    edit_poll_stop(o->hwnd);
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

// 正在编辑的文字（局部坐标，随标注一起裁剪/平移）
static void draw_editing(Graphics& g, Overlay* o) {
    if (!o->editing) return;
    std::wstring shown = o->editText + o->composing;
    Font font(FONT_UI(), SF(15.0f), FontStyleRegular, UnitPixel);
    SolidBrush br(PALETTE[o->colorIdx]);
    if (!shown.empty()) {
        g.DrawString(shown.c_str(), -1, &font, o->editPos, nullptr, &br);
    }
    RectF b;
    g.MeasureString(shown.c_str(), -1, &font, PointF(0, 0), &b);
    float w = shown.empty() ? 0.0f : b.Width;
    float cx = o->editPos.X + w;
    Pen caret(Color(255, 7, 193, 96), SF(1.6f));
    g.DrawLine(&caret, cx, o->editPos.Y + SF(1.0f), cx, o->editPos.Y + SF(18.0f));
    if (!o->composing.empty()) {
        Pen ul(Color(255, 7, 193, 96), SF(1.0f));
        float left = o->editPos.X + (o->editText.empty() ? 0.0f : 0.0f);
        RectF pb;
        g.MeasureString(o->editText.c_str(), -1, &font, PointF(0, 0), &pb);
        float x0 = o->editPos.X + (o->editText.empty() ? 0.0f : pb.Width);
        g.DrawLine(&ul, x0, o->editPos.Y + SF(19.0f), cx, o->editPos.Y + SF(19.0f));
    }
}

// ------------------------------------------------------------------ 输出
static Bitmap* render_result(Overlay* o) {
    if (!o->hasSel) return nullptr;
    int sw = o->sel.right - o->sel.left;
    int sh = o->sel.bottom - o->sel.top;
    if (sw <= 0 || sh <= 0) return nullptr;

    Bitmap* crop = o->shot.bmp->Clone(o->sel.left, o->sel.top, sw, sh, PixelFormat32bppRGB);
    if (!crop) return nullptr;

    Graphics g(crop);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    // 裁剪图的左上角就是选区原点，标注局部坐标直接落上去即可
    Bitmap* mos = o->mosaic;
    for (auto& a : o->anns) a.draw(g, mos);
    return crop;
}

static void do_copy(Overlay* o) {
    Bitmap* img = render_result(o);
    if (img) {
        copy_bitmap_to_clipboard(img);
        delete img;
    }
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static void do_save(Overlay* o) {
    Bitmap* img = render_result(o);
    if (!img) return;
    Settings s = load_settings();
    logf(L"保存: 弹出对话框");
    bool ok = save_bitmap_dialog(o->hwnd, img, s.save_dir);
    logf(L"保存: 结果=%d", (int)ok);
    delete img;
    if (ok) PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

static void do_pin(Overlay* o) {
    Bitmap* img = render_result(o);
    if (!img) return;
    int x = o->shot.vx + o->sel.left;
    int y = o->shot.vy + o->sel.top;
    pin_create(img, x, y);
    PostMessageW(o->hwnd, WM_CLOSE, 0, 0);
}

// ------------------------------------------------------------------ OCR / 翻译
static void set_panel_loading(Overlay* o, const wchar_t* title) {
    o->panel = PM_LOADING;
    o->panelTitle = title;
    o->panelBody.clear();
    o->panelOriginal.clear();
    o->showOriginal = false;
    o->panelScroll = 0;
    place_panel(o);
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void start_ocr(Overlay* o, bool translateToo) {
    if (!o->hasSel) return;
    int sw = o->sel.right - o->sel.left;
    int sh = o->sel.bottom - o->sel.top;
    if (sw < 2 || sh < 2) return;

    Bitmap* crop = o->shot.bmp->Clone(o->sel.left, o->sel.top, sw, sh, PixelFormat32bppRGB);
    if (!crop) return;

    set_panel_loading(o, translateToo ? L"翻译" : L"提取文字");

    HWND hwnd = o->hwnd;
    std::shared_ptr<Overlay> keep = g_ov;
    Settings cfg = load_settings();

    std::thread([hwnd, keep, crop, translateToo, cfg]() {
        std::wstring text, err;
        bool ok = ocr_recognize(crop, text, err);
        delete crop;

        auto* msg = new JobMsg();
        if (!ok) {
            msg->ok = false;
            msg->err = err;
        } else if (text.empty()) {
            msg->ok = false;
            msg->err = L"没有识别到文字";
        } else {
            msg->ok = true;
            msg->text = text;
            if (translateToo) {
                TranslateResult tr = translate_text(text, cfg);
                msg->translated = true;
                msg->tr = tr;
            }
        }
        if (!PostMessageW(hwnd, WM_JJ_OCR, 0, (LPARAM)msg)) delete msg;
    }).detach();
}

static void start_translate(Overlay* o) {
    std::wstring text = o->showOriginal ? o->panelOriginal : o->panelBody;
    if (text.empty()) {
        start_ocr(o, true);
        return;
    }
    o->panel = PM_LOADING;
    o->panelTitle = L"翻译";
    InvalidateRect(o->hwnd, nullptr, FALSE);

    HWND hwnd = o->hwnd;
    std::shared_ptr<Overlay> keep = g_ov;
    Settings cfg = load_settings();

    std::thread([hwnd, keep, text, cfg]() {
        TranslateResult tr = translate_text(text, cfg);
        auto* msg = new JobMsg();
        msg->ok = tr.ok();
        msg->err = tr.err;
        msg->tr = tr;
        msg->translated = true;
        msg->text = text;
        if (!PostMessageW(hwnd, WM_JJ_TRANS, 0, (LPARAM)msg)) delete msg;
    }).detach();
}

static void on_ocr_done(Overlay* o, JobMsg* m) {
    if (!m) return;
    if (!m->ok) {
        o->panel = PM_ERROR;
        o->panelTitle = L"提取文字";
        o->panelBody = m->err.empty() ? L"识别失败" : m->err;
        o->panelOriginal.clear();
    } else {
        o->panel = PM_TEXT;
        o->panelTitle = m->translated ? L"翻译" : L"提取文字";
        o->panelBody = m->text;
        o->panelOriginal.clear();
        o->showOriginal = false;
        o->panelScroll = 0;
        if (m->translated) {
            if (m->tr.ok()) {
                o->panelOriginal = m->text;
                o->panelBody = m->tr.text;
            } else {
                o->panelTitle = L"提取文字";
                o->panelHint = L"翻译失败：" + m->tr.err;
            }
        }
    }
    place_panel(o);
    InvalidateRect(o->hwnd, nullptr, FALSE);
    delete m;
}

static void on_trans_done(Overlay* o, JobMsg* m) {
    if (!m) return;
    if (!m->ok) {
        o->panel = PM_ERROR;
        o->panelTitle = L"翻译";
        o->panelBody = m->err.empty() ? L"翻译失败" : m->err;
    } else {
        o->panel = PM_TEXT;
        o->panelTitle = L"翻译";
        o->panelOriginal = m->text;
        o->panelBody = m->tr.text;
        o->showOriginal = false;
        o->panelScroll = 0;
    }
    place_panel(o);
    InvalidateRect(o->hwnd, nullptr, FALSE);
    delete m;
}

// ------------------------------------------------------------------ 工具动作
static void set_tool(Overlay* o, int tool) {
    o->tool = (o->tool == tool) ? T_NONE : tool;
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void undo(Overlay* o) {
    if (o->anns.empty()) return;
    o->anns.pop_back();
    o->numberSeq = 0;
    for (auto& a : o->anns) {
        if (a.kind == AK_NUMBER) o->numberSeq = std::max(o->numberSeq, a.index);
    }
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void cycle_color(Overlay* o) {
    o->colorIdx = (o->colorIdx + 1) % N_PALETTE;
    InvalidateRect(o->hwnd, nullptr, FALSE);
}

static void begin_select(Overlay* o, POINT pt) {
    o->mode = 1;
    o->dragFrom = pt;
    o->sel.left = pt.x; o->sel.top = pt.y;
    o->sel.right = pt.x + 1; o->sel.bottom = pt.y + 1;
    o->hasSel = true;
    o->anns.clear();
    o->hasCur = false;
    o->numberSeq = 0;
    o->hasHover = false;
    o->tbVisible = false;
    o->panel = PM_HIDDEN;
    delete o->mosaic;
    o->mosaic = nullptr;
}

static void set_cursor_for(Overlay* o, POINT pt) {
    if (!o->hasSel) {
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return;
    }
    int h = (o->tool == T_NONE) ? handle_at(o, pt) : -1;
    if (h >= 0) {
        SetCursor(LoadCursorW(nullptr, HANDLE_CURSORS[h]));
        return;
    }
    if (ptInRect(o->sel, pt.x, pt.y)) {
        SetCursor(LoadCursorW(nullptr, o->tool == T_NONE ? IDC_SIZEALL : IDC_CROSS));
        return;
    }
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
}

// ------------------------------------------------------------------ 窗口过程
static LRESULT CALLBACK ov_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Overlay* o = (Overlay*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        o = (Overlay*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)o);
        o->hwnd = h;
        layout_toolbar(o);
        // 覆盖层本身不吃输入法：否则字母快捷键会被 IME 变成 VK_PACKET
        ImmAssociateContext(h, nullptr);
        return 0;
    }

    case WM_SETFOCUS:
        // 焦点回来时再解绑一次（子编辑框有自己的输入法上下文，不影响中文输入）
        ImmAssociateContext(h, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (o) paint(o);
        return 0;

    case WM_CHAR: {
        if (!o || !o->editing) break;
        wchar_t c = (wchar_t)wp;
        if (c == L'\r' || c == L'\n') { commit_text(o); return 0; }
        if (c == 27) { cancel_text(o); return 0; }
        if (c == L'\b') {
            if (!o->editText.empty()) o->editText.pop_back();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (c >= 32) {
            o->editText.push_back(c);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }

    case WM_IME_CHAR:
        if (o && o->editing) return 0;   // 已在 WM_IME_COMPOSITION 里取过
        break;

    case WM_IME_STARTCOMPOSITION: {
        if (o && o->editing) {
            HIMC imc = ImmGetContext(h);
            if (imc) {
                COMPOSITIONFORM cf = {};
                cf.dwStyle = CFS_POINT;
                cf.ptCurrentPos.x = o->sel.left + (LONG)o->editPos.X;
                cf.ptCurrentPos.y = o->sel.top + (LONG)o->editPos.Y + S(20);
                ImmSetCompositionWindow(imc, &cf);
                ImmReleaseContext(h, imc);
            }
        }
        break;
    }

    case WM_IME_COMPOSITION: {
        if (!o || !o->editing) break;
        if (lp & GCS_RESULTSTR) {
            o->editText += ime_string(h, GCS_RESULTSTR);
            o->composing.clear();
        }
        if (lp & GCS_COMPSTR) {
            o->composing = ime_string(h, GCS_COMPSTR);
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (o && o->editing && wp == TIMER_EDIT) {
            bool composing = ime_composing(h);
            if (GetAsyncKeyState(VK_RETURN) & 0x0001) {
                if (!composing) { commit_text(o); return 0; }
            }
            if (GetAsyncKeyState(VK_ESCAPE) & 0x0001) {
                if (!composing) { cancel_text(o); return 0; }
            }
            if (GetAsyncKeyState(VK_BACK) & 0x0001) {
                if (!composing) {
                    if (!o->editText.empty()) o->editText.pop_back();
                    InvalidateRect(h, nullptr, FALSE);
                }
            }
            return 0;
        }
        return 0;

    case WM_JJ_OCR:
        if (o) on_ocr_done(o, (JobMsg*)lp);
        return 0;

    case WM_JJ_TRANS:
        if (o) on_trans_done(o, (JobMsg*)lp);
        return 0;

    case WM_MOUSEMOVE: {
        if (!o) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        o->magPos = pt;
        o->magValid = true;

        if (o->mode == 1) {
            o->sel = normRECT(o->dragFrom.x, o->dragFrom.y, pt.x, pt.y);
            delete o->mosaic;
            o->mosaic = nullptr;
        } else if (o->mode == 2) {
            int dx = pt.x - o->dragFrom.x;
            int dy = pt.y - o->dragFrom.y;
            RECT r = o->selOrigin;
            if (o->edge & 1) r.left = o->selOrigin.left + dx;
            if (o->edge & 2) r.right = o->selOrigin.right + dx;
            if (o->edge & 4) r.top = o->selOrigin.top + dy;
            if (o->edge & 8) r.bottom = o->selOrigin.bottom + dy;
            r = normRECT(r.left, r.top, r.right, r.bottom);
            if (r.right - r.left >= S(MIN_SEL) && r.bottom - r.top >= S(MIN_SEL)) {
                o->sel = r;
                delete o->mosaic;
                o->mosaic = nullptr;
            }
        } else if (o->mode == 3) {
            int dx = pt.x - o->dragFrom.x;
            int dy = pt.y - o->dragFrom.y;
            o->sel = o->selOrigin;
            o->sel.left += dx; o->sel.right += dx;
            o->sel.top += dy; o->sel.bottom += dy;
            delete o->mosaic;
            o->mosaic = nullptr;
        } else if (o->mode == 4 && o->hasCur) {
            PointF local((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
            if (o->cur.kind == AK_RECT || o->cur.kind == AK_ELLIPSE) {
                o->cur.rect = normRect(o->cur.rect.X, o->cur.rect.Y, local.X, local.Y);
            } else if (o->cur.kind == AK_ARROW) {
                o->cur.b = local;
            } else {
                o->cur.pts.push_back(local);
            }
        } else {
            if (!o->hasSel) {
                RECT wr;
                if (window_rect_at(o->shot.vx + pt.x, o->shot.vy + pt.y, wr)) {
                    o->hoverWin.left = wr.left - o->shot.vx;
                    o->hoverWin.top = wr.top - o->shot.vy;
                    o->hoverWin.right = wr.right - o->shot.vx;
                    o->hoverWin.bottom = wr.bottom - o->shot.vy;
                    o->hasHover = true;
                } else {
                    o->hasHover = false;
                }
            } else {
                o->hasHover = false;
            }
        }

        if (o->tbVisible) {
            int hi = toolbar_hit(o, pt);
            if (hi != o->hoverItem) o->hoverItem = hi;
        } else {
            o->hoverItem = -1;
        }

        if (o->panel != PM_HIDDEN) {
            int b = panel_button_at(o, pt);
            o->panelBtnHover = (b >= 0) ? b : -1;
        }

        set_cursor_for(o, pt);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSELEAVE:
        if (o) { o->magValid = false; InvalidateRect(h, nullptr, FALSE); }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!o) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        commit_text(o);

        // 工具条
        int hi = toolbar_hit(o, pt);
        if (hi >= 0) {
            int id = o->items[hi].id;
            switch (id) {
            case T_UNDO:   undo(o); break;
            case T_SAVE:   do_save(o); break;
            case T_COPY:   do_copy(o); break;
            case T_PIN:    do_pin(o); break;
            case T_CANCEL: PostMessageW(h, WM_CLOSE, 0, 0); break;
            case T_OCR:    start_ocr(o, false); break;
            case T_TRANS:  start_translate(o); break;
            case T_COLOR:  cycle_color(o); break;
            default:       set_tool(o, id); break;
            }
            return 0;
        }

        // 面板按钮
        if (o->panel != PM_HIDDEN) {
            int b = panel_button_at(o, pt);
            if (b == 0) {
                const wchar_t* txt = panel_current_text(o);
                if (OpenClipboard(h)) {
                    EmptyClipboard();
                    size_t len = wcslen(txt);
                    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
                    if (mem) {
                        void* dst = GlobalLock(mem);
                        memcpy(dst, txt, (len + 1) * sizeof(wchar_t));
                        GlobalUnlock(mem);
                        SetClipboardData(CF_UNICODETEXT, mem);
                    }
                    CloseClipboard();
                }
                return 0;
            }
            if (b == 1) {
                if (o->panel == PM_TEXT && !o->panelOriginal.empty()) {
                    o->showOriginal = !o->showOriginal;
                    o->panelScroll = 0;
                    InvalidateRect(h, nullptr, FALSE);
                } else {
                    start_translate(o);
                }
                return 0;
            }
            if (b == 2) {
                o->panel = PM_HIDDEN;
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (b == -2) return 0;   // 点在面板空白处
            // 面板外点击：关掉面板继续
            o->panel = PM_HIDDEN;
        }

        if (!o->hasSel || o->sel.right - o->sel.left < S(MIN_SEL) ||
            o->sel.bottom - o->sel.top < S(MIN_SEL)) {
            begin_select(o, pt);
            return 0;
        }

        int edge = handle_at(o, pt);
        if (edge >= 0) {
            o->mode = 2;
            o->edge = (1 << edge) & 0x0F;
            // 0 nw->l+t, 1 n->t, 2 ne->r+t, 3 e->r, 4 se->r+b, 5 s->b, 6 sw->l+b, 7 w->l
            static const int EDGE_MASK[8] = {1 | 4, 4, 2 | 4, 2, 2 | 8, 8, 1 | 8, 1};
            o->edge = EDGE_MASK[edge];
            o->dragFrom = pt;
            o->selOrigin = o->sel;
            o->tbVisible = false;
            return 0;
        }

        if (ptInRect(o->sel, pt.x, pt.y)) {
            if (o->tool == T_RECT || o->tool == T_ELLIPSE || o->tool == T_ARROW ||
                o->tool == T_PEN || o->tool == T_MOSAIC) {
                o->mode = 4;
                PointF local((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
                o->cur = Ann();
                o->cur.color = PALETTE[o->colorIdx];
                o->cur.width = (o->tool == T_MOSAIC) ? SF(24.0f) : SF(3.0f);
                if (o->tool == T_RECT) { o->cur.kind = AK_RECT; o->cur.rect = normRect(local.X, local.Y, local.X, local.Y); }
                else if (o->tool == T_ELLIPSE) { o->cur.kind = AK_ELLIPSE; o->cur.rect = normRect(local.X, local.Y, local.X, local.Y); }
                else if (o->tool == T_ARROW) { o->cur.kind = AK_ARROW; o->cur.a = local; o->cur.b = local; }
                else if (o->tool == T_PEN) { o->cur.kind = AK_PEN; o->cur.pts.push_back(local); }
                else { o->cur.kind = AK_MOSAIC; o->cur.pts.push_back(local); ensure_mosaic(o); }
                o->hasCur = true;
                o->tbVisible = false;
                return 0;
            }
            if (o->tool == T_TEXT) {
                begin_text(o, pt);
                return 0;
            }
            if (o->tool == T_NUMBER) {
                o->numberSeq += 1;
                Ann a;
                a.kind = AK_NUMBER;
                a.color = PALETTE[o->colorIdx];
                a.pos = PointF((float)(pt.x - o->sel.left), (float)(pt.y - o->sel.top));
                a.index = o->numberSeq;
                o->anns.push_back(a);
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            o->mode = 3;
            o->dragFrom = pt;
            o->selOrigin = o->sel;
            o->tbVisible = false;
            return 0;
        }

        begin_select(o, pt);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!o) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (o->mode == 1) {
            o->sel = normRECT(o->dragFrom.x, o->dragFrom.y, pt.x, pt.y);
            if (o->sel.right - o->sel.left < S(MIN_SEL) ||
                o->sel.bottom - o->sel.top < S(MIN_SEL)) {
                if (o->hasHover) {
                    o->sel = o->hoverWin;
                } else {
                    o->sel.left = 0; o->sel.top = 0;
                    o->sel.right = o->shot.vw; o->sel.bottom = o->shot.vh;
                }
            }
            delete o->mosaic;
            o->mosaic = nullptr;
        } else if (o->mode == 4 && o->hasCur) {
            bool keep = true;
            if (o->cur.kind == AK_RECT || o->cur.kind == AK_ELLIPSE) {
                keep = o->cur.rect.Width > 2 || o->cur.rect.Height > 2;
            } else if (o->cur.kind == AK_ARROW) {
                float dx = o->cur.b.X - o->cur.a.X, dy = o->cur.b.Y - o->cur.a.Y;
                keep = (dx * dx + dy * dy) > 4;
            }
            if (keep) o->anns.push_back(o->cur);
            o->hasCur = false;
        }
        o->mode = 0;
        o->edge = 0;
        if (o->hasSel) {
            place_toolbar(o);
            o->tbVisible = true;
            o->hoverItem = -1;
        }
        set_cursor_for(o, pt);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        if (o && o->hasSel) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (ptInRect(o->sel, pt.x, pt.y)) do_copy(o);
        }
        return 0;

    case WM_RBUTTONUP:
        if (o) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;

    case WM_MOUSEWHEEL: {
        if (!o || o->panel == PM_HIDDEN) return 0;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        if (!ptInRectF(o->panelRect, (float)pt.x, (float)pt.y)) return 0;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        o->panelScroll -= delta / 120.0f * SF(48.0f);
        if (o->panelScroll < 0) o->panelScroll = 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }

    case WM_KEYDOWN: {
        if (!o) return 0;
        if (o->editing) {
            // 回车 / 退格 / Esc 由键盘钩子处理，其余交给输入法
            return 0;
        }

        bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        switch (wp) {
        case VK_ESCAPE:
            if (o->panel != PM_HIDDEN) {
                o->panel = PM_HIDDEN;
                InvalidateRect(h, nullptr, FALSE);
            } else {
                PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        case VK_RETURN:
            do_copy(o);
            return 0;
        default: break;
        }

        if (ctrl) {
            switch (wp) {
            case 'C': do_copy(o); return 0;
            case 'S': do_save(o); return 0;
            case 'Z': undo(o); return 0;
            case 'T': do_pin(o); return 0;
            default: break;
            }
        } else {
            if (wp == 'C') { cycle_color(o); return 0; }
            if (wp == 'R') { set_tool(o, T_RECT); return 0; }
            if (wp == 'E') { set_tool(o, T_ELLIPSE); return 0; }
            if (wp == 'A') { set_tool(o, T_ARROW); return 0; }
            if (wp == 'P') { set_tool(o, T_PEN); return 0; }
            if (wp == 'M') { set_tool(o, T_MOSAIC); return 0; }
            if (wp == 'T') { set_tool(o, T_TEXT); return 0; }
            if (wp == 'N') { set_tool(o, T_NUMBER); return 0; }
            if (wp == 'O') { start_ocr(o, false); return 0; }
            if (wp == 'F') { start_translate(o); return 0; }
        }

        if (o->hasSel && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)) {
            int step = shift ? S(10) : 1;
            int dx = 0, dy = 0;
            if (wp == VK_LEFT) dx = -step;
            else if (wp == VK_RIGHT) dx = step;
            else if (wp == VK_UP) dy = -step;
            else dy = step;
            o->sel.left += dx; o->sel.right += dx;
            o->sel.top += dy; o->sel.bottom += dy;
            if (o->tbVisible) place_toolbar(o);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            if (o) set_cursor_for(o, pt);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (o) {
            o->alive = false;
            if (o->editing) edit_poll_stop(h);
            if (o->himc) {
                ImmAssociateContext(h, nullptr);
                ImmDestroyContext(o->himc);
                o->himc = nullptr;
            }
            delete o->mosaic;
            o->mosaic = nullptr;
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (g_ov && g_ov->hwnd == h) {
            g_ov.reset();
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------------------ 对外接口
static void ensure_class() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = ov_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = OV_CLASS;
    RegisterClassExW(&wc);
    done = true;
}

bool overlay_is_open() {
    return g_ov && g_ov->alive;
}

bool overlay_open() {
    if (overlay_is_open()) {
        logf(L"截图被忽略：覆盖层已存在");
        return false;
    }
    ensure_class();

    auto o = std::make_shared<Overlay>();
    if (!capture_desktop(o->shot)) {
        logf(L"抓屏失败");
        return false;
    }
    logf(L"抓屏 %dx%d @ (%d,%d)", o->shot.vw, o->shot.vh, o->shot.vx, o->shot.vy);

    g_ov = o;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OV_CLASS, L"简截",
                                WS_POPUP, o->shot.vx, o->shot.vy, o->shot.vw, o->shot.vh,
                                nullptr, nullptr, GetModuleHandleW(nullptr), o.get());
    if (!hwnd) {
        g_ov.reset();
        logf(L"创建覆盖层失败 %lu", GetLastError());
        return false;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    force_foreground(hwnd);
    HWND fg = GetForegroundWindow();
    logf(L"覆盖层已显示 fg=%p hwnd=%p match=%d", fg, hwnd, (int)(fg == hwnd));
    return true;
}

void overlay_destroy() {
    if (g_ov && g_ov->hwnd) {
        DestroyWindow(g_ov->hwnd);
    }
    g_ov.reset();
    if (g_backBuf) { DeleteObject(g_backBuf); g_backBuf = nullptr; }
    if (g_backDC) { DeleteDC(g_backDC); g_backDC = nullptr; }
    g_bw = g_bh = 0;
}

void overlay_request_capture() {
    overlay_open();
}
