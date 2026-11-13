// SPDX-License-Identifier: Apache-2.0
// Test fixture only: no RDP configuration, network listener, hooks or telemetry.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
constexpr wchar_t WindowClass[] = L"WayWinProbeWindow";
constexpr UINT TrayMessage = WM_APP + 1;
enum Command : UINT { NewWindow = 100, DelayedWindow, Popup, AddTray, ChangeTray, RemoveTray,
                       Minimize, Restore, Resize, LaunchNotepad, Quit,
                       StartMenu, InputSettings, KoreanImeSettings, QuickSettings };
constexpr wchar_t FontProperty[] = L"WayWinProbe.Font";
HINSTANCE instance = nullptr;
HWND mainWindow = nullptr;
std::vector<HWND> documents;
std::wstring identity;
std::wstring lastAction = L"Ready. Type disposable text below; it stays only in this process.";
DWORD sessionId = 0;
ULONGLONG started = 0;
unsigned documentSerial = 0;
unsigned trayClicks = 0;
unsigned trayMenus = 0;
bool trayPresent = false;
bool alternateIcon = false;
UINT taskbarCreated = 0;

int scale(HWND window, int value) { return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96); }

bool updateFont(HWND window) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                    GetDpiForWindow(window))) return false;
    const HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    if (!font) return false;
    const auto previous = GetPropW(window, FontProperty);
    if (!SetPropW(window, FontProperty, font)) { DeleteObject(font); return false; }
    EnumChildWindows(window, [](HWND child, LPARAM value) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
    if (previous) DeleteObject(previous);
    return true;
}

HMENU createProbeMenu() {
    const HMENU menu = CreateMenu();
    if (!menu) return nullptr;
    // Menus keep every action reachable at high DPI and on short displays.
    struct Entry { UINT id; const wchar_t* label; };
    const auto appendGroup = [&](const wchar_t* label, std::initializer_list<Entry> entries) {
        const HMENU group = CreatePopupMenu();
        if (!group) return false;
        for (const auto& entry : entries) {
            if (!AppendMenuW(group, MF_STRING, entry.id, entry.label)) {
                DestroyMenu(group);
                return false;
            }
        }
        if (!AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(group), label)) {
            DestroyMenu(group);
            return false;
        }
        return true;
    };
    if (!appendGroup(L"&Documents", {{NewWindow, L"&New window"}, {DelayedWindow, L"New in &15 seconds"},
            {Minimize, L"&Minimize documents"}, {Restore, L"&Restore documents"}, {Resize, L"Resi&ze documents"},
            {Popup, L"&Popup dialog"}, {LaunchNotepad, L"Launch &Notepad"}}) ||
        !appendGroup(L"&Tray", {{AddTray, L"&Add tray icon"}, {ChangeTray, L"&Change tray icon"},
            {RemoveTray, L"&Delete tray icon"}}) ||
        !appendGroup(L"Windows &shell", {{StartMenu, L"&Start menu"}, {InputSettings, L"&Input settings"},
            {KoreanImeSettings, L"&Korean IME settings"}, {QuickSettings, L"&Quick Settings"}}) ||
        !AppendMenuW(menu, MF_STRING, Quit, L"E&xit")) {
        DestroyMenu(menu);
        return nullptr;
    }
    return menu;
}

void refreshStatus() {
    const auto message = L"Session " + std::to_wstring(sessionId) + L" | PID " +
        std::to_wstring(GetCurrentProcessId()) + L" | alive " +
        std::to_wstring((GetTickCount64() - started) / 1000) + L"s\r\nInstance " + identity +
        L"\r\nTray clicks " + std::to_wstring(trayClicks) + L", menus " +
        std::to_wstring(trayMenus) + L" | DPI " + std::to_wstring(GetDpiForWindow(mainWindow)) +
        L"\r\nSession displays " + std::to_wstring(GetSystemMetrics(SM_CMONITORS)) + L" | bounds " +
        std::to_wstring(GetSystemMetrics(SM_XVIRTUALSCREEN)) + L"," +
        std::to_wstring(GetSystemMetrics(SM_YVIRTUALSCREEN)) + L" " +
        std::to_wstring(GetSystemMetrics(SM_CXVIRTUALSCREEN)) + L"x" +
        std::to_wstring(GetSystemMetrics(SM_CYVIRTUALSCREEN)) + L"\r\n" + lastAction;
    SetWindowTextW(GetDlgItem(mainWindow, 1), message.c_str());
}

