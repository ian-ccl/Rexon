#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <iostream>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

static constexpr std::string_view std_hpp = R"REXON(
#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace rexon {
    inline bool test_mode = false;
    template<class T>
    struct Slice {
        T* data{};
        std::size_t size{};
        T& operator[](std::size_t i) const { return data[i]; }
        T* begin() const { return data; }
        T* end() const { return data + size; }
    };

    // Dynamic allocations carry a small header immediately before the user
    // storage.  Besides making failure non-throwing, this lets the generated
    // delete operation reject an array passed as an individual pointer (and
    // vice versa) without relying on C++'s delete expression.
    inline constexpr std::uint64_t allocation_magic = 0x5245584f4e414c4Cull;
    enum class AllocationKind : unsigned char { single, array };
    struct AllocationHeader {
        std::uint64_t magic;
        AllocationKind kind;
        std::size_t count;
        void (*destroy)(void*) noexcept;
        AllocationHeader* next;
    };
    inline AllocationHeader* allocation_list = nullptr;

    template<class T>
    inline void destroy_one(void* p) noexcept { static_cast<T*>(p)->~T(); }

    template<class T>
    inline AllocationHeader* allocation_header(T* p) noexcept {
        if (!p) return nullptr;
        for (AllocationHeader* h = allocation_list; h; h = h->next)
            if (h->magic == allocation_magic && reinterpret_cast<T*>(h + 1) == p) return h;
        return nullptr;
    }

    inline void unlink_allocation(AllocationHeader* target) noexcept {
        if (!target) return;
        AllocationHeader** link = &allocation_list;
        while (*link && *link != target) link = &(*link)->next;
        if (*link == target) *link = target->next;
    }

    template<class T>
    inline T* allocate_storage(bool* ok, std::size_t count,
                               AllocationKind kind) noexcept {
        if (ok) *ok = false;
        if (count == 0 || count > (std::numeric_limits<std::size_t>::max() -
                                   sizeof(AllocationHeader)) / sizeof(T) ||
            alignof(T) > alignof(std::max_align_t)) return nullptr;
        void* raw = std::malloc(sizeof(AllocationHeader) + sizeof(T) * count);
        if (!raw) return nullptr;
        auto* h = static_cast<AllocationHeader*>(raw);
        h->magic = allocation_magic;
        h->kind = kind;
        h->count = count;
        h->destroy = &destroy_one<T>;
        h->next = allocation_list;
        allocation_list = h;
        if (ok) *ok = true;
        return reinterpret_cast<T*>(h + 1);
    }

    template<class T, class... Args>
    inline T* new_value(bool* ok, Args&&... args) noexcept {
        if constexpr (!std::is_nothrow_constructible_v<T, Args...>) {
            if (ok) *ok = false;
            return nullptr;
        } else {
            T* p = allocate_storage<T>(ok, 1, AllocationKind::single);
            if (!p) return nullptr;
            ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
            return p;
        }
    }

    template<class T>
    inline Slice<T> new_array(bool* ok, std::initializer_list<T> values) noexcept {
        Slice<T> result{};
        T* p = allocate_storage<T>(ok, values.size(), AllocationKind::array);
        if (!p) return result;
        std::size_t i = 0;
        if constexpr (!std::is_nothrow_constructible_v<T, const T&>) {
            AllocationHeader* h = allocation_header(p);
            unlink_allocation(h);
            std::free(static_cast<void*>(h));
            if (ok) *ok = false;
            return result;
        } else {
            for (const T& value : values) ::new (static_cast<void*>(p + i++)) T(value);
            result = {p, values.size()};
            return result;
        }
    }

    template<class T>
    inline void delete_value(T* p) noexcept {
        if (!p) return;
        AllocationHeader* h = allocation_header(p);
        if (!h || h->kind != AllocationKind::single) return;
        h->destroy(p);
        unlink_allocation(h);
        std::free(static_cast<void*>(h));
    }

    template<class T>
    inline void delete_array(Slice<T>& slice) noexcept {
        if (!slice.data) { slice.size = 0; return; }
        AllocationHeader* h = allocation_header(slice.data);
        if (!h || h->kind != AllocationKind::array) return;
        for (std::size_t i = 0; i < h->count; ++i)
            h->destroy(slice.data + i);
        unlink_allocation(h);
        std::free(static_cast<void*>(h));
        slice = {};
    }

    template<class T>
    inline void delete_value(Slice<T>& slice) noexcept {
        delete_array(slice);
    }

    template<class T>
    inline void resize_append(bool* ok, Slice<T>& slice, std::size_t added,
                              std::initializer_list<T> appended) noexcept {
        if (ok) *ok = false;
        if (added > std::numeric_limits<std::size_t>::max() - slice.size ||
            appended.size() > added || !slice.data) return;
        AllocationHeader* old = allocation_header(slice.data);
        if (!old || old->kind != AllocationKind::array || old->count != slice.size)
            return;
        if constexpr (!std::is_nothrow_constructible_v<T, const T&> ||
                      !std::is_nothrow_default_constructible_v<T>) return;
        T* replacement = allocate_storage<T>(ok, slice.size + added,
                                              AllocationKind::array);
        if (!replacement) return;
        std::size_t built = 0;
        for (; built < slice.size; ++built)
            ::new (static_cast<void*>(replacement + built)) T(slice.data[built]);
        std::size_t appended_count = 0;
        for (const T& value : appended)
            ::new (static_cast<void*>(replacement + built + appended_count++)) T(value);
        for (; built + appended_count < slice.size + added; ++built)
            ::new (static_cast<void*>(replacement + built)) T();
        for (std::size_t i = 0; i < old->count; ++i) old->destroy(slice.data + i);
        unlink_allocation(old);
        std::free(static_cast<void*>(old));
        slice = {replacement, slice.size + added};
        if (ok) *ok = true;
    }

    template<class T, std::size_t N>
    struct Array {
        std::remove_cv_t<T> data[N]{};
        Array() = default;
        Array(std::initializer_list<T> values) {
            std::size_t i = 0;
            for (const T& value : values) {
                if (i == N) break;
                data[i++] = value;
            }
        }
        template<std::size_t M>
        Array(const T (&v)[M]) {
            constexpr std::size_t count = M < N ? M : N;
            for (std::size_t i = 0; i < count; ++i) data[i] = v[i];
        }
        constexpr std::size_t size() const { return N; }
        T& operator[](std::size_t i) { return data[i]; }
        const T& operator[](std::size_t i) const { return data[i]; }
        friend std::ostream& operator<<(std::ostream& os, const Array& a) {
            if constexpr (std::is_same_v<std::remove_cv_t<T>, char>) {
                for (std::size_t i = 0; i < N && a.data[i] != '\0'; ++i) os << a.data[i];
            } else {
                for (std::size_t i = 0; i < N; ++i) {
                    if (i) os << ", ";
                    os << a.data[i];
                }
            }
            return os;
        }
    };

    template<class T>
    using Optional = std::optional<T>;

    inline bool random_mbool() {
        static std::mt19937 generator(std::random_device{}());
        static std::bernoulli_distribution distribution(0.5);
        return distribution(generator);
    }
    struct MaybeBoolean {
    };
    class mbool {
        enum class State { Maybe, False, True };
        State state_ = State::Maybe;

    public:
        mbool() = default;
        mbool(MaybeBoolean) {}
        mbool(bool value) : state_(value ? State::True : State::False) {}

        mbool& operator=(bool value) {
            state_ = value ? State::True : State::False;
            return *this;
        }

        operator bool() const {
            if (state_ == State::Maybe) return random_mbool();
            return state_ == State::True;
        }
    };
    inline const MaybeBoolean maybe{};

    template<class T>
    struct Error {
        T value{};
        int error_code{};
        bool has_error{};
        explicit operator bool() const { return !has_error; }
    };

    template<class... Args>
    inline void print(const char* fmt, Args&&... args) {
        std::ostringstream out;
        std::string_view text(fmt);
        std::size_t pos = 0;
        ([&](auto&& arg) {
            std::size_t placeholder = text.find("{}", pos);
            if (placeholder == std::string_view::npos) return;
            out << text.substr(pos, placeholder - pos);
            out << arg;
            pos = placeholder + 2;
        }(std::forward<Args>(args)), ...);
        out << text.substr(pos);
        std::printf("%s", out.str().c_str());
    }

    inline std::optional<std::vector<char>> fread_all(const char* path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return std::nullopt;
        std::vector<char> out((std::istreambuf_iterator<char>(f)), {});
        out.push_back('\0');
        return out;
    }

    inline std::optional<std::size_t> fwrite_all(const char* path, const char* data) {
        std::ofstream f(path, std::ios::binary);
        if (!f) return std::nullopt;
        const auto n = std::strlen(data);
        f.write(data, static_cast<std::streamsize>(n));
        if (!f || static_cast<std::size_t>(f.tellp()) != n) return std::nullopt;
        return n;
    }

    inline std::optional<std::size_t> fappend_all(const char* path, const char* data) {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        if (!f) return std::nullopt;
        const auto n = std::strlen(data);
        f.write(data, static_cast<std::streamsize>(n));
        if (!f) return std::nullopt;
        return n;
    }

    inline bool fexits(const char* path) {
        return std::filesystem::exists(path);
    }

    inline void assert_that(bool condition, const char* message) {
        if (condition) return;
        if (test_mode) throw std::runtime_error(message ? message : "assertion failed");
        assert(condition && message);
    }

    struct Defer {
        void (*fn)(void*);
        void* data;
        Defer(const Defer&) = delete;
        Defer& operator=(const Defer&) = delete;
        Defer(void (*f)(void*), void* p) : fn(f), data(p) {}
        ~Defer() { if (fn) fn(data); }
    };
}
)REXON";

