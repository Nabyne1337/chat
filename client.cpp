#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#define _RICHEDIT_VER 0x0200
#include <richedit.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commdlg.h>
#include <string>
#include <array>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <locale>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "Comdlg32.lib")

#define ID_CHAT_EDIT      1001
#define ID_MESSAGE_EDIT   1002
#define ID_SEND_BUTTON    1003
#define ID_NICKNAME_EDIT  1004
#define ID_SHIFT_EDIT     1005
#define ID_CONNECT_BUTTON 1006
#define ID_STATUS_BAR     1007
#define ID_CLEAR_BUTTON   1008
#define ID_EXIT_BUTTON    1009
#define ID_THEME_DARK     2001
#define ID_THEME_LIGHT    2002
#define ID_THEME_BLUE     2003
#define ID_FONT_SELECT    2101

HWND g_hChatEdit = nullptr;
HWND g_hMessageEdit = nullptr;
HWND g_hNicknameEdit = nullptr;
HWND g_hShiftEdit = nullptr;
HWND g_hSendButton = nullptr;
HWND g_hConnectButton = nullptr;
HWND g_hClearButton = nullptr;
HWND g_hExitButton = nullptr;
HWND g_hStatusBar = nullptr;
HWND g_hMainWindow = nullptr;
HMENU g_hMenu = nullptr;
HFONT g_hFont = nullptr;
HFONT g_hBoldFont = nullptr;
HBRUSH g_hBackgroundBrush = nullptr;
HBRUSH g_hEditBrush = nullptr;

struct ThemeColors {
    COLORREF backgroundColor;
    COLORREF textColor;
    COLORREF editBackColor;
    COLORREF editTextColor;
    COLORREF buttonBackColor;
    COLORREF buttonTextColor;
    COLORREF statusBarColor;
};

ThemeColors g_darkTheme = { RGB(45,45,45), RGB(240,240,240), RGB(60,60,60), RGB(220,220,220), RGB(80,80,80), RGB(240,240,240), RGB(30,30,30) };
ThemeColors g_lightTheme = { RGB(240,240,240), RGB(20,20,20), RGB(255,255,255), RGB(0,0,0), RGB(225,225,225), RGB(0,0,0), RGB(210,210,210) };
ThemeColors g_blueTheme = { RGB(37,57,97), RGB(240,240,240), RGB(47,67,107), RGB(220,220,220), RGB(57,87,127), RGB(240,240,240), RGB(27,47,87) };
ThemeColors g_currentTheme = g_darkTheme;

bool g_isConnected = false;
std::atomic<bool> g_running{ false };
std::thread g_readerThread;
std::mutex g_chatMutex;

std::string g_serverIp = "";
std::string g_port = "8000";
const std::string g_path = "/api/server/chats";

std::wstring g_wnick;
int g_shift = 0;

static const std::array<std::wstring, 4> alphabets = {
    L"abcdefghijklmnopqrstuvwxyz",
    L"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    L"абвгдеёжзийклмнопрстуфхцчшщъыьэюя",
    L"АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
};

std::wstring GetText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring s(len, L'\0');
    GetWindowTextW(hwnd, &s[0], len + 1);
    return s;
}

std::wstring utf8ToWstring(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

std::string wstringToUtf8(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::wstring shiftText(const std::wstring& in, int shift) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        bool found = false;
        for (auto& alpha : alphabets) {
            auto pos = alpha.find(c);
            if (pos != std::wstring::npos) {
                int n = (int)alpha.size();
                int np = ((int)pos + shift % n + n) % n;
                out.push_back(alpha[np]);
                found = true;
                break;
            }
        }
        if (!found) out.push_back(c);
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        }
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

SOCKET connectToHost(const std::string& host, const std::string& port) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return INVALID_SOCKET;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return s;
}

