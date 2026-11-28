#pragma once
#include <string_view>

namespace rexon_sources {
inline constexpr std::string_view std_hpp = R"REXON(
#pragma once
#include <cassert>
#include <chrono>
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
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace rexon {
    inline bool test_mode = false;
    // Rexon integer aliases with platform-independent widths.
    using int64 = std::int64_t;
    using uint64 = std::uint64_t;
    using ssize_t = std::make_signed_t<std::size_t>;

    // C's `int` type, exposed explicitly for C interop declarations.
    using c_int = int;

    template<class T>
    struct Slice {
        T* data{};
    private:
        std::size_t length_{};

    public:
        constexpr Slice() = default;
        constexpr Slice(T* pointer, std::size_t length) : data(pointer), length_(length) {}
        template<std::size_t N>
        Slice(const std::remove_cv_t<T> (&value)[N]) : data(value), length_(N) {
            if constexpr (std::is_same_v<std::remove_cv_t<T>, char>) {
                if (length_ && value[length_ - 1] == '\0') length_ -= 1;
            }
        }
        constexpr std::size_t size() const { return length_; }
        T& operator[](std::size_t i) const { return data[i]; }
        T* begin() const { return data; }
        T* end() const { return data + length_; }
        constexpr bool is_valid_idx(std::size_t i) const { return i < length_; }
        friend std::ostream& operator<<(std::ostream& os, const Slice& slice) {
            if constexpr (std::is_same_v<std::remove_cv_t<T>, char>) {
                for (std::size_t i = 0; i < slice.length_; ++i) os << slice.data[i];
            } else {
                for (std::size_t i = 0; i < slice.length_; ++i) {
                    if (i) os << ", ";
                    os << slice.data[i];
                }
            }
            return os;
        }
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
        if (!slice.data) { slice = {}; return; }
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
        if (added > std::numeric_limits<std::size_t>::max() - slice.size() ||
            appended.size() > added || !slice.data) return;
        AllocationHeader* old = allocation_header(slice.data);
        if (!old || old->kind != AllocationKind::array || old->count != slice.size())
            return;
        if constexpr (!std::is_nothrow_constructible_v<T, const T&> ||
                      !std::is_nothrow_default_constructible_v<T>) return;
        T* replacement = allocate_storage<T>(ok, slice.size() + added,
                                              AllocationKind::array);
        if (!replacement) return;
        std::size_t built = 0;
        for (; built < slice.size(); ++built)
            ::new (static_cast<void*>(replacement + built)) T(slice.data[built]);
        std::size_t appended_count = 0;
        for (const T& value : appended)
            ::new (static_cast<void*>(replacement + built + appended_count++)) T(value);
        for (; built + appended_count < slice.size() + added; ++built)
            ::new (static_cast<void*>(replacement + built)) T();
        for (std::size_t i = 0; i < old->count; ++i) old->destroy(slice.data + i);
        unlink_allocation(old);
        std::free(static_cast<void*>(old));
        slice = {replacement, slice.size() + added};
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
        constexpr bool is_valid_idx(std::size_t i) const { return i < N; }
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

    inline std::string getline() {
        std::string line;
        std::getline(std::cin, line);
        return line;
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

    // Correctly-spelled alias kept alongside fexits for compatibility.
    inline bool fexists(const char* path) {
        return fexits(path);
    }

    inline std::string cwd() {
        return std::filesystem::current_path().string();
    }

    inline std::string env(const char* name) {
        if (!name) return {};
        const char* value = std::getenv(name);
        return value ? value : "";
    }

    inline bool mkdir_all(const char* path) {
        if (!path) return false;
        std::error_code ec;
        return std::filesystem::create_directories(path, ec) ||
               (std::filesystem::is_directory(path, ec) && !ec);
    }

    inline bool remove_file(const char* path) {
        if (!path) return false;
        std::error_code ec;
        return std::filesystem::remove(path, ec) && !ec;
    }

    inline void sleep_ms(std::int64_t milliseconds) {
        if (milliseconds <= 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }

    inline int shell_status(int status) {
        if (status == -1) return -1;
#ifdef _WIN32
        return status;
#else
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return status;
#endif
    }

    inline std::string shell_quote(const char* command) {
        std::string quoted = "'";
        if (command) {
            for (char c = *command; c; c = *++command) {
                if (c == '\'') quoted += "'\\''";
                else quoted += c;
            }
        }
        return quoted + "'";
    }

    // Execute with the platform's default command interpreter, or explicitly
    // with bash, zsh, cmd, powershell, or pwsh. The command's stdout/stderr
    // remain attached to the Rexon process and the normalized exit code is
    // returned to the caller.
    inline int shell(const char* command) {
        return shell_status(std::system(command ? command : ""));
    }

    inline int shell(const char* interpreter, const char* command) {
        if (!interpreter || !command) return -1;
        std::string name(interpreter);
        std::string invocation;
        if (name == "bash" || name == "zsh") {
            invocation = name + " -c " + shell_quote(command);
        } else if (name == "powershell" || name == "pwsh") {
            invocation = name + " -NoProfile -Command " + shell_quote(command);
        } else if (name == "cmd") {
            invocation = "cmd /C \"" + std::string(command) + "\"";
        } else {
            return -1;
        }
        return shell_status(std::system(invocation.c_str()));
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

}