static constexpr std::string_view std_cpp = R"REXON(
#include "std.hpp"
)REXON";

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

struct Options {
    fs::path input;
    fs::path output;
    std::vector<fs::path> include_dirs;
    std::vector<std::string> clang_args;
    bool get_std_hpp = false;
    bool emit_cpp = false;
    bool keep_temp = false;
    bool print_tokens = false;
    bool run_tests = false;
    std::optional<std::string> unique_test;
};

struct TestCase {
    std::string name;
    std::string function;
};

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}
static void write_file(const fs::path& p, std::string_view s) {
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write " + p.string());
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
    if (!f) throw std::runtime_error("write failed: " + p.string());
}
static std::string cpp_quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            default: out += static_cast<char>(c); break;
        }
    }
    return out + "\"";
}

static std::string shell_quote(const std::string& value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

class Transpiler {
    const std::vector<Token>& t;
    std::size_t i = 0;
    std::string out;
    std::vector<Diagnostic>& errors;
    fs::path file;
    std::size_t unique_id = 0;
    std::unordered_set<std::string> optional_vars;
    std::vector<TestCase>& tests;
    std::size_t& next_test_id;
    bool test_mode = false;

    const Token& cur() const { return t[std::min(i, t.size() - 1)]; }
    const Token& look(std::size_t n = 1) const { return t[std::min(i + n, t.size() - 1)]; }
    bool is(std::string_view s) const { return cur().text == s; }
    bool eat(std::string_view s) {
        if (!is(s)) return false;
        ++i;
        return true;
    }
    void emit(std::string_view s) { out.append(s); }
    void error(std::string msg) {
        errors.push_back({file, cur().line, cur().column, std::move(msg)});
    }
    static bool ident(const Token& x) { return x.kind == Kind::Identifier; }
    static bool wordlike(const Token& x) {
        return x.kind == Kind::Identifier || x.kind == Kind::Number;
    }

    static std::string token_text(const Token& x) {
        if (x.kind == Kind::String) return cpp_quote(x.text);
        if (x.kind == Kind::Character) {
            std::string q = "'";
            for (unsigned char c : x.text) {
                if (c == '\\') q += "\\\\";
                else if (c == '\'') q += "\\'";
                else if (c == '\n') q += "\\n";
                else if (c == '\r') q += "\\r";
                else if (c == '\t') q += "\\t";
                else q += static_cast<char>(c);
            }
            return q + "'";
        }
        if (x.text == "null") return "std::nullopt";
        return x.text;
    }

    static bool needs_space(const Token& a, const Token& b) {
        if (a.kind == Kind::End || b.kind == Kind::End) return false;
        if (wordlike(a) && wordlike(b)) return true;
        if ((a.kind == Kind::String || a.kind == Kind::Character) && wordlike(b)) return true;
        if (wordlike(a) && (b.text == "*" || b.text == "&")) return false;
        if ((a.text == "*" || a.text == "&") && wordlike(b)) return true;
        if (a.text == "," || a.text == ";") return true;
        if (b.text == "," || b.text == ";" || b.text == ")" ||
            b.text == "]" || b.text == "." || b.text == "::" || b.text == "->")
            return false;
        if (a.text == "(" || a.text == "[" || a.text == "." ||
            a.text == "::" || a.text == "->") return false;
        if (b.text == "(" || b.text == "[") return false;
        if (a.text == "!" || a.text == "~") return false;
        if (b.text == "!" || b.text == "~") return false;
        if (a.text == "++" || a.text == "--" || b.text == "++" || b.text == "--")
            return false;
        return false;
    }

    std::string render(std::size_t first, std::size_t last) const {
        std::string s;
        for (std::size_t k = first; k < last; ++k) {
            const Token& x = t[k];
            if (k > first && needs_space(t[k - 1], x)) s += ' ';
            s += token_text(x);
        }
        return s;
    }

    std::string render_type(std::size_t first, std::size_t last) const {
        std::string s;
        const Token* previous = nullptr;
        for (std::size_t k = first; k < last; ++k) {
            // `#` is Rexon's constant type modifier and is not valid C++ syntax.
            if (t[k].text == "#") continue;
            if (previous && needs_space(*previous, t[k])) s += ' ';
            s += token_text(t[k]);
            previous = &t[k];
        }
        return s;
    }

    std::vector<std::pair<std::size_t, std::size_t>> split_args(
        std::size_t first, std::size_t last) const {
        std::vector<std::pair<std::size_t, std::size_t>> parts;
        std::size_t start = first;
        int paren = 0, bracket = 0, brace = 0, angle = 0;
        for (std::size_t k = first; k < last; ++k) {
            const auto& x = t[k].text;
            if (x == "(") ++paren;
            else if (x == ")") --paren;
            else if (x == "[") ++bracket;
            else if (x == "]") --bracket;
            else if (x == "{") ++brace;
            else if (x == "}") --brace;
            else if (x == "<") ++angle;
            else if (x == ">" && angle > 0) --angle;
            else if (x == "," && paren == 0 && bracket == 0 &&
                     brace == 0 && angle == 0) {
                parts.emplace_back(start, k);
                start = k + 1;
            }
        }
        if (start < last) parts.emplace_back(start, last);
        return parts;
    }

    std::string convert_type(std::size_t first, std::size_t last) const {
        if (first >= last) return "void";

        if (last == first + 1 && t[first].text == "mbool")
            return "rexon::mbool";

        // T? -> std::optional<T>
        if (last > first && t[last - 1].text == "?")
            return "std::optional<" + render_type(first, last - 1) + ">";

        // T[N] -> rexon::Array<T, N>; T[] -> rexon::Slice<T>
        if (last >= first + 2 && t[last - 1].text == "]") {
            std::size_t open = last - 2;
            while (open > first && t[open].text != "[") --open;
            if (t[open].text == "[") {
                std::string base = render_type(first, open);
                std::string extent = render_type(open + 1, last - 1);
                if (extent.empty()) return "rexon::Slice<" + base + ">";
                return "rexon::Array<" + base + ", " + extent + ">";
            }
        }
        return render_type(first, last);
    }

    // Converts type-before-name declaration: "const char* name" or "int x".
    std::string declaration(std::size_t first, std::size_t last) {
        if (first >= last) return {};
        std::size_t eq = last;
        int depth = 0;
        for (std::size_t k = first; k < last; ++k) {
            if (t[k].text == "(" || t[k].text == "[" || t[k].text == "<") ++depth;
            else if (t[k].text == ")" || t[k].text == "]" || t[k].text == ">") --depth;
            else if (t[k].text == "=" && depth == 0) { eq = k; break; }
        }

        std::size_t name = eq;
        while (name > first && !ident(t[name - 1])) --name;
        if (name == first) return render(first, last);

        std::size_t name_index = name - 1;
        if (name_index == first) return render(first, last);

        std::size_t type_end = name_index;
        std::string type = convert_type(first, type_end);
        const bool is_constant = std::any_of(t.begin() + first, t.begin() + type_end,
                                             [](const Token& token) {
                                                 return token.text == "#";
                                             });
        if (is_constant && !type.starts_with("const ")) type = type + " const";
        std::string var = t[name_index].text;

        // C-style suffix in Rexon type syntax: int[4] values -> int values[4].
        std::string suffix;
        if (type_end < eq && t[type_end].text == "[") {
            suffix = render(type_end, eq);
        }

        if (type.find("std::optional<") != std::string::npos)
            optional_vars.insert(var);

        std::string result = type + " " + var + suffix;
        if (eq < last) result += " " + render_expr(eq, last);
        return result;
    }

    std::string render_expr(std::size_t first, std::size_t last) const {
        std::string s;
        const Token* previous = nullptr;
        for (std::size_t k = first; k < last;) {
            // Rexon dynamic memory syntax is lowered here, after lexing and
            // parsing its delimiters, rather than by textual replacement.
            if (t[k].text == "new" || t[k].text == "resize" || t[k].text == "delete") {
                const std::string operation = t[k].text;
                std::size_t p = k + 1;
                if (operation == "delete") {
                    std::string value = render_expr(p, last);
                    return "rexon::delete_value(" + value + ")";
                }
                if (p >= last || t[p].text != "(") { s += token_text(t[k++]); continue; }
                int depth = 0;
                std::size_t close = p;
                for (; close < last; ++close) {
                    if (t[close].text == "(") ++depth;
                    else if (t[close].text == ")" && --depth == 0) break;
                }
                if (close >= last) { s += render(k, last); break; }
                auto status_args = split_args(p + 1, close);
                if (status_args.size() != 1) { s += render(k, close + 1); k = close + 1; continue; }
                auto status = render_expr(status_args[0].first, status_args[0].second);
                std::size_t type_first = close + 1;
                std::size_t brace = type_first;
                for (; brace < last && t[brace].text != "{"; ++brace) {}
                if (brace >= last) { s += render(k, last); break; }
                int braces = 0;
                std::size_t brace_close = brace;
                for (; brace_close < last; ++brace_close) {
                    if (t[brace_close].text == "{") ++braces;
                    else if (t[brace_close].text == "}" && --braces == 0) break;
                }
                if (brace_close >= last) { s += render(k, last); break; }
                auto values = split_args(brace + 1, brace_close);
                std::string values_cpp;
                for (std::size_t n = 0; n < values.size(); ++n) {
                    if (n) values_cpp += ", ";
                    values_cpp += render_expr(values[n].first, values[n].second);
                }
                if (operation == "new") {
                    bool array = type_first + 1 < brace &&
                                 t[brace - 1].text == "]" && t[brace - 2].text == "[";
                    std::string type = convert_type(type_first, array ? brace - 2 : brace);
                    if (array) {
                        s += "rexon::new_array<" + type + ">(" + status + ", {" + values_cpp + "})";
                    } else {
                        s += "rexon::new_value<" + type + ">(" + status;
                        if (!values.empty()) s += ", " + values_cpp;
                        s += ")";
                    }
                } else {
                    // resize(&ok) T[+N]{slice, appended...}
                    bool valid_array = type_first + 4 < brace &&
                                       t[brace - 1].text == "]" && t[brace - 3].text == "+" &&
                                       t[brace - 2].kind == Kind::Number && t[brace - 4].text == "[";
                    if (!valid_array || values.empty()) { s += render(k, brace_close + 1); k = brace_close + 1; continue; }
                    std::string type = convert_type(type_first, brace - 4);
                    auto delta = t[brace - 2].text;
                    auto original = render_expr(values[0].first, values[0].second);
                    std::string appended_cpp;
                    for (std::size_t n = 1; n < values.size(); ++n) {
                        if (n > 1) appended_cpp += ", ";
                        appended_cpp += render_expr(values[n].first, values[n].second);
                    }
                    s += "rexon::resize_append<" + type + ">(" + status + ", " +
                         original + ", " + delta + ", {" + appended_cpp + "})";
                }
                k = brace_close + 1;
                previous = nullptr;
                continue;
            }
            // Rexon optional unwrap: `value.?` becomes C++ `(*value)`.
            if (k + 2 < last && t[k].kind == Kind::Identifier &&
                t[k + 1].text == "." && t[k + 2].text == "?") {
                s += "(*" + t[k].text + ")";
                previous = &t[k + 2];
                k += 3;
                continue;
            }
            if (t[k].text == "@") {
                if (k + 2 >= last || t[k + 1].kind != Kind::Identifier ||
                    t[k + 2].text != "(") {
                    std::string piece = render(k, k + 1);
                    if (previous && needs_space(*previous, t[k])) s += ' ';
                    s += piece;
                    previous = &t[k];
                    ++k;
                    continue;
                }
                std::string name = t[k + 1].text;
                std::size_t open = k + 2;
                int depth = 0;
                std::size_t close = open;
                for (close = open; close < last; ++close) {
                    if (t[close].text == "(") ++depth;
                    else if (t[close].text == ")") {
                        --depth;
                        if (depth == 0) break;
                    }
                }
                if (close >= last) {
                    s += render(k, last);
                    break;
                }
                auto args = split_args(open + 1, close);
                auto arg = [&](std::size_t n) -> std::string {
                    if (n >= args.size()) return {};
                    auto [a, b] = args[n];
                    return render_expr(a, b);
                };
                if (name == "cast" && args.size() == 2) {
                    std::string value = arg(1);
                    if (args[1].second - args[1].first == 1 &&
                        ident(t[args[1].first]) &&
                        optional_vars.contains(t[args[1].first].text))
                        value = "(*" + value + ")";
                    s += "static_cast<" + arg(0) + ">(" + value + ")";
                } else if (name == "print") {
                    s += "rexon::print(";
                    for (std::size_t n = 0; n < args.size(); ++n) {
                        if (n) s += ", ";
                        s += arg(n);
                    }
                    s += ")";
                } else if (name == "assert" && !args.empty()) {
                    s += "rexon::assert_that((" + arg(0) + "), " +
                         (args.size() > 1 ? arg(1) : cpp_quote("assertion failed")) + ")";
                } else if (name == "exit") {
                    s += "std::exit(" + arg(0) + ")";
                } else {
                    s += render(k, close + 1);
                }
                k = close + 1;
                continue;
            }
            std::string piece = token_text(t[k]);
            if (t[k].text == "null") piece = "std::nullopt";
            else if (t[k].text == "maybe") piece = "rexon::maybe";
            if (previous && needs_space(*previous, t[k])) s += ' ';
            s += piece;
            previous = &t[k];
            ++k;
        }
        for (;;) {
            std::size_t p = s.find("this. ");
            if (p == std::string::npos) break;
            s.replace(p, 7, "this->");
        }
        for (;;) {
            std::size_t p = s.find("this.");
            if (p == std::string::npos) break;
            s.replace(p, 5, "this->");
        }
        for (;;) {
            std::size_t p = s.find("  ");
            if (p == std::string::npos) break;
            s.erase(p, 1);
        }
        // `this.member` is emitted as C++ `this->member`.
        return s;
    }

    void parse_builtin() {
        const auto at = i++;
        if (!eat(" ") && !ident(cur())) {
            // Whitespace is not represented in the token stream.
        }
        if (!ident(cur())) {
            errors.push_back({file, t[at].line, t[at].column, "expected builtin name after @"});
            return;
        }
        std::string name = cur().text;
        ++i;
        if (!eat("(")) { error("expected '(' after builtin"); return; }

        std::size_t first = i;
        int paren = 0, bracket = 0, brace = 0;
        while (cur().kind != Kind::End) {
            if (is(")") && paren == 0 && bracket == 0 && brace == 0) break;
            if (is("(")) ++paren;
            else if (is(")")) --paren;
            else if (is("[")) ++bracket;
            else if (is("]")) --bracket;
            else if (is("{")) ++brace;
            else if (is("}")) --brace;
            ++i;
        }
        std::size_t last = i;
        auto args = split_args(first, last);
        eat(")");
        eat(";");

        auto arg = [&](std::size_t n) -> std::string {
            if (n >= args.size()) return {};
            auto [a, b] = args[n];
            return render_expr(a, b);
        };

        if (name == "cpp") {
            if (args.size() != 1 || args[0].second - args[0].first != 1 ||
                t[args[0].first].kind != Kind::String) {
                errors.push_back({file, t[at].line, t[at].column,
                                  "@cpp requires exactly one string literal"});
                return;
            }
            emit(t[args[0].first].text);
        } else if (name == "cast") {
            if (args.size() != 2) {
                errors.push_back({file, t[at].line, t[at].column,
                                  "@cast requires exactly 2 arguments"});
                return;
            }
            std::string value = arg(1);
            if (args[1].second - args[1].first == 1 &&
                ident(t[args[1].first]) &&
                optional_vars.contains(t[args[1].first].text))
                value = "(*" + value + ")";
            emit("static_cast<" + arg(0) + ">(" + value + ")");
        } else if (name == "print") {
            emit("rexon::print(");
            for (std::size_t n = 0; n < args.size(); ++n) {
                if (n) emit(", ");
                emit(arg(n));
            }
            emit(")");
        } else if (name == "assert") {
            if (args.empty()) {
                errors.push_back({file, t[at].line, t[at].column,
                                  "@assert requires a condition"});
                return;
            }
            emit("rexon::assert_that((" + arg(0) + "), " +
                 (args.size() > 1 ? arg(1) : cpp_quote("assertion failed")) + ")");
        } else if (name == "exit") {
            emit("std::exit(" + arg(0) + ")");
        } else if (name == "fexits") {
            emit("rexon::fexits(" + arg(0) + ")");
        } else if (name == "fread") {
            emit("rexon::fread_all(" + arg(0) + ")");
        } else if (name == "fwrite") {
            emit("rexon::fwrite_all(" + arg(0) + ", " + arg(1) + ")");
        } else if (name == "fappend") {
            emit("rexon::fappend_all(" + arg(0) + ", " + arg(1) + ")");
        } else if (name == "embeb") {
            errors.push_back({file, t[at].line, t[at].column,
                              "@embeb must be handled by the compile-time file embedding pass"});
        } else {
            errors.push_back({file, t[at].line, t[at].column,
                              "unknown builtin @" + name});
        }
        if (name == "cpp") emit("\n");
        else emit(";\n");
    }

    void parse_block() {
        if (!eat("{")) { error("expected '{'"); return; }
        emit("{\n");
        while (cur().kind != Kind::End && !is("}")) {
            auto before = i;
            parse_item();
            if (i == before) {
                error("parser made no progress");
                ++i;
            }
        }
        if (!eat("}")) error("unterminated block");
        emit("}\n");
    }

    void parse_function(bool is_static = false) {
        if (!eat("fn")) return;
        if (!ident(cur())) { error("expected function name"); return; }
        std::string name = cur().text;
        ++i;
        if (name == "main" && test_mode) name = "__rexon_user_main";
        if (!eat("(")) { error("expected '(' after function name"); return; }

        std::size_t params_first = i;
        int depth = 0;
        while (cur().kind != Kind::End) {
            if (is(")") && depth == 0) break;
            if (is("(") || is("[") || is("<")) ++depth;
            else if (is(")") || is("]") || is(">")) --depth;
            ++i;
        }
        std::size_t params_last = i;
        eat(")");

        std::string params;
        auto parts = split_args(params_first, params_last);
        for (std::size_t n = 0; n < parts.size(); ++n) {
            if (n) params += ", ";
            params += declaration(parts[n].first, parts[n].second);
        }

        std::size_t ret_first = i;
        while (cur().kind != Kind::End && !is("{") && !is(";")) ++i;
        std::string ret = ret_first == i ? "void" : convert_type(ret_first, i);

        if (is_static) emit("static ");
        if (name == "main" && ret == "void")
            emit("auto main() -> int ");
        else
            emit("auto " + name + "(" + params + ") -> " + ret + " ");

        if (eat(";")) {
            emit(";\n");
            return;
        }
        parse_block();
    }

    void parse_test() {
        const auto test_token = cur();
        eat("test");
        if (cur().kind != Kind::String) {
            errors.push_back({file, test_token.line, test_token.column,
                              "expected a string test name after 'test'"});
            return;
        }
        std::string name = cur().text;
        ++i;
        const std::string function = "__rexon_test_" + std::to_string(next_test_id++);
        if (!is("{")) {
            error("expected '{' after test name");
            return;
        }
        emit("static void " + function + "() ");
        parse_block();
        tests.push_back({std::move(name), function});
    }

    void parse_struct() {
        eat("struct");
        if (!ident(cur())) { error("expected struct name"); return; }
        std::string name = cur().text;
        ++i;
        emit("struct " + name + " ");
        if (!eat("{")) { error("expected '{' after struct name"); return; }
        emit("{\n");
        while (cur().kind != Kind::End && !is("}")) {
            auto before = i;
            if (eat("static")) {
                if (is("fn")) parse_function(true);
                else {
                    error("expected fn after static");
                    while (cur().kind != Kind::End && !eat(";")) ++i;
                }
            } else if (is("fn")) {
                parse_function(false);
            } else if (is("export")) {
                ++i;
            } else {
                parse_simple_statement(true);
            }
            if (i == before) { error("parser made no progress in struct"); ++i; }
        }
        eat("}");
        emit("};\n");
        eat(";");
    }

    void parse_enum() {
        eat("enum");
        std::size_t start = i;
        while (cur().kind != Kind::End && !is("{")) ++i;
        if (cur().kind == Kind::End) { error("expected enum body"); return; }
        emit("enum " + render(start, i) + " ");
        int depth = 0;
        do {
            if (is("{")) ++depth;
            if (is("}")) --depth;
            emit(token_text(cur()));
            ++i;
        } while (cur().kind != Kind::End && depth > 0);
        emit(";\n");
        eat(";");
    }

    void parse_extern_type() {
        const auto type_token = cur();
        eat("extern_type");
        if (!ident(cur())) {
            errors.push_back({file, type_token.line, type_token.column,
                              "expected alias name after 'extern_type'"});
            return;
        }
        const std::string alias = cur().text;
        ++i;
        if (!eat("=")) {
            error("expected '=' after extern_type alias name");
            return;
        }
        const std::size_t first = i;
        while (cur().kind != Kind::End && !is(";")) ++i;
        if (first == i) {
            error("expected target type after extern_type alias");
            eat(";");
            return;
        }
        emit("using " + alias + " = " + render(first, i) + ";\n");
        eat(";");
    }

    void parse_control() {
        std::size_t start = i;
        while (cur().kind != Kind::End && !is("{") && !is(";")) ++i;
        emit(render_expr(start, i));
        if (is("{")) parse_block();
        else if (eat(";")) emit(";\n");
    }

    void parse_defer() {
        const auto id = unique_id++;
        eat("defer");
        if (is("{")) {
            // Capture the deferred block now, execute it at scope exit.
            std::size_t start = i;
            int depth = 0;
            do {
                if (is("{")) ++depth;
                else if (is("}")) --depth;
                ++i;
            } while (cur().kind != Kind::End && depth > 0);
            std::size_t last = i;
            std::string body = render(start + 1, last - 1);
            emit("auto __rexon_defer_" + std::to_string(id) +
                 " = rexon::make_defer([&]() { " + body + "; });\n");
        } else {
            std::size_t start = i;
            while (cur().kind != Kind::End && !is(";")) ++i;
            std::string expr = render_expr(start, i);
            eat(";");
            emit("auto __rexon_defer_" + std::to_string(id) +
                 " = rexon::make_defer([&]() { " + expr + "; });\n");
        }
    }

    void parse_simple_statement(bool in_struct = false) {
        std::size_t start = i;
        int paren = 0, bracket = 0, brace = 0;
        while (cur().kind != Kind::End) {
            if (is(";") && paren == 0 && bracket == 0 && brace == 0) break;
            if (is("{")) ++brace;
            else if (is("}")) {
                if (brace == 0) break;
                --brace;
            } else if (is("(")) ++paren;
            else if (is(")")) --paren;
            else if (is("[")) ++bracket;
            else if (is("]")) --bracket;
            ++i;
        }
        std::size_t last = i;
        eat(";");
        if (start == last) return;

        // Imports and exports are compile-time/module constructs, not C++ statements.
        if (t[start].text == "import" || t[start].text == "export") return;

        std::string s;
        // A declaration begins with a type-like sequence and has an identifier
        // before its initializer. Handle optional and fixed-array suffixes.
        bool looks_decl = false;
        if (start + 1 < last && ident(t[start])) {
            if (t[start].text == "return" || t[start].text == "break" ||
                t[start].text == "continue" || t[start].text == "if" ||
                t[start].text == "while" || t[start].text == "for" ||
                t[start].text == "switch" || t[start].text == "new" ||
                t[start].text == "delete" || t[start].text == "resize") {
                looks_decl = false;
            } else {
            std::size_t k = start + 1;
            while (k < last && (t[k].text == "*" || t[k].text == "#" ||
                   t[k].text == "?" || t[k].text == "[" ||
                   t[k].kind == Kind::Number || t[k].text == "]" ||
                   t[k].kind == Kind::Identifier)) {
                if (t[k].kind == Kind::Identifier) { looks_decl = true; break; }
                ++k;
            }
            }
        }
        if (looks_decl) s = declaration(start, last);
        else s = render_expr(start, last);

        emit(s);
        emit(";\n");
        (void)in_struct;
    }

    void parse_item() {
        if (eat("export")) {
            if (is("namespace")) { parse_item(); return; }
            if (is("struct")) { parse_struct(); return; }
            if (is("enum")) { parse_enum(); return; }
            if (is("extern_type")) { parse_extern_type(); return; }
            if (is("fn")) { parse_function(); return; }
            return;
        }
        if (eat("import")) {
            while (cur().kind != Kind::End && !eat(";")) ++i;
            return;
        }
        if (eat("static")) {
            if (is("fn")) { parse_function(true); return; }
            emit("static ");
            parse_simple_statement();
            return;
        }
        if (is("fn")) { parse_function(); return; }
        if (is("test")) { parse_test(); return; }
        if (is("struct")) { parse_struct(); return; }
        if (is("enum")) { parse_enum(); return; }
        if (is("extern_type")) { parse_extern_type(); return; }
        if (is("namespace")) {
            ++i;
            std::size_t start = i;
            while (cur().kind != Kind::End && !is("{")) ++i;
            emit("namespace " + render(start, i) + " ");
            parse_block();
            return;
        }
        if (is("@")) { parse_builtin(); return; }
        if (is("defer")) { parse_defer(); return; }
        if (is("if") || is("while") || is("for") || is("switch")) {
            parse_control();
            return;
        }
        if (is("{")) { parse_block(); return; }
        if (is("else")) {
            ++i;
            emit("else ");
            if (is("if")) parse_control();
            else if (is("{")) parse_block();
            else parse_simple_statement();
            return;
        }
        if (is("return") || is("break") || is("continue")) {
            parse_simple_statement();
            return;
        }
        parse_simple_statement();
    }

public:
    Transpiler(const std::vector<Token>& tokens, fs::path f, std::vector<Diagnostic>& e,
               std::vector<TestCase>& test_cases, std::size_t& test_id, bool tests_enabled)
        : t(tokens), errors(e), file(std::move(f)), tests(test_cases),
          next_test_id(test_id), test_mode(tests_enabled) {}

    std::string run() {
        emit("#include \"std.hpp\"\n");
        while (cur().kind != Kind::End) {
            std::size_t before = i;
            parse_item();
            if (i == before) {
                error("unable to parse token '" + cur().text + "'");
                ++i;
            }
        }
        return out;
    }
};

static void print_diagnostics(const std::vector<Diagnostic>& ds) {
    for (const auto& d : ds)
        std::cerr << d.file.string() << ':' << d.line << ':' << d.column
                  << ": error: " << d.message << '\n';
}

static std::optional<fs::path> resolve_lang(const fs::path& name,
                                             const fs::path& current_file,
                                             const std::vector<fs::path>& include_dirs,
                                             const fs::path& cwd) {
    std::vector<fs::path> candidates;
    candidates.push_back(cwd / name);
    candidates.push_back(current_file.parent_path() / name);
    for (const auto& d : include_dirs) candidates.push_back(d / name);
    for (auto p : candidates) {
        if (p.extension() != ".rxn") p += ".rxn";
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return fs::weakly_canonical(p, ec);
    }
    return std::nullopt;
}

static void collect_modules(const fs::path& file,
                            const std::vector<fs::path>& include_dirs,
                            const fs::path& cwd,
                            std::unordered_set<std::string>& visited,
                            std::vector<fs::path>& order,
                            std::vector<Diagnostic>& errors) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(file, ec);
    auto key = (ec ? file : canonical).string();
    if (!visited.insert(key).second) return;

    std::string src;
    try { src = read_file(file); }
    catch (const std::exception& e) {
        errors.push_back({file, 1, 1, e.what()});
        return;
    }
    // Import discovery is deliberately lexical and cycle-safe. The compiler
    // emits each module once; declarations are available through C++ headers.
    std::vector<Token> toks = Lexer(src, file, errors).run();
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
        if (toks[i].text == "import" && toks[i + 1].kind == Kind::Identifier) {
            auto dep = resolve_lang(toks[i + 1].text, file, include_dirs, cwd);
            if (!dep) {
                errors.push_back({file, toks[i].line, toks[i].column,
                                  "cannot resolve import '" + toks[i + 1].text + "'"});
            } else collect_modules(*dep, include_dirs, cwd, visited, order, errors);
        }
    }
    order.push_back(file);
}

