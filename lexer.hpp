#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace rexon_compiler {
struct Diagnostic {
    fs::path file;
    std::size_t line = 1;
    std::size_t column = 1;
    std::string message;
};

enum class Kind {
    Identifier, Number, String, Character, Punct, End
};

struct Token {
    Kind kind{};
    std::string text;
    std::size_t offset{};
    std::size_t line{1};
    std::size_t column{1};
};

class Lexer {
    std::string_view src;
    fs::path file;
    std::size_t i = 0, line = 1, col = 1;
    std::vector<Diagnostic>& errors;

    char peek(std::size_t n = 0) const {
        return i + n < src.size() ? src[i + n] : '\0';
    }
    char take() {
        char c = peek();
        if (i < src.size()) ++i;
        if (c == '\n') { ++line; col = 1; }
        else ++col;
        return c;
    }
    void error(std::string msg, std::size_t l, std::size_t c) {
        errors.push_back({file, l, c, std::move(msg)});
    }
    static bool ident_start(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }
    static bool ident_char(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }
    void skip_space() {
        for (;;) {
            while (std::isspace(static_cast<unsigned char>(peek()))) take();
            if (peek() == '/' && peek(1) == '/') {
                while (peek() && peek() != '\n') take();
            } else if (peek() == '/' && peek(1) == '*') {
                auto l = line, c = col;
                take(); take();
                while (peek() && !(peek() == '*' && peek(1) == '/')) take();
                if (!peek()) error("unterminated block comment", l, c);
                else { take(); take(); }
            } else break;
        }
    }
    Token string_token(char quote, std::size_t start, std::size_t l, std::size_t c) {
        std::string value;
        take();
        bool closed = false;
        while (peek()) {
            char ch = take();
            if (ch == quote) { closed = true; break; }
            if (ch == '\\') {
                char e = take();
                switch (e) {
                    case 'n': value += '\n'; break;
                    case 'r': value += '\r'; break;
                    case 't': value += '\t'; break;
                    case '0': value += '\0'; break;
                    case '\\': value += '\\'; break;
                    case '"': value += '"'; break;
                    case '\'': value += '\''; break;
                    case '\n': break;
                    default: value += '\\'; value += e; break;
                }
            } else value += ch;
        }
        if (!closed) error("unterminated string/character literal", l, c);
        return {quote == '"' ? Kind::String : Kind::Character, std::move(value), start, l, c};
    }
    Token multiline_string(std::size_t start, std::size_t l, std::size_t c) {
        // A line whose first non-indentation character is '\' contributes one line.
        std::string value;
        bool first = true;
        while (i < src.size()) {
            std::size_t p = i;
            while (peek() == ' ' || peek() == '\t') take();
            if (peek() != '\\') { i = p; col -= (i - p); break; }
            take();
            if (!first) value += '\n';
            first = false;
            while (peek() && peek() != '\n') {
                char ch = take();
                if (ch == '\\' && peek() == '\\') { take(); value += '\\'; }
                else value += ch;
            }
            if (peek() == '\n') take();
            // Stop when the next line is not another backslash line.
            std::size_t q = i;
            while (peek() == ' ' || peek() == '\t') take();
            bool next = peek() == '\\';
            i = q;
            // Recompute column from source position on the current line.
            col = 1;
            for (std::size_t k = i; k > 0 && src[k - 1] != '\n'; --k) ++col;
            if (!next) break;
        }
        return {Kind::String, std::move(value), start, l, c};
    }

public:
    Lexer(std::string_view s, fs::path f, std::vector<Diagnostic>& e)
        : src(s), file(std::move(f)), errors(e) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            skip_space();
            auto start = i, l = line, c = col;
            char ch = peek();
            if (!ch) {
                out.push_back({Kind::End, {}, i, line, col});
                break;
            }
            if (ident_start(ch)) {
                std::string s;
                while (ident_char(peek())) s += take();
                out.push_back({Kind::Identifier, std::move(s), start, l, c});
            } else if (std::isdigit(static_cast<unsigned char>(ch))) {
                std::string s;
                while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '.' || peek() == '_')
                    s += take();
                out.push_back({Kind::Number, std::move(s), start, l, c});
            } else if (ch == '"' || ch == '\'') {
                out.push_back(string_token(ch, start, l, c));
            } else if (ch == '\\') {
                // Multiline strings begin with a backslash at the start of a source line.
                bool only_indent = true;
                for (std::size_t k = start; k > 0 && src[k - 1] != '\n'; --k)
                    if (src[k - 1] != ' ' && src[k - 1] != '\t') only_indent = false;
                if (only_indent) out.push_back(multiline_string(start, l, c));
                else {
                    take();
                    error("unexpected backslash", l, c);
                    out.push_back({Kind::Punct, "\\", start, l, c});
                }
            } else {
                static constexpr std::string_view ops[] = {
                    "::", "->", "==", "!=", "<=", ">=", "&&", "||",
                    "+=", "-=", "*=", "/=", "%=", "++", "--", "...",
                    "<<", ">>"
                };
                std::string op(1, take());
                for (auto candidate : ops) {
                    if (candidate.size() > op.size() && candidate.substr(0, op.size()) == op &&
                        src.substr(start, candidate.size()) == candidate) {
                        while (op.size() < candidate.size()) op += take();
                        break;
                    }
                }
                out.push_back({Kind::Punct, std::move(op), start, l, c});
            }
        }
        return out;
    }
};


} // namespace rexon_compiler

