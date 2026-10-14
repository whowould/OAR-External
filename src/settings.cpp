#include "settings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace settings
{
namespace
{
struct Writer
{
    std::string out;
    void line(const char* key, const std::string& value)
    {
        out += key;
        out += '=';
        out += value;
        out += '\n';
    }
    void operator()(const char* k, bool& v) { line(k, v ? "1" : "0"); }
    void operator()(const char* k, int& v) { line(k, std::to_string(v)); }
    void operator()(const char* k, ImGuiKey& v) { line(k, std::to_string((int)v)); }
    void operator()(const char* k, float& v)
    {
        char b[32];
        snprintf(b, sizeof(b), "%g", v);
        line(k, b);
    }
    void operator()(const char* k, float (&v)[4])
    {
        char b[96];
        snprintf(b, sizeof(b), "%g,%g,%g,%g", v[0], v[1], v[2], v[3]);
        line(k, b);
    }
};

struct Reader
{
    const std::unordered_map<std::string, std::string>& kv;
    int hits = 0;
    const std::string* find(const char* k)
    {
        auto it = kv.find(k);
        if (it == kv.end()) return nullptr;
        hits++;
        return &it->second;
    }
    void operator()(const char* k, bool& v) { if (auto s = find(k)) v = *s == "1"; }
    void operator()(const char* k, int& v) { if (auto s = find(k)) v = atoi(s->c_str()); }
    void operator()(const char* k, ImGuiKey& v) { if (auto s = find(k)) v = (ImGuiKey)atoi(s->c_str()); }
    void operator()(const char* k, float& v) { if (auto s = find(k)) v = (float)atof(s->c_str()); }
    void operator()(const char* k, float (&v)[4])
    {
        if (auto s = find(k))
        {
            float t[4];
            if (sscanf(s->c_str(), "%f,%f,%f,%f", &t[0], &t[1], &t[2], &t[3]) == 4)
                for (int i = 0; i < 4; i++) v[i] = t[i];
        }
    }
};
}

std::string ToText(const Settings& s)
{
    Settings copy = s;
    Writer w;
    Visit(copy, w);
    return w.out;
}

bool FromText(std::string_view text, Settings& out)
{
    std::unordered_map<std::string, std::string> kv;
    for (size_t pos = 0; pos < text.size();)
    {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        kv.emplace(std::string(line.substr(0, eq)), std::string(line.substr(eq + 1)));
    }
    Settings s;
    Reader r{ kv };
    Visit(s, r);
    if (r.hits == 0) return false;
    Sanitize(s);
    out = s;
    return true;
}

void Sanitize(Settings& s)
{
    const Settings defaults;
    const auto limit = [](float v, float lo, float hi, float fallback) { return std::isfinite(v) ? std::min(std::max(v, lo), hi) : fallback; };
    const auto key = [](ImGuiKey k, ImGuiKey fallback) {
        return k == ImGuiKey_None || (k >= ImGuiKey_NamedKey_BEGIN && k < ImGuiKey_NamedKey_END) ? k : fallback;
    };
    for (float* c : { s.guardColor, s.guardAlertColor, s.guardDeadColor, s.camColor, s.camEmpColor, s.camDeadColor, s.camSeenColor, s.phoneColor, s.terminalColor })
        for (int i = 0; i < 4; i++)
            c[i] = limit(c[i], 0.0f, 1.0f, 1.0f);

    s.maxDistance = limit(s.maxDistance, 0.0f, 1000.0f, defaults.maxDistance);
    s.camConeLength = limit(s.camConeLength, 1.0f, 100.0f, defaults.camConeLength);
    s.checkinBelow = limit(s.checkinBelow, 0.0f, 60.0f, defaults.checkinBelow);
    s.boxThickness = limit(s.boxThickness, 0.5f, 8.0f, defaults.boxThickness);
    s.textScale = limit(s.textScale, 0.5f, 3.0f, defaults.textScale);
    s.opacity = limit(s.opacity, 0.1f, 1.0f, defaults.opacity);
    s.rounding = limit(s.rounding, 0.0f, 24.0f, defaults.rounding);
    s.accent = std::min(std::max(s.accent, 0), 63); // the theme clamps to its preset count
    s.espKey = key(s.espKey, defaults.espKey);
    s.checkinKey = key(s.checkinKey, defaults.checkinKey);
    s.checkinAutoKey = key(s.checkinAutoKey, defaults.checkinAutoKey);
}
}
