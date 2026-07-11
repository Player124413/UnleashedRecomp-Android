#include <stdafx.h>
#include "touch_controls.h"
#include "imgui_utils.h"
#include "button_guide.h"
#include "game_window.h"
#include <gpu/video.h>
#include <sdl_listener.h>
#include <user/config.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_set>

#ifdef __ANDROID__
#include "os/android/storage_android.h"
#endif

// ---------------------------------------------------------------------------
// On-screen touch controls with a drag-to-arrange layout editor.
//
// Every control (stick, A/B/X/Y, LB/RB, LT/RT, Start/Back) has its own editable
// position. The Android launcher requests the editor for one launch; each control
// can be dragged, the whole set resized, or reset to defaults. The layout is saved
// to <data>/touch_layout.ini and reloaded on the next launch.
//
// Positions are fractions of the viewport; X of the viewport width, Y of the
// viewport height. Sizes are fractions of the viewport height so the layout keeps
// its proportions across resolutions/aspect ratios.
// ---------------------------------------------------------------------------

namespace
{
    // Logical controls. Order is load-bearing: it must match kDefault, kIcon and
    // the save-file keys below.
    enum
    {
        TC_STICK = 0,
        TC_A, TC_B, TC_X, TC_Y,
        TC_LB, TC_RB,
        TC_LT, TC_RT,
        TC_START, TC_BACK,
        TC_COUNT
    };

    // Base sizes (fractions of viewport height), multiplied by the global scale.
    constexpr float STICK_BASE_R  = 0.150f;
    constexpr float STICK_THUMB_R = 0.070f;
    constexpr float STICK_ZONE_R  = 0.210f;
    constexpr float FACE_BTN_R    = 0.058f;
    constexpr float SHOULDER_HW   = 0.075f;
    constexpr float SHOULDER_HH   = 0.036f;
    constexpr float MENU_HW       = 0.032f;
    constexpr float MENU_HH       = 0.032f;

    constexpr float SCALE_MIN = 0.60f;
    constexpr float SCALE_MAX = 1.60f;

    struct Layout
    {
        float x[TC_COUNT];
        float y[TC_COUNT];
        float scale;
    };

    // Default layout. X-offsets that were expressed in height units in the old
    // fixed layout are baked here at the reference 2400x1080 aspect.
    const Layout kDefault =
    {
        //  stick    A       B       X       Y      LB      RB      LT      RT     Start   Back
        {  0.135f, 0.865f, 0.914f, 0.816f, 0.865f, 0.075f, 0.925f, 0.075f, 0.925f, 0.555f, 0.445f },
        {  0.760f, 0.868f, 0.760f, 0.760f, 0.652f, 0.090f, 0.090f, 0.185f, 0.185f, 0.070f, 0.070f },
        1.0f
    };

    Layout g_layout = kDefault;

    const EButtonIcon kIcon[TC_COUNT] =
    {
        EButtonIcon::A, // stick (unused)
        EButtonIcon::A, EButtonIcon::B, EButtonIcon::X, EButtonIcon::Y,
        EButtonIcon::LB, EButtonIcon::RB,
        EButtonIcon::LT, EButtonIcon::RT,
        EButtonIcon::Start, EButtonIcon::Back
    };

    const char* const kKey[TC_COUNT] =
    {
        "stick", "a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "back"
    };

    // ---- Finger tracking (SDL touch thread) --------------------------------

    struct Finger
    {
        SDL_FingerID id;
        float nx; // normalised [0,1] over the window
        float ny;
    };

    std::mutex g_mutex;
    std::vector<Finger> g_fingers;

    std::atomic<bool> g_autoVisible{ true };
    XAMINPUT_GAMEPAD g_state{};

    // Finger driving the analog stick (-1 = none). Render-thread only.
    SDL_FingerID g_stickFingerId = (SDL_FingerID)-1;

    // ---- Editor state (render thread only) ---------------------------------