void AppendToChatWindow(const std::wstring& txt) {
    std::lock_guard<std::mutex> lock(g_chatMutex);
    int len = GetWindowTextLengthW(g_hChatEdit);
    SendMessageW(g_hChatEdit, EM_SETSEL, len, len);
    std::wstring line = txt + L"\r\n";
    SendMessageW(g_hChatEdit, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    SendMessageW(g_hChatEdit, WM_VSCROLL, SB_BOTTOM, 0);
}

void UpdateStatus(const std::wstring& s) {
    SendMessageW(g_hStatusBar, SB_SETTEXT, 0, (LPARAM)s.c_str());
}

void postMessage(const std::string& host, const std::string& port, const std::string& path,
    const std::string& name, int shift, const std::string& msg) {
    SOCKET s = connectToHost(host, port);
    if (s == INVALID_SOCKET) { UpdateStatus(L"Ошибка подключения"); return; }
    std::string body = "name=" + urlEncode(name)
        + "&shift=" + std::to_string(shift)
        + "&message=" + urlEncode(msg);
    std::string req = "POST " + path + " HTTP/1.1\r\n"
        + "Host: " + host + "\r\n"
        + "Content-Type: application/x-www-form-urlencoded\r\n"
        + "Content-Length: " + std::to_string(body.size()) + "\r\n"
        + "Connection: close\r\n\r\n"
        + body;
    send(s, req.c_str(), (int)req.size(), 0);
    char buf[256];
    while (recv(s, buf, sizeof(buf), 0) > 0) {}
    closesocket(s);
    UpdateStatus(L"Сообщение отправлено");
}

void readEvents(const std::string& host, const std::string& port, const std::string& path,
    const std::string& , int shift, std::atomic<bool>& run) {
    const std::wstring sep = L" --> ";
    std::wstring me = g_wnick;
    while (run) {
        SOCKET s = connectToHost(host, port);
        if (s == INVALID_SOCKET) {
            UpdateStatus(L"Ошибка подключения. Повтор...");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        UpdateStatus(L"Подключено");
        std::string req = "GET " + path + "?shift=" + std::to_string(shift) + " HTTP/1.1\r\n"
            + "Host: " + host + "\r\n"
            + "Accept: text/event-stream\r\n"
            + "Connection: keep-alive\r\n\r\n";
        send(s, req.c_str(), (int)req.size(), 0);
        std::string buf;
        char tmp[1024];
        bool hdr = false;
        while (run) {
            int r = recv(s, tmp, sizeof(tmp), 0);
            if (r <= 0) break;
            buf.append(tmp, r);
            if (!hdr) {
                auto p = buf.find("\r\n\r\n");
                if (p != std::string::npos) { buf.erase(0, p + 4); hdr = true; }
            }
            size_t pos;
            while ((pos = buf.find("\n\n")) != std::string::npos) {
                std::string ev = buf.substr(0, pos);
                buf.erase(0, pos + 2);
                if (ev.rfind("data: ", 0) != 0) continue;
                std::wstring dec = shiftText(utf8ToWstring(ev.substr(6)), -shift);
                auto p2 = dec.find(sep);
                if (p2 == std::wstring::npos) continue;
                std::wstring sender = dec.substr(0, p2);
                std::wstring text = dec.substr(p2 + sep.size());
                if (sender == me) continue;
                AppendToChatWindow(L"【" + sender + L"】 " + text);
            }
        }
        closesocket(s);
        if (run) {
            UpdateStatus(L"Переподключение...");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}



void SendMessageUI() {
    if (!g_isConnected) {
        MessageBoxW(g_hMainWindow, L"Подключитесь сначала", L"AnonChat", MB_ICONINFORMATION);
        return;
    }
    std::wstring wmsg = GetText(g_hMessageEdit);
    if (wmsg.empty()) return;
    SetWindowTextW(g_hMessageEdit, L"");
    AppendToChatWindow(L"【Вы】 " + wmsg);
    std::string msg = wstringToUtf8(shiftText(wmsg, g_shift));
    std::string nick = wstringToUtf8(shiftText(g_wnick, g_shift));
    postMessage(g_serverIp, g_port, g_path, nick, g_shift, msg);
}

void ConnectToggle() {
    if (g_isConnected) {
        g_running = false;
        if (g_readerThread.joinable()) g_readerThread.join();
        g_isConnected = false;
        SetWindowTextW(g_hConnectButton, L"Подключиться");
        EnableWindow(g_hNicknameEdit, TRUE);
        EnableWindow(g_hShiftEdit, TRUE);
        UpdateStatus(L"Отключено");
        return;
    }
    g_wnick = GetText(g_hNicknameEdit);
    std::wstring shiftStr = GetText(g_hShiftEdit);
    if (g_wnick.empty()) {
        MessageBoxW(g_hMainWindow, L"Введите ник", L"AnonChat", MB_ICONINFORMATION);
        return;
    }
    try { g_shift = std::stoi(shiftStr); }
    catch (...) {
        MessageBoxW(g_hMainWindow, L"Неверный ключ", L"AnonChat", MB_ICONINFORMATION);
        return;
    }
    EnableWindow(g_hNicknameEdit, FALSE);
    EnableWindow(g_hShiftEdit, FALSE);
    SetWindowTextW(g_hConnectButton, L"Отключиться");
    g_running = true;
    g_isConnected = true;
    g_readerThread = std::thread(
        readEvents,
        g_serverIp, g_port, g_path,
        wstringToUtf8(shiftText(g_wnick, g_shift)),
        g_shift,
        std::ref(g_running)
    );
    AppendToChatWindow(L"✓ Подключено");
}

void ChangeTheme(int id) {
    if (g_hBackgroundBrush) DeleteObject(g_hBackgroundBrush);
    if (g_hEditBrush)       DeleteObject(g_hEditBrush);
    g_currentTheme =
        id == ID_THEME_LIGHT ? g_lightTheme :
        id == ID_THEME_BLUE ? g_blueTheme :
        g_darkTheme;
    g_hBackgroundBrush = CreateSolidBrush(g_currentTheme.backgroundColor);
    g_hEditBrush = CreateSolidBrush(g_currentTheme.editBackColor);

    CHARFORMAT2W cf = {};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = g_currentTheme.editTextColor;
    SendMessageW(g_hChatEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_currentTheme.editBackColor);
    SendMessageW(g_hChatEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(g_hMessageEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_currentTheme.editBackColor);
    SendMessageW(g_hMessageEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    InvalidateRect(g_hMainWindow, nullptr, TRUE);
}

void ChooseFont() {
    CHOOSEFONTW cf = {};
    LOGFONTW lf = {};
    cf.lStructSize = sizeof(cf);
    cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_EFFECTS;
    cf.lpLogFont = &lf;
    cf.rgbColors = g_currentTheme.textColor;
    if (g_hFont) GetObjectW(g_hFont, sizeof(lf), &lf);
    else { lf.lfHeight = 16; wcscpy_s(lf.lfFaceName, L"Segoe UI"); }
    if (ChooseFontW(&cf)) {
        if (g_hFont)     DeleteObject(g_hFont);
        if (g_hBoldFont) DeleteObject(g_hBoldFont);
        g_hFont = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_BOLD;
        g_hBoldFont = CreateFontIndirectW(&lf);

        HWND controls[] = {
            g_hChatEdit, g_hMessageEdit, g_hNicknameEdit, g_hShiftEdit,
            g_hConnectButton, g_hSendButton, g_hClearButton, g_hExitButton, g_hStatusBar
        };

        for (HWND h : controls) {
            SendMessageW(h, WM_SETFONT, (WPARAM)((h == g_hSendButton) ? g_hBoldFont : g_hFont), TRUE);
        }
    }
}

void ClearChat() {
    SetWindowTextW(g_hChatEdit, L"");
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HMODULE richEditLib = LoadLibraryW(L"Riched20.dll");
        if (!richEditLib) {
            MessageBoxW(hwnd, L"Не удалось загрузить RichEdit", L"Ошибка", MB_ICONERROR);
            return -1;
        }

        INITCOMMONCONTROLSEX ic = {};
        ic.dwSize = sizeof(ic);
        ic.dwICC = ICC_WIN95_CLASSES;
        InitCommonControlsEx(&ic);

        LOGFONTW lf = {};
        lf.lfHeight = 16;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        g_hFont = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_BOLD;
        g_hBoldFont = CreateFontIndirectW(&lf);

        g_hBackgroundBrush = CreateSolidBrush(g_currentTheme.backgroundColor);
        g_hEditBrush = CreateSolidBrush(g_currentTheme.editBackColor);

        g_hMenu = CreateMenu();
        HMENU f = CreatePopupMenu();
        HMENU s = CreatePopupMenu();
        HMENU t = CreatePopupMenu();
        AppendMenuW(f, MF_STRING, ID_CLEAR_BUTTON, L"Очистить чат");
        AppendMenuW(f, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(f, MF_STRING, ID_EXIT_BUTTON, L"Выход");
        AppendMenuW(t, MF_STRING, ID_THEME_DARK, L"Тёмная");
        AppendMenuW(t, MF_STRING, ID_THEME_LIGHT, L"Светлая");
        AppendMenuW(t, MF_STRING, ID_THEME_BLUE, L"Синяя");
        AppendMenuW(s, MF_POPUP, (UINT_PTR)t, L"Тема");
        AppendMenuW(s, MF_STRING, ID_FONT_SELECT, L"Шрифт...");
        AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)f, L"Меню");
        AppendMenuW(g_hMenu, MF_POPUP, (UINT_PTR)s, L"Настройки");
        SetMenu(hwnd, g_hMenu);

        CreateWindowW(L"STATIC", L"Никнейм:", WS_VISIBLE | WS_CHILD, 10, 10, 80, 24, hwnd, nullptr, GetModuleHandle(NULL), nullptr);
        g_hNicknameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 100, 10, 150, 24,
            hwnd, (HMENU)ID_NICKNAME_EDIT, GetModuleHandle(NULL), nullptr);
        CreateWindowW(L"STATIC", L"Ключ:", WS_VISIBLE | WS_CHILD, 260, 10, 50, 24, hwnd, nullptr, GetModuleHandle(NULL), nullptr);
        g_hShiftEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
            WS_VISIBLE | WS_CHILD | ES_NUMBER | ES_AUTOHSCROLL, 320, 10, 50, 24,
            hwnd, (HMENU)ID_SHIFT_EDIT, GetModuleHandle(NULL), nullptr);
        g_hConnectButton = CreateWindowW(L"BUTTON", L"Подключиться",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 390, 10, 120, 24,
            hwnd, (HMENU)ID_CONNECT_BUTTON, GetModuleHandle(NULL), nullptr);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - 20;
        int h = rc.bottom - 130;

        g_hChatEdit = CreateWindowExW(WS_EX_CLIENTEDGE, RICHEDIT_CLASSW,
            L"AnonChat Online Secure\r\nДобро пожаловать!\r\n",
            WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
            10, 44, w, h, hwnd, (HMENU)ID_CHAT_EDIT, GetModuleHandle(NULL), nullptr);

        if (!g_hChatEdit) {
            DWORD error = GetLastError();
            wchar_t buffer[256];
            swprintf_s(buffer, L"Не удалось создать окно чата. Код ошибки: %d", error);
            MessageBoxW(hwnd, buffer, L"Ошибка", MB_ICONERROR);
            return -1;
        }

        CHARFORMAT2W cf = {};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR;
        cf.crTextColor = g_currentTheme.editTextColor;
        SendMessageW(g_hChatEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_currentTheme.editBackColor);
        SendMessageW(g_hChatEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

        g_hMessageEdit = CreateWindowExW(WS_EX_CLIENTEDGE, RICHEDIT_CLASSW, nullptr,
            WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL,
            10, rc.bottom - 76, w - 130, 60,
            hwnd, (HMENU)ID_MESSAGE_EDIT, GetModuleHandle(NULL), nullptr);

        SendMessageW(g_hMessageEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)g_currentTheme.editBackColor);
        SendMessageW(g_hMessageEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        DWORD mask = (DWORD)SendMessageW(g_hMessageEdit, EM_GETEVENTMASK, 0, 0);
        SendMessageW(g_hMessageEdit, EM_SETEVENTMASK, 0, mask | ENM_KEYEVENTS);

        g_hSendButton = CreateWindowW(L"BUTTON", L"Отправить",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            rc.right - 120, rc.bottom - 76, 110, 30,
            hwnd, (HMENU)ID_SEND_BUTTON, GetModuleHandle(NULL), nullptr);

        g_hClearButton = CreateWindowW(L"BUTTON", L"Очистить",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            rc.right - 120, rc.bottom - 40, 50, 24,
            hwnd, (HMENU)ID_CLEAR_BUTTON, GetModuleHandle(NULL), nullptr);

        g_hExitButton = CreateWindowW(L"BUTTON", L"Выход",
            WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            rc.right - 60, rc.bottom - 40, 50, 24,
            hwnd, (HMENU)ID_EXIT_BUTTON, GetModuleHandle(NULL), nullptr);

        g_hStatusBar = CreateWindowW(STATUSCLASSNAMEW, nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0,
            hwnd, (HMENU)ID_STATUS_BAR, GetModuleHandle(NULL), nullptr);

        HWND controls[] = {
            g_hChatEdit, g_hMessageEdit, g_hNicknameEdit, g_hShiftEdit,
            g_hConnectButton, g_hSendButton, g_hClearButton,
            g_hExitButton, g_hStatusBar
        };

        for (HWND h : controls) {
            SendMessageW(h, WM_SETFONT,
                (WPARAM)((h == g_hSendButton) ? g_hBoldFont : g_hFont),
                TRUE);
        }

        SendMessageW(g_hChatEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        AppendToChatWindow(L"Чат инициализирован и готов к использованию!");
        UpdateStatus(L"Готов к подключению");
        return 0;
    }
    case WM_SIZE: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        SetWindowPos(g_hChatEdit, nullptr, 10, 44, rc.right - 20, rc.bottom - 130, SWP_NOZORDER);
        SetWindowPos(g_hMessageEdit, nullptr, 10, rc.bottom - 76, rc.right - 130, 60, SWP_NOZORDER);
        SetWindowPos(g_hSendButton, nullptr, rc.right - 120, rc.bottom - 76, 110, 30, SWP_NOZORDER);
        SetWindowPos(g_hClearButton, nullptr, rc.right - 120, rc.bottom - 40, 50, 24, SWP_NOZORDER);
        SetWindowPos(g_hExitButton, nullptr, rc.right - 60, rc.bottom - 40, 50, 24, SWP_NOZORDER);
        SendMessageW(g_hStatusBar, WM_SIZE, 0, 0);
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nmhdr = (NMHDR*)lParam;
        if (nmhdr->hwndFrom == g_hChatEdit && nmhdr->code == EN_MSGFILTER) {
            MSGFILTER* msgf = (MSGFILTER*)lParam;
            if (msgf->msg == WM_KEYDOWN && msgf->wParam == VK_RETURN) {
                if (GetKeyState(VK_CONTROL) < 0) {
                    SendMessageUI();
                    return 1;
                }
            }
        }
        break;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_SEND_BUTTON:    SendMessageUI();    return 0;
        case ID_CONNECT_BUTTON: ConnectToggle();   return 0;
        case ID_CLEAR_BUTTON:   ClearChat();       return 0;
        case ID_EXIT_BUTTON:    PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case ID_THEME_DARK:
        case ID_THEME_LIGHT:
        case ID_THEME_BLUE:     ChangeTheme(LOWORD(wParam)); return 0;
        case ID_FONT_SELECT:    ChooseFont();      return 0;
        }
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wParam;
        SetTextColor(dc, g_currentTheme.textColor);
        SetBkColor(dc, g_currentTheme.backgroundColor);
        return (LRESULT)g_hBackgroundBrush;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}


int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return 1;
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::locale::global(std::locale(""));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"AnonChatClass";

    if (!RegisterClassExW(&wc)) return 1;

    g_hMainWindow = CreateWindowExW(
        WS_EX_OVERLAPPEDWINDOW,
        L"AnonChatClass",
        L"AnonChat Online Secure",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 500,
        nullptr, nullptr, hInst, nullptr
    );

    if (!g_hMainWindow) return 1;

    ShowWindow(g_hMainWindow, nCmdShow);
    UpdateWindow(g_hMainWindow);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return (int)msg.wParam;
}