static std::optional<Options> parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-I") {
            if (++i >= argc) { std::cerr << "rexon: -I requires a directory\n"; return {}; }
            o.include_dirs.emplace_back(argv[i]);
        } else if (a.starts_with("-I") && a.size() > 2) {
            o.include_dirs.emplace_back(a.substr(2));
        } else if (a == "-o") {
            if (++i >= argc) { std::cerr << "rexon: -o requires a path\n"; return {}; }
            o.output = argv[i];
        } else if (a == "--get-std-hpp") o.get_std_hpp = true;
        else if (a == "--emit-cpp") o.emit_cpp = true;
        else if (a == "--keep-temp") o.keep_temp = true;
        else if (a == "--tokens") o.print_tokens = true;
        else if (a == "--test") o.run_tests = true;
        else if (a == "--unique-test") {
            if (++i >= argc) {
                std::cerr << "rexon: --unique-test requires a test name\n";
                return {};
            }
            o.run_tests = true;
            o.unique_test = argv[i];
        }
        else if (!a.empty() && a[0] == '-') {
            o.clang_args.push_back(a);
        } else if (o.input.empty()) {
            o.input = a;
        } else {
            o.clang_args.push_back(a);
        }
    }
    if (o.get_std_hpp) return o;
    if (o.input.empty()) {
        std::cerr << "usage: rexon [-I dir] [-o output] [--test [--unique-test name]] "
                     "[--emit-cpp] [--get-std-hpp] file.rxn [clang++ flags...]\n";
        return {};
    }
    if (o.output.empty()) {
        fs::path default_output = o.input;

        if (o.emit_cpp)
            default_output.replace_extension(".cpp");
        else
            default_output.replace_extension("");

        o.output = default_output;
    }
    return o;
}

