#include "settings.h"

const HotkeyPreset HOTKEY_PRESETS[6] = {
    {L"Alt+A",        MOD_ALT,                  0x41},
    {L"Ctrl+Alt+A",   MOD_CONTROL | MOD_ALT,    0x41},
    {L"Ctrl+Shift+A", MOD_CONTROL | MOD_SHIFT,  0x41},
    {L"Alt+Z",        MOD_ALT,                  0x5A},
    {L"Ctrl+Alt+Z",   MOD_CONTROL | MOD_ALT,    0x5A},
    {L"Alt+F9",       MOD_ALT,                  0x78},
};
const int N_HOTKEY_PRESETS = 6;

std::wstring settings_path() {
    return appdata_dir() + L"\\settings.json";
}

bool resolve_hotkey(const std::wstring& name, UINT& mods, UINT& vk) {
    for (int i = 0; i < N_HOTKEY_PRESETS; ++i) {
        if (name == HOTKEY_PRESETS[i].name) {
            mods = HOTKEY_PRESETS[i].mods;
            vk = HOTKEY_PRESETS[i].vk;
            return true;
        }
    }
    mods = MOD_ALT;
    vk = 0x41;
    return false;
}

// ------------------------------------------------------------ 极简 JSON 读取
static std::string read_all_utf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::string out;
    if (sz.QuadPart > 0 && sz.QuadPart < (1 << 20)) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr);
        out.resize(got);
    }
    CloseHandle(h);
    return out;
}

static std::string get_json_str(const std::string& js, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = js.find(pat);
    if (p == std::string::npos) return std::string();
    p = js.find(':', p + pat.size());
    if (p == std::string::npos) return std::string();
    p = js.find('"', p);
    if (p == std::string::npos) return std::string();
    std::string out;
    for (size_t i = p + 1; i < js.size(); ++i) {
        char c = js[i];
        if (c == '\\' && i + 1 < js.size()) {
            char n = js[++i];
            switch (n) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 < js.size()) {
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        char h = js[++i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    }
                    wchar_t wbuf[2] = {(wchar_t)cp, 0};
                    out += w_to_u8(wbuf);
                }
                break;
            }
            default: out.push_back(n);
            }
        } else if (c == '"') {
            break;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

Settings load_settings() {
    Settings s;
    std::string js = read_all_utf8(settings_path());
    if (js.empty()) return s;
    std::string v;
    if (!(v = get_json_str(js, "hotkey")).empty())     s.hotkey = u8_to_w(v);
    if (!(v = get_json_str(js, "save_dir")).empty())   s.save_dir = u8_to_w(v);
    if (!(v = get_json_str(js, "llm_base")).empty())   s.llm_base = u8_to_w(v);
    if (!(v = get_json_str(js, "llm_model")).empty())  s.llm_model = u8_to_w(v);
    v = get_json_str(js, "llm_key");
    s.llm_key = u8_to_w(v);   // 允许为空
    return s;
}

void save_settings(const Settings& s) {
    std::string js = "{\n";
    js += "  \"hotkey\": \"" + json_escape(w_to_u8(s.hotkey)) + "\",\n";
    js += "  \"save_dir\": \"" + json_escape(w_to_u8(s.save_dir)) + "\",\n";
    js += "  \"llm_base\": \"" + json_escape(w_to_u8(s.llm_base)) + "\",\n";
    js += "  \"llm_key\": \"" + json_escape(w_to_u8(s.llm_key)) + "\",\n";
    js += "  \"llm_model\": \"" + json_escape(w_to_u8(s.llm_model)) + "\"\n";
    js += "}\n";

    std::wstring path = settings_path();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, js.data(), (DWORD)js.size(), &wrote, nullptr);
    CloseHandle(h);
}