NOTIFYICONDATAW iconData() {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = mainWindow;
    data.uID = 1;
    data.uCallbackMessage = TrayMessage;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.hIcon = LoadIconW(nullptr, alternateIcon ? IDI_WARNING : IDI_INFORMATION);
    lstrcpynW(data.szTip, alternateIcon ? L"WayWin probe: changed" : L"WayWin probe: initial", 128);
    return data;
}

bool trayOperation(DWORD operation) {
    auto data = iconData();
    if (!Shell_NotifyIconW(operation, &data)) {
        lastAction = L"Tray operation failed. Record failure; no support can be inferred.";
        return false;
    }
    if (operation == NIM_ADD) {
        trayPresent = true;
        data.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
            Shell_NotifyIconW(NIM_DELETE, &data);
            trayPresent = false;
            lastAction = L"Tray version 4 failed; icon removed.";
            return false;
        }
    } else if (operation == NIM_DELETE) {
        trayPresent = false;
    }
    lastAction = L"Tray API succeeded. Separately check client icon rendering and interaction.";
    return true;
}

void createDocument() {
    const auto title = L"WayWin document " + std::to_wstring(++documentSerial) + L" " + identity;
    const auto window = CreateWindowExW(WS_EX_APPWINDOW, WindowClass, title.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560, 360,
        nullptr, nullptr, instance, reinterpret_cast<void*>(1));
    if (!window) { lastAction = L"Window creation failed."; return; }
    documents.push_back(window);
    ShowWindow(window, SW_SHOW);
    lastAction = L"Created document " + std::to_wstring(documentSerial);
}

void layout(HWND window) {
    RECT rect{};
    GetClientRect(window, &rect);
    const int gap = scale(window, 12);
    const bool main = GetWindowLongPtrW(window, GWLP_USERDATA) == 0;
    if (main) {
        MoveWindow(GetDlgItem(window, 1), gap, gap, std::max(1, static_cast<int>(rect.right) - 2 * gap),
                   scale(window, 160), TRUE);
        const int top = scale(window, 184);
        MoveWindow(GetDlgItem(window, 2), gap, top, std::max(1, static_cast<int>(rect.right) - 2 * gap),
                   std::max(1, static_cast<int>(rect.bottom) - top - gap), TRUE);
    } else {
        MoveWindow(GetDlgItem(window, 2), gap, gap, std::max(1, static_cast<int>(rect.right) - 2 * gap),
                   std::max(1, static_cast<int>(rect.bottom) - 2 * gap), TRUE);
    }
}

void openSettings(const wchar_t* uri) {
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(mainWindow, L"open", uri,
                                                              nullptr, nullptr, SW_SHOWNORMAL));
    lastAction = result > 32 ? L"Windows settings requested; verify the actual remote UI and input."
                            : L"Windows settings request failed or page unavailable.";
}

