#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mm {

struct HttpResponse {
    int status{0};
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error;
    bool completed{false};
};

struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeout_ms{15000};
    int max_redirects{5};
    std::size_t max_body_bytes{256ull * 1024ull * 1024ull};
};

// HTTPS only. Bounded connect and read timeouts. Does not disable certificate checks.
class HttpClient {
public:
    HttpResponse request(const HttpRequest& request) const;
    HttpResponse get(const std::string& url, int timeout_ms = 15000) const;
};

struct WsMessage {
    enum class Type { Text, Binary, Ping, Pong, Close, Timeout } type{Type::Timeout};
    std::string data;
    int close_code{0};
};

class WsClient {
public:
    WsClient();
    ~WsClient();
    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    void connect(const std::string& url, int timeout_ms);
    void close();
    bool connected() const { return connected_; }
    // false only when the socket has closed. Timeout returns true with Type::Timeout.
    bool read(WsMessage& out, int timeout_ms);
    void send_text(const std::string& text);

private:
    struct Impl;
    Impl* impl_{nullptr};
    bool connected_{false};
};

std::string sha256_hex_bytes(const std::uint8_t* data, std::size_t size);
std::string sha256_hex_file(const std::string& path);
std::string hmac_sha256_hex(const std::string& key, const std::string& data);

std::string lower_ascii(std::string value);
std::string hex_encode(const unsigned char* data, std::size_t size);

}  // namespace mm