    bool g_edit = false;
    int  g_dragElem = -1;                       // control being dragged (-1 = none)
    SDL_FingerID g_dragFinger = (SDL_FingerID)-1;
    float g_grabX = 0.0f, g_grabY = 0.0f;       // element-centre minus finger, in fractions
    std::vector<SDL_FingerID> g_prevIds;        // finger ids present last frame (for fresh-down detection)
    bool g_layoutLoaded = false;

    struct FingerPt { SDL_FingerID id; ImVec2 pos; };

    struct ElemRect { ImVec2 c; float hw; float hh; bool round; };

    ElemRect ElemRectOf(int i, float vw, float vh)
    {
        ImVec2 c(g_layout.x[i] * vw, g_layout.y[i] * vh);
        const float s = g_layout.scale;
        if (i == TC_STICK)                { const float r = STICK_BASE_R * vh * s; return { c, r, r, true }; }
        if (i >= TC_A && i <= TC_Y)       { const float r = FACE_BTN_R  * vh * s; return { c, r, r, true }; }
        if (i >= TC_LB && i <= TC_RT)     { return { c, SHOULDER_HW * vh * s, SHOULDER_HH * vh * s, false }; }
        return { c, MENU_HW * vh * s, MENU_HH * vh * s, false }; // Start / Back
    }

    // ---- Persistence -------------------------------------------------------

    std::filesystem::path LayoutFilePath()
    {
#ifdef __ANDROID__
        const std::filesystem::path& root = os::android::GetDataRoot();
        if (!root.empty())
            return root / "touch_layout.ini";
#endif
        return {};
    }

    void SaveLayout()
    {
        const std::filesystem::path path = LayoutFilePath();
        if (path.empty())
            return;

        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return;

        f << "version=1\n";
        f << "scale=" << g_layout.scale << "\n";
        for (int i = 0; i < TC_COUNT; ++i)
            f << kKey[i] << "=" << g_layout.x[i] << "," << g_layout.y[i] << "\n";
    }

