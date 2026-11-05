#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vr::json {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool lit(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }
    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned low;
                        if (!hex4(low)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool value(Value& out) {
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = std::move(str);
            return true;
        }
        if (c == '{' || c == '[') {
            out = nullptr;
            return skipContainer();
        }
        if (lit("true")) { out = true; return true; }
        if (lit("false")) { out = false; return true; }
        if (lit("null")) { out = nullptr; return true; }
        const char* begin = s.c_str() + i;
        char* end = nullptr;
        const double d = std::strtod(begin, &end);
        if (end == begin) return false;
        i += size_t(end - begin);
        out = d;
        return true;
    }
    bool skipContainer() {
        const char open = s[i++];
        const char close = open == '{' ? '}' : ']';
        ws();
        if (i < s.size() && s[i] == close) {
            ++i;
            return true;
        }
        for (;;) {
            if (open == '{') {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (i >= s.size() || s[i++] != ':') return false;
            }
            Value ignored;
            if (!value(ignored)) return false;
            ws();
            if (i >= s.size()) return false;
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == close) { ++i; return true; }
            return false;
        }
    }
};

void writeString(std::string& out, const std::string& str) {
    out += '"';
    for (const char ch : str) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += ch;
    }
    out += '"';
}

}  // namespace

std::optional<Object> parseObject(const std::string& text) {
    Parser p{text};
    p.ws();
    if (p.i >= text.size() || text[p.i] != '{') return std::nullopt;
    ++p.i;
    Object object;
    p.ws();
    if (p.i < text.size() && text[p.i] == '}') return object;
    for (;;) {
        p.ws();
        std::string key;
        if (!p.string(key)) return std::nullopt;
        p.ws();
        if (p.i >= text.size() || text[p.i++] != ':') return std::nullopt;
        Value v;
        if (!p.value(v)) return std::nullopt;
        object[key] = std::move(v);
        p.ws();
        if (p.i >= text.size()) return std::nullopt;
        if (text[p.i] == ',') { ++p.i; continue; }
        if (text[p.i] == '}') return object;
        return std::nullopt;
    }
}

std::string write(const Object& object) {
    std::string out = "{\n";
    size_t n = 0;
    for (const auto& [key, value] : object) {
        out += "  ";
        writeString(out, key);
        out += " : ";
        if (std::holds_alternative<std::nullptr_t>(value)) out += "null";
        else if (const bool* b = std::get_if<bool>(&value)) out += *b ? "true" : "false";
        else if (const double* d = std::get_if<double>(&value)) {
            char buf[64];
            if (std::isfinite(*d) && *d == std::floor(*d) && std::fabs(*d) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", *d);
            else std::snprintf(buf, sizeof buf, "%.17g", std::isfinite(*d) ? *d : 0.0);
            out += buf;
        } else writeString(out, std::get<std::string>(value));
        if (++n < object.size()) out += ",";
        out += "\n";
    }
    out += "}\n";
    return out;
}

}  // namespace vr::json
