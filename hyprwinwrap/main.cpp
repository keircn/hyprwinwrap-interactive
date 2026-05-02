#define WLR_USE_UNSTABLE

#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <vector>

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
inline CHyprSignalListener activeWindowListener;
inline CHyprSignalListener renderListener;
inline CHyprSignalListener configReloadedListener;

std::vector<PHLWINDOWREF> bgWindows;
std::map<PHLWINDOW, bool> interactableStates;
bool refocusingAway = false;

// Helper functions
static Hyprlang::INT configInt(const std::string& name) {
    return *(Hyprlang::INT const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:" + name)->getDataStaticPtr();
}

static bool configBool(const std::string& name) {
    return configInt(name) != 0;
}

static std::string configString(const std::string& name) {
    return *(Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:hyprwinwrap:" + name)->getDataStaticPtr();
}

static float configPercent(const std::string& name, float fallback) {
    try {
        return std::stof(configString(name));
    } catch (...) { return fallback; }
}

static bool isBgWindow(const PHLWINDOW& window) {
    return std::any_of(bgWindows.begin(), bgWindows.end(), [&window](const auto& ref) { return ref.lock() == window; });
}

static bool isWindowInteractable(const PHLWINDOW& window) {
    auto it = interactableStates.find(window);
    return it != interactableStates.end() && it->second;
}

static void setWrappedFullscreenState(const PHLWINDOW& window, bool interactive) {
    if (!window)
        return;

    window->m_fullscreenState = {
        .internal = configBool("force_fullscreen") && interactive ? FSMODE_FULLSCREEN : FSMODE_NONE,
        .client   = configBool("client_fullscreen") ? FSMODE_FULLSCREEN : FSMODE_NONE,
    };

    if (interactive && configBool("client_fullscreen"))
        g_pXWaylandManager->setWindowFullscreen(window, true);
}

static void setWindowGeometry(const PHLWINDOW& window, const Vector2D& position, const Vector2D& size) {
    if (!window)
        return;

    window->m_realSize->setValueAndWarp(size);
    window->m_realPosition->setValueAndWarp(position);
    window->m_size     = size;
    window->m_position = position;
    window->sendWindowSize(true);
}

static PHLWINDOW fallbackFocusWindow(const PHLWINDOW& deniedWindow) {
    PHLWINDOW fallback = nullptr;

    for (const auto& window : g_pCompositor->m_windows) {
        if (!Desktop::View::validMapped(window) || window == deniedWindow || window->m_hidden || isBgWindow(window))
            continue;

        if (deniedWindow && window->m_monitor == deniedWindow->m_monitor && window->m_workspace == deniedWindow->m_workspace)
            return window;

        if (!fallback)
            fallback = window;
    }

    return fallback;
}

static void preventKeyboardFocus(const PHLWINDOW& deniedWindow) {
    if (!configBool("focus_guard"))
        return;

    if (refocusingAway || !deniedWindow || !isBgWindow(deniedWindow) || isWindowInteractable(deniedWindow))
        return;

    const auto focusedWindow = Desktop::focusState()->window();
    if (focusedWindow != deniedWindow)
        return;

    refocusingAway = true;

    if (const auto fallback = fallbackFocusWindow(deniedWindow); fallback) {
        Desktop::focusState()->fullWindowFocus(fallback, Desktop::FOCUS_REASON_OTHER);
    } else {
        Desktop::focusState()->resetWindowFocus();
        if (const auto monitor = deniedWindow->m_monitor.lock(); monitor)
            Desktop::focusState()->rawMonitorFocus(monitor);
    }

    refocusingAway = false;
}

static void setWindowInteractable(const PHLWINDOW& window, bool interactable) {
    interactableStates[window] = interactable;
    setWrappedFullscreenState(window, interactable);
    window->m_hidden           = !interactable;

    if (!interactable)
        preventKeyboardFocus(window);
}

static void cleanupExpiredWindows() {
    std::erase_if(bgWindows, [](const auto& ref) { return ref.expired(); });
}

static bool windowMatchesConfig(const PHLWINDOW& pWindow) {
    if (!pWindow)
        return false;

    const std::string classRule = configString("class");
    const std::string titleRule = configString("title");

    const bool        initialClassMatches = !classRule.empty() && pWindow->m_initialClass == classRule;
    const bool        currentClassMatches = configBool("match_current_class") && !classRule.empty() && pWindow->m_class == classRule;
    const bool        titleMatches        = !titleRule.empty() && pWindow->m_title == titleRule;
    const bool        initialTitleMatches = configBool("match_initial_title") && !titleRule.empty() && pWindow->m_initialTitle == titleRule;

    return initialClassMatches || currentClassMatches || titleMatches || initialTitleMatches;
}

void applyConfiguredGeometry(PHLWINDOW pWindow) {
    if (!pWindow || !windowMatchesConfig(pWindow))
        return;

    const auto PMONITOR = pWindow->m_monitor.lock();
    if (!PMONITOR)
        return;

    if (configBool("force_float") && !pWindow->m_isFloating)
        g_layoutManager->changeFloatingMode(pWindow->m_target);

    float sx = configPercent("size_x", 100.f);
    float sy = configPercent("size_y", 100.f);
    float px = configPercent("pos_x", 0.f);
    float py = configPercent("pos_y", 0.f);

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

    const CBox     monitorBox  = PMONITOR->logicalBox();
    const Vector2D monitorSize = monitorBox.size();
    const Vector2D monitorPos  = monitorBox.pos();

    const Vector2D newSize = {std::round(monitorSize.x * (sx / 100.f)), std::round(monitorSize.y * (sy / 100.f))};

    const Vector2D newPos = {std::round(monitorPos.x + (monitorSize.x * (px / 100.f))), std::round(monitorPos.y + (monitorSize.y * (py / 100.f)))};

    pWindow->m_pinned = configBool("pin");
    setWindowGeometry(pWindow, newPos, newSize);
}

void onNewWindow(PHLWINDOW pWindow) {
    if (!pWindow || isBgWindow(pWindow) || !windowMatchesConfig(pWindow))
        return;

    bgWindows.push_back(pWindow);
    applyConfiguredGeometry(pWindow);
    setWindowInteractable(pWindow, configBool("start_interactive"));

    if (!configBool("start_interactive") && configBool("refocus_on_hide"))
        g_pInputManager->refocus();
    Log::logger->log(Log::INFO, "[hyprwinwrap] new window moved to bg {}", pWindow);
}

void onCloseWindow(PHLWINDOW pWindow) {
    std::erase_if(bgWindows, [pWindow](const auto& ref) { return ref.expired() || ref.lock() == pWindow; });
    interactableStates.erase(pWindow);
    Log::logger->log(Log::INFO, "[hyprwinwrap] closed window {}", pWindow);
}

void onActiveWindow(PHLWINDOW pWindow) {
    preventKeyboardFocus(pWindow);
}

void adoptExistingWindows() {
    cleanupExpiredWindows();

    for (const auto& window : g_pCompositor->m_windows) {
        if (!Desktop::View::validMapped(window))
            continue;

        onNewWindow(window);
    }
}

void releaseWindow(PHLWINDOW pWindow) {
    if (!pWindow)
        return;

    pWindow->m_hidden          = false;
    pWindow->m_fullscreenState = {
        .internal = FSMODE_NONE,
        .client   = FSMODE_NONE,
    };

    interactableStates.erase(pWindow);
    std::erase_if(bgWindows, [pWindow](const auto& ref) { return ref.expired() || ref.lock() == pWindow; });
}

void releaseAllWindows() {
    cleanupExpiredWindows();

    const auto windows = bgWindows;
    for (const auto& bg : windows)
        releaseWindow(bg.lock());
}

void applyAllWindows() {
    cleanupExpiredWindows();

    for (auto& bg : bgWindows) {
        if (const auto bgw = bg.lock(); bgw) {
            applyConfiguredGeometry(bgw);
            setWrappedFullscreenState(bgw, isWindowInteractable(bgw));
            preventKeyboardFocus(bgw);
        }
    }
}

void onRenderStage(eRenderStage stage) {
    if (stage != RENDER_PRE_WINDOWS && stage != RENDER_LAST_MOMENT)
        return;

    cleanupExpiredWindows();

    for (auto& bg : bgWindows) {
        const auto bgw = bg.lock();
        if (!bgw)
            continue;

        if (bgw->m_monitor != g_pHyprOpenGL->m_renderData.pMonitor)
            continue;

        const auto MON = bgw->m_monitor.lock();
        if (!MON)
            continue;

        const bool interactable = isWindowInteractable(bgw);
        if ((interactable && !configBool("render_interactive")) || (!interactable && !configBool("render_hidden")))
            continue;

        if (stage == RENDER_LAST_MOMENT && !configBool("interactive_above_layers"))
            continue;

        if ((stage == RENDER_PRE_WINDOWS && interactable) || (stage == RENDER_LAST_MOMENT && !interactable))
            continue;

        if (configBool("force_full_monitor") || (interactable && configBool("force_fullscreen"))) {
            const CBox fullMonitorBox = MON->logicalBox();
            setWindowGeometry(bgw, fullMonitorBox.pos(), fullMonitorBox.size());
        }

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
    const std::string classRule = configString("class");
    if (!classRule.empty() && configBool("auto_windowrules")) {
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"float, class:^("} + classRule + ")$");
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"size 100\% 100\%, class:^("} + classRule + ")$");
    }

    const std::string titleRule = configString("title");
    if (!titleRule.empty() && configBool("auto_windowrules")) {
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"float, title:^("} + titleRule + ")$");
        g_pConfigManager->parseKeyword("windowrulev2", std::string{"size 100\% 100\%, title:^("} + titleRule + ")$");
    }

    if (configBool("adopt_existing"))
        adoptExistingWindows();

    applyAllWindows();
}

