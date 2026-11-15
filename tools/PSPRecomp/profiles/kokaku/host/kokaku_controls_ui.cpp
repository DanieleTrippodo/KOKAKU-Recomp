#include "kokaku_controls_ui.hpp"
#include "kokaku_control_icons.hpp"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#include <windowsx.h>
#endif

namespace kokaku {
namespace {
std::mutex controls_mutex;
std::atomic<bool> opened{};
std::filesystem::path controls_path() {
    if (const char *path = std::getenv("PSPRECOMP_CONTROLS")) return path;
#if defined(_WIN32)
    wchar_t exe[32768]{}; GetModuleFileNameW(nullptr, exe, 32768);
    return std::filesystem::path(exe).parent_path() / "KokakuControls.ini";
#else
    return "KokakuControls.ini";
#endif
}
Controls &current() {
    static Controls value = [] {
        Controls c; std::string error;
        if (std::filesystem::exists(controls_path())) c.load(controls_path(), error);
        return c;
    }();
    return value;
}
#if defined(_WIN32)
HWND dialog{}, list{}, hint{}, sensitivity{}, invert{};
Controls draft;
int capture_action{-1}, capture_slot{};
constexpr int list_id=100, primary_id=101, secondary_id=102, clear_id=103,
    reset_id=104, save_id=105, cancel_id=106;
std::wstring key_name(int key) { return control_key_name(key); }
void refresh_list() {
    const auto selection = SendMessageW(list, LB_GETCURSEL, 0, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const std::wstring row = std::wstring(actions[i].label) + L"\t" +
            key_name(draft.keys[i][0]) + L"\t" + key_name(draft.keys[i][1]);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
    }
    SendMessageW(list, LB_SETCURSEL, selection == LB_ERR ? 0 : selection, 0);
}
void capture(int slot) {
    capture_action = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
    capture_slot = slot;
    if (capture_action >= 0) {
        SetWindowTextW(hint, L"Press a key, mouse button or scroll wheel. Esc cancels; F1/F2 are reserved.");
        SetFocus(dialog);
    }
}
void accept_key(int key) {
    if (capture_action < 0) return;
    if (key == VK_ESCAPE) {
        capture_action = -1;
        SetWindowTextW(hint, L"Binding cancelled.");
    } else if (!valid_binding(key)) {
        SetWindowTextW(hint, L"F1 and F2 are reserved. Choose another key.");
    } else {
        draft.bind(capture_action, capture_slot, key);
        capture_action = -1;
        refresh_list();
        SetWindowTextW(hint, L"Binding updated. Conflicts are swapped; Save applies your changes.");
    }
}
HWND child(const wchar_t *type, const wchar_t *label, DWORD style, int x, int y, int w, int h, int id) {
    HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style,
        x,y,w,h,dialog,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}
LRESULT CALLBACK options_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wp)==list_id && HIWORD(wp)==LBN_DBLCLK) { capture(0); return 0; }
        switch (LOWORD(wp)) {
        case primary_id: capture(0); return 0;
        case secondary_id: capture(1); return 0;
        case clear_id: {
            const int i = static_cast<int>(SendMessageW(list,LB_GETCURSEL,0,0));
            if (i>=0) { draft.keys[i]={0,0}; refresh_list(); }
            return 0;
        }
        case reset_id:
            draft = Controls{}; refresh_list();
            SetWindowTextW(sensitivity,L"0.12"); SendMessageW(invert,BM_SETCHECK,BST_UNCHECKED,0);
            return 0;
        case save_id: {
            wchar_t buffer[80]{}; GetWindowTextW(sensitivity,buffer,80);
            wchar_t *end{}; const float value = std::wcstof(buffer,&end);
            if (end == buffer || *end || !std::isfinite(value) || value<0.01f || value>2.f) {
                SetWindowTextW(hint,L"Invalid sensitivity: enter a value from 0.01 to 2.00 (use a decimal point)."); return 0;
            }
            draft.sensitivity=value;
            draft.invert_y=SendMessageW(invert,BM_GETCHECK,0,0)==BST_CHECKED;
            std::string error;
            if (!draft.save(controls_path(),error)) {
                MessageBoxA(window,error.c_str(),"Save controls",MB_OK|MB_ICONERROR); return 0;
            }
            { std::lock_guard lock(controls_mutex); current()=draft; }
            DestroyWindow(window); return 0;
        }
        case cancel_id: DestroyWindow(window); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        capture_action=-1; dialog=nullptr; opened.store(false);
        SetForegroundWindow(GetWindow(window,GW_OWNER)); return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}
