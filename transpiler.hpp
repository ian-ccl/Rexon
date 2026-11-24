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
#include "lexer.hpp"

namespace rexon_compiler {
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
            if (t[k].text == "c_int") s += "rexon::c_int";
            else s += token_text(t[k]);
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

        if (last == first + 1 && t[first].text == "c_int")
            return "rexon::c_int";

        if (last == first + 1 && t[first].text == "mbool")
            return "rexon::mbool";

        // T? -> std::optional<T>
        if (last > first && t[last - 1].text == "?")
            return "std::optional<" + render_type(first, last - 1) + ">";

        // T[N] -> rexon::Array<T, N>; T[] -> rexon::Slice<T>.
        // Recurse so types such as char[][] become Slice<Slice<char>>.
        if (last >= first + 2 && t[last - 1].text == "]") {
            std::size_t open = last - 2;
            while (open > first && t[open].text != "[") --open;
            if (t[open].text == "[") {
                std::string base = convert_type(first, open);
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
                    s += "static_cast<" + convert_type(args[0].first, args[0].second) + ">(" + value + ")";
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
            emit("static_cast<" + convert_type(args[0].first, args[0].second) + ">(" + value + ")");
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

    void parse_block(std::string_view prologue = {}) {
        if (!eat("{")) { error("expected '{'"); return; }
        emit("{\n");
        emit(prologue);
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

        const bool main_with_args = name == "main" && parts.size() == 1 &&
            convert_type(parts[0].first, parts[0].second - 1) ==
                "rexon::Slice<rexon::Slice<char>>" &&
            t[parts[0].second - 1].text == "args";

        std::size_t ret_first = i;
        while (cur().kind != Kind::End && !is("{") && !is(";")) ++i;
        std::string ret = ret_first == i ? "void" : convert_type(ret_first, i);

        if (is_static) emit("static ");
        if (name == "main") {
            if (main_with_args)
                emit("auto main(int argc, char** argv) -> int ");
            else
                emit("auto main() -> int ");
        }
        else
            emit("auto " + name + "(" + params + ") -> " + ret + " ");

        if (eat(";")) {
            emit(";\n");
            return;
        }
        if (main_with_args) {
            parse_block(
                "    std::vector<rexon::Slice<char>> __rexon_args_storage;\n"
                "    __rexon_args_storage.reserve(static_cast<std::size_t>(argc));\n"
                "    for (int i = 0; i < argc; ++i)\n"
                "        __rexon_args_storage.push_back({argv[i], std::strlen(argv[i])});\n"
                "    rexon::Slice<rexon::Slice<char>> args{\n"
                "        __rexon_args_storage.data(), __rexon_args_storage.size()};\n");
        } else {
            parse_block();
        }
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


} // namespace rexon_compiler