    void LoadLayout()
    {
        g_layout = kDefault;
        g_layoutLoaded = true;

        const std::filesystem::path path = LayoutFilePath();
        if (path.empty())
            return;

        std::ifstream f(path, std::ios::binary);
        if (!f)
            return;

        std::string line;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key = line.substr(0, eq);
            const std::string val = line.substr(eq + 1);

            if (key == "scale")
            {
                g_layout.scale = std::clamp((float)atof(val.c_str()), SCALE_MIN, SCALE_MAX);
                continue;
            }

            for (int i = 0; i < TC_COUNT; ++i)
            {
                if (key == kKey[i])
                {
                    const size_t comma = val.find(',');
                    if (comma != std::string::npos)
                    {
                        const float x = (float)atof(val.substr(0, comma).c_str());
                        const float y = (float)atof(val.substr(comma + 1).c_str());
                        g_layout.x[i] = std::clamp(x, 0.0f, 1.0f);
                        g_layout.y[i] = std::clamp(y, 0.0f, 1.0f);
                    }
                    break;
                }
            }
        }
    }

    // ---- Drawing helpers ---------------------------------------------------

    bool AnyFingerInCircle(const std::vector<ImVec2>& pts, ImVec2 c, float r)
    {
        const float r2 = r * r;
        for (const auto& p : pts)
        {
            const float dx = p.x - c.x;
            const float dy = p.y - c.y;
            if (dx * dx + dy * dy <= r2)
                return true;
        }
        return false;
    }

    bool AnyFingerInRect(const std::vector<ImVec2>& pts, ImVec2 mn, ImVec2 mx)
    {
        for (const auto& p : pts)
        {
            if (p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y)
                return true;
        }
        return false;
    }

    void DrawGlyph(ImDrawList* dl, ImVec2 c, float halfW, float halfH, EButtonIcon icon, int alpha)
    {
        auto ic = GetButtonIcon(icon);
        auto* tex = std::get<1>(ic);
        if (!tex)
            return;

        dl->AddImage(tex, { c.x - halfW, c.y - halfH }, { c.x + halfW, c.y + halfH },
            GET_UV_COORDS(std::get<0>(ic)), IM_COL32(255, 255, 255, alpha));
    }

    // Round face button (A/B/X/Y): dark backing disc + coloured glyph on top.
    void DrawFaceButton(ImDrawList* dl, const std::vector<ImVec2>& pts, ImVec2 c, float r,
        EButtonIcon icon, uint16_t bit, XAMINPUT_GAMEPAD& st)
    {
        const bool pressed = AnyFingerInCircle(pts, c, r * 1.3f);
        if (pressed)
            st.wButtons |= bit;

        dl->AddCircleFilled(c, r * 1.25f, IM_COL32(0, 0, 0, pressed ? 120 : 70), 32);
        DrawGlyph(dl, c, r, r, icon, pressed ? 255 : 210);
    }

    // Wide button (shoulders/triggers/start/back): rounded backing + glyph.
    bool DrawWideButton(ImDrawList* dl, const std::vector<ImVec2>& pts, ImVec2 c,
        float halfW, float halfH, float glyphHalfW, float glyphHalfH, EButtonIcon icon)
    {
        const bool pressed = AnyFingerInRect(pts, { c.x - halfW, c.y - halfH }, { c.x + halfW, c.y + halfH });

        dl->AddRectFilled({ c.x - halfW, c.y - halfH }, { c.x + halfW, c.y + halfH },
            IM_COL32(0, 0, 0, pressed ? 120 : 70), halfH * 0.5f);
        DrawGlyph(dl, c, glyphHalfW, glyphHalfH, icon, pressed ? 255 : 210);

        return pressed;
    }

    // Draw a control's static visual (no press detection) - used by the editor.
    void DrawElemVisual(ImDrawList* dl, int i, const ElemRect& r)
    {
        if (i == TC_STICK)
        {
            dl->AddCircleFilled(r.c, r.hw, IM_COL32(0, 0, 0, 55), 48);
            dl->AddCircle(r.c, r.hw, IM_COL32(255, 255, 255, 130), 48, 3.0f);
            const float thumb = r.hw * (STICK_THUMB_R / STICK_BASE_R);
            dl->AddCircleFilled(r.c, thumb, IM_COL32(255, 255, 255, 110), 32);
            return;
        }

        if (i >= TC_A && i <= TC_Y)
        {
            dl->AddCircleFilled(r.c, r.hw * 1.25f, IM_COL32(0, 0, 0, 70), 32);
            DrawGlyph(dl, r.c, r.hw, r.hw, kIcon[i], 210);
            return;
        }

        dl->AddRectFilled({ r.c.x - r.hw, r.c.y - r.hh }, { r.c.x + r.hw, r.c.y + r.hh },
            IM_COL32(0, 0, 0, 70), r.hh * 0.5f);

        float gw, gh;
        if (i == TC_LB || i == TC_RB)      { gw = r.hw * 0.75f; gh = r.hh * 0.85f; }
        else if (i == TC_LT || i == TC_RT) { gw = r.hh * 0.95f; gh = r.hh * 0.95f; }
        else                               { gw = r.hw;         gh = r.hh; } // Start / Back

        DrawGlyph(dl, r.c, gw, gh, kIcon[i], 210);
    }
}

// ---------------------------------------------------------------------------
// SDL touch event handling.
// ---------------------------------------------------------------------------

