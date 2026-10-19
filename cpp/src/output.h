#pragma once
#include "common.h"

// 把位图放进剪贴板（CF_DIB + PNG 双格式）
bool copy_bitmap_to_clipboard(Gdiplus::Bitmap* bmp);

// 弹出保存对话框并写盘；成功返回 true
bool save_bitmap_dialog(HWND owner, Gdiplus::Bitmap* bmp, const std::wstring& preferDir);

// 默认保存目录：图片\简截
std::wstring default_save_dir();