// Dispatchers

static void notify(const std::string& message, const CHyprColor& color = CHyprColor{0.2, 1.0, 0.2, 1.0}) {
    if (configBool("notifications"))
        HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] " + message, color, 3000);
}

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
        if (firstBg && isWindowInteractable(firstBg) && configBool("refocus_on_show")) {
            Desktop::focusState()->fullWindowFocus(firstBg, Desktop::FOCUS_REASON_KEYBIND);
        } else {
            if (configBool("refocus_on_hide"))
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
        if (firstBg && configBool("refocus_on_show"))
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

    if (configBool("refocus_on_hide"))
        g_pInputManager->refocus();

    return SDispatchResult{};
}

SDispatchResult dispatchAdopt(std::string args) {
    adoptExistingWindows();
    applyAllWindows();
    notify(std::format("Adopted {} background window(s)", bgWindows.size()));
    return SDispatchResult{};
}

SDispatchResult dispatchRelease(std::string args) {
    const auto count = bgWindows.size();
    releaseAllWindows();
    notify(std::format("Released {} background window(s)", count));
    return SDispatchResult{};
}

SDispatchResult dispatchRefresh(std::string args) {
    applyAllWindows();
    notify(std::format("Refreshed {} background window(s)", bgWindows.size()));
    return SDispatchResult{};
}