class SDLEventListenerForTouchControls : public SDLEventListener
{
public:
    bool OnSDLEvent(SDL_Event* event) override
    {
        switch (event->type)
        {
            case SDL_FINGERDOWN:
            {
                {
                    std::lock_guard lock(g_mutex);
                    g_fingers.push_back({ event->tfinger.fingerId, event->tfinger.x, event->tfinger.y });
                }

                // Any touch brings the overlay back.
                TouchControls::SetVisible(true);
                break;
            }

            case SDL_FINGERMOTION:
            {
                std::lock_guard lock(g_mutex);
                for (auto& f : g_fingers)
                {
                    if (f.id == event->tfinger.fingerId)
                    {
                        f.nx = event->tfinger.x;
                        f.ny = event->tfinger.y;
                        break;
                    }
                }
                break;
            }

            case SDL_FINGERUP:
            {
                std::lock_guard lock(g_mutex);
                std::erase_if(g_fingers, [&](const Finger& f) { return f.id == event->tfinger.fingerId; });
                break;
            }
        }

        return false;
    }
}
g_sdlEventListenerForTouchControls;

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool TouchControls::IsVisible()
{
    // The launcher-requested layout editor must remain reachable even when the
    // user's normal touch-control policy is Off.
    if (g_edit)
        return true;
#ifdef __ANDROID__
    switch (Config::TouchControls.Value)
    {
        case EAndroidTouchControlsPolicy::AlwaysOn:
            return true;
        case EAndroidTouchControlsPolicy::Off:
            return false;
        case EAndroidTouchControlsPolicy::Auto:
        default:
            break;
    }
#endif

    return g_autoVisible.load(std::memory_order_relaxed);
}

void TouchControls::SetVisible(bool visible)
{
    g_autoVisible.store(visible, std::memory_order_relaxed);
}

const XAMINPUT_GAMEPAD& TouchControls::GetGamepadState()
{
    return g_state;
}

void TouchControls::Init()
{
    // The glyph atlas is owned by ButtonGuide (initialised before us). Load the
    // saved on-screen layout, if any.
    LoadLayout();
#ifdef __ANDROID__
    // A one-shot marker is used instead of a permanent in-game EDIT overlay. Java
    // creates it before starting SDL and we consume it as soon as the renderer is ready.
    const std::filesystem::path marker = os::android::GetDataRoot() / "touch_layout_edit.txt";
    std::error_code ec;
    if (std::filesystem::exists(marker, ec))
    {
        g_edit = true;
        std::filesystem::remove(marker, ec);
    }
#endif
}

