#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <algorithm>
#include <vector>
#include <map>

#include <hyprland/src/includes.hpp>
#include <any>
#include <sstream>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/Subsurface.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/SharedDefs.hpp>
#undef private

#include "globals.hpp"

// Do NOT change this function
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// hooks
inline CFunctionHook* subsurfaceHook = nullptr;
inline CFunctionHook* commitHook     = nullptr;
typedef void (*origCommitSubsurface)(Desktop::View::CSubsurface* thisptr);
typedef void (*origCommitWindow)(Desktop::View::CWindow* thisptr);

inline CHyprSignalListener openWindowListener;
inline CHyprSignalListener closeWindowListener;
inline CHyprSignalListener renderListener;
inline CHyprSignalListener configReloadedListener;

std::vector<PHLWINDOWREF> bgWindows;
std::map<PHLWINDOW, bool> interactableStates;

// Helper functions
static bool isBgWindow(const PHLWINDOW& window) {
    return std::any_of(bgWindows.begin(), bgWindows.end(), [&window](const auto& ref) { return ref.lock() == window; });
}

static bool isWindowInteractable(const PHLWINDOW& window) {
    auto it = interactableStates.find(window);
    return it != interactableStates.end() && it->second;
}

static void setWindowInteractable(const PHLWINDOW& window, bool interactable) {
    interactableStates[window] = interactable;
    window->m_hidden           = !interactable;
}

static void cleanupExpiredWindows() {
    std::erase_if(bgWindows, [](const auto& ref) { return ref.expired(); });
}

static bool windowMatchesConfig(const PHLWINDOW& pWindow) {
    if (!pWindow)
        return false;

    static auto* const PCLASS = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:class")->getDataStaticPtr();
    static auto* const PTITLE = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:title")->getDataStaticPtr();

    const std::string  classRule(*PCLASS);
    const std::string  titleRule(*PTITLE);

    const bool         classMatches = !classRule.empty() && pWindow->m_initialClass == classRule;
    const bool         titleMatches = !titleRule.empty() && pWindow->m_title == titleRule;

    return classMatches || titleMatches;
}

void onNewWindow(PHLWINDOW pWindow) {
    static auto* const PSIZEX = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:size_x")->getDataStaticPtr();
    static auto* const PSIZEY = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:size_y")->getDataStaticPtr();
    static auto* const PPOSX  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_x")->getDataStaticPtr();
    static auto* const PPOSY  = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_y")->getDataStaticPtr();

    if (!pWindow || isBgWindow(pWindow) || !windowMatchesConfig(pWindow))
        return;

    const auto PMONITOR = pWindow->m_monitor.lock();
    if (!PMONITOR)
        return;

    if (!pWindow->m_isFloating)
        g_layoutManager->changeFloatingMode(pWindow->m_target);

    float sx = 100.f, sy = 100.f, px = 0.f, py = 0.f;

    try {
        sx = std::stof(*PSIZEX);
    } catch (...) {}
    try {
        sy = std::stof(*PSIZEY);
    } catch (...) {}
    try {
        px = std::stof(*PPOSX);
    } catch (...) {}
    try {
        py = std::stof(*PPOSY);
    } catch (...) {}

    sx = std::clamp(sx, 1.f, 100.f);
    sy = std::clamp(sy, 1.f, 100.f);
    px = std::clamp(px, 0.f, 100.f);
    py = std::clamp(py, 0.f, 100.f);

    if (px + sx > 100.f) {
        Log::logger->log(Log::WARN, "[hyprwinwrap] size_x ({:.1f}) + pos_x ({:.1f}) > 100, adjusting size_x to {:.1f}", sx, px, 100.f - px);
        sx = 100.f - px;
    }
    if (py + sy > 100.f) {
        Log::logger->log(Log::WARN, "[hyprwinwrap] size_y ({:.1f}) + pos_y ({:.1f}) > 100, adjusting size_y to {:.1f}", sy, py, 100.f - py);
        sy = 100.f - py;
    }

    const Vector2D monitorSize = PMONITOR->m_size;
    const Vector2D monitorPos  = PMONITOR->m_position;

    const Vector2D newSize = {static_cast<int>(monitorSize.x * (sx / 100.f)), static_cast<int>(monitorSize.y * (sy / 100.f))};

    const Vector2D newPos = {static_cast<int>(monitorPos.x + (monitorSize.x * (px / 100.f))), static_cast<int>(monitorPos.y + (monitorSize.y * (py / 100.f)))};

    pWindow->m_realSize->setValueAndWarp(newSize);
    pWindow->m_realPosition->setValueAndWarp(newPos);
    pWindow->m_size     = newSize;
    pWindow->m_position = newPos;
    pWindow->m_pinned   = true;
    pWindow->sendWindowSize(true);

    bgWindows.push_back(pWindow);
    setWindowInteractable(pWindow, false);

    g_pInputManager->refocus();
    Log::logger->log(Log::INFO, "[hyprwinwrap] new window moved to bg {}", pWindow);
}

