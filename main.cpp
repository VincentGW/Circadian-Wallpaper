#define _USE_MATH_DEFINES
#include <Windows.h>
#include <Dbt.h>
#include <SFML/Graphics.hpp>
#include <array>
#include <cstdint>
#include <iterator>
#include <iostream>
#include <ctime>
#include <cmath>
#include <string>
#include <fstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <memory>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

// Power management for wake-from-sleep detection
HWND g_hWnd = NULL;
bool g_justWokeUp = false;
// Set on WM_DISPLAYCHANGE / WM_DEVICECHANGE / wake so the main loop can
// re-enumerate monitors and sync windows to the current display topology.
bool g_topologyChanged = false;

// Set by WallpaperWndProc when Windows asks a wallpaper window to repaint.
// WM_PAINT only reaches a DWM-composited window when the compositor no longer
// has its surface (it was discarded while fully occluded, or across a sleep
// cycle) — exactly the case where the exposed region shows black until we
// present a fresh frame. The main loop repaints immediately when this is set.
bool g_exposeRequested = false;

// Every wallpaper window is subclassed with WallpaperWndProc; this maps each
// window back to the original SFML window procedure so messages are chained.
// Also doubles as "is this HWND one of ours" for the z-order check below.
std::unordered_map<HWND, WNDPROC> g_wallpaperPrevProcs;

// Subclass procedure for the wallpaper windows. Two anti-flash safeguards and
// the bottom-most z-order enforcement live here:
//  - WM_ERASEBKGND is swallowed. The app repaints every pixel of the window
//    each render tick, so letting the OS pre-erase to the class background
//    brush is pure flicker — it is exactly what shows as a black flash when
//    another window is opened, activated, or moved over the wallpaper.
//  - WM_WINDOWPOSCHANGING redirects any z-order change to HWND_BOTTOM. This
//    keeps the "always behind everything" behavior, but enforces it inside
//    the reposition operation itself, so the window can never appear above
//    other windows even for a single frame — unlike raise-then-re-pin, which
//    left a visible gap until the next periodic SetWindowPos.
LRESULT CALLBACK WallpaperWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto it = g_wallpaperPrevProcs.find(hwnd);
    WNDPROC prevProc = (it != g_wallpaperPrevProcs.end()) ? it->second : nullptr;

    switch (msg) {
        case WM_ERASEBKGND:
            return 1; // report "already erased" — we own every pixel

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE; // clicking the wallpaper never activates it

        case WM_PAINT:
            // DWM lost our surface (e.g. a window that fully covered us just
            // closed, or we resumed from sleep). Ask the main loop for an
            // immediate repaint; chaining to the original proc validates the
            // region as usual so this doesn't storm.
            g_exposeRequested = true;
            break;

        case WM_WINDOWPOSCHANGING: {
            WINDOWPOS* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            // Only intervene when the operation actually changes z-order
            // (hwndInsertAfter is ignored when SWP_NOZORDER is set), so plain
            // moves/resizes pass through untouched.
            if (!(pos->flags & SWP_NOZORDER)) {
                pos->hwndInsertAfter = HWND_BOTTOM;
            }
            pos->flags |= SWP_NOACTIVATE;
            break;
        }

        case WM_NCDESTROY:
            // Window is going away: un-subclass and forget the handle, then
            // let the original procedure see its final message.
            g_wallpaperPrevProcs.erase(hwnd);
            if (prevProc) {
                SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)prevProc);
                return CallWindowProc(prevProc, hwnd, msg, wParam, lParam);
            }
            break;
    }

    return prevProc ? CallWindowProc(prevProc, hwnd, msg, wParam, lParam)
                    : DefWindowProc(hwnd, msg, wParam, lParam);
}

struct Color {
    int r, g, b;
    Color(int red = 0, int green = 0, int blue = 0) : r(red), g(green), b(blue) {}
};

struct SolarTimes {
    double sunrise_hour;
    double sunset_hour;
    double solar_noon_hour;
    double civil_twilight_begin;
    double civil_twilight_end;
    bool valid = false;
    std::string fetch_date;
    std::string source; // "api", "cache", or "fallback"
};

struct SolarCache {
    std::vector<SolarTimes> days; // 8 days: today + next 7 days
    std::string last_updated;
};

struct Config {
    double latitude = 40.7128;   // Default: NYC
    double longitude = -74.0060;
    std::string location_name = "New York City";
    int update_interval_minutes = 1;
    bool debug_mode = false;
    bool auto_detect_location = true;
};

class CircadianWallpaper {
private:
    struct MonitorWindow {
        std::unique_ptr<sf::RenderWindow> window;
        sf::Sprite watermarkSprite;
        int x, y, width, height;
        // Off-screen target the sky gradient is rendered into on the GPU. Its
        // texture is then drawn to the window and also fed to the watermark
        // (Soft Light) shader as the background layer, exactly as the old
        // CPU-built gradient texture was.
        std::unique_ptr<sf::RenderTexture> gradientRT;
    };

    Config config;
    bool offscreenMode; // --earth-preview: no windows, no network, render offscreen only
    std::vector<MonitorWindow> monitors;
    // Earth globe: rasterized by the app from vector coastlines (earth_land.bin,
    // Natural Earth 1:110m land, ~16 KB) rather than from pre-rendered frames.
    // The view longitude is tied to UTC time of day: longitude 0 faces the
    // viewer at 12:00 UTC and the globe turns 0.25 deg per minute, so it
    // completes exactly one rotation per day. It is re-rendered once a minute.
    static const int kEarthSize = 768;            // globe texture is kEarthSize^2
    static const int kEarthMinutesPerDay = 24 * 60;
    static constexpr double kEarthRimPx = 1.5;    // opaque outline at the disc edge
    struct EarthVec { float x, y, z; };           // unit vector: x->lon 0, y->lon 90E, z->north
    std::vector<std::vector<EarthVec>> earthRings; // coastline rings (~4k vertices, ~50 KB)
    sf::Texture earthTexture;       // the rendered globe (the only large earth asset)
    int renderedEarthMinute;        // minute-since-noon-UTC earthTexture shows (-1 = none)
    bool hasWatermark;
    time_t lastAccentColorUpdate;
    sf::Shader watermarkShader;
    bool watermarkShaderLoaded;
    sf::Shader gradientShader;       // GPU sky-gradient fill (replaces CPU pixel loop)
    bool gradientShaderLoaded;
    sf::Texture fullscreenTex;       // 1x1 white texture, scaled to cover a target so
    bool fullscreenTexReady = false; // the gradient shader runs over every fragment

    struct ColorPoint {
        double hour;
        Color horizonColor;   // altitude 0.0 (bottom of screen / horizon)
        Color midSkyColor;    // altitude ~0.5 (mid-sky, captures pink/purple bands)
        Color zenithColor;    // altitude 1.0 (top of screen / zenith)
        double gamma;         // controls altitude blend curve (1.0 = linear, >1 compresses warm to horizon)
        std::string period;
    };

    struct SkySlice {
        Color horizon, midSky, zenith;
        double gamma;
    };

    SolarTimes todaysSolarTimes;
    SolarCache solarCache;

    // Async solar fetch. All HTTP happens on this worker thread so the render
    // loop never blocks on the network: the old synchronous fetch (up to 8
    // sequential requests with 5-10s timeouts each) ran on the render thread
    // right after wake / at the first frame of a new day, leaving the
    // wallpaper unpainted for seconds — visible as black flashing on login.
    // The worker stages its result in pendingSolarCache; the main loop adopts
    // it via applyPendingSolarResult(), so solarCache/todaysSolarTimes are
    // only ever touched by the main thread.
    std::thread solarFetchThread;
    std::atomic<bool> solarFetchInProgress{false};
    std::atomic<bool> solarResultReady{false};
    SolarCache pendingSolarCache;   // handoff buffer, guarded by pendingSolarMutex
    std::mutex pendingSolarMutex;

    struct MonitorRect {
        int x, y, width, height;
    };

    static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
        auto* monitors = reinterpret_cast<std::vector<MonitorWindow>*>(dwData);

        MONITORINFO info;
        info.cbSize = sizeof(MONITORINFO);
        GetMonitorInfo(hMonitor, &info);

        MonitorWindow mw;
        // Use full monitor bounds so the wallpaper spans the entire screen
        // top-to-bottom (rcMonitor includes the taskbar area; rcWork excludes it).
        mw.x = info.rcMonitor.left;
        mw.y = info.rcMonitor.top;
        mw.width = info.rcMonitor.right - info.rcMonitor.left;
        mw.height = info.rcMonitor.bottom - info.rcMonitor.top;

