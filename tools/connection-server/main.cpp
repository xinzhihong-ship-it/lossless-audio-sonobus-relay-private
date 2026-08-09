#include "aoo/aoo_net.h"

#include <algorithm>
#include <arpa/inet.h>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
aoonet_server *g_server = nullptr;
std::string g_admin_token;

int env_int(const char *name, int fallback)
{
    const char *value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    return std::atoi(value);
}

std::string env_string(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

bool secure_equal(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    return difference == 0;
}

bool authorized(const std::string& header)
{
    if (g_admin_token.empty()) return false;
    std::istringstream lines(header);
    std::string line;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        auto name = line.substr(0, colon);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name != "authorization") continue;
        auto value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        return secure_equal(value, "Bearer " + g_admin_token);
    }
    return false;
}

std::string json_escape(const std::string& value)
{
    std::ostringstream os;
    for (char c : value) {
        if (c == '"' || c == '\\') {
            os << '\\' << c;
        } else if (c == '\n') {
            os << "\\n";
        } else if (c == '\r') {
            os << "\\r";
        } else {
            os << c;
        }
    }
    return os.str();
}

std::string get_string_field(const std::string& body, const std::string& name)
{
    auto key = "\"" + name + "\"";
    auto key_pos = body.find(key);
    if (key_pos == std::string::npos) return "";
    auto colon = body.find(':', key_pos + key.size());
    if (colon == std::string::npos) return "";
    auto start = body.find('"', colon + 1);
    if (start == std::string::npos) return "";
    std::string out;
    bool escaping = false;
    for (size_t i = start + 1; i < body.size(); ++i) {
        char c = body[i];
        if (escaping) {
            out.push_back(c);
            escaping = false;
        } else if (c == '\\') {
            escaping = true;
        } else if (c == '"') {
            return out;
        } else {
            out.push_back(c);
        }
    }
    return "";
}

int get_int_field(const std::string& body, const std::string& name, int fallback)
{
    auto key = "\"" + name + "\"";
    auto key_pos = body.find(key);
    if (key_pos == std::string::npos) return fallback;
    auto colon = body.find(':', key_pos + key.size());
    if (colon == std::string::npos) return fallback;
    return std::atoi(body.c_str() + colon + 1);
}

std::string get_json(int (*fn)(aoonet_server *, char *, int))
{
    std::string buffer(64 * 1024, '\0');
    int n = fn(g_server, buffer.data(), (int)buffer.size());
    if (n < 0) {
        return "{\"error\":\"connection server admin failed\"}";
    }
    return std::string(buffer.data(), (size_t)n);
}

void write_response(int client, int status, const std::string& body)
{
    const char *status_text = status == 200 ? "OK" : "Error";
    std::ostringstream response;
    response << "HTTP/1.1 " << status << " " << status_text << "\r\n"
             << "Content-Type: application/json; charset=utf-8\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    auto text = response.str();
    send(client, text.data(), text.size(), 0);
}