void onCloseWindow(PHLWINDOW pWindow) {
    std::erase_if(bgWindows, [pWindow](const auto& ref) { return ref.expired() || ref.lock() == pWindow; });
    interactableStates.erase(pWindow);
    Log::logger->log(Log::INFO, "[hyprwinwrap] closed window {}", pWindow);
}

void adoptExistingWindows() {
    cleanupExpiredWindows();

    for (const auto& window : g_pCompositor->m_windows) {
        if (!Desktop::View::validMapped(window))
            continue;

        onNewWindow(window);
    }
}

void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_PRE_WINDOWS)
        return;

    cleanupExpiredWindows();

    for (auto& bg : bgWindows) {
        const auto bgw = bg.lock();
        if (!bgw)
            continue;

        if (bgw->m_monitor != g_pHyprOpenGL->m_renderData.pMonitor)
            continue;

        // cant use setHidden cuz that sends suspended and shit too that would be laggy
        const bool wasHidden = bgw->m_hidden;
        bgw->m_hidden        = false;

        g_pHyprRenderer->renderWindow(bgw, g_pHyprOpenGL->m_renderData.pMonitor.lock(), Time::steadyNow(), false, RENDER_PASS_ALL, false, true);

        // Only hide if not interactable
        if (!isWindowInteractable(bgw))
            bgw->m_hidden = true;
    }
}

void onCommitSubsurface(Desktop::View::CSubsurface* thisptr) {
    const auto PWINDOW = thisptr->m_windowParent.lock();

    if (!PWINDOW || !isBgWindow(PWINDOW)) {
        ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
        return;
    }

    PWINDOW->m_hidden = false;

    ((origCommitSubsurface)subsurfaceHook->m_original)(thisptr);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprOpenGL->markBlurDirtyForMonitor(MON);

    if (!isWindowInteractable(PWINDOW))
        PWINDOW->m_hidden = true;
}

void onCommitWindow(Desktop::View::CWindow* thisptr) {
    const auto PWINDOW = thisptr->m_self.lock();

    if (!isBgWindow(PWINDOW)) {
        ((origCommitWindow)commitHook->m_original)(thisptr);
        return;
    }

    PWINDOW->m_hidden = false;

    ((origCommitWindow)commitHook->m_original)(thisptr);
    if (const auto MON = PWINDOW->m_monitor.lock(); MON)
        g_pHyprOpenGL->markBlurDirtyForMonitor(MON);

    if (!isWindowInteractable(PWINDOW))
        PWINDOW->m_hidden = true;
}

void onConfigReloaded() {
    static auto* const PCLASS = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:class")->getDataStaticPtr();
    const std::string  classRule(*PCLASS);
    if (!classRule.empty()) {
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"float, class:^("} + classRule + ")$");
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"size 100\% 100\%, class:^("} + classRule + ")$");
    }

    static auto* const PTITLE = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:title")->getDataStaticPtr();
    const std::string  titleRule(*PTITLE);
    if (!titleRule.empty()) {
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"float, title:^("} + titleRule + ")$");
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"size 100\% 100\%, title:^("} + titleRule + ")$");
    }

    adoptExistingWindows();
}

// Dispatchers

