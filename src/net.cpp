#include "mm/net.hpp"

#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mm {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
inline int last_net_error() { return WSAGetLastError(); }
inline void close_socket(Socket s) { closesocket(s); }
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
};
WinsockInit g_winsock;
#else
using Socket = int;
constexpr Socket kInvalid = -1;
inline int last_net_error() { return errno; }
inline void close_socket(Socket s) { ::close(s); }
#endif

struct Url {
    std::string host;
    std::string path;
    int port{443};
    bool tls{true};
};

Url parse_url(const std::string& url, bool allow_ws) {
    Url out;
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        rest = url.substr(8);
        out.tls = true;
        out.port = 443;
    } else if (allow_ws && url.rfind("wss://", 0) == 0) {
        rest = url.substr(6);
        out.tls = true;
        out.port = 443;
    } else {
        throw std::runtime_error("only https and wss URLs are allowed: " + url);
    }
    const auto slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (hostport.empty() || hostport.find('@') != std::string::npos)
        throw std::runtime_error("refusing URL with empty or userinfo host");
    const auto colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(':') == colon) {
        out.host = hostport.substr(0, colon);
        out.port = std::stoi(hostport.substr(colon + 1));
    } else {
        out.host = hostport;
    }
    if (out.host.empty())
        throw std::runtime_error("URL host is empty");
    return out;
}

const char* ca_file() {
    if (const char* env = std::getenv("SSL_CERT_FILE")) {
        if (*env != '\0')
            return env;
    }
#ifdef _WIN32
    // Default for the MSYS2 UCRT64 toolchain used to build this project.
    return "C:/msys64/ucrt64/etc/ssl/cert.pem";
#else
    return "/etc/ssl/certs/ca-certificates.crt";
#endif
}

Socket connect_tcp(const std::string& host, int port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0)
        throw std::runtime_error("DNS lookup failed for " + host);
    Socket sock = kInvalid;
    std::string last = "connect failed";
    for (addrinfo* it = result; it; it = it->ai_next) {
        sock = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == kInvalid)
            continue;
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        const int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
        const int rc = ::connect(sock, it->ai_addr, static_cast<int>(it->ai_addrlen));
#ifdef _WIN32
        const bool in_progress = rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool in_progress = rc != 0 && (errno == EINPROGRESS || errno == EINTR);
#endif
        if (rc != 0 && !in_progress) {
            last = "connect rejected";
            close_socket(sock);
            sock = kInvalid;
            continue;
        }
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int selected = select(static_cast<int>(sock) + 1, nullptr, &write_set, nullptr, &tv);
        int so_error = 0;
#ifdef _WIN32
        int len = sizeof(so_error);
#else
        socklen_t len = sizeof(so_error);
#endif
        getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
        if (selected <= 0 || so_error != 0) {
            last = selected <= 0 ? "connect timed out" : "connect failed";
            close_socket(sock);
            sock = kInvalid;
            continue;
        }
#ifdef _WIN32
        mode = 0;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        fcntl(sock, F_SETFL, flags);
#endif
        break;
    }
    freeaddrinfo(result);
    if (sock == kInvalid)
        throw std::runtime_error(last + " for " + host);
    return sock;
}

void set_timeouts(Socket sock, int timeout_ms) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(std::max(timeout_ms, 1));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

struct Tls {
    Socket sock{kInvalid};
    SSL_CTX* ctx{nullptr};
    SSL* ssl{nullptr};

    ~Tls() { close(); }

    void close() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        if (sock != kInvalid) {
            close_socket(sock);
            sock = kInvalid;
        }
    }

    void connect(const std::string& host, int port, int timeout_ms) {
        static bool once = false;
        if (!once) {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_all_algorithms();
            once = true;
        }
        sock = connect_tcp(host, port, timeout_ms);
        set_timeouts(sock, timeout_ms);
        ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx)
            throw std::runtime_error("SSL_CTX_new failed");
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        if (SSL_CTX_load_verify_locations(ctx, ca_file(), nullptr) != 1)
            throw std::runtime_error(std::string("failed to load CA bundle ") + ca_file());
        ssl = SSL_new(ctx);
        SSL_set_tlsext_host_name(ssl, host.c_str());
        SSL_set1_host(ssl, host.c_str());
        SSL_set_fd(ssl, static_cast<int>(sock));
        if (SSL_connect(ssl) != 1)
            throw std::runtime_error("TLS handshake failed for " + host);
    }

    void write_all(const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int n = SSL_write(ssl, data.data() + sent, static_cast<int>(data.size() - sent));
            if (n <= 0)
                throw std::runtime_error("TLS write failed");
            sent += static_cast<std::size_t>(n);
        }
    }

    // 1 = bytes, 0 = timeout, -1 = closed/error
    int read_some(char* buffer, int capacity) {
        const int n = SSL_read(ssl, buffer, capacity);
        if (n > 0)
            return n;
        const int err = SSL_get_error(ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN)
            return -1;
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            return 0;
#ifdef _WIN32
        if (err == SSL_ERROR_SYSCALL && (WSAGetLastError() == WSAETIMEDOUT || WSAGetLastError() == WSAEWOULDBLOCK))
            return 0;
#else
        if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT))
            return 0;