void TouchControls::Draw()
{
    if (!IsVisible())
    {
        g_state = {};
        g_stickFingerId = (SDL_FingerID)-1;
        g_dragElem = -1;
        g_prevIds.clear();
        return;
    }

    if (!g_layoutLoaded)
        LoadLayout();

    const float vw = float(Video::s_viewportWidth);
    const float vh = float(Video::s_viewportHeight);
    if (vw <= 0.0f || vh <= 0.0f)
        return;

    // Map normalised finger coordinates (over the window/swapchain) into ImGui
    // viewport space (the viewport is centred within the swapchain for some
    // aspect-ratio settings).
    int pw = 0, ph = 0;
    GameWindow::GetSizeInPixels(&pw, &ph);
    const float sw = pw > 0 ? float(pw) : vw;
    const float sh = ph > 0 ? float(ph) : vh;
    const float offX = (sw - vw) * 0.5f;
    const float offY = (sh - vh) * 0.5f;

    std::vector<FingerPt> fps;
    {
        std::lock_guard lock(g_mutex);
        fps.reserve(g_fingers.size());
        for (const auto& f : g_fingers)
            fps.push_back({ f.id, { f.nx * sw - offX, f.ny * sh - offY } });
    }

    // Fresh finger-downs = ids present now but not last frame (one-shot taps).
    std::unordered_set<SDL_FingerID> prevSet(g_prevIds.begin(), g_prevIds.end());
    std::vector<FingerPt> fresh;
    for (const auto& fp : fps)
        if (!prevSet.count(fp.id))
            fresh.push_back(fp);

    std::vector<SDL_FingerID> curIds;
    curIds.reserve(fps.size());
    for (const auto& fp : fps)
        curIds.push_back(fp.id);

    auto* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float fontPx = vh * 0.030f;

    auto tapBox = [&](ImVec2 c, float hw, float hh, const char* label, bool accent)
    {
        const ImU32 bg = accent ? IM_COL32(70, 120, 200, 220) : IM_COL32(0, 0, 0, 180);
        dl->AddRectFilled({ c.x - hw, c.y - hh }, { c.x + hw, c.y + hh }, bg, 8.0f);
        dl->AddRect({ c.x - hw, c.y - hh }, { c.x + hw, c.y + hh }, IM_COL32(255, 255, 255, 190), 8.0f, 0, 2.0f);
        const ImVec2 ts = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, label);
        dl->AddText(font, fontPx, { c.x - ts.x * 0.5f, c.y - ts.y * 0.5f }, IM_COL32(255, 255, 255, 255), label);
    };

    // -----------------------------------------------------------------------
    // Gameplay mode.
    // -----------------------------------------------------------------------
    if (!g_edit)
    {
        XAMINPUT_GAMEPAD st{};

        // ---- Left analog stick ----
        const ImVec2 stickC(g_layout.x[TC_STICK] * vw, g_layout.y[TC_STICK] * vh);
        const float baseR  = STICK_BASE_R  * vh * g_layout.scale;
        const float thumbR = STICK_THUMB_R * vh * g_layout.scale;
        const float zoneR  = STICK_ZONE_R  * vh * g_layout.scale;

        const ImVec2* stickPos = nullptr;
        if (g_stickFingerId != (SDL_FingerID)-1)
        {
            for (const auto& fp : fps)
                if (fp.id == g_stickFingerId) { stickPos = &fp.pos; break; }

            if (!stickPos)
                g_stickFingerId = (SDL_FingerID)-1;
        }
        if (g_stickFingerId == (SDL_FingerID)-1)
        {
            for (const auto& fp : fps)
            {
                const float dx = fp.pos.x - stickC.x;
                const float dy = fp.pos.y - stickC.y;
                if (dx * dx + dy * dy <= zoneR * zoneR)
                {
                    g_stickFingerId = fp.id;
                    stickPos = &fp.pos;
                    break;
                }
            }
        }

        ImVec2 thumbPos = stickC;
        bool stickActive = false;
        if (stickPos)
        {
            const float dx = stickPos->x - stickC.x;
            const float dy = stickPos->y - stickC.y;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float cl = std::min(len, baseR);
            const float ux = len > 0.0f ? dx / len : 0.0f;
            const float uy = len > 0.0f ? dy / len : 0.0f;

            thumbPos = { stickC.x + ux * cl, stickC.y + uy * cl };

            const float ax = (ux * cl) / baseR;
            const float ay = (uy * cl) / baseR;
            st.sThumbLX = int16_t(std::clamp(ax * 32767.0f, -32767.0f, 32767.0f));
            st.sThumbLY = int16_t(std::clamp(-ay * 32767.0f, -32767.0f, 32767.0f));

            stickActive = true;
        }

        // Buttons are hit-tested against every finger except the stick's.
        std::vector<ImVec2> pts;
        pts.reserve(fps.size());
        for (const auto& fp : fps)
            if (fp.id != g_stickFingerId)
                pts.push_back(fp.pos);

        dl->AddCircleFilled(stickC, baseR, IM_COL32(0, 0, 0, stickActive ? 90 : 55), 48);
        dl->AddCircle(stickC, baseR, IM_COL32(255, 255, 255, 130), 48, 3.0f);
        dl->AddCircleFilled(thumbPos, thumbR, IM_COL32(255, 255, 255, stickActive ? 170 : 110), 32);

        // ---- Face buttons ----
        const float faceR = FACE_BTN_R * vh * g_layout.scale;
        DrawFaceButton(dl, pts, ElemRectOf(TC_A, vw, vh).c, faceR, EButtonIcon::A, XAMINPUT_GAMEPAD_A, st);
        DrawFaceButton(dl, pts, ElemRectOf(TC_B, vw, vh).c, faceR, EButtonIcon::B, XAMINPUT_GAMEPAD_B, st);
        DrawFaceButton(dl, pts, ElemRectOf(TC_X, vw, vh).c, faceR, EButtonIcon::X, XAMINPUT_GAMEPAD_X, st);
        DrawFaceButton(dl, pts, ElemRectOf(TC_Y, vw, vh).c, faceR, EButtonIcon::Y, XAMINPUT_GAMEPAD_Y, st);

        // ---- Shoulders ----
        const float shHW = SHOULDER_HW * vh * g_layout.scale;
        const float shHH = SHOULDER_HH * vh * g_layout.scale;
        if (DrawWideButton(dl, pts, ElemRectOf(TC_LB, vw, vh).c, shHW, shHH, shHW * 0.75f, shHH * 0.85f, EButtonIcon::LB))
            st.wButtons |= XAMINPUT_GAMEPAD_LEFT_SHOULDER;
        if (DrawWideButton(dl, pts, ElemRectOf(TC_RB, vw, vh).c, shHW, shHH, shHW * 0.75f, shHH * 0.85f, EButtonIcon::RB))
            st.wButtons |= XAMINPUT_GAMEPAD_RIGHT_SHOULDER;

        // ---- Triggers ----
        if (DrawWideButton(dl, pts, ElemRectOf(TC_LT, vw, vh).c, shHW, shHH, shHH * 0.95f, shHH * 0.95f, EButtonIcon::LT))
            st.bLeftTrigger = 255;
        if (DrawWideButton(dl, pts, ElemRectOf(TC_RT, vw, vh).c, shHW, shHH, shHH * 0.95f, shHH * 0.95f, EButtonIcon::RT))
            st.bRightTrigger = 255;

        // ---- Start / Back ----
        const float menuHW = MENU_HW * vh * g_layout.scale;
        const float menuHH = MENU_HH * vh * g_layout.scale;
        if (DrawWideButton(dl, pts, ElemRectOf(TC_START, vw, vh).c, menuHW, menuHH, menuHW, menuHH, EButtonIcon::Start))
            st.wButtons |= XAMINPUT_GAMEPAD_START;
        if (DrawWideButton(dl, pts, ElemRectOf(TC_BACK, vw, vh).c, menuHW, menuHH, menuHW, menuHH, EButtonIcon::Back))
            st.wButtons |= XAMINPUT_GAMEPAD_BACK;

        g_state = st;

        g_prevIds = std::move(curIds);
        return;
    }

    // -----------------------------------------------------------------------
    // Editor mode.
    // -----------------------------------------------------------------------
    g_state = {};
    g_stickFingerId = (SDL_FingerID)-1;

    // Dimmed backdrop.
    dl->AddRectFilled({ 0.0f, 0.0f }, { vw, vh }, IM_COL32(0, 0, 0, 120));

    // Action bar (top). Handle taps first so a tap on a button never starts a drag.
    const float barY   = vh * 0.055f;
    const float barHH  = vh * 0.040f;
    const float wideHW = vw * 0.085f;
    const float sizeHW = vh * 0.050f;

    const ImVec2 resetC(vw * 0.30f, barY);
    const ImVec2 minusC(vw * 0.44f, barY);
    const ImVec2 plusC (vw * 0.56f, barY);
    const ImVec2 doneC (vw * 0.70f, barY);

    bool changed = false;
    std::unordered_set<SDL_FingerID> consumed;

    // A tap that hits an action button is "consumed" so the same finger can't also
    // start dragging a control placed underneath the bar.
    auto tapConsume = [&](ImVec2 c, float hw, float hh) -> bool
    {
        bool hit = false;
        for (const auto& f : fresh)
            if (f.pos.x >= c.x - hw && f.pos.x <= c.x + hw && f.pos.y >= c.y - hh && f.pos.y <= c.y + hh)
            {
                hit = true;
                consumed.insert(f.id);
            }
        return hit;
    };

    if (tapConsume(doneC, wideHW, barHH))
    {
        SaveLayout();
        g_edit = false;
        g_dragElem = -1;
        g_prevIds = std::move(curIds);
        return;
    }
    if (tapConsume(resetC, wideHW, barHH))
    {
        g_layout = kDefault;
        changed = true;
    }
    if (tapConsume(minusC, sizeHW, barHH))
    {
        g_layout.scale = std::clamp(g_layout.scale - 0.05f, SCALE_MIN, SCALE_MAX);
        changed = true;
    }
    if (tapConsume(plusC, sizeHW, barHH))
    {
        g_layout.scale = std::clamp(g_layout.scale + 0.05f, SCALE_MIN, SCALE_MAX);
        changed = true;
    }

    // ---- Dragging ----
    if (g_dragElem >= 0)
    {
        const ImVec2* dp = nullptr;
        for (const auto& fp : fps)
            if (fp.id == g_dragFinger) { dp = &fp.pos; break; }

        if (!dp)
        {
            g_dragElem = -1;      // finger lifted -> commit
            changed = true;
        }
        else
        {
            g_layout.x[g_dragElem] = std::clamp(dp->x / vw + g_grabX, 0.02f, 0.98f);
            g_layout.y[g_dragElem] = std::clamp(dp->y / vh + g_grabY, 0.02f, 0.98f);
        }
    }

    if (g_dragElem < 0)
    {
        for (const auto& f : fresh)
        {
            if (consumed.count(f.id))
                continue;

            for (int i = 0; i < TC_COUNT; ++i)
            {
                const ElemRect r = ElemRectOf(i, vw, vh);
                bool hit;
                if (r.round)
                {
                    const float dx = f.pos.x - r.c.x;
                    const float dy = f.pos.y - r.c.y;
                    hit = dx * dx + dy * dy <= r.hw * r.hw;
                }
                else
                {
                    hit = f.pos.x >= r.c.x - r.hw && f.pos.x <= r.c.x + r.hw &&
                          f.pos.y >= r.c.y - r.hh && f.pos.y <= r.c.y + r.hh;
                }

                if (hit)
                {
                    g_dragElem = i;
                    g_dragFinger = f.id;
                    g_grabX = g_layout.x[i] - f.pos.x / vw;
                    g_grabY = g_layout.y[i] - f.pos.y / vh;
                    break;
                }
            }

            if (g_dragElem >= 0)
                break;
        }
    }

    // ---- Draw all controls with selection outlines ----
    for (int i = 0; i < TC_COUNT; ++i)
    {
        const ElemRect r = ElemRectOf(i, vw, vh);
        DrawElemVisual(dl, i, r);

        const ImU32 col = (i == g_dragElem) ? IM_COL32(255, 220, 60, 255) : IM_COL32(70, 200, 110, 220);
        if (r.round)
            dl->AddCircle(r.c, r.hw, col, 40, 3.0f);
        else
            dl->AddRect({ r.c.x - r.hw, r.c.y - r.hh }, { r.c.x + r.hw, r.c.y + r.hh }, col, 6.0f, 0, 3.0f);
    }

    // ---- Action bar on top ----
    tapBox(resetC, wideHW, barHH, "RESET", false);
    tapBox(minusC, sizeHW, barHH, "-", false);
    tapBox(plusC,  sizeHW, barHH, "+", false);
    tapBox(doneC,  wideHW, barHH, "DONE", true);

    char sizeLabel[32];
    snprintf(sizeLabel, sizeof(sizeLabel), "SIZE %d%%", int(g_layout.scale * 100.0f + 0.5f));
    const ImVec2 slSize = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, sizeLabel);
    dl->AddText(font, fontPx, { vw * 0.50f - slSize.x * 0.5f, barY + barHH * 1.4f }, IM_COL32(255, 255, 255, 230), sizeLabel);

    const char* hint = "Drag buttons to arrange";
    const ImVec2 hintSize = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, hint);
    dl->AddText(font, fontPx, { vw * 0.50f - hintSize.x * 0.5f, vh * 0.14f }, IM_COL32(255, 255, 255, 200), hint);

    if (changed)
        SaveLayout();

    g_prevIds = std::move(curIds);
}
