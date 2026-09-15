// json_min.cpp -- see json_min.h for the contract.
//
// Hand-written recursive descent, no exceptions, bounded depth and node count.
#include "json_min.h"

#include <cstdint>

namespace dshb::json {

const Value* Value::Find(const std::string& key) const {
    // Duplicate keys: the last occurrence wins, matching JSON.parse() semantics.
    const Value* found = nullptr;
    for (const auto& m : members) {
        if (m.first == key) found = &m.second;
    }
    return found;
}

namespace {

constexpr uint32_t kReplacementChar = 0xFFFD;

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Outcome Run() {
        Outcome out;
        SkipBom();
        Value root;
        if (!ParseValue(root, 0)) {
            out.ok = false;
            out.error = error_;
            out.errorAt = errorAt_;
            out.maxDepth = maxDepth_;
            out.nodes = nodes_;
            return out;
        }
        SkipWs();
        if (pos_ != s_.size()) {
            Fail("trailing data after the top-level value");
            out.ok = false;
            out.error = error_;
            out.errorAt = errorAt_;
            out.maxDepth = maxDepth_;
            out.nodes = nodes_;
            return out;
        }
        out.ok = true;
        out.root = std::move(root);
        out.maxDepth = maxDepth_;
        out.nodes = nodes_;
        return out;
    }

private:
    void Fail(const std::string& what) {
        if (failed_) return; // keep the first (deepest) reason
        failed_ = true;
        error_ = what + " at offset " + std::to_string(pos_);
        errorAt_ = pos_;
    }

    bool AtEnd() const { return pos_ >= s_.size(); }
    char Peek() const { return AtEnd() ? '\0' : s_[pos_]; }

    void SkipBom() {
        if (s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
            static_cast<unsigned char>(s_[1]) == 0xBB &&
            static_cast<unsigned char>(s_[2]) == 0xBF) {
            pos_ = 3;
        }
    }

    void SkipWs() {
        while (!AtEnd()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool CountNode() {
        if (++nodes_ > kMaxNodes) {
            Fail("node budget exceeded (" + std::to_string(kMaxNodes) + ")");
            return false;
        }
        return true;
    }

    bool EnterDepth(std::size_t depth) {
        if (depth > maxDepth_) maxDepth_ = depth;
        if (depth > kMaxDepth) {
            Fail("nesting deeper than " + std::to_string(kMaxDepth));
            return false;
        }
        return true;
    }

    bool ParseValue(Value& out, std::size_t depth) {
        if (failed_) return false;
        if (!CountNode()) return false;
        SkipWs();
        if (AtEnd()) {
            Fail("unexpected end of input, expected a value");
            return false;
        }
        switch (Peek()) {
        case '{': return ParseObject(out, depth);
        case '[': return ParseArray(out, depth);
        case '"': {
            out.kind = Kind::String;
            return ParseString(out.text);
        }
        case 't': return ParseLiteral("true", out, true);
        case 'f': return ParseLiteral("false", out, false);
        case 'n': return ParseLiteral("null", out, false);
        default: return ParseNumber(out);
        }
    }

    bool ParseLiteral(const char* word, Value& out, bool boolean) {
        const std::size_t n = std::char_traits<char>::length(word);
        if (s_.compare(pos_, n, word) != 0) {
            Fail("invalid literal");
            return false;
        }
        pos_ += n;
        out.kind = boolean ? Kind::Bool : Kind::Null;
        out.boolean = boolean;
        return true;
    }

    bool ParseObject(Value& out, std::size_t depth) {
        if (!EnterDepth(depth + 1)) return false;
        out.kind = Kind::Object;
        ++pos_; // '{'
        SkipWs();
        if (Peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            SkipWs();
            if (Peek() != '"') {
                Fail("expected a quoted object key");
                return false;
            }
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (Peek() != ':') {
                Fail("expected ':' after an object key");
                return false;
            }
            ++pos_;
            Value member;
            if (!ParseValue(member, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(member));
            SkipWs();
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == '}') {
                ++pos_;
                return true;
            }
            Fail("expected ',' or '}' in an object");
            return false;
        }
    }

    bool ParseArray(Value& out, std::size_t depth) {
        if (!EnterDepth(depth + 1)) return false;
        out.kind = Kind::Array;
        ++pos_; // '['
        SkipWs();
        if (Peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            Value item;
            if (!ParseValue(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            SkipWs();
            if (Peek() == ',') {
                ++pos_;
                continue;
            }
            if (Peek() == ']') {
                ++pos_;
                return true;
            }
            Fail("expected ',' or ']' in an array");
            return false;
        }
    }

    bool ParseHex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) {
            Fail("truncated \\u escape");
            return false;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s_[pos_ + static_cast<std::size_t>(i)];
            uint32_t d = 0;
            if (c >= '0' && c <= '9') {
                d = static_cast<uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                d = static_cast<uint32_t>(c - 'a') + 10;
            } else if (c >= 'A' && c <= 'F') {
                d = static_cast<uint32_t>(c - 'A') + 10;
            } else {
                Fail("invalid hex digit in a \\u escape");
                return false;
            }
            v = (v << 4) | d;
        }
        pos_ += 4;
        out = v;
        return true;
    }

    bool ParseString(std::string& out) {
        out.clear();
        ++pos_; // opening quote
        for (;;) {
            if (AtEnd()) {
                Fail("unterminated string");
                return false;
            }
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) {
                Fail("unescaped control character in a string");
                return false;
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                ++pos_;
                continue;
            }
            ++pos_; // backslash
            if (AtEnd()) {
                Fail("unterminated escape sequence");
                return false;
            }
            const char e = s_[pos_++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!ParseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    // High surrogate: try to pair it with a following low surrogate.
                    if (pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        const std::size_t save = pos_;
                        pos_ += 2;
                        uint32_t lo = 0;
                        if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            pos_ = save; // not a valid pair; keep the escape for the retry
                            cp = kReplacementChar;
                        }
                    } else {
                        cp = kReplacementChar; // lone surrogate
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = kReplacementChar; // lone low surrogate
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                Fail("unknown string escape");
                return false;
            }
        }
    }

    bool ParseNumber(Value& out) {
        const std::size_t start = pos_;
        if (Peek() == '-') ++pos_;
        if (AtEnd() || Peek() < '0' || Peek() > '9') {
            Fail("invalid number");
            return false;
        }
        if (Peek() == '0') {
            ++pos_;
        } else {
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        if (Peek() == '.') {
            ++pos_;
            if (AtEnd() || Peek() < '0' || Peek() > '9') {
                Fail("invalid fraction in a number");
                return false;
            }
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        if (Peek() == 'e' || Peek() == 'E') {
            ++pos_;
            if (Peek() == '+' || Peek() == '-') ++pos_;
            if (AtEnd() || Peek() < '0' || Peek() > '9') {
                Fail("invalid exponent in a number");
                return false;
            }
            while (!AtEnd() && Peek() >= '0' && Peek() <= '9') ++pos_;
        }
        out.kind = Kind::Number;
        out.text = s_.substr(start, pos_ - start);
        return true;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
    bool failed_ = false;
    std::string error_;
    std::size_t errorAt_ = 0;
    std::size_t maxDepth_ = 0;
    std::size_t nodes_ = 0;
};

} // namespace

Outcome Parse(const std::string& text) {
    Parser p(text);
    return p.Run();
}

} // namespace dshb::json