void handle_client(int client)
{
    std::string request;
    char chunk[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
        auto n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) return;
        request.append(chunk, (size_t)n);
        if (request.size() > 1024 * 1024) return;
    }

    auto header_end = request.find("\r\n\r\n");
    std::string header = request.substr(0, header_end);
    std::string body = request.substr(header_end + 4);
    size_t content_length = 0;
    auto length_pos = header.find("Content-Length:");
    if (length_pos != std::string::npos) {
        content_length = (size_t)std::atoi(header.c_str() + length_pos + 15);
    }
    while (body.size() < content_length) {
        auto n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) return;
        body.append(chunk, (size_t)n);
    }

    auto first_line_end = header.find("\r\n");
    auto first_line = header.substr(0, first_line_end);
    std::istringstream line(first_line);
    std::string method;
    std::string path;
    line >> method >> path;

    if (method == "GET" && path == "/health") {
        write_response(client, 200, "{\"ok\":true}");
    } else if (!authorized(header)) {
        write_response(client, 401, "{\"error\":\"unauthorized\"}");
    } else if (method == "GET" && path == "/connections") {
        write_response(client, 200, get_json(aoonet_server_get_state_json));
    } else if (method == "POST" && path == "/video/enrollment-secret") {
        std::string buffer(65, '\0');
        auto n = aoonet_server_get_video_enrollment_secret(
            g_server,
            get_string_field(body, "group").c_str(),
            get_string_field(body, "user").c_str(),
            get_string_field(body, "address").c_str(),
            buffer.data(),
            (int)buffer.size());
        if (n > 0) write_response(client, 200, "{\"secret\":\"" + std::string(buffer.data(), (size_t)n) + "\"}");
        else write_response(client, 404, "{\"error\":\"active enrollment session not found\"}");
    } else if (method == "POST" && path == "/connections/kick") {
        auto kicked = aoonet_server_kick(g_server, get_string_field(body, "group").c_str(), get_string_field(body, "user").c_str(), get_string_field(body, "address").c_str());
        write_response(client, 200, "{\"kicked\":" + std::to_string(kicked) + "}");
    } else if (method == "POST" && path == "/bans") {
        std::string buffer(4096, '\0');
        auto n = aoonet_server_ban(
            g_server,
            get_string_field(body, "group").c_str(),
            get_string_field(body, "user").c_str(),
            get_string_field(body, "address").c_str(),
            get_int_field(body, "ttlSeconds", 3600),
            buffer.data(),
            (int)buffer.size());
        write_response(client, 200, std::string(buffer.data(), (size_t)n));
    } else if (method == "GET" && path == "/bans") {
        write_response(client, 200, get_json(aoonet_server_get_bans_json));
    } else if (method == "POST" && path == "/bans/remove") {
        auto removed = aoonet_server_unban(g_server, get_string_field(body, "id").c_str(), get_string_field(body, "group").c_str(), get_string_field(body, "user").c_str(), get_string_field(body, "address").c_str());
        write_response(client, 200, "{\"removed\":" + std::to_string(removed) + "}");
    } else {
        write_response(client, 404, "{\"error\":\"not found\"}");
    }
}

void admin_thread(int port, const std::string& bind_address)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1
        || bind(server, (sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 16) < 0) {
        std::cerr << "admin http listen failed on " << bind_address << ":" << port << "\n";
        close(server);
        return;
    }

    while (true) {
        int client = accept(server, nullptr, nullptr);
        if (client >= 0) {
            handle_client(client);
            close(client);
        }
    }
}

void signal_handler(int)
{
    if (g_server) {
        aoonet_server_quit(g_server);
    }
}
}

int main()
{
    int port = env_int("CONNECTION_SERVER_PORT", 10998);
    int admin_port = env_int("CONNECTION_SERVER_ADMIN_PORT", 18098);
    const auto admin_bind = env_string("CONNECTION_SERVER_ADMIN_BIND", "127.0.0.1");
    const char *admin_token = std::getenv("CONNECTION_SERVER_ADMIN_TOKEN");
    if (!admin_token || std::strlen(admin_token) < 32) {
        std::cerr << "CONNECTION_SERVER_ADMIN_TOKEN must contain at least 32 characters\n";
        return 1;
    }
    g_admin_token = admin_token;

    int err = 0;
    g_server = aoonet_server_new(port, &err);
    if (!g_server) {
        std::cerr << "failed to start SonoBus connection server on :" << port << " error=" << err << "\n";
        return 1;
    }

    std::signal(SIGTERM, signal_handler);
    std::signal(SIGINT, signal_handler);

    std::thread admin(admin_thread, admin_port, admin_bind);
    admin.detach();

    std::cout << "SonoBus connection server listening on :" << port << ", admin " << admin_bind << ":" << admin_port << "\n";
    aoonet_server_run(g_server);
    aoonet_server_free(g_server);
    g_server = nullptr;
    return 0;
}