        monitors->push_back(std::move(mw));
        return TRUE;
    }

    static BOOL CALLBACK MonitorRectEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
        auto* rects = reinterpret_cast<std::vector<MonitorRect>*>(dwData);
        MONITORINFO info;
        info.cbSize = sizeof(MONITORINFO);
        GetMonitorInfo(hMonitor, &info);
        MonitorRect r;
        // Full monitor bounds (see MonitorEnumProc) — spans the whole screen.
        r.x = info.rcMonitor.left;
        r.y = info.rcMonitor.top;
        r.width = info.rcMonitor.right - info.rcMonitor.left;
        r.height = info.rcMonitor.bottom - info.rcMonitor.top;
        rects->push_back(r);
        return TRUE;
    }

    // Shared window setup used by initial construction and reconciliation.
    // Creates the SFML render window for a monitor entry whose x/y/width/height
    // are already populated, configures window style, and positions the
    // watermark sprite.
    void setupWindowForMonitor(MonitorWindow& m) {
        m.window = std::make_unique<sf::RenderWindow>(
            sf::VideoMode(m.width, m.height),
            "CircadianWallpaper",
            sf::Style::None
        );
        // No setFramerateLimit: presentation is driven by the change-detecting
        // main loop, not by SFML's internal sleep-to-cap logic.
        m.window->setPosition(sf::Vector2i(m.x, m.y));

        HWND hwnd = m.window->getSystemHandle();

        // Subclass so WM_ERASEBKGND is swallowed and bottom-most z-order is
        // enforced at reposition time (see WallpaperWndProc). Registered
        // before the window becomes interactive; single-threaded, so no
        // message can arrive between these two statements.
        WNDPROC prevProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC,
                                                     (LONG_PTR)WallpaperWndProc);
        g_wallpaperPrevProcs[hwnd] = prevProc;

        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        // WS_EX_TOOLWINDOW: keep it out of Alt-Tab / taskbar.
        // WS_EX_NOACTIVATE: clicking the wallpaper must never activate it or
        // pull it forward in the z-order, so it stays behind the taskbar.
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        ShowWindow(hwnd, SW_HIDE);
        ShowWindow(hwnd, SW_SHOW);

        // Pin to the very bottom of the z-order so it sits beneath the taskbar
        // and all other windows. SWP_NOACTIVATE avoids stealing focus.
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        // Off-screen target for the GPU-rendered sky gradient, sized to this
        // monitor. Recreated here so it tracks resolution changes.
        if (gradientShaderLoaded) {
            m.gradientRT = std::make_unique<sf::RenderTexture>();
            if (!m.gradientRT->create(m.width, m.height)) {
                m.gradientRT.reset(); // fall back to CPU path if RT unavailable
            }
        }

        if (hasWatermark) {
            // Ensure the globe is rendered for the current minute, then bind the
            // single texture. updateEarthSprites() refreshes this every
            // tick; this just gives a newly-(re)created window something
            // correct to show now.
            if (ensureEarthRendered(computeEarthMinuteForNow()) && earthTexture.getSize().x != 0) {
                m.watermarkSprite.setTexture(earthTexture, true);
                sf::FloatRect bounds = m.watermarkSprite.getLocalBounds();
                m.watermarkSprite.setOrigin(bounds.width / 2.0f, bounds.height / 2.0f);
                m.watermarkSprite.setPosition(m.width / 2.0f, m.height / 2.0f);
                m.watermarkSprite.setColor(sf::Color(255, 255, 255, 255));
            }
        }

        // Paint the window with the current sky right now. A freshly-created
        // window (startup, monitor hotplug, reconcile) otherwise shows its
        // uninitialized black surface until the next render tick — and at
        // startup that tick waits behind the solar-times fetch, which can
        // block on HTTP for seconds.
        {
            time_t now = time(0);
            tm* ti = localtime(&now);
            double hour = ti->tm_hour + (ti->tm_min / 60.0) + (ti->tm_sec / 3600.0);
            renderMonitor(m, getSkySliceForHour(hour), computeWatermarkInvertProgress());
        }
    }

    // Re-enumerate monitors and reconcile the `monitors` vector with the
    // current display topology. Existing windows whose rect is unchanged are
    // kept. Existing windows whose monitor resized/moved are updated in place.
    // Windows whose monitor is gone are destroyed. New monitors get new windows.
    void reconcileMonitors() {
        std::vector<MonitorRect> current;
        EnumDisplayMonitors(NULL, NULL, MonitorRectEnumProc, reinterpret_cast<LPARAM>(&current));

        // Mark which current rects have been matched to existing monitors.
        std::vector<bool> matched(current.size(), false);

        // First pass: update or drop existing entries.
        for (auto it = monitors.begin(); it != monitors.end(); ) {
            int bestIdx = -1;
            // Prefer an exact rect match (same monitor, unchanged).
            for (size_t i = 0; i < current.size(); i++) {
                if (matched[i]) continue;
                if (current[i].x == it->x && current[i].y == it->y &&
                    current[i].width == it->width && current[i].height == it->height) {
                    bestIdx = (int)i;
                    break;
                }
            }
            // Fall back to same origin with changed size (resolution change on the same monitor).
            if (bestIdx < 0) {
                for (size_t i = 0; i < current.size(); i++) {
                    if (matched[i]) continue;
                    if (current[i].x == it->x && current[i].y == it->y) {
                        bestIdx = (int)i;
                        break;
                    }
                }
            }

            if (bestIdx < 0) {
                // No matching monitor anymore — close and drop.
                std::cout << "Monitor removed: " << it->width << "x" << it->height
                          << " at (" << it->x << ", " << it->y << ")" << std::endl;
                if (it->window && it->window->isOpen()) it->window->close();
                it = monitors.erase(it);
                continue;
            }

            matched[bestIdx] = true;
            const MonitorRect& r = current[bestIdx];
            bool rectChanged = (r.x != it->x || r.y != it->y ||
                                r.width != it->width || r.height != it->height);
            bool windowAlive = it->window && it->window->isOpen();

            if (rectChanged || !windowAlive) {
                std::cout << "Monitor updated: " << r.width << "x" << r.height
                          << " at (" << r.x << ", " << r.y << ")" << std::endl;
                it->x = r.x;
                it->y = r.y;
                it->width = r.width;
                it->height = r.height;
                if (windowAlive) it->window->close();
                setupWindowForMonitor(*it);
            }
            ++it;
        }

        // Second pass: create windows for newly-appeared monitors.
        for (size_t i = 0; i < current.size(); i++) {
            if (matched[i]) continue;
            MonitorWindow mw;
            mw.x = current[i].x;
            mw.y = current[i].y;
            mw.width = current[i].width;
            mw.height = current[i].height;
            std::cout << "Monitor added: " << mw.width << "x" << mw.height
                      << " at (" << mw.x << ", " << mw.y << ")" << std::endl;
            setupWindowForMonitor(mw);
            monitors.push_back(std::move(mw));
        }

        logMessage("Monitor topology reconciled: " + std::to_string(monitors.size()) + " monitor(s)");
    }

    // Read-only check: does the live display topology (EnumDisplayMonitors,
    // queried fresh right now) still match what `monitors` thinks it is?
    // WM_DISPLAYCHANGE/WM_DEVICECHANGE fire during a hotplug transition, not
    // after it settles — EnumDisplayMonitors called immediately (or even ~1s
    // later, per the debounce below) can return a stale or half-applied
    // topology. Reconciling against that bad snapshot produces a wrongly
    // sized window (unplug) or silently drops a just-added monitor (plug),
    // and since the event-driven reconcile only runs once per burst, nothing
    // ever corrects it. This is polled periodically as a safety net so a bad
    // reconcile gets caught and retried instead of persisting indefinitely.
    bool topologyMismatch() {
        std::vector<MonitorRect> current;
        EnumDisplayMonitors(NULL, NULL, MonitorRectEnumProc, reinterpret_cast<LPARAM>(&current));

        if (current.size() != monitors.size()) return true;

        std::vector<bool> matched(current.size(), false);
        for (const auto& m : monitors) {
            bool found = false;
            for (size_t i = 0; i < current.size(); i++) {
                if (matched[i]) continue;
                if (current[i].x == m.x && current[i].y == m.y &&
                    current[i].width == m.width && current[i].height == m.height) {
                    matched[i] = true;
                    found = true;
                    break;
                }
            }
            if (!found) return true;
        }
        return false;
    }

    // True when nothing but our own wallpaper windows and the shell's desktop
    // windows (Progman / WorkerW, which legitimately live at the very bottom)
    // sit below `hwnd` in z-order — i.e. re-pinning would be a no-op.
    static bool isEffectivelyBottom(HWND hwnd) {
        for (HWND w = GetWindow(hwnd, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) {
            if (!IsWindowVisible(w)) continue;
            if (g_wallpaperPrevProcs.count(w)) continue; // another of our windows
            char cls[64] = {0};
            GetClassNameA(w, cls, sizeof(cls));
            if (strcmp(cls, "Progman") == 0 || strcmp(cls, "WorkerW") == 0) continue;
            return false;
        }
        return true;
    }

    // Re-assert bottom-of-z-order for every wallpaper window. Explorer pulls
    // windows forward when taskbar/display settings change, which can leave
    // the wallpaper drawn over the taskbar. The primary enforcement is now
    // WM_WINDOWPOSCHANGING in WallpaperWndProc (which blocks raises before
    // they happen); this periodic pass is a safety net, and it skips the
    // SetWindowPos entirely when the window is already at the bottom so the
    // once-a-second call no longer churns the z-order (a flicker source).
    void pinWindowsToBottom() {
        for (auto& m : monitors) {
            if (m.window && m.window->isOpen()) {
                HWND hwnd = m.window->getSystemHandle();
                if (isEffectivelyBottom(hwnd)) continue;
                SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }

public:
    CircadianWallpaper(bool offscreen = false) : offscreenMode(offscreen), renderedEarthMinute(-1), hasWatermark(false), lastAccentColorUpdate(0), watermarkShaderLoaded(false), gradientShaderLoaded(false) {
        std::cout << "Initializing CircadianWallpaper..." << std::endl;

        loadConfig();

        if (config.auto_detect_location && !offscreenMode) {
            autoDetectLocation();
        }

        // Load existing solar cache
        loadSolarCache();

        // Seed today's solar times from the cache (or fallback averages)
        // BEFORE any window is created: setupWindowForMonitor paints the
        // window immediately, and getSkySliceForHour must never read the
        // default-constructed (uninitialized) todaysSolarTimes. The real
        // fetch in run() refreshes these afterwards, in the background.
        applySolarTimesFromCache();

        // Load watermark
        loadWatermark();

        // Load watermark Soft Light blend shader.
        //
        // The earth frame is composited onto the sky gradient as if it were a
        // GIMP layer set to "Soft Light". GIMP's (legacy) Soft Light is the
        // Pegtop formula: it first computes Screen of base I and top M, then
        //     result = ((1 - I)*M + Screen(I,M)) * I
        // which expands algebraically to the Pegtop form
        //     soft(a, b) = (1 - 2*b) * a*a + 2*a*b
        // where a = base (sky) channel and b = top (earth) channel, both in
        // [0,1]. We apply it per RGB channel. The day/night inversion still
        // applies, but to the EARTH (top) channel before blending, so the
        // existing after-sunset invert effect is preserved.
        //
        // Soft Light needs the underlying sky pixel, which a fragment shader
        // can't read back from the framebuffer. So we pass the already-built
        // gradient as a second sampler and locate the matching sky pixel from
        // the fragment's window coordinates.
        if (hasWatermark && sf::Shader::isAvailable()) {
            const std::string fragShader = R"(
uniform sampler2D texture;       // earth frame (top layer)
uniform sampler2D background;    // sky gradient (bottom layer)
uniform float invertAmount;      // 0 = normal, 1 = inverted earth
uniform vec2 resolution;         // window size in pixels
uniform float backgroundFlipY;   // 1 = flip Y when sampling background, 0 = no flip

float softLight(float a, float b) {
    // a = base (sky), b = top (earth) — GIMP/Pegtop Soft Light
    return (1.0 - 2.0 * b) * a * a + 2.0 * a * b;
}

void main() {
    vec4 earth = texture2D(texture, gl_TexCoord[0].xy);

    // Day/night inversion on the earth (top) layer, matching prior behavior.
    vec3 earthRGB = mix(earth.rgb, vec3(1.0) - earth.rgb, invertAmount);

    // Sky pixel directly beneath this fragment. gl_FragCoord is the absolute
    // window pixel (origin bottom-left). A CPU-built image texture is y-down
    // (row 0 at top) and needs a flip; an sf::RenderTexture is GL-oriented
    // (row 0 at bottom) and must NOT be flipped. backgroundFlipY selects.
    float fy = gl_FragCoord.y / resolution.y;
    vec2 skyUV = vec2(gl_FragCoord.x / resolution.x,
                      mix(fy, 1.0 - fy, backgroundFlipY));
    vec3 sky = texture2D(background, skyUV).rgb;

    vec3 blended = vec3(
        softLight(sky.r, earthRGB.r),
        softLight(sky.g, earthRGB.g),
        softLight(sky.b, earthRGB.b)
    );

    // Earth alpha controls how much of the Soft Light result shows; fully
    // transparent earth pixels leave the sky untouched.
    float a = earth.a * gl_Color.a;
    gl_FragColor = vec4(mix(sky, blended, a), 1.0);
}
)";
            watermarkShaderLoaded = watermarkShader.loadFromMemory(fragShader, sf::Shader::Fragment);
            if (!watermarkShaderLoaded) {
                std::cout << "Warning: Watermark Soft Light shader failed to compile" << std::endl;
            }
        }

        // Load the sky-gradient shader. This reproduces the former CPU pixel
        // loop on the GPU: a 3-layer altitude blend (horizon -> midSky ->
        // zenith) shaped by gamma, plus the same 8x8 ordered (Bayer) dither
        // that smooths banding. Computed per-fragment, so filling the whole
        // screen costs the GPU almost nothing and removes the per-frame CPU
        // pixel work and the multi-MB texture upload entirely.
        if (sf::Shader::isAvailable()) {
            const std::string gradFrag = R"(
uniform vec3 horizon;    // altitude 0.0 color (0..1)
uniform vec3 midSky;     // altitude 0.5 color
uniform vec3 zenith;     // altitude 1.0 color
uniform float gamma;     // altitude blend curve
uniform vec2 resolution; // target size in pixels

// 8x8 Bayer matrix (values 0..63), same as the original CPU dither.
const float bayer[64] = float[64](
     0.0,32.0, 8.0,40.0, 2.0,34.0,10.0,42.0,
    48.0,16.0,56.0,24.0,50.0,18.0,58.0,26.0,
    12.0,44.0, 4.0,36.0,14.0,46.0, 6.0,38.0,
    60.0,28.0,52.0,20.0,62.0,30.0,54.0,22.0,
     3.0,35.0,11.0,43.0, 1.0,33.0, 9.0,41.0,
    51.0,19.0,59.0,27.0,49.0,17.0,57.0,25.0,
    15.0,47.0, 7.0,39.0,13.0,45.0, 5.0,37.0,
    63.0,31.0,55.0,23.0,61.0,29.0,53.0,21.0
);

// 3-layer altitude blend, matching the CPU blendSkyColor().
vec3 blendSky(float altitude) {
    float t = pow(clamp(altitude, 0.0, 1.0), gamma);
    if (t <= 0.5) {
        return mix(horizon, midSky, t * 2.0);
    }
    return mix(midSky, zenith, (t - 0.5) * 2.0);
}

void main() {
    // gl_TexCoord[0] comes from the full-screen sprite: (0,0) top-left,
    // (1,1) bottom-right, so altitude 1.0 (zenith) is at the top, matching the
    // original 'altitude = 1 - y/height'.
    vec2 uv = gl_TexCoord[0].xy;

    float altitude     = 1.0 - uv.y;
    float altitudeNext = 1.0 - (uv.y + 1.0 / resolution.y); // next row down

    vec3 baseC = blendSky(altitude);
    vec3 nextC = blendSky(altitudeNext);

    // Ordered dither in 0..255 space, mirroring the integer CPU math:
    //   ch + trunc(threshold * abs(next-base)*255 * 0.5), then /255.
    float px = floor(uv.x * resolution.x);
    float py = floor(uv.y * resolution.y);
    int ix = int(mod(px, 8.0));
    int iy = int(mod(py, 8.0));
    float threshold = bayer[iy * 8 + ix] / 64.0 - 0.5;

    vec3 base255 = baseC * 255.0;
    vec3 diff255 = abs(nextC - baseC) * 255.0;
    vec3 dith    = trunc(threshold * diff255 * 0.5);
    vec3 out255  = clamp(base255 + dith, 0.0, 255.0);

    gl_FragColor = vec4(out255 / 255.0, 1.0);
}
)";
            gradientShaderLoaded = gradientShader.loadFromMemory(gradFrag, sf::Shader::Fragment);
            if (!gradientShaderLoaded) {
                std::cout << "Warning: Sky gradient shader failed to compile - using CPU fallback" << std::endl;
            }
        }

        // 1x1 white texture used as a stretchable full-screen quad: scaling its
        // sprite to the target size makes the gradient shader execute for every
        // fragment, while gl_TexCoord spans 0..1 across the target.
        {
            sf::Image white;
            white.create(1, 1, sf::Color::White);
            fullscreenTexReady = fullscreenTex.loadFromImage(white);
        }

        // Enumerate all monitors and create windows — skipped in offscreen
        // mode (see runEarthPreview()).
        if (!offscreenMode) {
            EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));

            std::cout << "Found " << monitors.size() << " monitor(s)" << std::endl;

            // Create a window for each monitor
            for (size_t i = 0; i < monitors.size(); i++) {
                auto& m = monitors[i];
                std::cout << "Monitor " << i << ": " << m.width << "x" << m.height
                          << " at (" << m.x << ", " << m.y << ")" << std::endl;
                setupWindowForMonitor(m);
            }
        }

        std::cout << "\n=== CircadianWallpaper v3.0 - SFML Edition ===" << std::endl;
        std::cout << "=================================================" << std::endl;
        std::cout << "Location: " << config.location_name << " (" << config.latitude << ", " << config.longitude << ")" << std::endl;
        std::cout << "Update interval: " << config.update_interval_minutes << " minute(s)" << std::endl;
        std::cout << "Monitors: " << monitors.size() << std::endl;
        std::cout << "Solar cache: " << getSolarCachePath() << " (8-day storage)" << std::endl;

        logMessage("CircadianWallpaper v3.0 - SFML Edition (8-Day Cache)");
        logMessage("=================================================");
        logMessage("Location: " + config.location_name + " (" + std::to_string(config.latitude) + ", " + std::to_string(config.longitude) + ")");
        logMessage("Update interval: " + std::to_string(config.update_interval_minutes) + " minute(s)");
        logMessage("Monitors: " + std::to_string(monitors.size()));
        logMessage("Solar cache: " + getSolarCachePath() + " (8-day storage)");
    }

    ~CircadianWallpaper() {
        // Reap the async solar worker before tearing anything else down — it
        // captures `this` and must not outlive the object.
        if (solarFetchThread.joinable()) solarFetchThread.join();
        for (auto& m : monitors) {
            if (m.window && m.window->isOpen()) {
                m.window->close();
            }
        }
    }
    
    void setWindowsAccentColor(const Color& bgColor) {
        // Determine brightness to decide whether to blend with black or white
        int brightness = (bgColor.r + bgColor.g + bgColor.b) / 3;

        Color accentColor;
        float blendAmount = 0.15f; // 15% opacity overlay

        if (brightness > 128) {
            // Light background - overlay black at 15% opacity
            accentColor.r = (unsigned char)(bgColor.r * (1.0f - blendAmount));
            accentColor.g = (unsigned char)(bgColor.g * (1.0f - blendAmount));
            accentColor.b = (unsigned char)(bgColor.b * (1.0f - blendAmount));
        } else {
            // Dark background - overlay white at 15% opacity
            accentColor.r = (unsigned char)(bgColor.r * (1.0f - blendAmount) + 255 * blendAmount);
            accentColor.g = (unsigned char)(bgColor.g * (1.0f - blendAmount) + 255 * blendAmount);
            accentColor.b = (unsigned char)(bgColor.b * (1.0f - blendAmount) + 255 * blendAmount);
        }

        // Skip the registry write and the system-wide broadcast when the
        // color hasn't actually changed: WM_SETTINGCHANGE/"ImmersiveColorSet"
        // makes Explorer re-theme the taskbar and window frames, which is
        // itself a visible flash. Sky colors drift slowly, so most periodic
        // updates dedupe away here.
        static Color lastAppliedAccent;
        static bool accentApplied = false;
        if (accentApplied &&
            accentColor.r == lastAppliedAccent.r &&
            accentColor.g == lastAppliedAccent.g &&
            accentColor.b == lastAppliedAccent.b) {
            return;
        }
        lastAppliedAccent = accentColor;
        accentApplied = true;

        // Format the color as ABGR DWORD (Windows format)
        DWORD colorValue = 0xFF000000 | (accentColor.b << 16) | (accentColor.g << 8) | accentColor.r;

        // Update registry directly without restarting explorer
        HKEY hKey;

        // Set DWM accent color
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows\\DWM", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            RegSetValueExA(hKey, "AccentColor", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));
            RegSetValueExA(hKey, "ColorizationColor", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));
            RegSetValueExA(hKey, "ColorizationAfterglow", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));
            RegSetValueExA(hKey, "AccentColorMenu", 0, REG_DWORD, (const BYTE*)&colorValue, sizeof(DWORD));

            // Create accent palette (8 color variations)
            BYTE palette[32];
            for (int i = 0; i < 8; i++) {
                int factor = 100 + (i - 4) * 10;
                palette[i * 4 + 0] = std::min(255, std::max(0, (accentColor.r * factor) / 100));
                palette[i * 4 + 1] = std::min(255, std::max(0, (accentColor.g * factor) / 100));
                palette[i * 4 + 2] = std::min(255, std::max(0, (accentColor.b * factor) / 100));
                palette[i * 4 + 3] = 255;
            }
            RegSetValueExA(hKey, "AccentPalette", 0, REG_BINARY, palette, 32);

            RegCloseKey(hKey);
        }

        // Notify system of color change
        SendMessageTimeout(HWND_BROADCAST, WM_DWMCOLORIZATIONCOLORCHANGED, colorValue, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
        SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)"ImmersiveColorSet", SMTO_ABORTIFHUNG, 1000, nullptr);

        logMessage("Accent color updated: RGB(" + std::to_string(accentColor.r) + ", " + std::to_string(accentColor.g) + ", " + std::to_string(accentColor.b) + ")");
    }

    std::string getConfigPath() {
        // Get the directory where the executable is located
        char exePath[MAX_PATH];
        GetModuleFileNameA(NULL, exePath, MAX_PATH);
        std::string exeDir = std::string(exePath);
        size_t lastSlash = exeDir.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            exeDir = exeDir.substr(0, lastSlash + 1);
        } else {
            exeDir = "";
        }
        
        // Go up one directory level
        size_t parentSlash = exeDir.find_last_of("\\/", exeDir.length() - 2);
        std::string parentDir;
        if (parentSlash != std::string::npos) {
            parentDir = exeDir.substr(0, parentSlash + 1);
        } else {
            parentDir = exeDir; // Fallback to current directory if parent not found
        }
        
        return parentDir + "config.ini";
    }
    
    void loadConfig() {
        std::string configPath = getConfigPath();
        std::ifstream configFile(configPath);
        if (configFile.is_open()) {
            std::string line;
            while (std::getline(configFile, line)) {
                if (line.empty() || line[0] == '#') continue;
                
                size_t equalPos = line.find('=');
                if (equalPos != std::string::npos) {
                    std::string key = line.substr(0, equalPos);
                    std::string value = line.substr(equalPos + 1);
                    
                    // Trim whitespace
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    value.erase(value.find_last_not_of(" \t") + 1);
                    
                    if (key == "latitude") config.latitude = std::stod(value);
                    else if (key == "longitude") config.longitude = std::stod(value);
                    else if (key == "location_name") config.location_name = value;
                    else if (key == "update_interval_minutes") config.update_interval_minutes = std::stoi(value);
                    else if (key == "debug_mode") config.debug_mode = (value == "true");
                    else if (key == "auto_detect_location") config.auto_detect_location = (value == "true");
                }
            }
            configFile.close();
            if (config.debug_mode) logMessage("Config loaded from config.ini");
        } else {
            createDefaultConfig();
        }
    }
    
    void createDefaultConfig() {
        std::string configPath = getConfigPath();
        std::ofstream configFile(configPath);
        if (configFile.is_open()) {
            configFile << "# CircadianWallpaper Configuration" << std::endl;
            configFile << "# auto_detect_location=true: Automatically detect your location via IP geolocation" << std::endl;
            configFile << "# auto_detect_location=false: Use manual coordinates below as primary location" << std::endl;
            configFile << "# Manual coordinates also serve as backup if auto-detection fails" << std::endl;
            configFile << "# Get coordinates from: https://www.latlong.net/" << std::endl;
            configFile << std::endl;
            configFile << "latitude=" << config.latitude << std::endl;
            configFile << "longitude=" << config.longitude << std::endl;
            configFile << "location_name=" << config.location_name << std::endl;
            configFile << "update_interval_minutes=" << config.update_interval_minutes << std::endl;
            configFile << "debug_mode=" << (config.debug_mode ? "true" : "false") << std::endl;
            configFile << "auto_detect_location=" << (config.auto_detect_location ? "true" : "false") << std::endl;
            configFile.close();
            logMessage("Created default config.ini - location will be auto-detected!");
        }
    }
    
    std::string httpGetWithTimeout(const std::string& url, int timeoutMs = 5000) {
        HINTERNET hInternet = InternetOpenA("CircadianWallpaper/2.0", INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
        if (!hInternet) return "";
        
        // Set timeouts
        DWORD timeout = timeoutMs;
        InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        
        HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), nullptr, 0, 
                                         INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        if (!hUrl) {
            InternetCloseHandle(hInternet);
            return "";
        }
        
        std::string response;
        char buffer[4096];
        DWORD bytesRead;
        
        while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            response.append(buffer, bytesRead);
        }
        
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return response;
    }
    
    bool detectLocationFromIP() {
        if (config.debug_mode) logMessage("Trying IP geolocation...");
        
        std::string response = httpGetWithTimeout("http://ip-api.com/json/?fields=status,lat,lon,city,regionName,country", 8000);
        
        if (response.empty()) {
            if (config.debug_mode) logMessage("IP geolocation failed - no response");
            return false;
        }
        
        if (response.find("\"status\":\"success\"") == std::string::npos) {
            if (config.debug_mode) logMessage("IP geolocation failed - invalid response");
            return false;
        }
        
        try {
            // Parse latitude
            size_t latPos = response.find("\"lat\":");
            if (latPos != std::string::npos) {
                size_t startPos = latPos + 6;
                size_t endPos = response.find_first_of(",}", startPos);
                if (endPos != std::string::npos) {
                    std::string latStr = response.substr(startPos, endPos - startPos);
                    config.latitude = std::stod(latStr);
                }
            }
            
            // Parse longitude
            size_t lonPos = response.find("\"lon\":");
            if (lonPos != std::string::npos) {
                size_t startPos = lonPos + 6;
                size_t endPos = response.find_first_of(",}", startPos);
                if (endPos != std::string::npos) {
                    std::string lonStr = response.substr(startPos, endPos - startPos);
                    config.longitude = std::stod(lonStr);
                }
            }
            
            // Parse city name
            size_t cityPos = response.find("\"city\":\"");
            if (cityPos != std::string::npos) {
                size_t startPos = cityPos + 8;
                size_t endPos = response.find("\"", startPos);
                if (endPos != std::string::npos) {
                    std::string city = response.substr(startPos, endPos - startPos);
                    
                    // Also get region for better location name
                    size_t regionPos = response.find("\"regionName\":\"");
                    if (regionPos != std::string::npos) {
                        size_t regionStart = regionPos + 14;
                        size_t regionEnd = response.find("\"", regionStart);
                        if (regionEnd != std::string::npos) {
                            std::string region = response.substr(regionStart, regionEnd - regionStart);
                            config.location_name = city + ", " + region;
                        } else {
                            config.location_name = city;
                        }
                    } else {
                        config.location_name = city;
                    }
                }
            }
            
            if (config.debug_mode) {
                logMessage("IP geolocation successful:");
                logMessage("  Location: " + config.location_name);
                logMessage("  Coordinates: " + std::to_string(config.latitude) + ", " + std::to_string(config.longitude));
            }
            
            return true;
            
        } catch (...) {
            if (config.debug_mode) logMessage("IP geolocation failed - parsing error");
            return false;
        }
    }
    
    bool autoDetectLocation() {
        if (!config.auto_detect_location) {
            if (config.debug_mode) logMessage("Auto-detection disabled in config");
            return false;
        }
        
        if (config.debug_mode) logMessage("Auto-detecting location...");
        
        // Try IP geolocation first (most reliable)
        if (detectLocationFromIP()) {
            saveLocationToConfig();
            return true;
        }
        
        if (config.debug_mode) logMessage("All location detection methods failed");
        return false;
    }
    
    void saveLocationToConfig() {
        // Update the config file with detected location
        std::string configPath = getConfigPath();
        std::ofstream configFile(configPath);
        if (configFile.is_open()) {
            configFile << "# CircadianWallpaper Configuration" << std::endl;
            configFile << "# Location auto-detected successfully!" << std::endl;
            configFile << "# Set auto_detect_location=false to use manual coordinates as primary location." << std::endl;
            configFile << "# Manual coordinates below serve as backup if auto-detection fails." << std::endl;
            configFile << std::endl;
            configFile << "latitude=" << config.latitude << std::endl;
            configFile << "longitude=" << config.longitude << std::endl;
            configFile << "location_name=" << config.location_name << std::endl;
            configFile << "update_interval_minutes=" << config.update_interval_minutes << std::endl;
            configFile << "debug_mode=" << (config.debug_mode ? "true" : "false") << std::endl;
            configFile << "auto_detect_location=" << (config.auto_detect_location ? "true" : "false") << std::endl;
            configFile.close();
            
            if (config.debug_mode) logMessage("Location saved to config.ini");
        }
    }
    
    // Convert an ISO-8601 UTC timestamp from the API
    // ("2026-11-02T22:02:18+00:00") to a local hour-of-day. The conversion
    // goes through the CRT's localtime so the DST rule in force ON THAT DATE
    // applies, not today's. The old code applied today's DST state to every
    // day in the 8-day cache, so a fetch made before a DST change stamped
    // the wrong offset onto the days after it (sunset an hour late).
    double parseTimeString(const std::string& timeStr) {
        int Y, M, D, h, m, s;
        if (std::sscanf(timeStr.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) {
            return 0.0;
        }
        tm utc;
        std::memset(&utc, 0, sizeof(utc));
        utc.tm_year = Y - 1900;
        utc.tm_mon  = M - 1;
        utc.tm_mday = D;
        utc.tm_hour = h;
        utc.tm_min  = m;
        utc.tm_sec  = s;
        time_t t = _mkgmtime(&utc);
        if (t == (time_t)-1) return 0.0;
        tm* local = localtime(&t);
        if (!local) return 0.0;
        return local->tm_hour + local->tm_min / 60.0 + local->tm_sec / 3600.0;
    }

    double parseTimeFromJson(const std::string& json, const std::string& key) {
        size_t keyPos = json.find(key);
        if (keyPos == std::string::npos) return 0.0;
        
        size_t startQuote = json.find("\"", keyPos + key.length());
        if (startQuote == std::string::npos) return 0.0;
        startQuote++;
        
        size_t endQuote = json.find("\"", startQuote);
        if (endQuote == std::string::npos) return 0.0;
        
        std::string timeStr = json.substr(startQuote, endQuote - startQuote);
        return parseTimeString(timeStr);
    }
    
    bool fetchSolarTimesForDate(const std::string& targetDate, SolarTimes& solarTimes, bool isRetry = false) {
        std::stringstream urlBuilder;
        urlBuilder << "https://api.sunrise-sunset.org/json?lat=" << config.latitude
                   << "&lng=" << config.longitude << "&date=" << targetDate << "&formatted=0";

        std::string url = urlBuilder.str();
        if (config.debug_mode) logMessage("Fetching solar times for " + targetDate + "..." + std::string(isRetry ? " (retry)" : ""));

        std::string response = httpGetWithTimeout(url, isRetry ? 10000 : 5000);

        if (response.empty()) {
            if (!isRetry) {
                if (config.debug_mode) logMessage("API request timed out, retrying once...");
                return fetchSolarTimesForDate(targetDate, solarTimes, true);
            }
            if (config.debug_mode) logMessage("API Error: Request timed out for " + targetDate);
            return false;
        }

        if (response.find("\"status\":\"OK\"") == std::string::npos) {
            if (config.debug_mode) logMessage("API Error: Invalid response for " + targetDate);
            return false;
        }

        try {
            solarTimes.sunrise_hour = parseTimeFromJson(response, "\"sunrise\":");
            solarTimes.sunset_hour = parseTimeFromJson(response, "\"sunset\":");
            solarTimes.solar_noon_hour = parseTimeFromJson(response, "\"solar_noon\":");
            solarTimes.civil_twilight_begin = parseTimeFromJson(response, "\"civil_twilight_begin\":");
            solarTimes.civil_twilight_end = parseTimeFromJson(response, "\"civil_twilight_end\":");

            if (solarTimes.sunrise_hour > 0 && solarTimes.sunset_hour > 0) {
                solarTimes.valid = true;
                solarTimes.fetch_date = targetDate;
                solarTimes.source = "api";

                if (config.debug_mode) {
                    logMessage("Solar times fetched successfully for " + targetDate + ":");
                    logMessage("  Sunrise: " + formatHour(solarTimes.sunrise_hour));
                    logMessage("  Sunset: " + formatHour(solarTimes.sunset_hour));
                }

                return true;
            }
        } catch (...) {
            if (config.debug_mode) logMessage("JSON parsing error for " + targetDate);
        }

        return false;
    }

    // Launch the 8-day solar fetch on a background thread. The result is
    // staged into pendingSolarCache and adopted by the main loop via
    // applyPendingSolarResult(); nothing here blocks the render loop. At most
    // one worker runs at a time.
    void startAsyncSolarFetch() {
        if (solarFetchInProgress.exchange(true)) return; // already running
        if (solarFetchThread.joinable()) solarFetchThread.join(); // reap finished worker
        solarFetchThread = std::thread([this]() {
            SolarCache fresh;
            fresh.days.resize(8);
            int successCount = 0;

            // Fetch data for today and next 7 days (8 days total)
            for (int dayOffset = 0; dayOffset < 8; dayOffset++) {
                std::string targetDate = getDateOffset(dayOffset);
                if (fetchSolarTimesForDate(targetDate, fresh.days[dayOffset])) {
                    successCount++;
                }
            }

            if (successCount > 0) {
                fresh.last_updated = getCurrentDate();
                std::lock_guard<std::mutex> lock(pendingSolarMutex);
                pendingSolarCache = std::move(fresh);
                solarResultReady = true;
            } else {
                logMessage("Async solar fetch: failed to fetch any data from API");
            }
            solarFetchInProgress = false;
        });
    }

    // Called once per main-loop iteration: if the worker finished, adopt its
    // cache, refresh todaysSolarTimes, and persist to disk — all on the main
    // thread, so the rest of the code never needs locking.
    void applyPendingSolarResult() {
        if (!solarResultReady.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lock(pendingSolarMutex);
            solarCache = pendingSolarCache;
        }
        SolarTimes* todaysData = findCachedDataForDate(getCurrentDate());
        if (todaysData && todaysData->valid) {
            todaysSolarTimes = *todaysData;
        }
        saveSolarCache();
        logMessage("Adopted async solar update (fetched " + solarCache.last_updated + ")");
    }

    // Pick the best available solar times WITHOUT touching the network:
    // today's cached entry, else any valid cached day, else fallback averages.
    // Always leaves todaysSolarTimes in a renderable state.
    void applySolarTimesFromCache() {
        std::string today = getCurrentDate();

        SolarTimes* todaysData = findCachedDataForDate(today);
        if (todaysData && todaysData->valid) {
            todaysSolarTimes = *todaysData;
            if (config.debug_mode) logMessage("Using cached solar times for " + today + " (source: " + todaysData->source + ")");
            return;
        }

        for (size_t i = 0; i < solarCache.days.size(); i++) {
            if (solarCache.days[i].valid) {
                todaysSolarTimes = solarCache.days[i];
                todaysSolarTimes.fetch_date = today;
                todaysSolarTimes.source = "cache-backup-day" + std::to_string(i);
                logMessage("Using cached solar times from day " + std::to_string(i) + " as backup for " + today);
                return;
            }
        }

        logMessage("No cached data available. Using fallback times.");
        useFallbackSolarTimes();
    }

    // True when today's data is stale or missing and no fetch is already
    // running — i.e. a background refresh is worth kicking off.
    bool solarRefreshNeeded() {
        return (shouldUpdateCache() || !todaysSolarTimes.valid) && !solarFetchInProgress;
    }

    SolarTimes* findCachedDataForDate(const std::string& targetDate) {
        for (size_t i = 0; i < solarCache.days.size(); i++) {
            if (solarCache.days[i].fetch_date == targetDate) {
                return &solarCache.days[i];
            }
        }
        return nullptr;
    }

    bool shouldUpdateCache() {
        std::string today = getCurrentDate();

        // Only update once per day - if we already updated today, don't update again
        if (solarCache.last_updated == today) {
            return false;
        }

        return true;
    }

    bool useFallbackSolarTimes() {
        todaysSolarTimes.sunrise_hour = 7.2;
        todaysSolarTimes.sunset_hour = 17.1;
        todaysSolarTimes.solar_noon_hour = 12.15;
        todaysSolarTimes.civil_twilight_begin = 6.7;
        todaysSolarTimes.civil_twilight_end = 17.6;
        todaysSolarTimes.valid = false;
        todaysSolarTimes.fetch_date = getCurrentDate();
        todaysSolarTimes.source = "fallback";

        logMessage("Using fallback solar times (NYC January average)");
        return false;
    }
    
    std::string getCurrentDate() {
        time_t now = time(0);
        tm* timeinfo = localtime(&now);

        std::stringstream ss;
        ss << (timeinfo->tm_year + 1900) << "-"
           << std::setfill('0') << std::setw(2) << (timeinfo->tm_mon + 1) << "-"
           << std::setw(2) << timeinfo->tm_mday;
        return ss.str();
    }

    // Local calendar date `dayOffset` days from today. Uses calendar
    // arithmetic (mktime normalizes tm_mday) rather than adding 86400 s: on
    // the 25-hour day a DST change ends, the seconds approach landed on the
    // same date twice and the cache got two entries for one day.
    std::string getDateOffset(int dayOffset) {
        time_t now = time(0);
        tm local = *localtime(&now);
        local.tm_mday += dayOffset;
        local.tm_isdst = -1;
        mktime(&local);
        tm* timeinfo = &local;

        std::stringstream ss;
        ss << (timeinfo->tm_year + 1900) << "-"
           << std::setfill('0') << std::setw(2) << (timeinfo->tm_mon + 1) << "-"
           << std::setw(2) << timeinfo->tm_mday;
        return ss.str();
    }

    std::string getSolarCachePath() {
        std::string configPath = getConfigPath();
        size_t lastSlash = configPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return configPath.substr(0, lastSlash + 1) + "solar_cache.txt";
        }
        return "solar_cache.txt";
    }

    void saveSolarCache() {
        std::string cachePath = getSolarCachePath();
        std::ofstream cacheFile(cachePath);
        if (!cacheFile.is_open()) {
            if (config.debug_mode) logMessage("Failed to save solar cache to " + cachePath);
            return;
        }

        cacheFile << "# CircadianWallpaper Solar Cache - Eight Day Data (Today + Next 7 Days)" << std::endl;
        cacheFile << "last_updated=" << solarCache.last_updated << std::endl;
        cacheFile << std::endl;

        // Save all 8 days of data
        for (size_t i = 0; i < solarCache.days.size(); i++) {
            const SolarTimes& dayData = solarCache.days[i];

            cacheFile << "[day" << i << "]" << std::endl;
            cacheFile << "date=" << dayData.fetch_date << std::endl;
            cacheFile << "sunrise=" << std::fixed << std::setprecision(6) << dayData.sunrise_hour << std::endl;
            cacheFile << "sunset=" << std::fixed << std::setprecision(6) << dayData.sunset_hour << std::endl;
            cacheFile << "solar_noon=" << std::fixed << std::setprecision(6) << dayData.solar_noon_hour << std::endl;
            cacheFile << "civil_twilight_begin=" << std::fixed << std::setprecision(6) << dayData.civil_twilight_begin << std::endl;
            cacheFile << "civil_twilight_end=" << std::fixed << std::setprecision(6) << dayData.civil_twilight_end << std::endl;
            cacheFile << "valid=" << (dayData.valid ? "true" : "false") << std::endl;
            cacheFile << "source=" << dayData.source << std::endl;
            cacheFile << std::endl;
        }

        cacheFile.close();
        if (config.debug_mode) logMessage("Solar cache saved successfully (8 days)");
    }

    bool loadSolarCache() {
        std::string cachePath = getSolarCachePath();
        std::ifstream cacheFile(cachePath);
        if (!cacheFile.is_open()) {
            if (config.debug_mode) logMessage("No existing solar cache found");
            return false;
        }

        // Initialize with 8 empty entries
        solarCache.days.clear();
        solarCache.days.resize(8);

        std::string line;
        int currentDayIndex = -1;

        while (std::getline(cacheFile, line)) {
            if (line.empty() || line[0] == '#') continue;

            // Check for day sections [day0], [day1], etc.
            if (line.substr(0, 4) == "[day" && line.back() == ']') {
                std::string dayStr = line.substr(4, line.length() - 5);
                currentDayIndex = std::stoi(dayStr);
                if (currentDayIndex < 0 || currentDayIndex >= 8) {
                    currentDayIndex = -1; // Invalid index
                }
                continue;
            }

            size_t equalPos = line.find('=');
            if (equalPos != std::string::npos) {
                std::string key = line.substr(0, equalPos);
                std::string value = line.substr(equalPos + 1);

                if (key == "last_updated") {
                    solarCache.last_updated = value;
                } else if (currentDayIndex >= 0 && currentDayIndex < 8) {
                    SolarTimes& currentTimes = solarCache.days[currentDayIndex];
                    if (key == "date") currentTimes.fetch_date = value;
                    else if (key == "sunrise") currentTimes.sunrise_hour = std::stod(value);
                    else if (key == "sunset") currentTimes.sunset_hour = std::stod(value);
                    else if (key == "solar_noon") currentTimes.solar_noon_hour = std::stod(value);
                    else if (key == "civil_twilight_begin") currentTimes.civil_twilight_begin = std::stod(value);
                    else if (key == "civil_twilight_end") currentTimes.civil_twilight_end = std::stod(value);
                    else if (key == "valid") currentTimes.valid = (value == "true");
                    else if (key == "source") currentTimes.source = value;
                }
            }
        }

        cacheFile.close();
        if (config.debug_mode) logMessage("Solar cache loaded successfully (8 days)");
        return true;
    }
    
    // Debug-mode log of the solar times the palette is built around.
    void logSolarSchedule() {
        if (config.debug_mode) {
            logMessage("Solar-based continuous color calculation initialized");
            logMessage("  Sunrise: " + formatHour(todaysSolarTimes.sunrise_hour));
            logMessage("  Solar Noon: " + formatHour(todaysSolarTimes.solar_noon_hour));
            logMessage("  Sunset: " + formatHour(todaysSolarTimes.sunset_hour));
            logMessage("  Civil Twilight End: " + formatHour(todaysSolarTimes.civil_twilight_end));
        }
    }
    
    Color getCurrentColor() {
        time_t now = time(0);
        tm* timeinfo = localtime(&now);
        // Include seconds for 15-second granularity
        double currentHour = timeinfo->tm_hour + (timeinfo->tm_min / 60.0) + (timeinfo->tm_sec / 3600.0);

        return getColorForHour(currentHour);
    }
    
    
    SkySlice getSkySliceForHour(double hour, std::string* outPeriod = nullptr) {
        double sunrise = todaysSolarTimes.sunrise_hour;
        double sunset = todaysSolarTimes.sunset_hour;
        double solar_noon = todaysSolarTimes.solar_noon_hour;

        // Normalize hour to 0-24 range
        while (hour < 0) hour += 24.0;
        while (hour >= 24) hour -= 24.0;

        // Create base timeline points ensuring no overlaps
        std::vector<ColorPoint> points;
        
        // Night and early morning
        points.push_back({0.0, Color(7, 8, 26), Color(6, 7, 22), Color(5, 6, 20), 1.0, "Deep Night"});
        points.push_back({std::max(3.0, sunrise - 3), Color(8, 9, 27), Color(7, 8, 24), Color(6, 7, 21), 1.0, "Pre-Dawn"});
        points.push_back({std::max(5.0, sunrise - 1.5), Color(18, 21, 53), Color(15, 18, 48), Color(12, 14, 42), 1.0, "Early Dawn"});
        points.push_back({std::max(5.0, sunrise - 1), Color(25, 26, 65), Color(20, 22, 58), Color(15, 17, 50), 0.9, "Early Dawn"});
        points.push_back({std::max(6.0, sunrise - 0.5), Color(50, 55, 60), Color(50, 50, 70), Color(30, 35, 65), 0.6, "Dawn"});
        points.push_back({std::max(6.0, sunrise - 0.25), Color(106, 80, 55), Color(80, 65, 108), Color(45, 50, 85), 0.5, "Dawn"});

        // Sunrise and morning
        points.push_back({std::max(6.0, sunrise), Color(210, 140, 70), Color(151, 120, 140), Color(100, 120, 170), 0.5, "Sunrise"});
        points.push_back({std::max(7.0, sunrise + 0.25), Color(220, 180, 120), Color(180, 190, 170), Color(150, 190, 220), 0.7, "Sunrise"});
        points.push_back({std::max(7.0, sunrise + 0.5), Color(200, 210, 190), Color(185, 205, 210), Color(170, 200, 225), 0.8, "Early Morning"});
        points.push_back({std::max(7.0, sunrise + 1), Color(190, 212, 215), Color(180, 210, 225), Color(160, 200, 230), 1.0, "Early Morning"});
        points.push_back({std::max(8.0, sunrise + 2), Color(185, 215, 225), Color(175, 212, 230), Color(150, 195, 235), 1.0, "Morning"});

        // Day time
        points.push_back({std::max(11.0, solar_noon - 1.5), Color(185, 215, 225), Color(175, 212, 230), Color(140, 190, 235), 1.0, "Late Morning"});
        points.push_back({std::max(11.0, solar_noon - 1), Color(185, 215, 225), Color(175, 212, 230), Color(140, 190, 235), 1.0, "Late Morning"});
        points.push_back({std::max(12.0, solar_noon), Color(185, 215, 225), Color(175, 212, 230), Color(140, 190, 235), 1.0, "Noon"});
        points.push_back({std::max(13.0, solar_noon + 1), Color(185, 215, 225), Color(175, 212, 230), Color(140, 190, 235), 1.0, "Early Afternoon"});
        points.push_back({std::max(13.5, solar_noon + 1.5), Color(185, 215, 225), Color(175, 212, 230), Color(140, 190, 235), 1.0, "Early Afternoon"});

        // Afternoon to sunset - ensure progressive timing
        double pre_sunset_start = sunset - 1.5;
        double pre_sunset_mid = sunset - 1.0;
        double pre_sunset_end = sunset - 0.5;
        double sunset_time = sunset;

        points.push_back({pre_sunset_start, Color(185, 215, 225), Color(175, 212, 230), Color(140, 190, 235), 1.0, "Late Afternoon"});
        points.push_back({pre_sunset_mid, Color(185, 215, 225), Color(175, 212, 230), Color(140, 190, 235), 1.0, "Pre-Sunset"});
        points.push_back({pre_sunset_end, Color(195, 200, 210), Color(180, 205, 225), Color(155, 195, 235), 0.8, "Pre-Sunset"});
        points.push_back({sunset_time, Color(210, 170, 120), Color(198, 181, 192), Color(170, 200, 230), 0.7, "Sunset"});

        // Post-sunset to evening - ensure monotonic progression
        double post_sunset_1 = sunset_time + 0.1;
        double post_sunset_2 = post_sunset_1 + 0.1;
        double post_sunset_3 = post_sunset_2 + 0.1;
        double twilight_1 = post_sunset_3 + 0.1;
        double twilight_2 = twilight_1 + 0.1;
        double twilight_3 = twilight_2 + 0.1;

        points.push_back({post_sunset_1, Color(210, 140, 70), Color(154, 128, 140), Color(130, 155, 190), 0.5, "Sunset"});
        points.push_back({post_sunset_2, Color(124, 108, 112), Color(93, 90, 120), Color(88, 100, 140), 0.5, "Post-Sunset"});
        points.push_back({post_sunset_3, Color(82, 83, 96), Color(86, 91, 142), Color(63, 75, 120), 0.5, "Post-Sunset"});
        points.push_back({twilight_1, Color(59, 72, 107), Color(73, 76, 140), Color(56, 60, 100), 0.5, "Post-Sunset"});
        points.push_back({twilight_2, Color(29, 45, 75), Color(34, 40, 75), Color(30, 38, 80), 0.7, "Civil Twilight"});
        points.push_back({twilight_3, Color(25, 43, 70), Color(34, 38, 62), Color(28, 32, 55), 1.0, "Evening"});

        // Evening progression - based on twilight end
        double evening_start = twilight_3 + 0.1;
        points.push_back({evening_start, Color(26, 33, 60), Color(23, 29, 53), Color(18, 24, 46), 1.0, "Evening"});
        points.push_back({evening_start + 0.10, Color(22, 25, 57), Color(19, 22, 50), Color(15, 18, 43), 1.0, "Evening"});
        points.push_back({evening_start + 0.20, Color(18, 21, 53), Color(15, 18, 46), Color(12, 14, 40), 1.0, "Evening"});
        points.push_back({evening_start + 0.30, Color(13, 15, 43), Color(11, 13, 38), Color(9, 11, 33), 1.0, "Evening"});
        points.push_back({evening_start + 0.80, Color(12, 14, 39), Color(10, 12, 34), Color(8, 10, 30), 1.0, "Evening"});
        points.push_back({evening_start + 1.05, Color(11, 12, 35), Color(9, 10, 31), Color(7, 9, 27), 1.0, "Evening"});
        points.push_back({evening_start + 1.55, Color(10, 11, 31), Color(8, 9, 27), Color(7, 8, 24), 1.0, "Late Evening"});
        points.push_back({evening_start + 2.05, Color(9, 10, 28), Color(7, 8, 25), Color(6, 7, 22), 1.0, "Late Evening"});
        points.push_back({evening_start + 2.75, Color(8, 9, 27), Color(7, 8, 24), Color(6, 7, 21), 1.0, "Late Evening"});
        points.push_back({evening_start + 3.25, Color(7, 8, 26), Color(6, 7, 22), Color(5, 6, 20), 1.0, "Night"});
        points.push_back({23.99, Color(7, 8, 26), Color(6, 7, 22), Color(5, 6, 20), 1.0, "Night"});
        
        // Fix times outside 0-24 range
        for (auto& point : points) {
            while (point.hour < 0) point.hour += 24.0;
            while (point.hour >= 24) point.hour -= 24.0;
        }
        
        // Sort points by time
        std::sort(points.begin(), points.end(), 
                  [](const ColorPoint& a, const ColorPoint& b) {
                      return a.hour < b.hour;
                  });
        
        // Helper to interpolate a SkySlice between two ColorPoints
        auto interpolateSlice = [this](const ColorPoint& a, const ColorPoint& b, double progress) -> SkySlice {
            return {
                interpolateColor(a.horizonColor, b.horizonColor, progress),
                interpolateColor(a.midSkyColor, b.midSkyColor, progress),
                interpolateColor(a.zenithColor, b.zenithColor, progress),
                a.gamma * (1.0 - progress) + b.gamma * progress
            };
        };

        // Find the appropriate color interpolation
        for (size_t i = 0; i < points.size() - 1; i++) {
            if (hour >= points[i].hour && hour <= points[i + 1].hour) {
                double progress = (hour - points[i].hour) / (points[i + 1].hour - points[i].hour);
                if (outPeriod) *outPeriod = points[i].period;
                return interpolateSlice(points[i], points[i + 1], progress);
            }
        }

        // Handle wrap-around (from last point to first point)
        double lastHour = points.back().hour;
        double firstHour = points.front().hour + 24;

        if (hour >= lastHour) {
            double progress = (hour - lastHour) / (firstHour - lastHour);
            if (outPeriod) *outPeriod = points.back().period;
            return interpolateSlice(points.back(), points.front(), progress);
        }

        if (outPeriod) *outPeriod = points.front().period;
        return { points.front().horizonColor, points.front().midSkyColor, points.front().zenithColor, points.front().gamma };
    }

    // Backward-compatible wrapper: returns horizon color for a given hour
    Color getColorForHour(double hour, std::string* outPeriod = nullptr) {
        SkySlice slice = getSkySliceForHour(hour, outPeriod);
        return slice.horizon;
    }
    
    std::string getCurrentPeriod() {
        time_t now = time(0);
        tm* timeinfo = localtime(&now);
        double currentHour = timeinfo->tm_hour + (timeinfo->tm_min / 60.0);

        std::string period;
        getColorForHour(currentHour, &period);
        return period;
    }
    
    Color interpolateColor(Color start, Color end, double ratio) {
        ratio = std::max(0.0, std::min(1.0, ratio));
        
        int r = static_cast<int>(start.r * (1 - ratio) + end.r * ratio);
        int g = static_cast<int>(start.g * (1 - ratio) + end.g * ratio);
        int b = static_cast<int>(start.b * (1 - ratio) + end.b * ratio);
        
        return Color(r, g, b);
    }
    
    std::string formatHour(double hour) {
        int h = static_cast<int>(hour);
        int m = static_cast<int>((hour - h) * 60);
        
        std::string ampm = "AM";
        if (h == 0) h = 12;
        else if (h == 12) ampm = "PM";
        else if (h > 12) { h -= 12; ampm = "PM"; }
        
        std::stringstream ss;
        ss << h << ":" << std::setfill('0') << std::setw(2) << m << " " << ampm;
        return ss.str();
    }
    
    void loadWatermark() {
        std::string configPath = getConfigPath();
        size_t lastSlash = configPath.find_last_of("\\/");
        std::string baseDir;
        if (lastSlash != std::string::npos) {
            baseDir = configPath.substr(0, lastSlash + 1);
        } else {
            baseDir = "";
        }
        std::string landPath = baseDir + "earth_land.bin";

        // Vector coastlines (Natural Earth 1:110m land, simplified for a
        // 768 px globe by tools/build_earth_land.py). Format:
        //   "CWEL" u16 version u16 pad u32 ringCount
        //   per ring: u32 n, then n x (i16 lon*100, i16 lat*100)
        // Vertices are converted to unit vectors once here; everything at
        // runtime is a rotation + hemisphere clip + scanline fill.
        std::ifstream f(landPath, std::ios::binary);
        std::vector<unsigned char> bytes;
        if (f.is_open()) {
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        auto rd16 = [&](size_t off) -> int16_t {
            return (int16_t)(bytes[off] | (bytes[off + 1] << 8));
        };
        auto rd32 = [&](size_t off) -> uint32_t {
            return (uint32_t)bytes[off] | ((uint32_t)bytes[off + 1] << 8) |
                   ((uint32_t)bytes[off + 2] << 16) | ((uint32_t)bytes[off + 3] << 24);
        };

        earthRings.clear();
        bool ok = bytes.size() >= 12 && std::memcmp(bytes.data(), "CWEL", 4) == 0 && rd16(4) == 1;
        size_t off = 12;
        uint32_t ringCount = ok ? rd32(8) : 0;
        size_t vertexTotal = 0;
        for (uint32_t r = 0; ok && r < ringCount; r++) {
            if (off + 4 > bytes.size()) { ok = false; break; }
            uint32_t n = rd32(off);
            off += 4;
            if (n < 3 || off + (size_t)n * 4 > bytes.size()) { ok = false; break; }
            std::vector<EarthVec> ring(n);
            for (uint32_t i = 0; i < n; i++, off += 4) {
                double lon = rd16(off) / 100.0 * M_PI / 180.0;
                double lat = rd16(off + 2) / 100.0 * M_PI / 180.0;
                double cl = std::cos(lat);
                ring[i] = { (float)(cl * std::cos(lon)), (float)(cl * std::sin(lon)), (float)std::sin(lat) };
            }
            vertexTotal += n;
            earthRings.push_back(std::move(ring));
        }

        if (!ok || earthRings.empty()) {
            hasWatermark = false;
            earthRings.clear();
            std::cout << "ERROR: earth_land.bin missing or invalid at " << landPath
                      << " - watermark disabled" << std::endl;
            logMessage("earth_land.bin missing or invalid - watermark disabled");
            return;
        }

        if (!earthTexture.create(kEarthSize, kEarthSize)) {
            hasWatermark = false;
            earthRings.clear();
            std::cout << "ERROR: could not create earth texture - watermark disabled" << std::endl;
            logMessage("Earth texture creation failed - watermark disabled");
            return;
        }
        earthTexture.setSmooth(true);

        hasWatermark = true;
        // Draw the globe for the current UTC minute so there's something
        // correct to show immediately.
        ensureEarthRendered(computeEarthMinuteForNow());

        std::cout << "Loaded earth coastlines: " << earthRings.size() << " rings, "
                  << vertexTotal << " vertices (" << (bytes.size() / 1024) << " KB); "
                  << "re-rendered once per minute" << std::endl;
        logMessage("Earth coastlines loaded: " + std::to_string(vertexTotal) + " vertices");
    }

    // Minutes since 12:00 UTC (0..1439). The globe is viewed from directly
    // above the meridian that faces the sun at noon UTC (longitude 0) and
    // turns 0.25 deg per minute, one full rotation per day.
    int computeEarthMinuteForNow() {
        time_t now = time(0);
        tm* utc = gmtime(&now);
        int minutesSinceNoon = (utc->tm_hour - 12) * 60 + utc->tm_min;
        if (minutesSinceNoon < 0) minutesSinceNoon += 24 * 60;
        return minutesSinceNoon % kEarthMinutesPerDay;
    }

    // Make earthTexture show the globe for `minuteSinceNoonUTC`, rendering
    // only if it isn't already showing that minute. Returns true if the
    // texture holds a valid globe afterward.
    bool ensureEarthRendered(int minuteSinceNoonUTC) {
        if (!hasWatermark) return false;
        if (minuteSinceNoonUTC == renderedEarthMinute) return true;
        renderEarth(-360.0 * minuteSinceNoonUTC / kEarthMinutesPerDay);
        renderedEarthMinute = minuteSinceNoonUTC;
        return true;
    }

    // Rasterize the globe with longitude `lon0Deg` facing the viewer into
    // earthTexture. Output matches the old pre-rendered frames: ocean is
    // opaque black, land and the area outside the disc are transparent, with
    // hard (unantialiased) edges. A thin opaque rim (kEarthRimPx) always
    // outlines the disc so land that reaches the limb doesn't merge into the
    // sky behind it. North is up; no axial tilt.
    //
    // Each ring is rotated on the sphere, clipped to the visible hemisphere
    // (Sutherland-Hodgman against z >= 0; the cut is closed along the limb
    // circle so coastlines meeting the edge of the disc stay flush with it),
    // projected orthographically, and filled with an even-odd scanline pass.
    // Even-odd handles holes (the Caspian) and the seam-free merged rings.
    // ~4k vertices and 768 rows: about 2 ms of work including the texture
    // upload (see --earth-preview), once a minute. The RGBA staging buffer is
    // local so no multi-MB CPU copy lingers between renders.
    void renderEarth(double lon0Deg) {
        const int S = kEarthSize;
        const double R = S / 2.0, cx = S / 2.0, cy = S / 2.0;
        const double c0 = std::cos(lon0Deg * M_PI / 180.0);
        const double s0 = std::sin(lon0Deg * M_PI / 180.0);

        // Per-scanline x crossings of land-ring edges (pixel-centre sampling).
        std::vector<std::vector<float>> rows(S);
        std::vector<std::pair<double,double>> pts; // projected ring, unit disc coords
        std::vector<std::array<double,3>> view;

        auto addEdge = [&](double x0, double y0, double x1, double y1) {
            if (y0 == y1) return;
            if (y0 > y1) { std::swap(x0, x1); std::swap(y0, y1); }
            int r0 = std::max(0, (int)std::ceil(y0 - 0.5));
            int r1 = std::min(S - 1, (int)std::ceil(y1 - 0.5) - 1);
            if (r1 < r0) return;
            double slope = (x1 - x0) / (y1 - y0);
            for (int r = r0; r <= r1; r++) {
                rows[r].push_back((float)(x0 + (r + 0.5 - y0) * slope));
            }
        };
        auto emitArc = [&](double ax, double ay, double bx, double by) {
            // Walk the limb circle the short way from a to b.
            double a1 = std::atan2(ay, ax), a2 = std::atan2(by, bx);
            double d = a2 - a1;
            while (d >  M_PI) d -= 2 * M_PI;
            while (d < -M_PI) d += 2 * M_PI;
            int steps = std::max(1, (int)(std::fabs(d) / (M_PI / 180.0)));
            for (int k = 1; k < steps; k++) {
                double ang = a1 + d * k / steps;
                pts.emplace_back(std::cos(ang), std::sin(ang));
            }
        };

        for (const auto& ring : earthRings) {
            const size_t n = ring.size();
            view.resize(n);
            int start = -1;
            for (size_t i = 0; i < n; i++) {
                const EarthVec& e = ring[i];
                double x = e.y * c0 - e.x * s0;   // cos(lat) sin(lon - lon0)
                double z = e.x * c0 + e.y * s0;   // cos(lat) cos(lon - lon0)
                view[i] = { x, (double)e.z, z };
                if (start < 0 && z >= 0) start = (int)i;
            }
            if (start < 0) continue; // whole ring on the far side

            pts.clear();
            bool havePendingExit = false;
            double exitX = 0, exitY = 0;
            for (size_t k = 0; k < n; k++) {
                const auto& p = view[(start + k) % n];
                const auto& q = view[(start + k + 1) % n];
                bool pin = p[2] >= 0, qin = q[2] >= 0;
                if (pin && qin) {
                    pts.emplace_back(q[0], q[1]);
                } else if (pin != qin) {
                    double t = p[2] / (p[2] - q[2]);
                    double ix = p[0] + (q[0] - p[0]) * t;
                    double iy = p[1] + (q[1] - p[1]) * t;
                    if (pin) {               // leaving the visible hemisphere
                        pts.emplace_back(ix, iy);
                        exitX = ix; exitY = iy; havePendingExit = true;
                    } else {                 // re-entering
                        if (havePendingExit) { emitArc(exitX, exitY, ix, iy); havePendingExit = false; }
                        pts.emplace_back(ix, iy);
                        pts.emplace_back(q[0], q[1]);
                    }
                }
            }
            if (pts.size() < 3) continue;
            for (size_t i = 0; i < pts.size(); i++) {
                const auto& a = pts[i];
                const auto& b = pts[(i + 1) % pts.size()];
                addEdge(cx + a.first * R, cy - a.second * R, cx + b.first * R, cy - b.second * R);
            }
        }

        std::vector<sf::Uint8> rgba((size_t)S * S * 4, 0); // transparent black
        for (int y = 0; y < S; y++) {
            double dy = (y + 0.5 - cy) / R;
            if (std::fabs(dy) >= 1.0) continue;
            double half = std::sqrt(1.0 - dy * dy) * R;
            int x0 = std::max(0, (int)std::ceil(cx - half - 0.5));
            int x1 = std::min(S - 1, (int)std::ceil(cx + half - 0.5) - 1);
            sf::Uint8* row = &rgba[(size_t)y * S * 4];
            for (int x = x0; x <= x1; x++) row[x * 4 + 3] = 255; // ocean
            auto& xs = rows[y];
            std::sort(xs.begin(), xs.end());
            for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                int a = std::max(x0, (int)std::ceil(xs[i] - 0.5));
                int b = std::min(x1, (int)std::ceil(xs[i + 1] - 0.5) - 1);
                for (int x = a; x <= b; x++) row[x * 4 + 3] = 0;   // land
            }
            // Rim: re-opaque the pixels between the inner and outer radius.
            double innerR = R - kEarthRimPx;
            double dyPx = std::fabs(y + 0.5 - cy);
            if (dyPx >= innerR) {
                for (int x = x0; x <= x1; x++) row[x * 4 + 3] = 255;
            } else {
                double halfI = std::sqrt(innerR * innerR - dyPx * dyPx);
                int xi0 = std::max(x0, (int)std::ceil(cx - halfI - 0.5));
                int xi1 = std::min(x1, (int)std::ceil(cx + halfI - 0.5) - 1);
                for (int x = x0; x < xi0; x++) row[x * 4 + 3] = 255;
                for (int x = xi1 + 1; x <= x1; x++) row[x * 4 + 3] = 255;
            }
        }
        earthTexture.update(rgba.data());
    }

    // Called once per render tick. The view longitude is derived from UTC
    // time, so most ticks this is just a cheap re-bind of the resident
    // texture; a re-render only happens when the minute changes. Being a pure
    // function of time, it also self-corrects after sleep/wake.
    void updateEarthSprites() {
        if (!hasWatermark) return;

        ensureEarthRendered(computeEarthMinuteForNow());

        if (earthTexture.getSize().x == 0) return;

        for (auto& m : monitors) {
            m.watermarkSprite.setTexture(earthTexture, true);
            sf::FloatRect bounds = m.watermarkSprite.getLocalBounds();
            m.watermarkSprite.setOrigin(bounds.width / 2.0f, bounds.height / 2.0f);
            m.watermarkSprite.setPosition(m.width / 2.0f, m.height / 2.0f);
            m.watermarkSprite.setColor(sf::Color(255, 255, 255, 255));
        }
    }

    float computeWatermarkInvertProgress() {
        time_t now = time(0);
        tm* ti = localtime(&now);
        double h = ti->tm_hour + ti->tm_min / 60.0 + ti->tm_sec / 3600.0;
        return computeWatermarkInvertProgressForHour(h);
    }

    // Same day/night inversion curve, but for an arbitrary local hour.
    float computeWatermarkInvertProgressForHour(double h) {
        double sunrise = todaysSolarTimes.sunrise_hour;
        double sunset  = todaysSolarTimes.sunset_hour;

        const double transitionHours = 2.0 / 60.0; // 2 minutes transition time for smooth fade
        const double offsetHours     = 1;         // 60 minutes after sunset

        double invertStart = sunset + offsetHours;
        double invertFull  = invertStart + transitionHours;
        double revertFull  = sunrise - offsetHours;
        double revertStart = revertFull - transitionHours;

        // Transitioning to inverted (evening, shortly after sunset+90min)
        if (h >= invertStart && h < invertFull) {
            return (float)((h - invertStart) / transitionHours);
        }
        // Fully inverted — wraps midnight: after invertFull in the evening OR before revertStart in the morning
        if (h >= invertFull || h < revertStart) {
            return 1.0f;
        }
        // Transitioning back to normal (early morning, before sunrise-90min)
        if (h >= revertStart && h < revertFull) {
            return (float)(1.0 - (h - revertStart) / transitionHours);
        }
        // Daytime — normal
        return 0.0f;
    }

    void renderDebugStrip() {
        float invertProgress = computeWatermarkInvertProgress();
        updateEarthSprites();
        for (auto& m : monitors) {
            if (m.window && m.window->isOpen()) {
                const int stripHeight = 300;
                const int stripY = (m.height - stripHeight) / 2;

                sf::Image stripImage;
                stripImage.create(m.width, m.height, sf::Color(10, 10, 20));

                for (int x = 0; x < m.width; x++) {
                    double hour = (static_cast<double>(x) / m.width) * 24.0;
                    Color c = getColorForHour(hour);
                    sf::Color px(c.r, c.g, c.b);

                    int yEnd = std::min(stripY + stripHeight, m.height);
                    for (int y = stripY; y < yEnd; y++) {
                        stripImage.setPixel(x, y, px);
                    }
                }

                sf::Texture stripTexture;
                stripTexture.loadFromImage(stripImage);
                sf::Sprite stripSprite(stripTexture);

                m.window->clear(sf::Color(10, 10, 20));
                m.window->draw(stripSprite);
                if (hasWatermark) {
                    if (watermarkShaderLoaded) {
                        watermarkShader.setUniform("texture", sf::Shader::CurrentTexture);
                        watermarkShader.setUniform("background", stripTexture);
                        watermarkShader.setUniform("invertAmount", invertProgress);
                        watermarkShader.setUniform("resolution",
                            sf::Glsl::Vec2((float)m.width, (float)m.height));
                        watermarkShader.setUniform("backgroundFlipY", 1.0f); // image texture, top-down
                        m.window->draw(m.watermarkSprite, &watermarkShader);
                    } else {
                        m.window->draw(m.watermarkSprite);
                    }
                }
                m.window->display();
            }
        }
    }

    // Fill the gradient on the GPU via the gradient shader, rendering into the
    // monitor's off-screen RenderTexture. Returns true on success. The result
    // texture (m.gradientRT->getTexture()) is then both drawn to the window and
    // fed to the watermark shader as the sky/background layer.
    bool buildGradientGPU(MonitorWindow& m, const SkySlice& slice) {
        return buildGradientInto(m.gradientRT.get(), m.width, m.height, slice);
    }

    // Core of the GPU gradient fill, target-agnostic (takes any RenderTexture).
    bool buildGradientInto(sf::RenderTexture* rt, int width, int height, const SkySlice& slice) {
        if (!gradientShaderLoaded || !fullscreenTexReady || !rt) return false;

        auto to01 = [](const Color& c) {
            return sf::Glsl::Vec3(c.r / 255.f, c.g / 255.f, c.b / 255.f);
        };
        gradientShader.setUniform("horizon", to01(slice.horizon));
        gradientShader.setUniform("midSky",  to01(slice.midSky));
        gradientShader.setUniform("zenith",  to01(slice.zenith));
        gradientShader.setUniform("gamma",   (float)slice.gamma);
        gradientShader.setUniform("resolution",
            sf::Glsl::Vec2((float)width, (float)height));

        // Stretch the 1x1 white texture to cover the target so the shader runs
        // for every fragment; gl_TexCoord then spans 0..1 across the screen.
        sf::Sprite quad(fullscreenTex);
        quad.setScale((float)width, (float)height);

        rt->draw(quad, &gradientShader);
        rt->display();
        return true;
    }

    // CPU fallback: original per-pixel dithered gradient. Used only when the
    // gradient shader / RenderTexture is unavailable on this machine.
    void buildGradientCPU(int width, int height, const SkySlice& slice, sf::Image& out) {
        static const int bayerMatrix[8][8] = {
            { 0, 32,  8, 40,  2, 34, 10, 42},
            {48, 16, 56, 24, 50, 18, 58, 26},
            {12, 44,  4, 36, 14, 46,  6, 38},
            {60, 28, 52, 20, 62, 30, 54, 22},
            { 3, 35, 11, 43,  1, 33,  9, 41},
            {51, 19, 59, 27, 49, 17, 57, 25},
            {15, 47,  7, 39, 13, 45,  5, 37},
            {63, 31, 55, 23, 61, 29, 53, 21}
        };

        auto blendSkyColor = [&](double altitude) -> Color {
            double t = std::pow(std::max(0.0, std::min(1.0, altitude)), slice.gamma);
            if (t <= 0.5) {
                return interpolateColor(slice.horizon, slice.midSky, t * 2.0);
            } else {
                return interpolateColor(slice.midSky, slice.zenith, (t - 0.5) * 2.0);
            }
        };

        out.create(width, height);
        for (int y = 0; y < height; y++) {
            double altitude = 1.0 - (static_cast<double>(y) / height);
            Color baseColor = blendSkyColor(altitude);
            double nextAltitude = 1.0 - (static_cast<double>(y + 1) / height);
            Color nextColor = blendSkyColor(nextAltitude);

            for (int x = 0; x < width; x++) {
                int bayerValue = bayerMatrix[y % 8][x % 8];
                double threshold = (bayerValue / 64.0) - 0.5;

                Color colorDiff;
                colorDiff.r = nextColor.r - baseColor.r;
                colorDiff.g = nextColor.g - baseColor.g;
                colorDiff.b = nextColor.b - baseColor.b;

                int r = std::max(0, std::min(255, baseColor.r + static_cast<int>(threshold * abs(colorDiff.r) * 0.5)));
                int g = std::max(0, std::min(255, baseColor.g + static_cast<int>(threshold * abs(colorDiff.g) * 0.5)));
                int b = std::max(0, std::min(255, baseColor.b + static_cast<int>(threshold * abs(colorDiff.b) * 0.5)));

                out.setPixel(x, y, sf::Color(r, g, b));
            }
        }
    }

    // Draw the sky gradient + watermark into one monitor's window and present
    // it. Factored out of renderFrame() so a newly-created window can be
    // painted immediately (see setupWindowForMonitor) instead of sitting
    // black until the next render tick.
    void renderMonitor(MonitorWindow& m, const SkySlice& slice, float invertProgress) {
        if (!m.window || !m.window->isOpen()) return;

        // Build the sky gradient. Prefer the GPU shader (near-zero CPU,
        // no texture upload); fall back to the CPU pixel loop only if
        // shaders/RenderTextures are unavailable.
        bool gpu = buildGradientGPU(m, slice);

        sf::Texture cpuTexture; // kept alive for the draw below if used
        const sf::Texture* gradientTexturePtr = nullptr;
        if (gpu) {
            gradientTexturePtr = &m.gradientRT->getTexture();
        } else {
            sf::Image gradientImage;
            buildGradientCPU(m.width, m.height, slice, gradientImage);
            cpuTexture.loadFromImage(gradientImage);
            gradientTexturePtr = &cpuTexture;
        }
        const sf::Texture& gradientTexture = *gradientTexturePtr;
        sf::Sprite gradientSprite(gradientTexture);

        m.window->clear();
        m.window->draw(gradientSprite);

        // Draw watermark if available
        if (hasWatermark) {
            if (watermarkShaderLoaded) {
                watermarkShader.setUniform("texture", sf::Shader::CurrentTexture);
                watermarkShader.setUniform("background", gradientTexture);
                watermarkShader.setUniform("invertAmount", invertProgress);
                watermarkShader.setUniform("resolution",
                    sf::Glsl::Vec2((float)m.width, (float)m.height));
                // RenderTexture (GPU path) is GL-oriented -> no flip;
                // CPU image texture is top-down -> flip.
                watermarkShader.setUniform("backgroundFlipY", gpu ? 0.0f : 1.0f);
                m.window->draw(m.watermarkSprite, &watermarkShader);
            } else {
                m.window->draw(m.watermarkSprite);
            }
        }

        m.window->display();
    }

    void renderFrame() {
        if (config.debug_mode) {
            renderDebugStrip();
            return;
        }

        float invertProgress = computeWatermarkInvertProgress();
        updateEarthSprites();

        // Get current time and compute sky slice once per frame
        time_t now = time(0);
        tm* timeinfo = localtime(&now);
        double currentHour = timeinfo->tm_hour + (timeinfo->tm_min / 60.0) + (timeinfo->tm_sec / 3600.0);
        SkySlice slice = getSkySliceForHour(currentHour);

        // Render to all monitor windows
        for (auto& m : monitors) {
            renderMonitor(m, slice, invertProgress);
        }
    }

    void updateDisplay() {
        renderFrame();
    }

    // Dynamic tick: how long the main loop waits between change checks.
    // Within 10 minutes of sunrise or sunset the palette moves fastest, so
    // check every 0.25 s; within 30 minutes, every 0.5 s; otherwise every
    // second. Distances wrap around midnight.
    float currentTickIntervalSec() {
        time_t now = time(0);
        tm* ti = localtime(&now);
        double hour = ti->tm_hour + ti->tm_min / 60.0 + ti->tm_sec / 3600.0;
        auto minutesFrom = [hour](double eventHour) {
            double d = std::fabs(hour - eventHour);
            if (d > 12.0) d = 24.0 - d;
            return d * 60.0;
        };
        double m = std::min(minutesFrom(todaysSolarTimes.sunrise_hour),
                            minutesFrom(todaysSolarTimes.sunset_hour));
        if (m <= 10.0) return 0.25f;
        if (m <= 30.0) return 0.5f;
        return 1.0f;
    }

    // Everything that determines what a frame looks like, reduced to a
    // comparable value. If two ticks produce equal keys, repainting would
    // present a pixel-identical frame, so the main loop skips it. The
    // palette is already 8-bit; gamma and the inversion ramp are continuous
    // and are quantized to 1/256, far below anything visible.
    struct FrameKey {
        int c[9];           // horizon, midSky, zenith RGB
        int gammaQ;
        int invertQ;
        int earthMinute;
        size_t monitorCount;
        bool debug;
        bool operator!=(const FrameKey& o) const {
            return std::memcmp(this, &o, sizeof(FrameKey)) != 0;
        }
    };
    FrameKey computeFrameKey() {
        time_t now = time(0);
        tm* ti = localtime(&now);
        double hour = ti->tm_hour + ti->tm_min / 60.0 + ti->tm_sec / 3600.0;
        SkySlice s = getSkySliceForHour(hour);
        FrameKey k;
        std::memset(&k, 0, sizeof(k)); // zero padding so memcmp is exact
        const Color* cs[3] = { &s.horizon, &s.midSky, &s.zenith };
        for (int i = 0; i < 3; i++) {
            k.c[i * 3] = cs[i]->r; k.c[i * 3 + 1] = cs[i]->g; k.c[i * 3 + 2] = cs[i]->b;
        }
        k.gammaQ = (int)std::lround(s.gamma * 256.0);
        k.invertQ = (int)std::lround(computeWatermarkInvertProgress() * 256.0);
        k.earthMinute = hasWatermark ? computeEarthMinuteForNow() : -1;
        k.monitorCount = monitors.size();
        k.debug = config.debug_mode;
        return k;
    }
    
    void createMessageWindow() {
        // Create a hidden window to receive power management messages
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = PowerEventWndProc;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "CircadianWallpaperPowerWindow";
        RegisterClassA(&wc);
        
        g_hWnd = CreateWindowA("CircadianWallpaperPowerWindow", "CircadianWallpaper Power", 
                              0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandle(NULL), this);
    }

    static LRESULT CALLBACK PowerEventWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (uMsg == WM_POWERBROADCAST) {
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                g_justWokeUp = true;
                // Sleep can silently change monitor topology (dock re-enumerates,
                // external panel power-cycles, GPU driver re-attaches). Force a
                // reconcile on wake so we don't render to stale window handles.
                g_topologyChanged = true;
                CircadianWallpaper* instance = (CircadianWallpaper*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
                if (instance) {
                    instance->logMessage("System resumed from sleep - updating wallpaper immediately");
                }
            }
        } else if (uMsg == WM_DISPLAYCHANGE) {
            // Resolution, color depth, or monitor arrangement changed.
            g_topologyChanged = true;
            CircadianWallpaper* instance = (CircadianWallpaper*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
            if (instance) instance->logMessage("WM_DISPLAYCHANGE received - reconciling monitors");
        } else if (uMsg == WM_DEVICECHANGE) {
            // Monitor plugged or unplugged (and other device events — the
            // reconcile is a cheap no-op when topology hasn't actually changed).
            if (wParam == DBT_DEVNODES_CHANGED) {
                g_topologyChanged = true;
            }
        }
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    void run() {
        std::cout << "\nStarting CircadianWallpaper..." << std::endl;
        std::cout << "Display repaints only when the sky, the globe, or the desktop changes" << std::endl;
        std::cout << "Use Task Manager to terminate the application" << std::endl << std::endl;

        logMessage("Starting CircadianWallpaper...");
        logMessage("Display repaints only when the sky, the globe, or the desktop changes");

        // Create hidden window for power management messages
        createMessageWindow();
        if (g_hWnd) {
            SetWindowLongPtr(g_hWnd, GWLP_USERDATA, (LONG_PTR)this);
            std::cout << "Power management enabled - will update on wake from sleep" << std::endl;
            logMessage("Power management enabled - will update on wake from sleep");
        }

        // Initial setup. Solar times were already seeded from cache/fallback
        // in the constructor, so the first frames render immediately; any
        // network refresh happens on the worker thread.
        if (solarRefreshNeeded()) {
            std::cout << "Refreshing solar times in background..." << std::endl;
            startAsyncSolarFetch();
        }
        std::cout << "Generating color schedule..." << std::endl;
        logSolarSchedule();

        int updateCount = 0;
        std::string lastDate = getCurrentDate();
        sf::Clock updateClock;
        sf::Clock zOrderClock;  // re-pins the wallpaper below the taskbar
        sf::Clock tickClock;    // paces the main loop
        sf::Clock topologyPollClock; // periodic self-healing topology check
        sf::Clock solarRetryClock;   // re-attempts a failed daily solar refresh

        // Loop cadence. Each tick only *checks* whether anything on screen
        // would differ (see computeFrameKey) and repaints when it would;
        // the check is a few microseconds. The tick length adapts to how
        // fast the sky is moving (see currentTickIntervalSec): 1 s for most
        // of the day, tightening to 0.5 s and then 0.25 s around sunrise and
        // sunset, where the palette steps fastest. Expose, wake and topology
        // events still cut the wait short and repaint immediately.
        FrameKey lastFrameKey = computeFrameKey();
        long repaintCount = 0;

        // Do initial render and set initial accent color
        std::cout << "Rendering initial frame..." << std::endl;
        updateDisplay();
        repaintCount++;
        updateCount++;

        Color initialColor = getCurrentColor();
        setWindowsAccentColor(initialColor);
        lastAccentColorUpdate = time(0);

        std::cout << "\nEntering main loop..." << std::endl;

        // Topology-change debounce (see below).
        bool topologyReconcilePending = false;
        sf::Clock topologySettleClock;

        bool shouldRun = true;
        while (shouldRun) {
            // Handle SFML events for all windows
            for (auto& m : monitors) {
                if (m.window && m.window->isOpen()) {
                    sf::Event event;
                    while (m.window->pollEvent(event)) {
                        if (event.type == sf::Event::Closed) {
                            shouldRun = false;
                        }
                        // No keyboard shortcuts - use Task Manager to terminate
                    }
                }
            }

            // Process Windows messages for power events
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            try {
                // Adopt any solar data a background fetch finished producing.
                applyPendingSolarResult();

                // Handle any display topology changes flagged by the window
                // proc (WM_DISPLAYCHANGE, WM_DEVICECHANGE) or by the wake path.
                // These events arrive in BURSTS — plugging a monitor or
                // resuming from sleep fires several over a couple of seconds,
                // and reconciling on each one can destroy/recreate windows
                // repeatedly (each recreate is a visible flash). Debounce:
                // reconcile once, after the burst has been quiet for a second.
                // Existing windows keep rendering in the meantime.
                bool forceUpdate = false;
                if (g_topologyChanged) {
                    g_topologyChanged = false;
                    topologyReconcilePending = true;
                    topologySettleClock.restart();
                }
                // Safety net: independent of any message ever arriving, poll
                // the real topology every 2s and re-arm the same debounced
                // reconcile path if it no longer matches what we last
                // reconciled to. This is what actually fixes the case where
                // WM_DISPLAYCHANGE/WM_DEVICECHANGE fired but EnumDisplayMonitors
                // hadn't settled yet when we reconciled against it (or fired
                // too early/late to be seen at all) — the next poll catches
                // the drift and tries again a second later.
                if (!topologyReconcilePending &&
                    topologyPollClock.getElapsedTime().asSeconds() >= 2.0f) {
                    topologyPollClock.restart();
                    if (topologyMismatch()) {
                        topologyReconcilePending = true;
                        topologySettleClock.restart();
                    }
                }
                if (topologyReconcilePending &&
                    topologySettleClock.getElapsedTime().asSeconds() >= 1.0f) {
                    topologyReconcilePending = false;
                    reconcileMonitors();
                    forceUpdate = true;
                }
                // Check if we just woke up from sleep. Everything here must be
                // fast and local: the old path could force a synchronous API
                // fetch (many seconds of HTTP on the render thread), during
                // which the wallpaper sat unpainted — the black flashing seen
                // right after opening the laptop and logging in.
                if (g_justWokeUp) {
                    logMessage("Wake from sleep detected - using cached data, refreshing in background");
                    loadSolarCache();
                    applySolarTimesFromCache();
                    if (solarRefreshNeeded()) startAsyncSolarFetch();
                    // Defer the next accent-color update to ~60s after wake.
                    // It broadcasts a system-wide re-theme (a flash source of
                    // its own), and after a long sleep the 30-minute timer has
                    // always expired — so without this it fired at the worst
                    // possible moment, right while the desktop was settling.
                    lastAccentColorUpdate = std::max(lastAccentColorUpdate,
                                                     time(0) - 1740);
                    forceUpdate = true;
                    g_justWokeUp = false;
                }

                // Check if we need to refresh solar times (new day). The
                // 8-day cache almost always already holds the new day's data,
                // so adopt it instantly and top the cache back up off-thread.
                std::string currentDate = getCurrentDate();
                if (currentDate != lastDate) {
                    logMessage("New day detected, refreshing solar times...");
                    applySolarTimesFromCache();
                    logSolarSchedule();
                    if (solarRefreshNeeded()) startAsyncSolarFetch();
                    lastDate = currentDate;
                    forceUpdate = true;
                }

                // If the daily refresh failed (e.g. no network at midnight),
                // try again every 15 minutes rather than riding stale data
                // until tomorrow.
                if (solarRetryClock.getElapsedTime().asSeconds() >= 900.0f) {
                    solarRetryClock.restart();
                    if (solarRefreshNeeded()) {
                        logMessage("Solar data stale - retrying background refresh");
                        startAsyncSolarFetch();
                    }
                }

                // Periodic status tick (every 15 seconds). Informational only;
                // it no longer forces a repaint.
                bool statusTick = false;
                if (updateClock.getElapsedTime().asSeconds() >= 15.0f) {
                    statusTick = true;
                    updateClock.restart();
                }

                // Keep the wallpaper pinned beneath the taskbar. Changing a
                // taskbar setting makes Explorer raise the taskbar's container
                // above us until the next z-order shuffle; re-pinning ~1/sec
                // corrects that promptly without measurable cost.
                if (forceUpdate || zOrderClock.getElapsedTime().asSeconds() >= 1.0f) {
                    pinWindowsToBottom();
                    zOrderClock.restart();
                }

                // Repaint only when the frame would actually differ from the
                // one on screen (sky slice, inversion, globe minute, monitor
                // count), or when something external demands it: an expose
                // (DWM dropped our surface), wake, topology reconcile, or a
                // new day's palette. Between those, the loop just ticks.
                tickClock.restart();
                if (g_exposeRequested) {
                    g_exposeRequested = false;
                    forceUpdate = true;
                    logMessage("Window exposed (WM_PAINT) - repainting immediately");
                }
                FrameKey key = computeFrameKey();
                if (forceUpdate || key != lastFrameKey) {
                    updateDisplay();
                    lastFrameKey = key;
                    repaintCount++;
                }
                if (statusTick) {
                    updateCount++;

                    {
                        time_t now = time(0);
                        tm* timeinfo = localtime(&now);
                        Color currentColor = getCurrentColor();
                        std::string period = getCurrentPeriod();

                        std::string statusMsg = "[" + std::to_string(updateCount) + "] "
                                               + formatHour(timeinfo->tm_hour + (timeinfo->tm_min / 60.0))
                                               + " | " + period
                                               + " | RGB(" + std::to_string(currentColor.r) + ", "
                                               + std::to_string(currentColor.g) + ", "
                                               + std::to_string(currentColor.b) + ")"
                                               + " | Source: " + todaysSolarTimes.source
                                               + " | repaints: " + std::to_string(repaintCount);

                        std::cout << statusMsg << std::endl;
                        // Only persist the 15-second status line in debug mode
                        // so log.txt stays a low-volume event log.
                        if (config.debug_mode) logMessage(statusMsg);
                    }
                }

                // Check if it's time to update Windows accent color (every 30 minutes)
                time_t now = time(0);
                if (difftime(now, lastAccentColorUpdate) >= 1800) { // 30 minutes = 1800 seconds
                    Color currentColor = getCurrentColor();
                    setWindowsAccentColor(currentColor);
                    lastAccentColorUpdate = now;
                }

                // Tick-budget wait: spend the rest of this tick's
                // currentTickIntervalSec() budget waiting ON THE MESSAGE QUEUE
                // instead of sleeping blind. A plain sleep left window
                // messages queued for up to 250 ms — so when a window closed
                // over the wallpaper and DWM asked us to repaint the exposed
                // (black) region, the request sat unhandled for a visible
                // fraction of a second. MsgWaitForMultipleObjects wakes the
                // moment anything arrives; exposure/wake/topology events then
                // cut the wait short so the next render happens immediately.
                for (;;) {
                    float spent = tickClock.getElapsedTime().asSeconds();
                    float remaining = currentTickIntervalSec() - spent;
                    if (remaining <= 0.0f) break;
                    DWORD wait = MsgWaitForMultipleObjects(
                        0, NULL, FALSE, (DWORD)(remaining * 1000.0f), QS_ALLINPUT);
                    if (wait != WAIT_OBJECT_0) break; // timeout (or failure)
                    MSG queued;
                    while (PeekMessage(&queued, NULL, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&queued);
                        DispatchMessage(&queued);
                    }
                    if (g_exposeRequested || g_justWokeUp || g_topologyChanged) break;
                }
                
            } catch (const std::exception& e) {
                std::string errorMsg = "Error in main loop: " + std::string(e.what());
                logMessage(errorMsg);
                // Back off briefly, but keep rendering: the old 1-minute sleep
                // froze the wallpaper (stale or black) on any transient error.
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }
    }

    // Dev aid: dump the rasterized globe at four UTC hours so the vector
    // renderer can be eyeballed without running the wallpaper.
    int runEarthPreview(const std::string& dir) {
        if (!hasWatermark) {
            std::cout << "Preview requires earth_land.bin next to config.ini - aborting" << std::endl;
            return 1;
        }
        CreateDirectoryA(dir.c_str(), NULL);
        for (int hour : {0, 6, 12, 18}) {
            int minute = ((hour - 12) * 60 + kEarthMinutesPerDay) % kEarthMinutesPerDay;
            renderedEarthMinute = -1;
            auto t0 = std::chrono::steady_clock::now();
            ensureEarthRendered(minute);
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            std::cout << "Rendered globe for " << hour << ":00 UTC in " << us << " us" << std::endl;
            std::stringstream name;
            name << dir << "\\earth_utc" << std::setfill('0') << std::setw(2) << hour << ".png";
            if (!earthTexture.copyToImage().saveToFile(name.str())) {
                std::cout << "Failed to save " << name.str() << std::endl;
                return 1;
            }
            std::cout << "Wrote " << name.str() << std::endl;
        }
        return 0;
    }

    std::string getLogPath() {
        std::string configPath = getConfigPath();
        size_t lastSlash = configPath.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            return configPath.substr(0, lastSlash + 1) + "log.txt";
        }
        return "log.txt";
    }

    void logMessage(const std::string& message) {
        // Low-volume event log (wake, reconcile, expose, errors) — needed to
        // diagnose issues like post-wake flashing that are hard to reproduce
        // on demand. The per-15-seconds status line is only written in
        // debug_mode (see run()), so this stays a few lines per day. The file
        // is truncated when it grows past 1 MB. Mutex because the async solar
        // worker logs too.
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);

        std::string logPath = getLogPath();
        {
            std::ifstream check(logPath, std::ios::binary | std::ios::ate);
            if (check.good() && check.tellg() > 1024 * 1024) {
                check.close();
                std::ofstream(logPath, std::ios::trunc).close();
            }
        }

        std::ofstream log(logPath, std::ios::app);
        if (!log.is_open()) return;
        time_t now = time(0);
        tm* ti = localtime(&now);
        log << std::put_time(ti, "%Y-%m-%d %H:%M:%S") << " | " << message << "\n";
    }

};