#endif
        return -1;
    }
};

std::string header_get(const std::map<std::string, std::string>& headers, const std::string& key) {
    const auto it = headers.find(lower_ascii(key));
    return it == headers.end() ? std::string{} : it->second;
}

HttpResponse read_http(Tls& tls, std::size_t max_body) {
    std::string raw;
    char buffer[8192];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const int n = tls.read_some(buffer, sizeof(buffer));
        if (n == 0)
            throw std::runtime_error("HTTP header read timed out");
        if (n < 0)
            throw std::runtime_error("HTTP connection closed before headers");
        raw.append(buffer, buffer + n);
        if (raw.size() > 1024 * 1024)
            throw std::runtime_error("HTTP headers exceeded 1MB");
    }
    const auto split = raw.find("\r\n\r\n");
    const std::string head = raw.substr(0, split);
    std::string body = raw.substr(split + 4);
    std::istringstream lines(head);
    std::string status_line;
    std::getline(lines, status_line);
    if (!status_line.empty() && status_line.back() == '\r')
        status_line.pop_back();
    HttpResponse response;
    {
        std::istringstream status(status_line);
        std::string http;
        status >> http >> response.status;
        if (response.status <= 0)
            throw std::runtime_error("malformed HTTP status line");
    }
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = lower_ascii(line.substr(0, colon));
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        response.headers[key] = value;
    }
    const std::string chunked = lower_ascii(header_get(response.headers, "transfer-encoding"));
    if (chunked.find("chunked") != std::string::npos) {
        std::string assembled;
        std::string pending = std::move(body);
        while (true) {
            auto nl = pending.find("\r\n");
            while (nl == std::string::npos) {
                const int n = tls.read_some(buffer, sizeof(buffer));
                if (n <= 0)
                    throw std::runtime_error("truncated chunked HTTP body");
                pending.append(buffer, buffer + n);
                nl = pending.find("\r\n");
            }
            const int size = std::stoi(pending.substr(0, nl), nullptr, 16);
            pending.erase(0, nl + 2);
            if (size == 0)
                break;
            if (assembled.size() + static_cast<std::size_t>(size) > max_body)
                throw std::runtime_error("HTTP body exceeded cap");
            while (pending.size() < static_cast<std::size_t>(size) + 2) {
                const int n = tls.read_some(buffer, sizeof(buffer));
                if (n <= 0)
                    throw std::runtime_error("truncated chunk payload");
                pending.append(buffer, buffer + n);
            }
            assembled.append(pending, 0, static_cast<std::size_t>(size));
            pending.erase(0, static_cast<std::size_t>(size) + 2);
        }
        response.body = std::move(assembled);
    } else if (response.headers.count("content-length")) {
        const auto length = static_cast<std::size_t>(std::stoull(response.headers["content-length"]));
        if (length > max_body)
            throw std::runtime_error("HTTP body exceeded cap");
        while (body.size() < length) {
            const int n = tls.read_some(buffer, sizeof(buffer));
            if (n == 0)
                throw std::runtime_error("HTTP body read timed out");
            if (n < 0)
                throw std::runtime_error("HTTP connection closed before content-length");
            body.append(buffer, buffer + n);
        }
        body.resize(length);
        response.body = std::move(body);
    } else {
        while (body.size() <= max_body) {
            const int n = tls.read_some(buffer, sizeof(buffer));
            if (n == 0)
                break;
            if (n < 0)
                break;
            body.append(buffer, buffer + n);
        }
        if (body.size() > max_body)
            throw std::runtime_error("HTTP body exceeded cap");
        response.body = std::move(body);
    }
    response.completed = true;
    return response;
}

}  // namespace

std::string base64_encode(const unsigned char* data, std::size_t size) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned int n = (static_cast<unsigned int>(data[i]) << 16) |
                               ((i + 1 < size ? data[i + 1] : 0) << 8) |
                               (i + 2 < size ? data[i + 2] : 0);
        out.push_back(table[(n >> 18) & 63]);
        out.push_back(table[(n >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? table[n & 63] : '=');
    }
    return out;
}

std::string lower_ascii(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string hex_encode(const unsigned char* data, std::size_t size) {
    static const char* hex = "0123456789abcdef";
    std::string out(size * 2, '\0');
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    return out;
}

std::string sha256_hex_bytes(const std::uint8_t* data, std::size_t size) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data, size, md, &len, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("sha256 failed");
    return hex_encode(md, len);
}

