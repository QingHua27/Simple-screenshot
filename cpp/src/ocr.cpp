#include "ocr.h"

#include <mutex>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Media::Ocr;
using namespace Windows::Globalization;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Storage::Streams;

// WinRT 的激活工厂缓存是进程级共享的，多个 OCR 线程同时首次激活同一类型时
// 存在竞态（曾出现过缓存的工厂指针失效、调用虚表时 0xc0000005 闪退）。
// 这里把整段 WinRT 调用串行化，宁可慢一点也不能崩。
static std::mutex g_ocrLock;

// 中日韩字符 / 全角标点：这些字符之间的空格是 OCR 的噪声
static bool is_wide(wchar_t c) {
    unsigned u = (unsigned)c;
    return (u >= 0x4E00 && u <= 0x9FFF) ||
           (u >= 0x3400 && u <= 0x4DBF) ||
           (u >= 0x3000 && u <= 0x303F) ||
           (u >= 0xFF00 && u <= 0xFFEF) ||
           (u >= 0x2018 && u <= 0x201D);
}

static std::wstring tidy(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'\uFF0E') c = L'\u00B7';   // ．-> ·
        if (c == L' ') {
            wchar_t prev = out.empty() ? 0 : out.back();
            wchar_t next = 0;
            for (size_t j = i + 1; j < in.size(); ++j) {
                if (in[j] != L' ') { next = in[j]; break; }
            }
            if (is_wide(prev) && is_wide(next)) continue;
            if (prev == 0) continue;
        }
        out.push_back(c);
    }
    return out;
}

bool ocr_recognize(Gdiplus::Bitmap* bmp, std::wstring& text, std::wstring& err) {
    text.clear();
    err.clear();
    if (!bmp) { err = L"没有可识别的图像"; return false; }

    std::lock_guard<std::mutex> guard(g_ocrLock);

    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (hresult_error const& e) {
        // 已经初始化过就忽略；但要留痕，COM 状态不对时后面的调用容易出问题
        logf(L"OCR 线程 COM 初始化返回 0x%08X（继续）", (unsigned)e.code());
    } catch (...) {
        logf(L"OCR 线程 COM 初始化失败（继续）");
    }

    try {
        int w = (int)bmp->GetWidth();
        int h = (int)bmp->GetHeight();
        if (w < 2 || h < 2) { err = L"选区太小"; return false; }

        // 小图放大 2 倍，中文小字识别率提升明显
        std::unique_ptr<Gdiplus::Bitmap> scaled;
        Gdiplus::Bitmap* src = bmp;
        if (w <= 1400 && h <= 1000) {
            scaled.reset(new Gdiplus::Bitmap(w * 2, h * 2, PixelFormat32bppRGB));
            Gdiplus::Graphics g(scaled.get());
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            g.DrawImage(bmp, Gdiplus::Rect(0, 0, w * 2, h * 2), 0, 0, w, h, Gdiplus::UnitPixel);
            src = scaled.get();
            w *= 2;
            h *= 2;
        }

        Gdiplus::Rect lockRect(0, 0, w, h);
        Gdiplus::BitmapData data;
        if (src->LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
            err = L"读取图像失败";
            return false;
        }
        std::vector<uint8_t> px((size_t)w * h * 4);
        for (int y = 0; y < h; ++y) {
            memcpy(px.data() + (size_t)y * w * 4,
                   (uint8_t*)data.Scan0 + (size_t)y * data.Stride,
                   (size_t)w * 4);
        }
        src->UnlockBits(&data);

        DataWriter dw;
        dw.WriteBytes(array_view<uint8_t const>(px.data(), px.data() + px.size()));
        auto buf = dw.DetachBuffer();
        auto sb = SoftwareBitmap::CreateCopyFromBuffer(buf, BitmapPixelFormat::Bgra8, w, h,
                                                       BitmapAlphaMode::Premultiplied);

        OcrEngine engine{nullptr};
        engine = OcrEngine::TryCreateFromLanguage(Language(L"zh-Hans-CN"));
        if (!engine) engine = OcrEngine::TryCreateFromUserProfileLanguages();
        if (!engine) { err = L"系统未安装中文 OCR 语言包"; return false; }

        auto result = engine.RecognizeAsync(sb).get();
        std::wstring out;
        for (auto const& line : result.Lines()) {
            if (!out.empty()) out += L"\n";
            out += line.Text().c_str();
        }
        text = tidy(out);
        return true;
    } catch (hresult_error const& e) {
        wchar_t buf[128];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"OCR 失败 0x%08X", (unsigned)e.code());
        err = buf;
        logf(L"ocr error 0x%08X: %s", (unsigned)e.code(), e.message().c_str());
        return false;
    } catch (...) {
        err = L"OCR 失败";
        return false;
    }
}