int main(int argc, char* argv[]) {
    // Set DPI awareness to prevent scaling issues on high-DPI displays
    SetProcessDPIAware();

    if (argc > 1) {
        std::string mode = argv[1];
        if (mode == "--help" || mode == "-h") {
            std::cout << "\nUsage:" << std::endl;
            std::cout << "  CircadianWallpaper.exe              - Run fullscreen color overlay based on solar position" << std::endl;
            std::cout << "  CircadianWallpaper.exe --earth-preview <dir> - Write the globe at 00:00, 06:00, 12:00 and 18:00 UTC as PNGs into <dir>" << std::endl;
            std::cout << "  CircadianWallpaper.exe --help       - Show this help" << std::endl;
            std::cout << "\nFeatures:" << std::endl;
            std::cout << "  • Fullscreen SFML overlay (fast, no wallpaper API calls)" << std::endl;
            std::cout << "  • Automatic location detection via IP geolocation" << std::endl;
            std::cout << "  • Real astronomical data for your location" << std::endl;
            std::cout << "  • Real-time color transitions based on sun position" << std::endl;
            std::cout << "  • Animated earth watermark (vector coastlines in earth_land.bin, one rotation per day, redrawn every minute, longitude 0 faces viewer at noon UTC)" << std::endl;
            std::cout << "  • Wake from sleep detection - updates immediately on resume" << std::endl;
            std::cout << "  • All output is logged to log.txt file" << std::endl;
            std::cout << "\nControls:" << std::endl;
            std::cout << "  ESC - Exit application" << std::endl;
            std::cout << "\nEdit config.ini to set manual location coordinates if needed." << std::endl;
            return 0;
        }
    }

    // Preview mode renders offscreen and exits; it may run alongside the live
    // wallpaper, so it deliberately skips the single-instance guard.
    if (argc > 2 && std::string(argv[1]) == "--earth-preview") {
        CircadianWallpaper app(true);
        return app.runEarthPreview(argv[2]);
    }

    // Single-instance guard: startup can fire a second launch (e.g. if the Run
    // key and a scheduled task both trigger). A named mutex scoped to the user
    // session lets the OS arbitrate: CreateMutexA still succeeds for the second
    // caller but GetLastError reports ERROR_ALREADY_EXISTS. The handle is held
    // for the process lifetime; the OS releases it on exit.
    static HANDLE g_singleInstanceMutex = CreateMutexA(nullptr, FALSE, "Local\\CircadianWallpaper_SingleInstance");
    if (g_singleInstanceMutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cout << "Another instance of CircadianWallpaper is already running. Exiting." << std::endl;
        return 0;
    }

    CircadianWallpaper app;
    app.run();
    return 0;
}