std::string sha256_hex_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("unable to read " + path);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("sha256 init failed");
    char buffer[1 << 16];
    while (input) {
        input.read(buffer, sizeof(buffer));
        if (input.gcount() > 0 && EVP_DigestUpdate(ctx, buffer, static_cast<std::size_t>(input.gcount())) != 1)
            throw std::runtime_error("sha256 update failed");
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx, md, &len) != 1)
        throw std::runtime_error("sha256 final failed");
    EVP_MD_CTX_free(ctx);
    return hex_encode(md, len);
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(), md, &len))
        throw std::runtime_error("HMAC failed");
    return hex_encode(md, len);
}

HttpResponse HttpClient::get(const std::string& url, int timeout_ms) const {
    HttpRequest request;
    request.url = url;
    request.timeout_ms = timeout_ms;
    return this->request(request);
}

HttpResponse HttpClient::request(const HttpRequest& request) const {
    if (request.timeout_ms <= 0 || request.timeout_ms > 120000)
        throw std::runtime_error("HTTP timeout must be in (0, 120000] ms");
    std::string url = request.url;
    HttpResponse response;
    for (int hop = 0; hop <= request.max_redirects; ++hop) {
        const Url parsed = parse_url(url, false);
        Tls tls;
        try {
            tls.connect(parsed.host, parsed.port, request.timeout_ms);
        } catch (const std::exception& error) {
            response.error = error.what();
            response.completed = false;
            return response;
        }
        std::ostringstream req;
        req << request.method << ' ' << parsed.path << " HTTP/1.1\r\n";
        req << "Host: " << parsed.host << "\r\n";
        req << "User-Agent: binance-usdm-mm/1.0\r\n";
        req << "Accept: */*\r\n";
        req << "Connection: close\r\n";
        for (const auto& header : request.headers)
            req << header.first << ": " << header.second << "\r\n";
        if (!request.body.empty()) {
            req << "Content-Length: " << request.body.size() << "\r\n";
        }
        req << "\r\n";
        try {
            tls.write_all(req.str());
            if (!request.body.empty())
                tls.write_all(request.body);
            response = read_http(tls, request.max_body_bytes);
        } catch (const std::exception& error) {
            response.error = error.what();
            response.completed = false;
            return response;
        }
        if (response.status == 301 || response.status == 302 || response.status == 307 || response.status == 308) {
            const std::string location = header_get(response.headers, "location");
            if (location.empty())
                return response;
            if (location.rfind("https://", 0) == 0)
                url = location;
            else if (!location.empty() && location.front() == '/')
                url = "https://" + parsed.host + location;
            else {
                response.error = "refusing relative redirect";
                return response;
            }
            continue;
        }
        return response;
    }
    response.error = "too many redirects";
    response.completed = false;
    return response;
}

struct WsClient::Impl {
    Tls tls;
    std::string host;
    std::string leftover;
};

WsClient::WsClient() : impl_(new Impl) {}

WsClient::~WsClient() {
    close();
    delete impl_;
    impl_ = nullptr;
}

void WsClient::connect(const std::string& url, int timeout_ms) {
    close();
    const Url parsed = parse_url(url, true);
    impl_->tls.connect(parsed.host, parsed.port, timeout_ms);
    impl_->host = parsed.host;
    unsigned char key_raw[16];
    if (RAND_bytes(key_raw, sizeof(key_raw)) != 1)
        throw std::runtime_error("RAND_bytes failed");
    const std::string key = base64_encode(key_raw, sizeof(key_raw));
    std::ostringstream req;
    req << "GET " << parsed.path << " HTTP/1.1\r\n";
    req << "Host: " << parsed.host << "\r\n";
    req << "Upgrade: websocket\r\n";
    req << "Connection: Upgrade\r\n";
    req << "Sec-WebSocket-Key: " << key << "\r\n";
    req << "Sec-WebSocket-Version: 13\r\n";
    req << "User-Agent: binance-usdm-mm/1.0\r\n\r\n";
    impl_->tls.write_all(req.str());
    std::string raw;
    char buffer[2048];
    while (raw.find("\r\n\r\n") == std::string::npos) {
        const int n = impl_->tls.read_some(buffer, sizeof(buffer));
        if (n == 0)
            throw std::runtime_error("websocket handshake timed out");
        if (n < 0)
            throw std::runtime_error("websocket handshake closed");
        raw.append(buffer, buffer + n);
        if (raw.size() > 65536)
            throw std::runtime_error("websocket handshake too large");
    }
    const auto split = raw.find("\r\n\r\n");
    const std::string status = raw.substr(0, raw.find("\r\n"));
    if (status.find(" 101 ") == std::string::npos)
        throw std::runtime_error("websocket upgrade rejected: " + status + " body " +
                                 raw.substr(split + 4, 180));
    impl_->leftover = raw.substr(split + 4);
    connected_ = true;
}

