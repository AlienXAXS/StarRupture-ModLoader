#include "pch.h"
#include "toast.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "imgui/imgui.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace UI::Toast
{
    // On screen for kLifeSeconds from the first frame it is drawn, the last
    // kFadeSeconds of it fading out.
    static constexpr double kLifeSeconds = 7.0;
    static constexpr double kFadeSeconds = 1.5;
    static constexpr size_t kMaxToasts   = 6;    // a burst of crashes drops the oldest

    struct Entry
    {
        std::string key;
        std::string text;
        unsigned    id      = 0;      // stable window id while it is on screen
        double      shownAt = -1.0;   // ImGui::GetTime() of its first frame; -1 until drawn
    };

    // s_mutex guards s_toasts: Show runs on the game thread, Render on the render thread.
    static std::mutex         s_mutex;
    static std::vector<Entry> s_toasts;   // oldest first
    static unsigned           s_nextId = 1;

    void Show(const std::string& key, const std::string& text)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto it = s_toasts.begin(); it != s_toasts.end(); ++it)
        {
            if (it->key == key)
            {
                s_toasts.erase(it);
                break;
            }
        }
        if (s_toasts.size() >= kMaxToasts)
            s_toasts.erase(s_toasts.begin());
        s_toasts.push_back({ key, text, s_nextId++, -1.0 });
    }

    void Render()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_toasts.empty())
            return;

        const double now = ImGui::GetTime();
        ImGuiIO& io = ImGui::GetIO();

        // Bottom-right corner, 10 px from the edges, the same box as the
        // loader's own overlay in the bottom-left (overlay.cpp). Newest at the
        // bottom, older ones stacked above it.
        const float PAD = 10.0f;
        float bottom = io.DisplaySize.y - PAD;

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoInputs;

        for (auto it = s_toasts.rbegin(); it != s_toasts.rend(); ++it)
        {
            if (it->shownAt < 0.0)
                it->shownAt = now;
            const double left  = kLifeSeconds - (now - it->shownAt);
            const float  alpha = left >= kFadeSeconds ? 1.0f : static_cast<float>(left > 0.0 ? left / kFadeSeconds : 0.0);
            if (alpha <= 0.0f)
                continue;

            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - PAD, bottom), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::SetNextWindowSize(ImVec2(0, 0));

            char id[32];
            sprintf_s(id, "##loader_toast_%u", it->id);

            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
            if (ImGui::Begin(id, nullptr, flags))
            {
                // Wrap long lines rather than run a toast across the screen.
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
                ImGui::TextUnformatted(it->text.c_str());
                ImGui::PopTextWrapPos();
                bottom -= ImGui::GetWindowHeight() + ImGui::GetStyle().ItemSpacing.y;
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        s_toasts.erase(std::remove_if(s_toasts.begin(), s_toasts.end(),
                                      [now](const Entry& e) { return e.shownAt >= 0.0 && now - e.shownAt >= kLifeSeconds; }),
                       s_toasts.end());
    }
}

#endif // MODLOADER_CLIENT_BUILD
