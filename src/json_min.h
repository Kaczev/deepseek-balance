// json_min.h -- minimal JSON reader for the DeepSeek balance response.
//
// Why hand-written: the project takes no third-party dependencies, and the balance
// endpoint needs only a small subset of JSON. The reader is deliberately bounded:
//
//   * no exceptions -- every failure comes back as Outcome::ok == false plus error text
//   * nesting is capped at kMaxDepth, so a hostile body such as "[[[[[[..." returns an
//     error instead of overflowing the stack
//   * a node budget (kMaxNodes) caps memory for an oversized body
//   * \uXXXX escapes (including surrogate pairs) are decoded to UTF-8
//   * strings keep their bytes verbatim; only control characters are rejected, as JSON
//     requires. Nothing here validates or converts UTF-8.
//
// Numbers keep their verbatim source text: the plan requires decimal handling without
// binary floating point (design 3.1), so this reader never produces a double.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dshb::json {

enum class Kind { Null, Bool, Number, String, Array, Object };

struct Value {
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text;                                   // String (decoded) or Number (verbatim)
    std::vector<Value> items;                           // Array
    std::vector<std::pair<std::string, Value>> members; // Object, source order preserved

    bool IsNull() const { return kind == Kind::Null; }
    bool IsBool() const { return kind == Kind::Bool; }
    bool IsNumber() const { return kind == Kind::Number; }
    bool IsString() const { return kind == Kind::String; }
    bool IsArray() const { return kind == Kind::Array; }
    bool IsObject() const { return kind == Kind::Object; }

    // Object member lookup; nullptr when absent (or when this is not an object).
    const Value* Find(const std::string& key) const;
};

inline constexpr std::size_t kMaxDepth = 64;
inline constexpr std::size_t kMaxNodes = 200000;

struct Outcome {
    bool ok = false;
    Value root;
    std::string error;        // human readable, includes the byte offset
    std::size_t errorAt = 0;  // byte offset of the failure
    std::size_t maxDepth = 0; // deepest nesting actually reached
    std::size_t nodes = 0;    // values produced
};

// Parses one complete JSON value. Trailing non-whitespace is an error, so a truncated
// or concatenated body can never be accepted by accident.
Outcome Parse(const std::string& text);

} // namespace dshb::json