void WsClient::close() {
    if (impl_ && connected_) {
        try {
            unsigned char frame[6] = {0x88, 0x80, 0, 0, 0, 0};
            RAND_bytes(frame + 2, 4);
            impl_->tls.write_all(std::string(reinterpret_cast<char*>(frame), 6));
        } catch (...) {
        }
    }
    if (impl_)
        impl_->tls.close();
    connected_ = false;
}

void WsClient::send_text(const std::string& text) {
    if (!connected_)
        throw std::runtime_error("websocket is not connected");
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    const auto len = text.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(0x80 | len));
    } else if (len <= 0xffff) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((len >> 8) & 0xff));
        frame.push_back(static_cast<char>(len & 0xff));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>((static_cast<unsigned long long>(len) >> shift) & 0xff));
    }
    unsigned char mask[4];
    if (RAND_bytes(mask, 4) != 1)
        throw std::runtime_error("RAND_bytes failed");
    frame.append(reinterpret_cast<char*>(mask), 4);
    for (std::size_t i = 0; i < text.size(); ++i)
        frame.push_back(static_cast<char>(text[i] ^ mask[i % 4]));
    impl_->tls.write_all(frame);
}

bool WsClient::read(WsMessage& out, int timeout_ms) {
    if (!connected_)
        return false;
    set_timeouts(impl_->tls.sock, timeout_ms);
    auto pull = [&](std::size_t n) {
        while (impl_->leftover.size() < n) {
            char buffer[8192];
            const int got = impl_->tls.read_some(buffer, sizeof(buffer));
            if (got == 0) {
                out.type = WsMessage::Type::Timeout;
                return false;
            }
            if (got < 0) {
                connected_ = false;
                return false;
            }
            impl_->leftover.append(buffer, buffer + got);
        }
        return true;
    };
    if (!pull(2))
        return connected_;
    const auto* bytes = reinterpret_cast<const unsigned char*>(impl_->leftover.data());
    const int opcode = bytes[0] & 0x0f;
    const bool masked = (bytes[1] & 0x80) != 0;
    std::uint64_t len = bytes[1] & 0x7f;
    std::size_t header = 2;
    if (len == 126) {
        if (!pull(4))
            return connected_;
        bytes = reinterpret_cast<const unsigned char*>(impl_->leftover.data());
        len = (static_cast<std::uint64_t>(bytes[2]) << 8) | bytes[3];
        header = 4;
    } else if (len == 127) {
        if (!pull(10))
            return connected_;
        bytes = reinterpret_cast<const unsigned char*>(impl_->leftover.data());
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | bytes[2 + i];
        header = 10;
    }
    if (len > 8ull * 1024ull * 1024ull)
        throw std::runtime_error("websocket frame too large");
    const std::size_t mask_len = masked ? 4 : 0;
    if (!pull(header + mask_len + static_cast<std::size_t>(len)))
        return connected_;
    bytes = reinterpret_cast<const unsigned char*>(impl_->leftover.data());
    std::string payload(static_cast<std::size_t>(len), '\0');
    const unsigned char* mask = masked ? bytes + header : nullptr;
    const unsigned char* data = bytes + header + mask_len;
    for (std::uint64_t i = 0; i < len; ++i)
        payload[static_cast<std::size_t>(i)] = static_cast<char>(data[i] ^ (mask ? mask[i % 4] : 0));
    impl_->leftover.erase(0, header + mask_len + static_cast<std::size_t>(len));
    if (opcode == 0x9) {
        // Pong the ping and report it so the caller can keep looping.
        std::string pong;
        pong.push_back(static_cast<char>(0x8A));
        pong.push_back(static_cast<char>(0x80 | payload.size()));
        unsigned char pmask[4] = {1, 2, 3, 4};
        pong.append(reinterpret_cast<char*>(pmask), 4);
        for (std::size_t i = 0; i < payload.size(); ++i)
            pong.push_back(static_cast<char>(payload[i] ^ pmask[i % 4]));
        impl_->tls.write_all(pong);
        out.type = WsMessage::Type::Ping;
        out.data = payload;
        return true;
    }
    if (opcode == 0x8) {
        out.type = WsMessage::Type::Close;
        if (payload.size() >= 2)
            out.close_code = (static_cast<unsigned char>(payload[0]) << 8) |
                             static_cast<unsigned char>(payload[1]);
        connected_ = false;
        return false;
    }
    if (opcode == 0x1)
        out.type = WsMessage::Type::Text;
    else if (opcode == 0x2)
        out.type = WsMessage::Type::Binary;
    else if (opcode == 0xA)
        out.type = WsMessage::Type::Pong;
    else
        out.type = WsMessage::Type::Binary;
    out.data = std::move(payload);
    return true;
}

}  // namespace mm
