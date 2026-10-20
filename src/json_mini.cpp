#include "mm/json_mini.hpp"

#include <cctype>
#include <cmath>
#include <utility>

namespace mm {
namespace {

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Json parse() {
        skip();
        Json value = parse_value();
        skip();
        if (index_ != text_.size())
            throw std::runtime_error("trailing data in JSON");
        return value;
    }

private:
    const std::string& text_;
    std::size_t index_{0};

    [[noreturn]] void fail(const char* what) const {
        throw std::runtime_error(std::string("JSON parse error at ") + std::to_string(index_) + ": " + what);
    }

    void skip() {
        while (index_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[index_])))
            ++index_;
    }

    char peek() const {
        if (index_ >= text_.size())
            fail("unexpected end");
        return text_[index_];
    }

    char get() {
        const char c = peek();
        ++index_;
        return c;
    }

    Json parse_value() {
        skip();
        const char c = peek();
        if (c == '{')
            return parse_object();
        if (c == '[')
            return parse_array();
        if (c == '"')
            return Json{Json::Type::String, false, 0.0, parse_string(), {}, {}};
        if (c == 't' || c == 'f')
            return parse_bool();
        if (c == 'n')
            return parse_null();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
            return parse_number();
        fail("unexpected token");
    }

    Json parse_object() {
        get();
        Json value;
        value.type = Json::Type::Object;
        skip();
        if (peek() == '}') {
            get();
            return value;
        }
        while (true) {
            skip();
            if (get() != '"')
                fail("object key must be a string");
            --index_;
            const std::string key = parse_string();
            skip();
            if (get() != ':')
                fail("expected colon");
            value.o.emplace(key, parse_value());
            skip();
            const char sep = get();
            if (sep == '}')
                break;
            if (sep != ',')
                fail("expected comma or end of object");
        }
        return value;
    }

    Json parse_array() {
        get();
        Json value;
        value.type = Json::Type::Array;
        skip();
        if (peek() == ']') {
            get();
            return value;
        }
        while (true) {
            value.a.push_back(parse_value());
            skip();
            const char sep = get();
            if (sep == ']')
                break;
            if (sep != ',')
                fail("expected comma or end of array");
        }
        return value;
    }

    std::string parse_string() {
        if (get() != '"')
            fail("expected string");
        std::string out;
        while (true) {
            if (index_ >= text_.size())
                fail("unterminated string");
            const char c = get();
            if (c == '"')
                break;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (index_ >= text_.size())
                fail("unterminated escape");
            const char e = get();
            switch (e) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(e);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    if (index_ + 4 > text_.size())
                        fail("short unicode escape");
                    int code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = get();
                        code <<= 4;
                        if (h >= '0' && h <= '9')
                            code += h - '0';
                        else if (h >= 'a' && h <= 'f')
                            code += h - 'a' + 10;
                        else if (h >= 'A' && h <= 'F')
                            code += h - 'A' + 10;
                        else
                            fail("bad unicode escape");
                    }
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default:
                    fail("unknown escape");
            }
        }
        return out;
    }

    Json parse_bool() {
        if (text_.compare(index_, 4, "true") == 0) {
            index_ += 4;
            return Json{Json::Type::Bool, true, 0.0, {}, {}, {}};
        }
        if (text_.compare(index_, 5, "false") == 0) {
            index_ += 5;
            return Json{Json::Type::Bool, false, 0.0, {}, {}, {}};
        }
        fail("expected boolean");
    }

    Json parse_null() {
        if (text_.compare(index_, 4, "null") != 0)
            fail("expected null");
        index_ += 4;
        return Json{};
    }

    Json parse_number() {
        const std::size_t start = index_;
        if (peek() == '-')
            get();
        if (!std::isdigit(static_cast<unsigned char>(peek())))
            fail("expected digit");
        while (index_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[index_])))
            ++index_;
        if (index_ < text_.size() && text_[index_] == '.') {
            ++index_;
            if (index_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[index_])))
                fail("expected fraction");
            while (index_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[index_])))
                ++index_;
        }
        if (index_ < text_.size() && (text_[index_] == 'e' || text_[index_] == 'E')) {
            ++index_;
            if (index_ < text_.size() && (text_[index_] == '+' || text_[index_] == '-'))
                ++index_;
            if (index_ >= text_.size() || !std::isdigit(static_cast<unsigned char>(text_[index_])))
                fail("expected exponent");
            while (index_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[index_])))
                ++index_;
        }
        const double n = std::stod(text_.substr(start, index_ - start));
        return Json{Json::Type::Number, false, n, {}, {}, {}};
    }
};

}  // namespace

const Json* Json::find(const std::string& key) const {
    if (type != Type::Object)
        return nullptr;
    const auto it = o.find(key);
    return it == o.end() ? nullptr : &it->second;
}

const Json& Json::at(const std::string& key) const {
    const Json* value = find(key);
    if (!value)
        throw std::runtime_error("missing JSON key " + key);
    return *value;
}

const Json& Json::at(std::size_t index) const {
    if (type != Type::Array || index >= a.size())
        throw std::runtime_error("JSON index out of range");
    return a[index];
}

std::string Json::str() const {
    if (type == Type::String)
        return s;
    if (type == Type::Number) {
        const auto as_int = static_cast<std::int64_t>(n);
        if (static_cast<double>(as_int) == n)
            return std::to_string(as_int);
    }
    throw std::runtime_error("JSON value is not a string");
}

double Json::num() const {
    if (type == Type::Number)
        return n;
    if (type == Type::String)
        return std::stod(s);
    throw std::runtime_error("JSON value is not a number");
}

std::int64_t Json::i64() const {
    const double value = num();
    if (!std::isfinite(value))
        throw std::runtime_error("JSON number is not finite");
    return static_cast<std::int64_t>(std::llround(value));
}

bool Json::boolean() const {
    if (type != Type::Bool)
        throw std::runtime_error("JSON value is not a boolean");
    return b;
}

Json parse_json(const std::string& text) { return Parser(text).parse(); }

bool parse_api_error(int http_status, const std::string& body, ApiError& out) {
    if (http_status < 400)
        return false;
    try {
        const Json json = parse_json(body);
        const Json* code = json.find("code");
        const Json* msg = json.find("msg");
        if (!code || !msg)
            return false;
        out.http_status = http_status;
        out.code = static_cast<int>(code->i64());
        out.message = msg->type == Json::Type::String ? msg->s : msg->str();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace mm