SDispatchResult dispatchStatus(std::string args) {
    cleanupExpiredWindows();

    size_t visible = 0, hidden = 0;
    for (auto& bg : bgWindows) {
        const auto bgw = bg.lock();
        if (!bgw)
            continue;

        if (isWindowInteractable(bgw))
            visible++;
        else
            hidden++;
    }

    HyprlandAPI::addNotification(PHANDLE, std::format("[hyprwinwrap] {} window(s): {} interactive, {} hidden", bgWindows.size(), visible, hidden), CHyprColor{0.2, 0.7, 1.0, 1.0}, 4000);
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
    activeWindowListener   = Event::bus()->m_events.window.active.listen([](PHLWINDOW window, Desktop::eFocusReason reason) { onActiveWindow(window); });
    renderListener         = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) { onRenderStage(stage); });
    configReloadedListener = Event::bus()->m_events.config.reloaded.listen([]() { onConfigReloaded(); });
    // clang-format on

    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:toggle", dispatchToggle);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:show", dispatchShow);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:hide", dispatchHide);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:adopt", dispatchAdopt);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:release", dispatchRelease);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:refresh", dispatchRefresh);
    HyprlandAPI::addDispatcherV2(PHANDLE, "hyprwinwrap:status", dispatchStatus);
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

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:match_current_class", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:match_initial_title", Hyprlang::INT{0});

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:adopt_existing", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:start_interactive", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:force_float", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:pin", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:auto_windowrules", Hyprlang::INT{0});

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:render_hidden", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:render_interactive", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:force_full_monitor", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:force_fullscreen", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:client_fullscreen", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:interactive_above_layers", Hyprlang::INT{0});

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:focus_guard", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:refocus_on_show", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:refocus_on_hide", Hyprlang::INT{0});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:hyprwinwrap:notifications", Hyprlang::INT{0});

    if (configBool("adopt_existing"))
        adoptExistingWindows();

    HyprlandAPI::addNotification(PHANDLE, "[hyprwinwrap] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);

    return {"hyprwinwrap", "A clone of xwinwrap for Hyprland with toggle support", "Vaxry & keircn", "1.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    releaseAllWindows();
}
