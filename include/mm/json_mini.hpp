#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mm {

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type{Type::Null};
    bool b{false};
    double n{0.0};
    std::string s;
    std::vector<Json> a;
    std::map<std::string, Json> o;

    const Json* find(const std::string& key) const;
    const Json& at(const std::string& key) const;
    const Json& at(std::size_t index) const;
    std::string str() const;
    double num() const;
    std::int64_t i64() const;
    bool boolean() const;
};

Json parse_json(const std::string& text);

struct ApiError {
    int http_status{0};
    int code{0};
    std::string message;
};

// Binance error bodies look like {"code":-1121,"msg":"..."}.
// Returns nullopt when the body is not that shape.
bool parse_api_error(int http_status, const std::string& body, ApiError& out);

}  // namespace mm
