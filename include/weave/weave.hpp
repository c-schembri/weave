#pragma once

#include <weave/async.hpp>
#include <winsock2.h>
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <system_error>
#include <thread>

namespace weave {
using Error = std::error_code;
template<class T> using Result = std::expected<T, Error>;

class TcpStream;
class TcpListener;
namespace detail {
// Hot state is embedded in the suspended caller's coroutine frame.
struct Operation {
    OVERLAPPED overlapped{};
    std::coroutine_handle<> continuation{};
    SOCKET socket = INVALID_SOCKET;
    DWORD transferred = 0;
    Error error{};
};
}

class Context {
public:
    Context() noexcept;
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Result<void> status() const noexcept;
    Result<TcpListener> listen(const char* ipv4, std::uint16_t port, int backlog = SOMAXCONN);
    Async<Result<TcpStream>> connect(const char* ipv4, std::uint16_t port);

    template<class T> T block_on(Async<T> operation) {
        check_thread();
        detail::require(!running_ && !error_ && operation.handle_);
        running_ = true;
        operation.handle_.promise().started = true;
        operation.handle_.resume();
        while (!operation.handle_.done()) poll();
        running_ = false;
        return operation.handle_.promise().take();
    }

    struct Metrics {
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t dequeue_calls = 0;
    };
    const Metrics& metrics() const noexcept { return metrics_; }

private:
    friend class TcpStream;
    friend class TcpListener;
    friend struct IoAwaiter;
    void check_thread() const noexcept;
    Result<SOCKET> make_socket();
    void poll();
    HANDLE port_ = nullptr;
    Error error_;
    std::thread::id owner_ = std::this_thread::get_id();
    std::array<OVERLAPPED_ENTRY, 64> completions_{};
    Metrics metrics_{};
    std::size_t handles_ = 0;
    bool winsock_ = false;
    bool running_ = false;
};

class TcpStream {
public:
    TcpStream(TcpStream&& other) noexcept;
    TcpStream(const TcpStream&) = delete;
    ~TcpStream();
    Async<Result<std::size_t>> read(std::span<std::byte> buffer);
    Async<Result<void>> read_exactly(std::span<std::byte> buffer);
    Async<Result<void>> write_all(std::span<const std::byte> buffer);
    Result<void> shutdown_send();
    Result<void> cancel();
    Result<void> close();
    Result<void> no_delay(bool enabled = true);
private:
    friend class Context;
    friend class TcpListener;
    TcpStream(Context& ctx, SOCKET socket) noexcept : ctx_(&ctx), socket_(socket) {}
    Context* ctx_;
    SOCKET socket_;
    bool reading_ = false;
    bool writing_ = false;
};

class TcpListener {
public:
    TcpListener(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    ~TcpListener();
    Async<Result<TcpStream>> accept();
    Result<std::uint16_t> local_port() const;
    Result<void> cancel();
    Result<void> close();
private:
    friend class Context;
    TcpListener(Context& ctx, SOCKET socket) noexcept : ctx_(&ctx), socket_(socket) {}
    Context* ctx_;
    SOCKET socket_;
    bool accepting_ = false;
};
}