#endif
}
Controls controls_snapshot() { std::lock_guard lock(controls_mutex); return current(); }
bool controls_commit(const Controls &value, std::string &error) {
    std::lock_guard lock(controls_mutex);
    if (!value.save(controls_path(), error)) return false;
    current() = value;
    return true;
}
bool controls_open() noexcept { return opened.load(); }
void show_controls(void *owner) {
#if defined(_WIN32)
    if (dialog) { SetForegroundWindow(dialog); return; }
    draft=controls_snapshot();
    WNDCLASSW wc{}; wc.lpfnWndProc=options_proc; wc.hInstance=GetModuleHandleW(nullptr);
    wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1); wc.lpszClassName=L"KokakuControls";
    RegisterClassW(&wc);
    RECT bounds{0,0,780,660}; AdjustWindowRectEx(&bounds,WS_CAPTION|WS_SYSMENU,FALSE,WS_EX_DLGMODALFRAME);
    dialog=CreateWindowExW(WS_EX_DLGMODALFRAME,wc.lpszClassName,L"Options - Mouse and keyboard",
        WS_CAPTION|WS_SYSMENU,CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,
        static_cast<HWND>(owner),nullptr,wc.hInstance,nullptr);
    if (!dialog) return;
    opened.store(true);
    child(L"STATIC",L"Select an action and edit its primary or secondary binding. The game is paused.",0,16,14,748,22,0);
    child(L"STATIC",L"Action                                               Primary                            Secondary",0,16,44,748,22,0);
    list=child(L"LISTBOX",L"",WS_BORDER|WS_VSCROLL|WS_TABSTOP|LBS_NOTIFY|LBS_USETABSTOPS,16,68,748,400,list_id);
    const int tabs[]{160,270}; SendMessageW(list,LB_SETTABSTOPS,2,reinterpret_cast<LPARAM>(tabs));
    child(L"BUTTON",L"Primary binding",WS_TABSTOP,16,478,148,30,primary_id);
    child(L"BUTTON",L"Secondary binding",WS_TABSTOP,174,478,148,30,secondary_id);
    child(L"BUTTON",L"Clear bindings",WS_TABSTOP,332,478,170,30,clear_id);
    child(L"BUTTON",L"Restore defaults",WS_TABSTOP,514,478,174,30,reset_id);
    child(L"STATIC",L"Mouse sensitivity",0,16,524,260,22,0);
    sensitivity=child(L"EDIT",std::to_wstring(draft.sensitivity).c_str(),WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,278,520,86,26,0);
    invert=child(L"BUTTON",L"Invert Y axis",BS_AUTOCHECKBOX|WS_TABSTOP,392,520,226,28,0);
    SendMessageW(invert,BM_SETCHECK,draft.invert_y?BST_CHECKED:BST_UNCHECKED,0);
    hint=child(L"STATIC",L"F1 captures/releases the mouse. Gameplay uses the PSP Type A layout.",0,16,558,748,38,0);
    child(L"BUTTON",L"Save and close",BS_DEFPUSHBUTTON|WS_TABSTOP,460,610,148,32,save_id);
    child(L"BUTTON",L"Cancel",WS_TABSTOP,620,610,144,32,cancel_id);
    refresh_list(); ShowWindow(dialog,SW_SHOW); SetForegroundWindow(dialog); SetFocus(list);
#endif
}
bool controls_message(void *message) {
#if defined(_WIN32)
    auto &msg=*static_cast<MSG*>(message);
    if (!dialog || (msg.hwnd!=dialog && !IsChild(dialog,msg.hwnd))) return false;
    if (capture_action>=0) {
        int key=-1;
        if (msg.message==WM_KEYDOWN || msg.message==WM_SYSKEYDOWN) {
            if (!(msg.lParam & (1ll<<30))) key=static_cast<int>(msg.wParam);
        } else if (msg.message==WM_LBUTTONDOWN) key=VK_LBUTTON;
        else if (msg.message==WM_RBUTTONDOWN) key=VK_RBUTTON;
        else if (msg.message==WM_MBUTTONDOWN) key=VK_MBUTTON;
        else if (msg.message==WM_XBUTTONDOWN) key=GET_XBUTTON_WPARAM(msg.wParam)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2;
        else if (msg.message==WM_MOUSEWHEEL) key=GET_WHEEL_DELTA_WPARAM(msg.wParam)>0?wheel_up:wheel_down;
        if (key>=0) { accept_key(key); return true; }
        if (msg.message>=WM_KEYFIRST && msg.message<=WM_KEYLAST) return true;
    }
    return IsDialogMessageW(dialog,&msg)!=FALSE;
#else
    return false;
#endif
}
}
