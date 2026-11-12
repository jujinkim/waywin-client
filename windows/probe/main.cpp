// SPDX-License-Identifier: Apache-2.0
// Test fixture only: no RDP configuration, network listener, hooks or telemetry.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {
constexpr wchar_t WindowClass[] = L"WayWinProbeWindow";
constexpr UINT TrayMessage = WM_APP + 1;
enum Command : UINT { NewWindow = 100, DelayedWindow, Popup, AddTray, ChangeTray, RemoveTray,
                       Minimize, Restore, Resize, LaunchNotepad, Quit };
struct Button { Command command; const wchar_t* label; };
constexpr Button Buttons[] = {{NewWindow, L"&New window"}, {DelayedWindow, L"New in &15 seconds"},
    {Popup, L"&Popup dialog"}, {AddTray, L"&Add tray icon"}, {ChangeTray, L"&Change tray icon"},
    {RemoveTray, L"&Delete tray icon"}, {Minimize, L"&Minimize documents"},
    {Restore, L"&Restore documents"}, {Resize, L"Resi&ze documents"},
    {LaunchNotepad, L"Launch &Notepad"}, {Quit, L"E&xit fixture"}};
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

void refreshStatus() {
    const auto message = L"Session " + std::to_wstring(sessionId) + L" | PID " +
        std::to_wstring(GetCurrentProcessId()) + L" | alive " +
        std::to_wstring((GetTickCount64() - started) / 1000) + L"s\r\nInstance " + identity +
        L"\r\nTray clicks " + std::to_wstring(trayClicks) + L", menus " +
        std::to_wstring(trayMenus) + L"\r\n" + lastAction;
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

void trayOperation(DWORD operation) {
    auto data = iconData();
    if (!Shell_NotifyIconW(operation, &data)) {
        lastAction = L"Tray operation failed. Record failure; no support can be inferred.";
        return;
    }
    if (operation == NIM_ADD) {
        trayPresent = true;
        data.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
            Shell_NotifyIconW(NIM_DELETE, &data);
            trayPresent = false;
            lastAction = L"Tray version 4 failed; icon removed.";
            return;
        }
    } else if (operation == NIM_DELETE) {
        trayPresent = false;
    }
    lastAction = L"Tray API succeeded. Separately check client icon rendering and interaction.";
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
        const int buttons = scale(window, 194);
        MoveWindow(GetDlgItem(window, 1), gap, gap, std::max(1, static_cast<int>(rect.right) - 2 * gap),
                   scale(window, 100), TRUE);
        const int top = scale(window, 124);
        MoveWindow(GetDlgItem(window, 2), gap, top, std::max(1, static_cast<int>(rect.right) - buttons - 3 * gap),
                   std::max(1, static_cast<int>(rect.bottom) - top - gap), TRUE);
        int y = top;
        for (const auto& button : Buttons) {
            MoveWindow(GetDlgItem(window, button.command), static_cast<int>(rect.right) - buttons - gap,
                       y, buttons, scale(window, 30), TRUE);
            y += scale(window, 36);
        }
    } else {
        MoveWindow(GetDlgItem(window, 2), gap, gap, std::max(1, static_cast<int>(rect.right) - 2 * gap),
                   std::max(1, static_cast<int>(rect.bottom) - 2 * gap), TRUE);
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
        if (trayPresent) { alternateIcon = !alternateIcon; trayOperation(NIM_MODIFY); }
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
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        if (main) {
            mainWindow = window;
            CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1,
                            window, reinterpret_cast<HMENU>(1), instance, nullptr);
            for (const auto& button : Buttons)
                CreateWindowExW(0, L"BUTTON", button.label, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                0, 0, 1, 1, window, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(button.command)),
                                instance, nullptr);
            SetTimer(window, 1, 1000, nullptr);
        }
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, 0, 0, 1, 1,
            window, reinterpret_cast<HMENU>(2), instance, nullptr);
        SendDlgItemMessageW(window, 2, EM_SETLIMITTEXT, 32768, 0);
        layout(window);
        return 0;
    }
    case WM_SIZE: layout(window); return 0;
    case WM_DPICHANGED: {
        const auto rect = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(window);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {scale(window, main ? 700 : 320), scale(window, main ? 610 : 240)};
        return 0;
    }
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
            ++trayMenus;
            POINT point{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            if (point.x == -1 && point.y == -1) GetCursorPos(&point);
            const HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, NewWindow, L"New probe window");
            AppendMenuW(menu, MF_STRING, ChangeTray, L"Change icon");
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
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE appInstance, HINSTANCE, PWSTR, int show) {
    instance = appInstance;
    started = GetTickCount64();
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
        const HWND root = GetAncestor(message.hwnd, GA_ROOT);
        if (!root || !IsDialogMessageW(root, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result == -1 ? 1 : 0;
}
