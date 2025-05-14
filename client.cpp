#include <iostream>
#include <string>
#include <array>
#include <thread>
#include <atomic>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <windows.h>

#pragma comment(lib, "Ws2_32.lib")

static const std::array<std::wstring, 4> alphabets = {
    L"abcdefghijklmnopqrstuvwxyz",
    L"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    L"абвгдеёжзийклмнопрстуфхцчшщъыьэюя",
    L"АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
};

std::wstring utf8ToWstring(const std::string& str) {
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], len);
    result.pop_back();
    return result;
}

std::string wstringToUtf8(const std::wstring& wstr) {
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::wstring safeGetline() {
    std::string utf8line;
    std::getline(std::cin, utf8line);
    return utf8ToWstring(utf8line);
}

int safeGetInt() {
    int v;
    while (!(std::cin >> v)) {
        std::cin.clear();
        std::cin.ignore(10000, '\n');
        std::cout << "Пожалуйста, введите число: ";
    }
    std::cin.ignore(10000, '\n');
    return v;
}

std::wstring shiftText(const std::wstring& input, int shift) {
    std::wstring result;
    result.reserve(input.size());
    for (wchar_t c : input) {
        bool found = false;
        for (auto& alpha : alphabets) {
            auto pos = alpha.find(c);
            if (pos != std::wstring::npos) {
                int n = (int)alpha.size();
                int np = (static_cast<int>(pos) + shift % n + n) % n;
                result.push_back(alpha[np]);
                found = true;
                break;
            }
        }
        if (!found) {
            result.push_back(c);
        }
    }
    return result;
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
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) {
        return INVALID_SOCKET;
    }
    SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        return INVALID_SOCKET;
    }
    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return sock;
}

void readEvents(const std::string& host,
    const std::string& port,
    const std::string& path,
    const std::string& nickname,
    int shift,
    std::atomic<bool>& running)
{
    std::wstring wnick = utf8ToWstring(nickname);
    const std::wstring sep = L" --> ";
    while (running) {
        SOCKET sock = connectToHost(host, port);
        if (sock == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        std::string req =
            "GET " + path + "?shift=" + std::to_string(shift) + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Accept: text/event-stream\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        send(sock, req.c_str(), (int)req.size(), 0);
        std::string buffer;
        char tmp[1024];
        bool headersSkipped = false;
        while (running) {
            int recvBytes = recv(sock, tmp, sizeof(tmp), 0);
            if (recvBytes <= 0) break;
            buffer.append(tmp, recvBytes);
            if (!headersSkipped) {
                auto p = buffer.find("\r\n\r\n");
                if (p != std::string::npos) {
                    buffer.erase(0, p + 4);
                    headersSkipped = true;
                }
            }
            size_t evPos;
            while ((evPos = buffer.find("\n\n")) != std::string::npos) {
                std::string ev = buffer.substr(0, evPos);
                buffer.erase(0, evPos + 2);
                if (ev.rfind("data: ", 0) != 0) continue;
                std::string raw = ev.substr(6);
                std::wstring wraw = utf8ToWstring(raw);
                std::wstring decoded = shiftText(wraw, -shift);
                auto pos = decoded.find(sep);
                if (pos == std::wstring::npos) continue;
                std::wstring sender = decoded.substr(0, pos);
                std::wstring text = decoded.substr(pos + sep.size());
                if (sender == wnick) continue;
                std::cout << "\r" << wstringToUtf8(sender) << ": " << wstringToUtf8(text)
                    << "\n<" << wstringToUtf8(wnick) << ">: " << std::flush;
            }
        }
        closesocket(sock);
        if (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void postMessage(const std::string& host,
    const std::string& port,
    const std::string& path,
    const std::string& name,
    int shift,
    const std::string& msg)
{
    SOCKET sock = connectToHost(host, port);
    if (sock == INVALID_SOCKET) return;
    std::string body = "name=" + urlEncode(name) + "&shift=" + std::to_string(shift) + "&message=" + urlEncode(msg);
    std::string req =
        "POST " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        body;
    send(sock, req.c_str(), (int)req.size(), 0);
    char tmp[256];
    while (recv(sock, tmp, sizeof(tmp), 0) > 0) {}
    closesocket(sock);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    std::locale::global(std::locale(""));
    std::cout << "AnonChat Online Secure\n\nВведите nickname: ";
    std::wstring wnick = safeGetline();
    std::cout << "Введите сдвиг (ключ шифрования): ";
    int shift = safeGetInt();
    std::wstring encNickW = shiftText(wnick, shift);
    std::string nickname = wstringToUtf8(encNickW);
    std::string serverIp = "37.252.21.108";
    std::string port = "8000";
    const std::string path = "/api/server/chats";
    std::atomic<bool> running{ true };
    std::thread reader(readEvents, serverIp, port, path, nickname, shift, std::ref(running));
    while (running) {
        std::cout << "<" << wstringToUtf8(wnick) << ">: ";
        std::wstring wmsg = safeGetline();
        if (wmsg == L"/quit") { running = false; break; }
        if (wmsg.empty()) continue;
        std::wstring encMsgW = shiftText(wmsg, shift);
        std::string msg = wstringToUtf8(encMsgW);
        postMessage(serverIp, port, path, nickname, shift, msg);
    }
    reader.join();
    WSACleanup();
    return 0;
}
