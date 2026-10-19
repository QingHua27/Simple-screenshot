#pragma once
#include "common.h"

struct Settings {
    std::wstring hotkey    = L"Alt+A";
    std::wstring save_dir;
    std::wstring llm_base  = L"https://api.deepseek.com/v1";
    std::wstring llm_key;
    std::wstring llm_model = L"deepseek-chat";
};

Settings load_settings();
void save_settings(const Settings& s);
std::wstring settings_path();

struct HotkeyPreset {
    const wchar_t* name;
    UINT mods;
    UINT vk;
};
extern const HotkeyPreset HOTKEY_PRESETS[6];
extern const int N_HOTKEY_PRESETS;

bool resolve_hotkey(const std::wstring& name, UINT& mods, UINT& vk);