int main(int argc, char** argv) {
    auto parsed = parse_args(argc, argv);
    if (!parsed) return 2;
    Options opt = *parsed;
    if (opt.get_std_hpp) {
        std::cout << std_hpp;
        return 0;
    }
    const fs::path cwd = fs::current_path();

    std::vector<Diagnostic> errors;
    std::unordered_set<std::string> visited;
    std::vector<fs::path> modules;
    std::vector<TestCase> tests;
    std::size_t next_test_id = 0;
    collect_modules(opt.input, opt.include_dirs, cwd, visited, modules, errors);
    if (!errors.empty()) { print_diagnostics(errors); return 1; }

    fs::path generated = opt.emit_cpp ? opt.output : fs::path(opt.output.string() + ".rexon.cpp");
    std::ostringstream combined;
    combined << "// Generated by rexon compiler\n";
    for (const auto& module : modules) {
        std::string src;
        try { src = read_file(module); }
        catch (const std::exception& e) {
            errors.push_back({module, 1, 1, e.what()});
            continue;
        }
        auto tokens = Lexer(src, module, errors).run();
        if (opt.print_tokens) {
            for (const auto& t : tokens)
                std::cerr << module.string() << ':' << t.line << ':' << t.column
                          << "  " << t.text << '\n';
        }
        Transpiler tr(tokens, module, errors, tests, next_test_id, opt.run_tests);
        combined << "\n// module: " << module.generic_string() << "\n";
        combined << tr.run() << "\n";
    }
    if (!errors.empty()) { print_diagnostics(errors); return 1; }

    if (opt.run_tests) {
        if (tests.empty()) {
            std::cerr << "rexon: --test requires at least one test declaration\n";
            return 1;
        }
        std::unordered_set<std::string> test_names;
        for (const auto& test : tests) {
            if (!test_names.insert(test.name).second) {
                std::cerr << "rexon: duplicate test name: " << test.name << '\n';
                return 1;
            }
        }
        if (opt.unique_test && std::none_of(tests.begin(), tests.end(),
                                            [&](const TestCase& test) {
                                                return test.name == *opt.unique_test;
                                            })) {
            std::cerr << "rexon: test not found: " << *opt.unique_test << '\n';
            return 1;
        }
        combined << "\nint main() {\n"
                 << "    rexon::test_mode = true;\n"
                 << "    int passed = 0;\n"
                 << "    int failed = 0;\n"
                 << "    auto run_test = [&](const char* name, void (*fn)()) {\n"
                 << "        rexon::print(\"[TEST] {}\\n\", name);\n"
                 << "        try { fn(); ++passed; rexon::print(\"[PASS] {}\\n\", name); }\n"
                 << "        catch (const std::exception& e) { ++failed; rexon::print(\"[FAIL] {}: {}\\n\", name, e.what()); }\n"
                 << "        catch (...) { ++failed; rexon::print(\"[FAIL] {}: unknown exception\\n\", name); }\n"
                 << "    };\n";
        for (const auto& test : tests) {
            if (opt.unique_test && test.name != *opt.unique_test) continue;
            combined << "    run_test(" << cpp_quote(test.name) << ", "
                     << test.function << ");\n";
        }
        combined << "    rexon::print(\"\\nResults: {} passed, {} failed\\n\", passed, failed);\n"
                 << "    return failed == 0 ? 0 : 1;\n}\n";
    }

    fs::path tempdir = fs::temp_directory_path() / "rexonc";
    fs::create_directories(tempdir);
    fs::path stdhpp = tempdir / "std.hpp";
    fs::path stdcpp = tempdir / "std.cpp";
    write_file(stdhpp, std_hpp);
    write_file(stdcpp, std_cpp);
    write_file(generated, combined.str());

    if (opt.emit_cpp) {
        std::cout << generated.string() << '\n';
        return 0;
    }

    std::string command = "clang++ ";
    command += shell_quote(generated.string()) + " ";
    command += shell_quote(stdcpp.string()) + " ";
    command += "-I" + shell_quote(tempdir.string()) + " ";
    command += "-I" + shell_quote(cwd.string()) + " ";
    command += "-Drexon_version=0.1 ";
    command += "-std=c++26 ";
    for (const auto& d : opt.include_dirs)
        command += "-I" + shell_quote(d.string()) + " ";
    for (const auto& a : opt.clang_args) {
        command += shell_quote(a) + " ";
    }
    command += "-o " + shell_quote(opt.output.string());

    int result = std::system(command.c_str());
    if (!opt.keep_temp) {
        std::error_code ec;
        fs::remove(generated, ec);
    }
    if (result != 0) return 1;
    if (opt.run_tests) {
        const fs::path executable = fs::absolute(opt.output);
        return std::system(shell_quote(executable.string()).c_str()) == 0 ? 0 : 1;
    }
    return 0;
}
