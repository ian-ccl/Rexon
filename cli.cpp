#include "cli.hpp"
#include "runtime_sources.hpp"

namespace rexon_compiler {

static const char* version = "0.3.0";

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



int run(int argc, char** argv) {
    auto parsed = parse_args(argc, argv);
    if (!parsed) return 2;
    Options opt = *parsed;
    if (opt.get_std_hpp) {
        std::cout << rexon_sources::std_hpp;
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
    write_file(stdhpp, rexon_sources::std_hpp);
    write_file(stdcpp, rexon_sources::std_cpp);
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
    command += std::format("-Drexon_version={} ", version);
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

} // namespace rexon_compiler

