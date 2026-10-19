#pragma once
#include "common.h"

// 用 Windows.Media.Ocr 识别位图文字。成功返回 true。
bool ocr_recognize(Gdiplus::Bitmap* bmp, std::wstring& text, std::wstring& err);