void requestQuickSettings() {
    // A user-invoked shell shortcut in this Windows session. Never a key logger.
    if (GetForegroundWindow() != mainWindow) {
        lastAction = L"Focus this fixture before requesting Quick Settings.";
        return;
    }
    for (int key : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN, static_cast<int>('A')}) {
        if (GetAsyncKeyState(key) & 0x8000) {
            lastAction = L"Release modifier keys and A, then select Quick Settings again.";
            return;
        }
    }
    INPUT input[4]{};
    for (auto& event : input) event.type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[3].ki.wVk = VK_LWIN;
    input[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[1].ki.wVk = input[2].ki.wVk = 'A';
    input[2].ki.dwFlags = KEYEVENTF_KEYUP;
    input[3].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    const UINT sent = SendInput(4, input, sizeof(INPUT));
    if (sent != 4) {
        // Release only synthetic keys potentially left pressed by a partial insertion.
        if (sent > 0) SendInput(2, &input[2], sizeof(INPUT));
        lastAction = L"Quick Settings shortcut failed; record the session/policy limitation.";
    } else {
        lastAction = L"Quick Settings shortcut sent; this does not prove the flyout was mirrored.";
    }
}

void perform(UINT command) {
    switch (command) {
    case NewWindow: createDocument(); break;
    case DelayedWindow:
        lastAction = SetTimer(mainWindow, 2, 15000, nullptr) ? L"One new window scheduled in 15 seconds."
                                                          : L"Scheduling failed.";
        break;
    case Popup:
        MessageBoxW(mainWindow, L"Test focus, Enter/Escape, movement and return to the parent.",
                    L"WayWin modal popup", MB_OKCANCEL | MB_ICONINFORMATION);
        lastAction = L"Popup closed.";
        break;
    case AddTray:
        if (!trayPresent) trayOperation(NIM_ADD);
        else lastAction = L"Tray icon already present.";
        break;
    case ChangeTray:
        if (trayPresent) {
            alternateIcon = !alternateIcon;
            // Keep the displayed state equal to the icon the shell actually has.
            if (!trayOperation(NIM_MODIFY)) alternateIcon = !alternateIcon;
        }
        else lastAction = L"Add an icon first.";
        break;
    case RemoveTray:
        if (trayPresent) trayOperation(NIM_DELETE);
        else lastAction = L"Tray icon already absent.";
        break;
    case Minimize: case Restore: case Resize:
        for (const HWND window : documents) {
            if (command == Resize)
                SetWindowPos(window, nullptr, 0, 0, scale(window, 680), scale(window, 440),
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            else ShowWindow(window, command == Restore ? SW_RESTORE : SW_MINIMIZE);
        }
        lastAction = L"Document operation requested. Verify each local window.";
        break;
    case LaunchNotepad: {
        wchar_t directory[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (!length || length >= MAX_PATH) { lastAction = L"System directory unavailable."; break; }
        const auto executable = std::wstring(directory) + L"\\notepad.exe";
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(mainWindow, L"open",
            executable.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        lastAction = result > 32 ? L"Notepad launch requested; record actual result."
                                : L"Notepad launch failed.";
        break;
    }
    case StartMenu:
        lastAction = PostMessageW(mainWindow, WM_SYSCOMMAND, SC_TASKLIST, 0)
            ? L"Start menu requested; verify remote display, search, selection and dismissal."
            : L"Start menu request failed.";
        break;
    case InputSettings: openSettings(L"ms-settings:keyboard-advanced"); break;
    case KoreanImeSettings: openSettings(L"ms-settings:regionlanguage-korime"); break;
    case QuickSettings: requestQuickSettings(); break;
    case Quit: SendMessageW(mainWindow, WM_CLOSE, 0, 0); return;
    default: return;
    }
    refreshStatus();
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    const bool main = GetWindowLongPtrW(window, GWLP_USERDATA) == 0;
    if (taskbarCreated && message == taskbarCreated && main && trayPresent) {
        trayPresent = false;
        trayOperation(NIM_ADD);
        refreshStatus();
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        if (main) {
            mainWindow = window;
            const HMENU menu = createProbeMenu();
            if (!menu) return -1;
            if (!SetMenu(window, menu)) { DestroyMenu(menu); return -1; }
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1,
                            window, reinterpret_cast<HMENU>(1), instance, nullptr);
            SetTimer(window, 1, 1000, nullptr);
        }
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, 0, 0, 1, 1,
            window, reinterpret_cast<HMENU>(2), instance, nullptr);
        SendDlgItemMessageW(window, 2, EM_SETLIMITTEXT, 32768, 0);
        if (!updateFont(window)) return -1;
        layout(window);
        return 0;
    }
    case WM_SIZE: layout(window); return 0;
    case WM_DPICHANGED: {
        const auto rect = reinterpret_cast<RECT*>(lParam);
        if (!updateFont(window)) lastAction = L"DPI font update failed.";
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(window);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {scale(window, main ? 380 : 320), scale(window, main ? 320 : 240)};
        return 0;
    }
    case WM_DISPLAYCHANGE:
        if (main) refreshStatus();
        return 0;
    case WM_TIMER:
        if (wParam == 2) { KillTimer(window, 2); createDocument(); }
        if (main) refreshStatus();
        return 0;
    case WM_COMMAND:
        if (main && HIWORD(wParam) == BN_CLICKED) perform(LOWORD(wParam));
        return 0;
    case TrayMessage:
        if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT) {
            ++trayClicks;
            ShowWindow(mainWindow, SW_RESTORE);
            SetForegroundWindow(mainWindow);
            refreshStatus();
        } else if (LOWORD(lParam) == WM_CONTEXTMENU) {
            POINT point{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            const HMENU menu = CreatePopupMenu();
            if (!menu || !AppendMenuW(menu, MF_STRING, NewWindow, L"New probe window") ||
                !AppendMenuW(menu, MF_STRING, ChangeTray, L"Change icon")) {
                if (menu) DestroyMenu(menu);
                lastAction = L"Tray menu could not be created; the request was not counted.";
                refreshStatus();
                return 0;
            }
            ++trayMenus;  // Counts menus that were created and requested.
            SetForegroundWindow(mainWindow);
            const UINT selected = static_cast<UINT>(TrackPopupMenu(menu,
                TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, mainWindow, nullptr));
            DestroyMenu(menu);
            PostMessageW(mainWindow, WM_NULL, 0, 0);
            auto data = iconData();
            Shell_NotifyIconW(NIM_SETFOCUS, &data);
            perform(selected);
            refreshStatus();
        }
        return 0;
    case WM_CLOSE:
        if (main && MessageBoxW(window, L"Exit the fixture and discard its test text?\n"
            L"For reconnection tests, disconnect the RDP client instead.", L"WayWin probe",
            MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (main) {
            KillTimer(window, 1);
            KillTimer(window, 2);
            if (trayPresent) trayOperation(NIM_DELETE);
            PostQuitMessage(0);
        } else {
            documents.erase(std::remove(documents.begin(), documents.end(), window), documents.end());
        }
        return 0;
    case WM_NCDESTROY:
        if (const auto font = RemovePropW(window, FontProperty)) DeleteObject(font);
        break;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE appInstance, HINSTANCE, PWSTR, int show) {
    instance = appInstance;
    started = GetTickCount64();
    // ShellExecuteW may activate COM handlers (the ms-settings: pages do).
    struct ComApartment {
        bool ready = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        ~ComApartment() { if (ready) CoUninitialize(); }
    } com;
    GUID guid{};
    wchar_t buffer[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, buffer, 40) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) return 1;
    identity = buffer;
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW cls{};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    cls.lpszClassName = WindowClass;
    if (!RegisterClassW(&cls)) return 1;
    mainWindow = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, L"WayWin feasibility fixture",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 660,
        nullptr, nullptr, instance, nullptr);
    if (!mainWindow) return 1;
    ShowWindow(mainWindow, show);
    createDocument();
    refreshStatus();
    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        // Escape would reach a window as a cancel or close request and silently destroy a
        // document window with its test text. Menus and message boxes run their own loops.
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) continue;
        const HWND root = GetAncestor(message.hwnd, GA_ROOT);
        if (!root || !IsDialogMessageW(root, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result == -1 ? 1 : 0;
}
