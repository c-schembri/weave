#include <weave/weave.hpp>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <limits>
#include <type_traits>

namespace weave {
namespace {
Error win_error(int code) { return {code, std::system_category()}; }
Error last_error() { return win_error(WSAGetLastError()); }
Error invalid() { return std::make_error_code(std::errc::invalid_argument); }
Error busy() { return std::make_error_code(std::errc::operation_in_progress); }
Result<sockaddr_in> address(const char* ipv4, std::uint16_t port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(port);
    if (!ipv4 || InetPtonA(AF_INET, ipv4, &result.sin_addr) != 1)
        return std::unexpected(invalid());
    return result;
}
template<class T> Result<T> extension(SOCKET socket, GUID id) {
    T function = nullptr;
    DWORD bytes = 0;
    if (WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id),
                 &function, sizeof(function), &bytes, nullptr, nullptr) != 0)
        return std::unexpected(last_error());
    return function;
}
struct Flag {
    bool& value;
    explicit Flag(bool& flag) : value(flag) { value = true; }
    ~Flag() { value = false; }
};
}

struct IoAwaiter {
    enum Kind { receive, send, accept, connect } kind;
    Context& ctx;
    detail::Operation operation{};
    WSABUF buffer{};
    SOCKET accepted = INVALID_SOCKET;
    sockaddr_in endpoint{};
    LPFN_ACCEPTEX accept_fn = nullptr;
    LPFN_CONNECTEX connect_fn = nullptr;
    std::array<std::byte, 2 * (sizeof(sockaddr_in) + 16)> addresses{};

    IoAwaiter(Kind k, Context& c, SOCKET s) : kind(k), ctx(c) { operation.socket = s; }
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> continuation) noexcept {
        ctx.check_thread();
        operation.continuation = continuation;
        DWORD flags = 0;
        DWORD bytes = 0;
        int result = 0;
        switch (kind) {
        case receive:
            result = WSARecv(operation.socket, &buffer, 1, &bytes, &flags,
                             &operation.overlapped, nullptr);
            break;
        case send:
            result = WSASend(operation.socket, &buffer, 1, &bytes, 0,
                             &operation.overlapped, nullptr);
            break;
        case accept:
            result = accept_fn(operation.socket, accepted, addresses.data(), 0,
                sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16,
                &bytes, &operation.overlapped) ? 0 : SOCKET_ERROR;
            break;
        case connect:
            result = connect_fn(operation.socket, reinterpret_cast<sockaddr*>(&endpoint),
                sizeof(endpoint), nullptr, 0, &bytes, &operation.overlapped) ? 0 : SOCKET_ERROR;
            break;
        }
        if (result != 0) {
            auto error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                operation.error = win_error(error);
                return false;
            }
        }
        // Synchronous success ALSO queues an IOCP packet. Never resume twice.
        ++ctx.metrics_.submitted;
        return true;
    }
    Result<std::size_t> await_resume() const noexcept {
        if (operation.error) return std::unexpected(operation.error);
        return operation.transferred;
    }
};

Context::Context() noexcept {
    WSADATA data{};
    if (auto result = WSAStartup(MAKEWORD(2, 2), &data); result != 0) {
        error_ = win_error(result);
        return;
    }
    winsock_ = true;
    port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (!port_) error_ = win_error(static_cast<int>(GetLastError()));
}
Context::~Context() {
    check_thread();
    detail::require(!running_ && handles_ == 0 && metrics_.submitted == metrics_.completed);
    if (port_) CloseHandle(port_);
    if (winsock_) WSACleanup();
}
void Context::check_thread() const noexcept { detail::require(owner_ == std::this_thread::get_id()); }
Result<void> Context::status() const noexcept {
    if (error_) return std::unexpected(error_);
    return {};
}
Result<SOCKET> Context::make_socket() {
    check_thread();
    if (error_) return std::unexpected(error_);
    SOCKET socket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket == INVALID_SOCKET) return std::unexpected(last_error());
    if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket), port_, 0, 0)) {
        auto error = win_error(static_cast<int>(GetLastError()));
        closesocket(socket);
        return std::unexpected(error);
    }
    ++handles_;
    return socket;
}
void Context::poll() {
    ULONG count = 0;
    ++metrics_.dequeue_calls;
    if (!GetQueuedCompletionStatusEx(port_, completions_.data(),
        static_cast<ULONG>(completions_.size()), &count, INFINITE, FALSE)) {
        // Port failure is a runtime invariant failure, not an individual I/O error.
        std::abort();
    }
    static_assert(std::is_standard_layout_v<detail::Operation>);
    static_assert(offsetof(detail::Operation, overlapped) == 0);
    for (ULONG i = 0; i < count; ++i) {
        const auto entry = completions_[i];
        auto* op = reinterpret_cast<detail::Operation*>(entry.lpOverlapped);
        detail::require(op != nullptr);
        op->transferred = entry.dwNumberOfBytesTransferred;
        if (entry.Internal != 0) {
            DWORD flags = 0, transferred = 0;
            if (!WSAGetOverlappedResult(op->socket, &op->overlapped, &transferred, FALSE, &flags))
                op->error = last_error();
        }
        ++metrics_.completed;
        // Resumption may destroy this operation. Do not touch op afterward.
        op->continuation.resume();
    }
}