SDispatchResult dispatchToggle(std::string args) {
    cleanupExpiredWindows();

    if (bgWindows.empty()) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] No background windows to toggle", CHyprColor{1.0, 1.0, 0.2, 1.0}, 3000);
        return SDispatchResult{};
    }

    int toggledCount = 0;
    for (auto& bg : bgWindows) {
        const auto bgw = bg.lock();
        if (!bgw)
            continue;

        bool newState = !isWindowInteractable(bgw);
        setWindowInteractable(bgw, newState);
        toggledCount++;

        Log::logger->log(Log::INFO, "[hyprwinwrap] Toggled window {} to {}", bgw, newState ? "interactable" : "non-interactable");
    }

    if (toggledCount > 0 && !bgWindows.empty()) {
        const auto firstBg = bgWindows.front().lock();
        if (firstBg && isWindowInteractable(firstBg)) {
            Desktop::focusState()->fullWindowFocus(firstBg, Desktop::FOCUS_REASON_KEYBIND);
        } else {
            g_pInputManager->refocus();
        }
    }

    return SDispatchResult{};
}

SDispatchResult dispatchShow(std::string args) {
    cleanupExpiredWindows();

    if (bgWindows.empty()) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] No background windows", CHyprColor{1.0, 1.0, 0.2, 1.0}, 3000);
        return SDispatchResult{};
    }

    for (auto& bg : bgWindows) {
        const auto bgw = bg.lock();
        if (!bgw)
            continue;

        setWindowInteractable(bgw, true);
        Log::logger->log(Log::INFO, "[hyprwinwrap] Set window {} to interactable", bgw);
    }

    if (!bgWindows.empty()) {
        const auto firstBg = bgWindows.front().lock();
        if (firstBg)
            Desktop::focusState()->fullWindowFocus(firstBg, Desktop::FOCUS_REASON_KEYBIND);
    }

    return SDispatchResult{};
}

SDispatchResult dispatchHide(std::string args) {
    cleanupExpiredWindows();

    for (auto& bg : bgWindows) {
        const auto bgw = bg.lock();
        if (!bgw)
            continue;

        setWindowInteractable(bgw, false);
        Log::logger->log(Log::INFO, "[hyprwinwrap] Set window {} to non-interactable", bgw);
    }

    g_pInputManager->refocus();

    return SDispatchResult{};
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH       = __hyprland_api_get_hash();
    const std::string CLIENTHASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENTHASH) {
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hyprwinwrap] Version mismatch");
    }

    // clang-format off
    openWindowListener     = Event::bus()->m_events.window.open.listen([](PHLWINDOW window) { onNewWindow(window); });
    closeWindowListener    = Event::bus()->m_events.window.close.listen([](PHLWINDOW window) { onCloseWindow(window); });
    renderListener         = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });
    configReloadedListener = Event::bus()->m_events.config.reloaded.listen([]() { onConfigReloaded(); });
    // clang-format on

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:toggle", dispatchToggle);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:show", dispatchShow);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:hide", dispatchHide);
    // Legacy dispatcher name for backwards compatibility
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap_toggle", dispatchToggle);

    auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, "onCommit");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: onCommit not found");
    for (auto& fn : fns) {
        if (!fn.demangled.contains("CSubsurface"))
            continue;
        subsurfaceHook = HyprlandAPI::createFunctionHook(PHANDLE, fn.address, (void*)&onCommitSubsurface);
    }

    fns = HyprlandAPI::findFunctionsByName(PHANDLE, "commitWindow");
    if (fns.size() < 1)
        throw std::runtime_error("hyprwinwrap: commitWindow not found");
    commitHook = HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, (void*)&onCommitWindow);

    bool hkResult = subsurfaceHook->hook();
    hkResult      = hkResult && commitHook->hook();

    if (!hkResult)
        throw std::runtime_error("hyprwinwrap: hooks failed");

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:class", Hyprlang::STRING{"kitty-bg"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:title", Hyprlang::STRING{""});

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:size_x", Hyprlang::STRING{"100"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:size_y", Hyprlang::STRING{"100"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_x", Hyprlang::STRING{"0"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:pos_y", Hyprlang::STRING{"0"});

    adoptExistingWindows();

    HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwinwrap", "A clone of xwinwrap for Hyprland with toggle support", "Vaxry & keircn", "1.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto& bg : bgWindows) {
        const auto bgw = bg.lock();
        if (bgw)
            bgw->m_hidden = false;
    }
    bgWindows.clear();
    interactableStates.clear();
}
