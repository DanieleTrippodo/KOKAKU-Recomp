#include "display_window.hpp"
#include "kokaku_controls_ui.hpp"
#include "kokaku_native_menu.hpp"
#include "kokaku_weapon_input.hpp"
#include "kokaku_mod_capture.hpp"
#include "dx12_presenter.hpp"
#include "ge_gpu_backend.hpp"
#include "vcs_config.hpp"
#include "vcs_runtime_log.hpp"
#include "vcs_vehicle_input.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <thread>

namespace vcs {
namespace {

constexpr std::uint32_t kPspSelect = 0x000001u;
constexpr std::uint32_t kPspStart = 0x000008u;
constexpr std::uint32_t kPspUp = 0x000010u;
constexpr std::uint32_t kPspRight = 0x000020u;
constexpr std::uint32_t kPspDown = 0x000040u;
constexpr std::uint32_t kPspLeft = 0x000080u;
constexpr std::uint32_t kPspLTrigger = 0x000100u;
constexpr std::uint32_t kPspRTrigger = 0x000200u;
constexpr std::uint32_t kPspTriangle = 0x001000u;
constexpr std::uint32_t kPspCircle = 0x002000u;
constexpr std::uint32_t kPspCross = 0x004000u;
constexpr std::uint32_t kPspSquare = 0x008000u;

constexpr UINT kMessagePresent = WM_APP + 1u;
kokaku::WeaponCycleInput weapon_cycle_input;
std::uint64_t weapon_input_frame{}, weapon_update_frame{};
bool weapon_update_seen{};

struct KeyBinding {
    int virtual_key;
    std::uint32_t psp_button;
};

// Direct PSP bindings for Kokaku. Keep all face buttons available in menus;
// Enter/Escape send Start and WASD controls the PSP analog stick.
constexpr KeyBinding kKeyBindings[] = {
    {VK_RETURN, kPspStart}, {VK_ESCAPE, kPspStart},
    {VK_TAB, kPspSelect},
    {VK_SPACE, kPspCross}, {'Z', kPspCross},
    {'X', kPspCircle}, {'C', kPspSquare}, {'V', kPspTriangle},
    {'Q', kPspLTrigger}, {'E', kPspRTrigger},
    {VK_UP, kPspUp}, {VK_DOWN, kPspDown},
    {VK_LEFT, kPspLeft}, {VK_RIGHT, kPspRight},
};

constexpr KeyBinding kMouseBindings[] = {
    {VK_LBUTTON, kPspRTrigger},
    {VK_RBUTTON, kPspLTrigger},
};
// WASD drives the analog stick rather than the D-pad, matching San Andreas.
constexpr int kMoveForward = 'W';
constexpr int kMoveBack = 'S';
constexpr int kMoveLeft = 'A';
constexpr int kMoveRight = 'D';


struct WindowState {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready_signal;
    std::vector<std::uint32_t> pixels;  // 0x00RRGGBB, top-down
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint64_t frame_index{};
    std::atomic<HWND> window{nullptr};
    std::atomic<bool> ready{false};
    std::atomic<bool> focused{false};
    std::atomic<bool> close_requested{false};
    // Raw mouse motion accumulated by the window thread and drained by the
    // guest's controller poll. Raw input rather than cursor position: the
    // cursor stops at the screen edge, and a camera that stops turning when
    // the pointer reaches the edge of a 3440-wide monitor is unusable.
    std::atomic<std::int32_t> mouse_dx{0};
    std::atomic<std::int32_t> mouse_dy{0};
    std::atomic<std::int32_t> wheel{0};
    int wheel_remainder{}; // Window thread: preserve fractional high-resolution wheel deltas.
    // Set while a movie is on screen; see display_window_set_aspect_lock.
    // Atomic because the guest thread raises it and the window thread paints.
    std::atomic<bool> aspect_lock{false};
    std::atomic<bool> fps_mouse{false};
    bool resume_mouse_after_controls{}; // Owned by the UI thread.
    std::array<std::atomic<ULONGLONG>, 256> key_press_until{};
    std::string status{"booting"};
    DisplayConfiguration configuration{};
    int client_width{480};
    int client_height{272};
    // Back buffer for WM_PAINT. Filling the window black and then stretching
    // into it directly made the whole client flash black every frame, which is
    // very visible once the client is desktop sized.
    HDC back_buffer_dc{nullptr};
    HBITMAP back_buffer_bitmap{nullptr};
    HGDIOBJ back_buffer_previous{nullptr};
    int back_buffer_width{};
    int back_buffer_height{};
};

constexpr UINT_PTR kStatusTimer = 1u;

WindowState &window_state() {
    static WindowState state;
    return state;
}

bool key_down(int virtual_key) noexcept {
    // Retain quick taps through at least one 30 Hz game input update.
    // GetAsyncKeyState alone misses a down/up pair between controller polls.
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0 ||
        GetTickCount64() < window_state().key_press_until[virtual_key].load(std::memory_order_relaxed);
}

// XInput, loaded at run time. Linking it would make the executable refuse to
// start on a machine without the redistributable, for a feature that is
// optional by definition -- a missing gamepad is not an error.
struct XInputGamepad {
    std::uint16_t buttons;
    std::uint8_t left_trigger;
    std::uint8_t right_trigger;
    std::int16_t lx, ly, rx, ry;
};
struct XInputStatePacket {
    std::uint32_t packet;
    XInputGamepad gamepad;
};
using PfnXInputGetState = std::uint32_t(WINAPI *)(std::uint32_t, XInputStatePacket *);

constexpr std::uint16_t kPadDpadUp = 0x0001u;
constexpr std::uint16_t kPadDpadDown = 0x0002u;
constexpr std::uint16_t kPadDpadLeft = 0x0004u;
constexpr std::uint16_t kPadDpadRight = 0x0008u;
constexpr std::uint16_t kPadStart = 0x0010u;
constexpr std::uint16_t kPadBack = 0x0020u;
constexpr std::uint16_t kPadLeftShoulder = 0x0100u;
constexpr std::uint16_t kPadRightShoulder = 0x0200u;
constexpr std::uint16_t kPadA = 0x1000u;
constexpr std::uint16_t kPadB = 0x2000u;
constexpr std::uint16_t kPadX = 0x4000u;
constexpr std::uint16_t kPadY = 0x8000u;

[[nodiscard]] PfnXInputGetState xinput_get_state() noexcept {
    // Newest first: 1_4 ships with Windows 8 and later, 9_1_0 is the version
    // present on every machine since Vista, and 1_3 covers the old SDK
    // redistributable.
    static PfnXInputGetState resolved = [] () -> PfnXInputGetState {
        for (const wchar_t *name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            if (HMODULE module = LoadLibraryW(name)) {
                if (auto function = reinterpret_cast<PfnXInputGetState>(
                        reinterpret_cast<void *>(GetProcAddress(module, "XInputGetState"))))
                    return function;
            }
        }
        return nullptr;
    }();
    return resolved;
}

// Analog sticks arrive as signed 16-bit and leave as the PSP's 0..255 with 128
// at rest. The dead zone is applied before the rescale so the rest position is
// exactly 128 rather than a value that drifts by a unit or two.
[[nodiscard]] std::uint8_t stick_to_psp(std::int16_t value, bool invert) noexcept {
    constexpr int kDeadZone = 7849;  // XInput's own documented left-stick figure
    int magnitude = std::abs(static_cast<int>(value));
    if (magnitude <= kDeadZone) return 128u;
    magnitude = (magnitude - kDeadZone) * 32767 / (32767 - kDeadZone);
    int signed_value = value < 0 ? -magnitude : magnitude;
    if (invert) signed_value = -signed_value;
    return static_cast<std::uint8_t>(std::clamp(128 + signed_value * 127 / 32767, 0, 255));
}

int legacy_configured_scale() {
    const char *text = std::getenv("PSPRECOMP_WINDOW_SCALE");
    if (text == nullptr || *text == '\0') return 0;
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 16) return 0;
    return static_cast<int>(value);
}

void resolve_client_size(WindowState &state) {
    const int desktop_width = std::max(1, GetSystemMetrics(SM_CXSCREEN));
    const int desktop_height = std::max(1, GetSystemMetrics(SM_CYSCREEN));

    if (state.configuration.fullscreen ||
        state.configuration.resolution_mode == DisplayResolutionMode::Desktop) {
        state.client_width = desktop_width;
        state.client_height = desktop_height;
    } else if (state.configuration.resolution_mode == DisplayResolutionMode::Custom) {
        state.client_width = static_cast<int>(state.configuration.custom_width);
        state.client_height = static_cast<int>(state.configuration.custom_height);
    } else {
        state.client_width = 480;
        state.client_height = 272;
    }

    // Backwards compatibility with the stage scripts that predate VCSNative.ini.
    // An explicit legacy scale overrides the windowed client dimensions only.
    if (!state.configuration.fullscreen) {
        const int legacy_scale = legacy_configured_scale();
        if (legacy_scale != 0) {
            state.client_width = 480 * legacy_scale;
            state.client_height = 272 * legacy_scale;
        }
    }
}

void set_fps_mouse(HWND window, bool enabled) {
    window_state().mouse_dx.store(0);
    window_state().mouse_dy.store(0);
    window_state().fps_mouse.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        RECT rectangle{};
        GetClientRect(window, &rectangle);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT *>(&rectangle), 2);
        ClipCursor(&rectangle);
    } else {
        ClipCursor(nullptr);
    }
}

LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    WindowState &state = window_state();
    switch (message) {
    case kMessagePresent:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == 2001) {
            state.resume_mouse_after_controls = state.fps_mouse.load();
            set_fps_mouse(window, false); kokaku::show_controls(window); return 0;
        }
        break;
    case WM_SETFOCUS:
        state.focused.store(true, std::memory_order_relaxed);
        if (state.resume_mouse_after_controls && !kokaku::controls_open()) {
            state.resume_mouse_after_controls = false;
            set_fps_mouse(window, true);
        }
        return 0;
    case WM_KILLFOCUS:
        for (auto &deadline : state.key_press_until) deadline.store(0, std::memory_order_relaxed);
        state.wheel.store(0, std::memory_order_relaxed);
        state.wheel_remainder = 0;
        state.focused.store(false, std::memory_order_relaxed);
        set_fps_mouse(window, false);
        return 0;
    case WM_KEYDOWN:
        if (kokaku::native_menu_active()) {
            if (!(lparam & (1ll << 30))) kokaku::native_menu_key(static_cast<int>(wparam));
            return 0;
        }
        if (wparam < 256u) state.key_press_until[wparam].store(GetTickCount64() + 50u, std::memory_order_relaxed);
        if (wparam == VK_F1 && (lparam & (1ll << 30)) == 0)
            set_fps_mouse(window, !state.fps_mouse.load(std::memory_order_relaxed));
        if (wparam == VK_F7 && (lparam & (1ll << 30)) == 0)
            kokaku::request_mod_reference_capture();
        // Escape is the pause button now that it is bound to Start, the way it
        // is in San Andreas. Alt+F4 and the window's close box still close.
        return 0;
    case WM_INPUT: {
        // Raw mouse deltas. Sized from the message rather than assumed: the
        // header is followed by a union whose size differs between builds.
        UINT size = 0u;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr,
                        &size, sizeof(RAWINPUTHEADER));
        if (size != 0u && size <= 256u) {
            alignas(8) std::byte buffer[256];
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer,
                                &size, sizeof(RAWINPUTHEADER)) == size) {
                const RAWINPUT *raw = reinterpret_cast<const RAWINPUT *>(buffer);
                if (raw->header.dwType == RIM_TYPEMOUSE &&
                    (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    state.mouse_dx.fetch_add(raw->data.mouse.lLastX, std::memory_order_relaxed);
                    state.mouse_dy.fetch_add(raw->data.mouse.lLastY, std::memory_order_relaxed);
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (kokaku::native_menu_key(message == WM_LBUTTONDOWN ? VK_LBUTTON : message == WM_RBUTTONDOWN ? VK_RBUTTON :
            message == WM_MBUTTONDOWN ? VK_MBUTTON : HIWORD(wparam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2)) return 0;
        state.key_press_until[message == WM_LBUTTONDOWN ? VK_LBUTTON : message == WM_RBUTTONDOWN ? VK_RBUTTON :
            message == WM_MBUTTONDOWN ? VK_MBUTTON : HIWORD(wparam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2].store(
            GetTickCount64() + 50u, std::memory_order_relaxed);
        return 0;
    case WM_MOUSEWHEEL: {
        if (kokaku::native_menu_key(GET_WHEEL_DELTA_WPARAM(wparam)>0 ? kokaku::wheel_up : kokaku::wheel_down)) return 0;
        state.wheel_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
        state.wheel.fetch_add(state.wheel_remainder / WHEEL_DELTA, std::memory_order_relaxed);
        state.wheel_remainder %= WHEEL_DELTA;
        return 0;
    }
    case WM_SETCURSOR:
        // Hide the pointer over the client area: the mouse is aiming the
        // camera, not pointing at anything. Answering WM_SETCURSOR rather than
        // calling ShowCursor avoids its counter, which has to be balanced
        // exactly and leaves the cursor invisible everywhere if it is not.
        // The non-client area keeps its arrow so the title bar stays usable.
        if (LOWORD(lparam) == HTCLIENT && state.fps_mouse.load(std::memory_order_relaxed)) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER: {
        if (wparam != kStatusTimer) break;
        std::wstring title = L"KOKAKU Recomp — Ghost in the Shell: Stand Alone Complex — ";
        title += state.fps_mouse.load(std::memory_order_relaxed) ? L"[F1: mouse ON] [OPTION: controls] " : L"[F1: mouse OFF] [OPTION: controls] ";
        {
            std::lock_guard<std::mutex> guard(state.mutex);
            title.append(state.status.begin(), state.status.end());
        }
        title += L" — ";
        title += std::to_wstring(state.client_width);
        title += L"x";
        title += std::to_wstring(state.client_height);
        title += state.configuration.upscale_filter == DisplayUpscaleFilter::Bilinear
                     ? L" bilinear" : L" nearest";
        SetWindowTextW(window, title.c_str());
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC window_context = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        // Once DirectX 12 owns presentation there is no GDI back buffer to
        // repaint. Begin/EndPaint still validates the update region; DWM keeps
        // the last flip-model swapchain image visible between presents.
        if (dx12_presenter_active()) {
            EndPaint(window, &paint);
            return 0;
        }
        const int client_w = std::max(1L, client.right - client.left);
        const int client_h = std::max(1L, client.bottom - client.top);
        std::lock_guard<std::mutex> guard(state.mutex);
        if (state.back_buffer_dc == nullptr || state.back_buffer_width != client_w ||
            state.back_buffer_height != client_h) {
            if (state.back_buffer_dc != nullptr) {
                SelectObject(state.back_buffer_dc, state.back_buffer_previous);
                DeleteObject(state.back_buffer_bitmap);
                DeleteDC(state.back_buffer_dc);
            }
            state.back_buffer_dc = CreateCompatibleDC(window_context);
            state.back_buffer_bitmap =
                CreateCompatibleBitmap(window_context, client_w, client_h);
            state.back_buffer_previous =
                SelectObject(state.back_buffer_dc, state.back_buffer_bitmap);
            state.back_buffer_width = client_w;
            state.back_buffer_height = client_h;
        }
        HDC context = state.back_buffer_dc != nullptr ? state.back_buffer_dc : window_context;
        if (state.width != 0u && state.height != 0u && !state.pixels.empty()) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(state.width);
            // Negative height selects a top-down DIB, matching our row order.
            info.bmiHeader.biHeight = -static_cast<LONG>(state.height);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            FillRect(context, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            const PresentationRectangle output = calculate_presentation_rectangle(
                static_cast<std::uint32_t>(std::max(0L, client.right - client.left)),
                static_cast<std::uint32_t>(std::max(0L, client.bottom - client.top)),
                state.width, state.height,
                state.aspect_lock.load(std::memory_order_relaxed)
                    ? DisplayAspectMode::Preserve
                    : state.configuration.aspect_mode,
                state.configuration.integer_scale);
            // HALFTONE is expensive and buys nothing when the blit is 1:1,
            // which is the normal case once the internal target matches the
            // client size.
            const bool scaling = output.width != static_cast<int>(state.width) ||
                                 output.height != static_cast<int>(state.height);
            if (scaling && state.configuration.upscale_filter == DisplayUpscaleFilter::Bilinear) {
                SetStretchBltMode(context, HALFTONE);
                SetBrushOrgEx(context, 0, 0, nullptr);
            } else {
                SetStretchBltMode(context, COLORONCOLOR);
            }
            StretchDIBits(context,
                          output.x, output.y, output.width, output.height,
                          0, 0, static_cast<int>(state.width), static_cast<int>(state.height),
                          state.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(context, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            const std::wstring message(state.status.begin(), state.status.end());
            SetBkMode(context, TRANSPARENT);
            SetTextColor(context, RGB(200, 200, 200));
            DrawTextW(context, message.c_str(), -1, &client,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (context != window_context)
            BitBlt(window_context, 0, 0, client_w, client_h, context, 0, 0, SRCCOPY);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        set_fps_mouse(window, false);
        state.close_requested.store(true, std::memory_order_relaxed);
        return 0;
    case WM_DESTROY:
        set_fps_mouse(window, false);
        PostQuitMessage(0);
        return 0;
    case WM_MOVE:
    case WM_SIZE:
        if (state.fps_mouse.load(std::memory_order_relaxed)) set_fps_mouse(window, true);
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void window_thread_main() {
    std::cout << "[window] thread started\n";
    WindowState &state = window_state();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    // The project does not define UNICODE, so IDC_ARROW expands to the ANSI
    // MAKEINTRESOURCE form; select the wide one explicitly.
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.lpszClassName = L"KokakuNativeDisplay";
    if (!RegisterClassExW(&window_class)) {
        std::cerr << "[window] RegisterClassExW failed: " << GetLastError() << "\n";
    }

    resolve_client_size(state);
    const DWORD style = state.configuration.fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT bounds{0, 0, state.client_width, state.client_height};
    int window_x = 0;
    int window_y = 0;
    if (!state.configuration.fullscreen) {
        AdjustWindowRect(&bounds, style, FALSE);
        // Desktop resolution mode asks for a client as large as the monitor, and
        // the frame AdjustWindowRect adds on top of it made the window larger
        // than the screen. Placed at CW_USEDEFAULT it was also offset, so the
        // right and bottom of the client sat outside the monitor: the radar was
        // halved and the money/weapon icons were clipped by the screen edge, not
        // by the renderer. Fit the whole window inside the work area instead and
        // shrink the client by whatever the frame costs.
        RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int frame_width = (bounds.right - bounds.left) - state.client_width;
        const int frame_height = (bounds.bottom - bounds.top) - state.client_height;
        const int available_width =
            static_cast<int>(std::max(1L, work.right - work.left)) - frame_width;
        const int available_height =
            static_cast<int>(std::max(1L, work.bottom - work.top)) - frame_height;
        state.client_width = std::clamp(state.client_width, 1, std::max(1, available_width));
        state.client_height = std::clamp(state.client_height, 1, std::max(1, available_height));
        bounds = RECT{0, 0, state.client_width, state.client_height};
        AdjustWindowRect(&bounds, style, FALSE);
        window_x = static_cast<int>(work.left);
        window_y = static_cast<int>(work.top);
    }
    const HWND window = CreateWindowExW(
        0, window_class.lpszClassName, L"KOKAKU Recomp — Ghost in the Shell: Stand Alone Complex",
        style, window_x, window_y,
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        std::cerr << "[window] CreateWindowExW failed: " << GetLastError() << "\n";
    } else {
        std::cout << "[window] Window created HWND=" << window << "\n";
    }
    state.window.store(window, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        state.ready.store(true, std::memory_order_release);
    }
    state.ready_signal.notify_all();
    if (window == nullptr) return;

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);
    state.focused.store(true, std::memory_order_relaxed);
    SetTimer(window, kStatusTimer, 250u, nullptr);

    // Raw mouse input for the camera. Registered on this window rather than
    // with RIDEV_INPUTSINK, so the game stops turning when you tab away.
    const RAWINPUTDEVICE mouse{0x01u, 0x02u, 0u, window};
    RegisterRawInputDevices(&mouse, 1u, sizeof(mouse));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (kokaku::controls_message(&message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    state.window.store(nullptr, std::memory_order_release);
}

void ensure_window_started() {
    WindowState &state = window_state();
    static std::once_flag once;
    std::call_once(once, [&state] {
        state.configuration = vcs_configuration().display;
        // Detached: the UI thread outlives every emulation stop path, and a
        // function-local static std::thread must never be destroyed joinable.
        state.thread = std::thread(window_thread_main);
        state.thread.detach();
        std::unique_lock<std::mutex> guard(state.mutex);
        state.ready_signal.wait(guard, [&state] { return state.ready.load(std::memory_order_acquire); });
    });
}

} // namespace

bool display_window_enabled() {
    const char *text = std::getenv("PSPRECOMP_WINDOW");
    if (text != nullptr && *text != '\0') return std::string(text) != "0";
    const VcsConfiguration &configuration = vcs_configuration();
    return !configuration.initialized || configuration.display.enabled;
}

void display_window_start() {
    std::cout << "[window] display_window_start: enabled=" << display_window_enabled()
              << " initialized=" << vcs_configuration().initialized
              << " display.enabled=" << vcs_configuration().display.enabled
              << " backend=" << static_cast<int>(vcs_configuration().rendering.backend) << "\n";
    if (!display_window_enabled()) return;
    ensure_window_started();
    if (vcs_configuration().rendering.backend != RenderingBackend::DirectX12) return;

    WindowState &state = window_state();
    HWND window = state.window.load(std::memory_order_acquire);
    if (ge_gpu_backend_active()) {
        // Native GE owns the D3D12 queue/swapchain. Avoid creating a second
        // presenter/device for the same HWND.
        ge_gpu_backend_set_native_window(window);
        return;
    }

    // Native GE may have been intentionally disabled or may have failed its
    // feature probe. In non-strict play mode the stable software GE can still
    // be shown through the standalone D3D12 presenter.
    std::string error;
    if (window == nullptr || !dx12_presenter_initialize(window, error)) {
        runtime_log_error("dx12 initialize", error.empty() ? "window unavailable" : error);
        std::cerr << "[dx12] initialize failed at startup: "
                  << (error.empty() ? "window unavailable" : error) << "\n";
        return;
    }
    const Dx12PresenterStatus dx = dx12_presenter_status();
    std::cout << "DirectX 12 presenter:  " << dx.adapter_name
              << " / " << dx.frames_in_flight << " frames in flight"
              << (dx.tearing_supported ? " / tearing" : "") << "\n";
}

void display_window_set_aspect_lock(bool locked) noexcept {
    if (!display_window_enabled()) return;
    window_state().aspect_lock.store(locked, std::memory_order_relaxed);
}

void display_window_set_status(const char *status) {
    if (!display_window_enabled() || status == nullptr) return;
    WindowState &state = window_state();
    if (!state.ready.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> guard(state.mutex);
    state.status = status;
}

void display_window_present(const psprecomp::GuestMemory &memory,
                            const FramebufferDescription &description) {
    if (!display_window_enabled()) return;
    ensure_window_started();
    if (description.address == 0u || description.width == 0u || description.height == 0u ||
        description.stride == 0u) {
        return;
    }
    WindowState &state = window_state();
    const HWND window = state.window.load(std::memory_order_acquire);
    if (window == nullptr) return;

    if (vcs_configuration().rendering.backend == RenderingBackend::DirectX12 &&
        !vcs_configuration().rendering.dx12_ge_color) {
        if (!dx12_presenter_active()) {
            std::string error;
            if (!dx12_presenter_initialize(window, error)) {
                runtime_log_error("dx12 initialize", error);
                std::cerr << "[dx12] initialize failed: " << error << "\n";
            }
        }
        if (dx12_presenter_active()) {
            try {
                std::vector<std::byte> rgba = decode_framebuffer_rgba(memory, description);
                std::string error;
                if (!dx12_presenter_present_rgba(
                        rgba, description.width, description.height, state.configuration,
                        state.aspect_lock.load(std::memory_order_relaxed), error)) {
                    runtime_log_error("dx12 present", error);
                    std::cerr << "[dx12] present failed: " << error << "\n";
                    dx12_presenter_shutdown();
                } else {
                    return;
                }
            } catch (const std::exception &error) {
                runtime_log_error("dx12 framebuffer decode", error.what());
                std::cerr << "[dx12] framebuffer decode failed: " << error.what() << "\n";
                return;
            }
        }
    }

    std::vector<std::uint8_t> rgb;
    try {
        rgb = decode_framebuffer_rgb(memory, description);
    } catch (const std::exception &) {
        // A transient framebuffer pointer outside EDRAM must never take the
        // host down; the previous frame simply stays on screen.
        return;
    }

    const std::size_t pixel_count = static_cast<std::size_t>(description.width) * description.height;
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        state.width = description.width;
        state.height = description.height;
        state.pixels.resize(pixel_count);
        for (std::size_t index = 0; index < pixel_count; ++index) {
            const std::uint32_t red = rgb[index * 3u + 0u];
            const std::uint32_t green = rgb[index * 3u + 1u];
            const std::uint32_t blue = rgb[index * 3u + 2u];
            state.pixels[index] = (red << 16u) | (green << 8u) | blue;
        }
        ++state.frame_index;
    }
    PostMessageW(window, kMessagePresent, 0, 0);
}

void display_window_present_rgba(std::span<const std::byte> rgba,
                                 std::uint32_t width,
                                 std::uint32_t height) {
    if (!display_window_enabled() || width == 0u || height == 0u) return;
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    if (rgba.size() < pixel_count * 4u) return;
    ensure_window_started();
    WindowState &state = window_state();
    const HWND window = state.window.load(std::memory_order_acquire);
    if (window == nullptr) return;
    if (vcs_configuration().rendering.backend == RenderingBackend::DirectX12 &&
        !vcs_configuration().rendering.dx12_ge_color) {
        if (!dx12_presenter_active()) {
            std::string error;
            if (!dx12_presenter_initialize(window, error)) {
                runtime_log_error("dx12 initialize", error);
                std::cerr << "[dx12] initialize failed: " << error << "\n";
            }
        }
        if (dx12_presenter_active()) {
            std::string error;
            if (!dx12_presenter_present_rgba(
                    rgba, width, height, state.configuration,
                    state.aspect_lock.load(std::memory_order_relaxed), error)) {
                runtime_log_error("dx12 present", error);
                std::cerr << "[dx12] present failed: " << error << "\n";
                dx12_presenter_shutdown();
            } else {
                return;
            }
        }
    }
    {
        std::lock_guard<std::mutex> guard(state.mutex);
        state.width = width;
        state.height = height;
        state.pixels.resize(pixel_count);
        // One 32-bit load and a red/blue swap per pixel. The byte-at-a-time
        // version cost several milliseconds per frame once the internal target
        // reached desktop resolution (a 3440x1440 frame is 4.95M pixels).
        for (std::size_t index = 0u; index < pixel_count; ++index) {
            std::uint32_t source = 0u;
            std::memcpy(&source, rgba.data() + index * 4u, sizeof(source));
            state.pixels[index] = (source & 0x0000FF00u) |
                                  ((source & 0x000000FFu) << 16u) |
                                  ((source >> 16u) & 0x000000FFu);
        }
        ++state.frame_index;
    }
    PostMessageW(window, kMessagePresent, 0, 0);
}

DisplayWindowSurface display_window_surface() {
    if (!display_window_enabled()) return {};
    ensure_window_started();
    WindowState &state = window_state();
    HWND window = state.window.load(std::memory_order_acquire);
    if (window == nullptr) return {};
    RECT client{};
    GetClientRect(window, &client);
    return {window, GetModuleHandleW(nullptr),
            static_cast<std::uint32_t>(std::max(0L, client.right - client.left)),
            static_cast<std::uint32_t>(std::max(0L, client.bottom - client.top))};
}

std::uint32_t display_window_buttons() {
    // Shares one reading with the analog path. Doing its own pass over
    // kKeyBindings is what left the mouse out entirely: the mouse buttons and
    // the wheel live in the reading below, so punching and tuning the radio
    // never reached the guest while the keyboard worked fine.
    return display_window_input().buttons;
}

void display_window_analog(std::uint8_t &x, std::uint8_t &y) {
    const HostInputState input = display_window_input();
    x = input.analog_x;
    y = input.analog_y;
}

bool display_window_fps_mouse() {
    // Headless keyboard diagnostics run the same binding path without an HWND.
    if (!display_window_enabled()) {
        const char *test = std::getenv("PSPRECOMP_KEY_TEST");
        const char *fps = std::getenv("PSPRECOMP_KEY_TEST_FPS");
        return test && (!fps || std::strcmp(fps, "0") != 0);
    }
    return window_state().fps_mouse.load(std::memory_order_relaxed);
}

HostInputState display_window_input() {
    // Scripted controller pulses remain active during windowed performance
    // tests, while the user's work in another app cannot alter the scenario.
    static const bool scripted_only = [] {
        const char *v = std::getenv("PSPRECOMP_DISABLE_HOST_INPUT");
        return v && *v == '1';
    }();
    if (scripted_only) return {};
    // The buttons and the analog stick are fetched by separate callers within
    // one controller poll, and the mouse deltas can only be drained once -- so
    // the reading is cached for a few milliseconds and both callers get the
    // same one. Shorter than a frame, so nothing here is ever perceptibly old.
    static std::mutex cache_mutex;
    static HostInputState cached{};
    static std::chrono::steady_clock::time_point cached_at{};
    const std::lock_guard<std::mutex> guard(cache_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (cached_at.time_since_epoch().count() != 0 &&
        now - cached_at < std::chrono::milliseconds(4))
        return cached;
    cached_at = now;
    cached = HostInputState{};

    HostInputState input{};
    const auto publish = [&]() -> HostInputState & { cached = input; return cached; };
    if (!display_window_enabled()) {
        if (const char *test = std::getenv("PSPRECOMP_KEY_TEST")) {
            const auto frame_value = [](const char *name, std::uint64_t fallback) {
                const char *value = std::getenv(name);
                return value ? std::strtoull(value, nullptr, 10) : fallback;
            };
            const int key = std::atoi(test);
            input.fps_mouse = display_window_fps_mouse();
            const bool held = weapon_input_frame >= frame_value("PSPRECOMP_KEY_TEST_START", 5060) &&
                              weapon_input_frame <= frame_value("PSPRECOMP_KEY_TEST_END", 5070);
            input.buttons = kokaku::control_buttons(kokaku::controls_snapshot(),
                kokaku::input_control_context(input.fps_mouse), [=](int candidate) { return held && key && candidate == key; });
        }
        return publish();
    }
    WindowState &state = window_state();
    if (!state.ready.load(std::memory_order_acquire)) return publish();

    if (!state.focused.load(std::memory_order_relaxed) || kokaku::controls_open()) {
        weapon_cycle_input.suspend();
        state.wheel.exchange(0, std::memory_order_relaxed);
        return publish();
    }
    input.fps_mouse = display_window_fps_mouse();
    const auto controls = kokaku::controls_snapshot();
    const int wheel = state.wheel.exchange(0, std::memory_order_relaxed);
    static ULONGLONG wheel_up_until{}, wheel_down_until{};
    const auto tick = GetTickCount64();
    if (wheel > 0) wheel_up_until = tick + 50;
    if (wheel < 0) wheel_down_until = tick + 50;
    const auto pressed = [&](int key) {
        if (kokaku::native_menu_active()) return false;
        if (!key) return false;
        if (key == kokaku::wheel_up) return tick < wheel_up_until;
        if (key == kokaku::wheel_down) return tick < wheel_down_until;
        return key_down(key);
    };
    const auto action_down = [&](std::size_t i) {
        return pressed(controls.keys[i][0]) || pressed(controls.keys[i][1]);
    };
    const bool weapon_active = input.fps_mouse && weapon_update_seen && weapon_input_frame-weapon_update_frame<=2 &&
        !kokaku::native_menu_active() && !kokaku::native_menu_navigation_active();
    weapon_cycle_input.sample(controls, wheel, weapon_active, key_down);
    input.buttons = kokaku::control_buttons(controls, kokaku::input_control_context(input.fps_mouse), pressed);
    const int move_x = int(action_down(3)) - int(action_down(2));
    const int move_y = int(action_down(1)) - int(action_down(0));
    const int reach = action_down(4) ? 60 : 127;
    input.analog_x = static_cast<std::uint8_t>(std::clamp(128 + move_x * reach, 0, 255));
    input.analog_y = static_cast<std::uint8_t>(std::clamp(128 + move_y * reach, 0, 255));
    const bool driving = false;
    if (const PfnXInputGetState get_state = xinput_get_state()) {
        XInputStatePacket pad{};
        if (get_state(0u, &pad) == 0u) {
            const std::uint16_t b = pad.gamepad.buttons;
            // The pad follows San Andreas' console layout, which is also the
            // scheme ThirteenAG's plugin assumes: cross accelerates and
            // sprints, square brakes and jumps, triangle enters vehicles.
            if (b & kPadA) input.buttons |= kPspCross;
            if (b & kPadX) input.buttons |= kPspSquare;
            if (b & kPadY) input.buttons |= kPspTriangle;
            if (b & kPadB) input.buttons |= kPspCircle;
            if (b & kPadLeftShoulder) input.buttons |= kPspLTrigger;
            if (b & kPadRightShoulder) input.buttons |= kPspRTrigger;
            if (b & kPadStart) input.buttons |= kPspStart;
            if (b & kPadBack) input.buttons |= kPspSelect;
            if (b & kPadDpadUp) input.buttons |= kPspUp;
            if (b & kPadDpadDown) input.buttons |= kPspDown;
            if (b & kPadDpadLeft) input.buttons |= kPspLeft;
            if (b & kPadDpadRight) input.buttons |= kPspRight;
            // On foot the triggers aim and look behind, which is where San
            // Andreas puts them. In a vehicle they must not: R is the aim
            // button, and holding aim suppresses the throttle, so a trigger
            // that sent R while driving accelerated and immediately undid it.
            // That is why the brake worked from LT and the accelerator did not
            // from RT -- L is look-behind and blocks nothing.
            if (!driving) {
                if (pad.gamepad.left_trigger > 64u) input.buttons |= kPspLTrigger;
                if (pad.gamepad.right_trigger > 64u) input.buttons |= kPspRTrigger;
            }
            // ...and they drive, the way every GTA on a modern pad does.
            //
            // Through the vehicle accessors rather than by moving them onto the
            // pad's R and L: those two are aim and look-behind on foot, so a
            // trigger wired straight to them would aim every time the player
            // walked with a finger resting on it. Routed this way the trigger
            // accelerates in a car and still aims out of one, and it needs no
            // help from ModernControlScheme -- that option is about which pad
            // button the game itself reads, which is a different question.
            if (pad.gamepad.right_trigger > 64u) input.accelerate = true;
            if (pad.gamepad.left_trigger > 64u) input.brake = true;

            const std::uint8_t pad_x = stick_to_psp(pad.gamepad.lx, false);
            // PSP Y grows downwards, the stick's grows upwards.
            const std::uint8_t pad_y = stick_to_psp(pad.gamepad.ly, true);
            if (pad_x != 128u || pad_y != 128u) {
                input.analog_x = pad_x;
                input.analog_y = pad_y;
            }
            // Not inverted, unlike the left stick: the camera axis reads
            // positive as up, which is the direction the stick already gives.
            const int camera_x = stick_to_psp(pad.gamepad.rx, false) - 128;
            int camera_y = stick_to_psp(pad.gamepad.ry, false) - 128;
            if (controls.invert_y) camera_y = -camera_y;
            if (camera_x != 0 || camera_y != 0) {
                input.camera_x = std::clamp(camera_x, -127, 127);
                input.camera_y = std::clamp(camera_y, -127, 127);
            }
        }
    }
    return publish();
}

MouseMotion display_window_consume_mouse() {
    WindowState &state = window_state();
    const int dx = state.mouse_dx.exchange(0), dy = state.mouse_dy.exchange(0);
    const bool enabled = state.focused.load() && state.fps_mouse.load() && !kokaku::controls_open() && !kokaku::native_menu_active();
    if (!enabled) return {};
    const auto c = kokaku::controls_snapshot();
    return {true, dx * c.sensitivity, dy * c.sensitivity * (c.invert_y ? -1.f : 1.f)};
}

int display_window_weapon_cycle(bool ready) {
    weapon_update_frame = weapon_input_frame;
    weapon_update_seen = true;
    const auto &state = window_state();
    const bool test = std::getenv("PSPRECOMP_WEAPON_TEST") != nullptr || std::getenv("PSPRECOMP_RELOAD_TEST") != nullptr;
    if ((!test && (!state.focused.load() || !state.fps_mouse.load())) ||
        kokaku::controls_open() || kokaku::native_menu_active() || kokaku::native_menu_navigation_active()) {
        weapon_cycle_input.clear();
        return 0;
    }
    return ready ? weapon_cycle_input.pop() : 0;
}

void display_window_weapon_frame(std::uint64_t frame) {
    weapon_input_frame = frame;
    // Menus/cutscenes stop the local motion update. Do not save their scrolls
    // for the next mission; use guest frames so a slow PC doesn't lose inputs.
    if (!weapon_update_seen || frame-weapon_update_frame>2) weapon_cycle_input.clear();
}

void display_window_test_weapon_wheel(int detents) {
    weapon_cycle_input.sample(kokaku::controls_snapshot(), detents, true, [](int) { return false; });
}
void display_window_test_reload() {
    const auto controls = kokaku::controls_snapshot();
    const int key = controls.keys[kokaku::reload_action][0] ? controls.keys[kokaku::reload_action][0] : controls.keys[kokaku::reload_action][1];
    weapon_cycle_input.sample(controls, 0, true, [=](int k) { return k == key; });
    weapon_cycle_input.sample(controls, 0, true, [](int) { return false; });
}

bool display_window_close_requested() {
    if (!display_window_enabled()) return false;
    return window_state().close_requested.load(std::memory_order_relaxed);
}

// Keeps the last rendered frame on screen after the guest stops so the run can
// be inspected.  PSPRECOMP_WINDOW_HOLD=0 closes immediately instead.
void display_window_shutdown() {
    if (!display_window_enabled()) {
        dx12_presenter_shutdown();
        return;
    }
    WindowState &state = window_state();
    const HWND window = state.window.load(std::memory_order_acquire);
    if (window == nullptr) return;
    const char *hold = std::getenv("PSPRECOMP_WINDOW_HOLD");
    if (hold != nullptr && std::string(hold) == "0") {
        dx12_presenter_shutdown();
        PostMessageW(window, WM_CLOSE, 0, 0);
        return;
    }
    SetWindowTextW(window, L"KOKAKU Recomp — stopped (close this window)");
    while (!state.close_requested.load(std::memory_order_relaxed) &&
           state.window.load(std::memory_order_acquire) != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    dx12_presenter_shutdown();
}

} // namespace vcs

#else

namespace vcs {

bool display_window_enabled() { return false; }
void display_window_start() {}
void display_window_set_status(const char *) {}
void display_window_set_aspect_lock(bool) noexcept {}
void display_window_present(const psprecomp::GuestMemory &, const FramebufferDescription &) {}
void display_window_present_rgba(std::span<const std::byte>, std::uint32_t, std::uint32_t) {}
DisplayWindowSurface display_window_surface() { return {}; }
std::uint32_t display_window_buttons() { return 0u; }
void display_window_analog(std::uint8_t &x, std::uint8_t &y) { x = 128u; y = 128u; }
HostInputState display_window_input() { return {}; }
bool display_window_fps_mouse() { return false; }
MouseMotion display_window_consume_mouse() { return {}; }
int display_window_weapon_cycle(bool) { return 0; }
void display_window_weapon_frame(std::uint64_t) {}
void display_window_test_weapon_wheel(int) {}
void display_window_test_reload() {}
bool display_window_close_requested() { return false; }
void display_window_shutdown() {}

} // namespace vcs

#endif