Result<TcpListener> Context::listen(const char* ipv4, std::uint16_t port, int backlog) {
    check_thread();
    auto endpoint = address(ipv4, port);
    if (!endpoint) return std::unexpected(endpoint.error());
    auto socket = make_socket();
    if (!socket) return std::unexpected(socket.error());
    TcpListener listener(*this, *socket);
    BOOL exclusive = TRUE;
    if (setsockopt(*socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
        bind(*socket, reinterpret_cast<const sockaddr*>(&*endpoint), sizeof(*endpoint)) != 0 ||
        ::listen(*socket, backlog) != 0)
        return std::unexpected(last_error());
    return listener;
}

Async<Result<TcpStream>> Context::connect(const char* ipv4, std::uint16_t port) {
    check_thread();
    auto endpoint = address(ipv4, port);
    if (!endpoint) co_return std::unexpected(endpoint.error());
    auto socket = make_socket();
    if (!socket) co_return std::unexpected(socket.error());
    TcpStream stream(*this, *socket);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    if (bind(*socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
        co_return std::unexpected(last_error());
    auto function = extension<LPFN_CONNECTEX>(*socket, WSAID_CONNECTEX);
    if (!function) co_return std::unexpected(function.error());
    IoAwaiter operation(IoAwaiter::connect, *this, *socket);
    operation.endpoint = *endpoint;
    operation.connect_fn = *function;
    auto connected = co_await operation;
    if (!connected) co_return std::unexpected(connected.error());
    if (setsockopt(*socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0)
        co_return std::unexpected(last_error());
    co_return std::move(stream);
}

TcpStream::TcpStream(TcpStream&& other) noexcept : ctx_(other.ctx_), socket_(INVALID_SOCKET) {
    detail::require(!other.reading_ && !other.writing_);
    socket_ = std::exchange(other.socket_, INVALID_SOCKET);
}
TcpStream::~TcpStream() { detail::require(static_cast<bool>(close())); }
Result<void> TcpStream::close() {
    ctx_->check_thread();
    if (reading_ || writing_) return std::unexpected(busy());
    if (socket_ != INVALID_SOCKET) {
        if (closesocket(socket_) != 0) return std::unexpected(last_error());
        socket_ = INVALID_SOCKET;
        --ctx_->handles_;
    }
    return {};
}
Result<void> TcpStream::no_delay(bool enabled) {
    ctx_->check_thread();
    BOOL value = enabled;
    if (setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&value), sizeof(value)) != 0)
        return std::unexpected(last_error());
    return {};
}
Result<void> TcpStream::shutdown_send() {
    ctx_->check_thread();
    if (writing_) return std::unexpected(busy());
    if (shutdown(socket_, SD_SEND) != 0) return std::unexpected(last_error());
    return {};
}
Result<void> TcpStream::cancel() {
    ctx_->check_thread();
    if (socket_ == INVALID_SOCKET) return std::unexpected(win_error(WSAENOTSOCK));
    if (!CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr)) {
        auto error = GetLastError();
        if (error != ERROR_NOT_FOUND) return std::unexpected(win_error(static_cast<int>(error)));
    }
    return {};
}
Async<Result<std::size_t>> TcpStream::read(std::span<std::byte> buffer) {
    ctx_->check_thread();
    if (reading_) co_return std::unexpected(busy());
    if (socket_ == INVALID_SOCKET) co_return std::unexpected(win_error(WSAENOTSOCK));
    if (buffer.empty()) co_return std::size_t{0};
    Flag guard(reading_);
    IoAwaiter operation(IoAwaiter::receive, *ctx_, socket_);
    operation.buffer.buf = reinterpret_cast<char*>(buffer.data());
    operation.buffer.len = static_cast<ULONG>((std::min)(buffer.size(),
        static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
    co_return co_await operation;
}
Async<Result<void>> TcpStream::read_exactly(std::span<std::byte> buffer) {
    // read() owns the in-flight guard; the context runs continuations serially.
    while (!buffer.empty()) {
        auto n = co_await read(buffer);
        if (!n) co_return std::unexpected(n.error());
        if (*n == 0) co_return std::unexpected(std::make_error_code(std::errc::connection_reset));
        buffer = buffer.subspan(*n);
    }
    co_return Result<void>{};
}
Async<Result<void>> TcpStream::write_all(std::span<const std::byte> buffer) {
    ctx_->check_thread();
    if (writing_) co_return std::unexpected(busy());
    if (socket_ == INVALID_SOCKET) co_return std::unexpected(win_error(WSAENOTSOCK));
    Flag guard(writing_);
    while (!buffer.empty()) {
        IoAwaiter operation(IoAwaiter::send, *ctx_, socket_);
        operation.buffer.buf = const_cast<char*>(reinterpret_cast<const char*>(buffer.data()));
        operation.buffer.len = static_cast<ULONG>((std::min)(buffer.size(),
            static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
        auto n = co_await operation;
        if (!n) co_return std::unexpected(n.error());
        if (*n == 0) co_return std::unexpected(std::make_error_code(std::errc::broken_pipe));
        buffer = buffer.subspan(*n);
    }
    co_return Result<void>{};
}

TcpListener::TcpListener(TcpListener&& other) noexcept : ctx_(other.ctx_), socket_(INVALID_SOCKET) {
    detail::require(!other.accepting_);
    socket_ = std::exchange(other.socket_, INVALID_SOCKET);
}
TcpListener::~TcpListener() { detail::require(static_cast<bool>(close())); }
Result<void> TcpListener::close() {
    ctx_->check_thread();
    if (accepting_) return std::unexpected(busy());
    if (socket_ != INVALID_SOCKET) {
        if (closesocket(socket_) != 0) return std::unexpected(last_error());
        socket_ = INVALID_SOCKET;
        --ctx_->handles_;
    }
    return {};
}
Result<std::uint16_t> TcpListener::local_port() const {
    ctx_->check_thread();
    sockaddr_in endpoint{};
    int size = sizeof(endpoint);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&endpoint), &size) != 0)
        return std::unexpected(last_error());
    return ntohs(endpoint.sin_port);
}
Result<void> TcpListener::cancel() {
    ctx_->check_thread();
    if (socket_ == INVALID_SOCKET) return std::unexpected(win_error(WSAENOTSOCK));
    if (!CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr)) {
        auto error = GetLastError();
        if (error != ERROR_NOT_FOUND) return std::unexpected(win_error(static_cast<int>(error)));
    }
    return {};
}
Async<Result<TcpStream>> TcpListener::accept() {
    ctx_->check_thread();
    if (accepting_) co_return std::unexpected(busy());
    Flag guard(accepting_);
    auto function = extension<LPFN_ACCEPTEX>(socket_, WSAID_ACCEPTEX);
    if (!function) co_return std::unexpected(function.error());
    auto socket = ctx_->make_socket();
    if (!socket) co_return std::unexpected(socket.error());
    TcpStream stream(*ctx_, *socket);
    IoAwaiter operation(IoAwaiter::accept, *ctx_, socket_);
    operation.accepted = *socket;
    operation.accept_fn = *function;
    auto accepted = co_await operation;
    if (!accepted) co_return std::unexpected(accepted.error());
    if (setsockopt(*socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<const char*>(&socket_), sizeof(socket_)) != 0)
        co_return std::unexpected(last_error());
    co_return std::move(stream);
}
}
