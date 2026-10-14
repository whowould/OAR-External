#include "menu/internal.h"

#include "imgui_internal.h"

#include <cstdio>
#include <cstring>

using namespace theme;
using namespace ui;

namespace menu
{
namespace
{
constexpr const char* kClipboardHeader = "OAR-CONFIG\n";

void Refresh()
{
    S.configs = store::List();
    S.configsStale = false;
    S.selConfig = ImClamp(S.selConfig, 0, ImMax(0, (int)S.configs.size() - 1));
}

bool ConfigRow(int i)
{
    const store::Entry& c = S.configs[i];
    bool sel = i == S.selConfig, active = c.name == S.activeConfig;
    ImGui::PushID(i);
    bool clicked = ImGui::Selectable("##cfg", sel, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, dp(42)));
    bool dbl = clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cy = (a.y + b.y) * 0.5f;
    ImVec2 ic(a.x + dp(16), cy);
    dl->AddRectFilled(ic - ImVec2(dp(11), dp(11)), ic + ImVec2(dp(11), dp(11)), Col(active ? WithAlpha(P.accent, 0.18f) : P.field), dp(6));
    DrawIcon(dl, active ? icon::Star : icon::Folder, ic, dp(11), Col(active ? P.accent : P.textDim));

    float right = b.x - dp(8);
    if (active)
    {
        const char* t = "ACTIVE";
        ImVec2 ts = Measure(F.semibold, dp(kTextXS), t);
        ImVec2 ba(b.x - ts.x - dp(18), cy - ts.y * 0.5f - dp(2));
        dl->AddRectFilled(ba, ba + ImVec2(ts.x + dp(10), ts.y + dp(4)), Col(P.success, 0.14f), dp(8));
        TextAt(dl, F.semibold, dp(kTextXS), ba + ImVec2(dp(5), dp(2)), Col(P.success), t);
        right = ba.x - dp(8);
    }
    float tx = a.x + dp(36);
    TextFit(dl, F.semibold, dp(kTextM), ImVec2(tx, a.y + dp(3)), Col(P.text), c.name.c_str(), right - tx);
    char ago[16], meta[48];
    AgoFromAge(store::AgeSeconds(c), ago, sizeof(ago));
    snprintf(meta, sizeof(meta), !strcmp(ago, "now") ? "saved just now" : "saved %s ago", ago);
    TextFit(dl, F.regular, dp(kTextXS), ImVec2(tx, a.y + dp(22)), Col(P.textMute), meta, right - tx);
    ImGui::PopID();
    if (clicked)
        S.selConfig = i;
    return dbl;
}

void LoadConfig(const std::string& name)
{
    Settings loaded;
    if (store::Load(name, loaded))
    {
        S.cfg = loaded;
        ApplyTheme();
        S.activeConfig = name;
        Notice(Toast_Success, icon::Folder, "Config loaded", name.c_str());
    }
    else
        Notify(Toast_Error, "Could not read", name.c_str());
}

void SaveConfig(const std::string& name, const char* verb)
{
    if (store::Save(name, S.cfg))
    {
        S.activeConfig = store::Sanitize(name);
        S.configsStale = true;
        Notice(Toast_Success, icon::Save, verb, S.activeConfig.c_str());
    }
    else
        Notify(Toast_Error, "Could not write", name.c_str());
}

// "Name copy", "Name copy 2", ... until the name is free.
std::string FreeName(const std::string& base)
{
    if (!store::Exists(base)) return base;
    for (int n = 1;; n++)
    {
        std::string candidate = base + (n == 1 ? std::string(" copy") : " copy " + std::to_string(n));
        if (!store::Exists(candidate)) return candidate;
    }
}

void DeleteDialog()
{
    if (!S.pendingDelete.empty())
        ImGui::OpenPopup("##delete");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(dp(300), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(dp(16), dp(16)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, RGroup());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Hex(0x14151C));
    bool open = ImGui::BeginPopupModal("##delete", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (!open)
        return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    dl->AddCircleFilled(p + ImVec2(dp(16), dp(16)), dp(16), Col(P.danger, 0.15f));
    DrawIcon(dl, icon::Trash, p + ImVec2(dp(16), dp(16)), dp(13), Col(P.danger));
    TextAt(dl, F.semibold, dp(kTextL), p + ImVec2(dp(44), 0), Col(P.text), "Delete config?");
    TextFit(dl, F.regular, dp(kTextS), p + ImVec2(dp(44), dp(19)), Col(P.textDim), S.pendingDelete.c_str(), w - dp(44));
    ImGui::Dummy(ImVec2(0, dp(42)));
    float bw = (w - dp(8)) * 0.5f;
    if (Button("Cancel", 0, Btn_Ghost, ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        S.pendingDelete.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0, dp(8));
    if (Button("Delete", icon::Trash, Btn_Danger, ImVec2(bw, 0)))
    {
        if (store::Remove(S.pendingDelete))
        {
            if (S.activeConfig == S.pendingDelete) S.activeConfig.clear();
            Notice(Toast_Error, icon::Trash, "Config deleted", S.pendingDelete.c_str());
        }
        S.configsStale = true;
        S.pendingDelete.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
}

void PageConfigs()
{
    if (S.configsStale)
        Refresh();

    if (!BeginColumns("##cfg", 0.55f))
        return;
    if (BeginGroup("##g_list", icon::Database, "Configs"))
    {
        if (S.configs.empty())
            Note("No saved configs yet. Create one from your current settings below.");
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, dp(2)));
        for (int i = 0; i < (int)S.configs.size(); i++)
            if (ConfigRow(i))
                LoadConfig(S.configs[i].name);
        ImGui::PopStyleVar();
        Divider();
        float bw = dp(88);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(dp(10), (dp(32) - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, P.borderStrong);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - dp(8));
        bool enter = ImGui::InputTextWithHint("##newcfg", "New config name", S.newConfig, sizeof(S.newConfig), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::SameLine(0, dp(8));
        if (Button("Create", icon::Plus, Btn_Primary, ImVec2(bw, dp(32))) || enter)
        {
            if (!store::Sanitize(S.newConfig).empty())
            {
                SaveConfig(S.newConfig, "Config created");
                Refresh();
                for (int i = 0; i < (int)S.configs.size(); i++)
                    if (S.configs[i].name == S.activeConfig) S.selConfig = i;
                S.newConfig[0] = 0;
            }
            else
                Notify(Toast_Warning, "Name required");
        }
    }
    EndGroup();
    NextColumn();

    const bool have = S.selConfig >= 0 && S.selConfig < (int)S.configs.size();
    if (BeginGroup("##g_detail", icon::Info, "Details") && !have)
        Note("Select a config to load, copy or delete it.");
    if (have)
    {
        const store::Entry& c = S.configs[S.selConfig];
        ImDrawList* dl = ImGui::GetWindowDrawList();
        TextFit(dl, F.semibold, dp(kTextL), ImGui::GetCursorScreenPos(), Col(P.text), c.name.c_str(), ImGui::GetContentRegionAvail().x);
        ImGui::Dummy(ImVec2(0, dp(22)));
        char ago[16], buf[48];
        AgoFromAge(store::AgeSeconds(c), ago, sizeof(ago));
        snprintf(buf, sizeof(buf), "%s ago", ago);
        KeyValue("Saved", strcmp(ago, "now") ? buf : "just now");
        KeyValue("Status", c.name == S.activeConfig ? "Active" : "Saved");
        ImGui::Dummy(ImVec2(0, dp(4)));
        float bw = (ImGui::GetContentRegionAvail().x - dp(8)) * 0.5f;
        if (Button("Load", icon::Refresh, Btn_Primary, ImVec2(bw, 0)))
            LoadConfig(c.name);
        ImGui::SameLine(0, dp(8));
        if (Button("Overwrite", icon::Save, Btn_Ghost, ImVec2(bw, 0)))
            SaveConfig(c.name, "Config saved");
        if (Button("Copy", icon::Copy, Btn_Ghost, ImVec2(bw, 0)))
        {
            Settings loaded;
            const std::string copy = FreeName(c.name);
            if (store::Load(c.name, loaded) && store::Save(copy, loaded))
            {
                S.configsStale = true;
                Log(icon::Copy, "Duplicated %s", c.name.c_str());
            }
        }
        ImGui::SameLine(0, dp(8));
        if (Button("Delete", icon::Trash, Btn_Danger, ImVec2(bw, 0)))
            S.pendingDelete = c.name;
    }
    EndGroup();

    if (BeginGroup("##g_share", icon::Copy, "Share"))
    {
        float bw = (ImGui::GetContentRegionAvail().x - dp(8)) * 0.5f;
        if (Button("Export", icon::Copy, Btn_Ghost, ImVec2(bw, 0)) && have)
        {
            Settings loaded;
            if (store::Load(S.configs[S.selConfig].name, loaded))
            {
                ImGui::SetClipboardText((std::string(kClipboardHeader) + settings::ToText(loaded)).c_str());
                Notice(Toast_Success, icon::Copy, "Copied to clipboard", S.configs[S.selConfig].name.c_str());
            }
        }
        ImGui::SameLine(0, dp(8));
        if (Button("Import", icon::Plus, Btn_Ghost, ImVec2(bw, 0)))
        {
            const char* clip = ImGui::GetClipboardText();
            Settings imported;
            if (clip && !strncmp(clip, kClipboardHeader, strlen(kClipboardHeader)) && settings::FromText(clip + strlen(kClipboardHeader), imported))
            {
                const std::string name = FreeName("Imported");
                if (store::Save(name, imported))
                {
                    S.configsStale = true;
                    Notice(Toast_Success, icon::Plus, "Config imported", name.c_str());
                }
            }
            else
                Notify(Toast_Warning, "Nothing to import", "clipboard");
        }
    }
    EndGroup();
    EndColumns();

    DeleteDialog();
}
}
