#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <map>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using SOCKET = int;
const SOCKET INVALID_SOCKET = -1;

struct Subscriber { SOCKET sock; int shift; };

std::vector<Subscriber> subscribers;
std::mutex subsMutex;

std::string urlDecode(const std::string& s) {
    std::string out;
    unsigned int ii;
    char ch;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '%') {
            std::sscanf(s.substr(i + 1, 2).c_str(), "%2x", &ii);
            ch = static_cast<char>(ii);
            out += ch;
            i += 2;
        }
        else if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> m;
    std::istringstream iss(q);
    std::string part;
    while (std::getline(iss, part, '&')) {
        auto pos = part.find('=');
        if (pos != std::string::npos) {
            m[part.substr(0, pos)] = urlDecode(part.substr(pos + 1));
        }
    }
    return m;
}

void broadcast(int shift, const std::string& name, const std::string& msg) {
    std::lock_guard<std::mutex> lock(subsMutex);
    std::string data = "data: " + name + " --> " + msg + "\n\n";
    for (auto& s : subscribers) {
        if (s.shift == shift) send(s.sock, data.c_str(), data.size(), 0);
    }
}

void handleClient(SOCKET client) {
    sockaddr_in addr;
    socklen_t addrLen = sizeof(addr);
    getpeername(client, reinterpret_cast<sockaddr*>(&addr), &addrLen);

    char buf[4096];
    int len = recv(client, buf, sizeof(buf) - 1, 0);
    if (len <= 0) { close(client); return; }
    buf[len] = '\0';

    std::istringstream ss(buf);
    std::string method, path, ver;
    ss >> method >> path >> ver;

    if (method == "GET") {
        auto pos = path.find('?');
        std::string query = pos == std::string::npos ? "" : path.substr(pos + 1);
        int shift = std::stoi(parseQuery(query)["shift"]);
        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: keep-alive\r\n\r\n";
        send(client, header.c_str(), header.size(), 0);
        {
            std::lock_guard<std::mutex> lock(subsMutex);
            subscribers.push_back({ client, shift });
        }
        while (recv(client, buf, 1, MSG_PEEK) > 0) std::this_thread::sleep_for(std::chrono::seconds(1));
        {
            std::lock_guard<std::mutex> lock(subsMutex);
            subscribers.erase(std::remove_if(subscribers.begin(), subscribers.end(),
                [&](const Subscriber& s) { return s.sock == client; }), subscribers.end());
        }
        close(client);
    }
    else if (method == "POST") {
        auto pos = std::string(buf).find("\r\n\r\n");
        if (pos != std::string::npos) {
            auto params = parseQuery(std::string(buf).substr(pos + 4));
            int shift = std::stoi(params["shift"]);
            std::string name = params["name"];
            std::string msg = params["message"];
            broadcast(shift, name, msg);
        }
        std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        send(client, resp.c_str(), resp.size(), 0);
        close(client);
    }
    else {
        close(client);
    }
}

int main() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) return 1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8000);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return 1;
    if (listen(listener, SOMAXCONN) < 0) return 1;
    while (true) {
        sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        SOCKET client = accept(listener, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (client != INVALID_SOCKET) std::thread(handleClient, client).detach();
    }
    close(listener);
    return 0;
}