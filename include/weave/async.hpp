#pragma once

#include <coroutine>
#include <array>
#include <cstdlib>
#include <optional>
#include <utility>

namespace weave {
namespace detail {
template<std::size_t N> struct AllAwaiter;
inline void require(bool condition) noexcept {
    if (!condition) std::abort();
}

template<class T> struct AsyncValue {
    std::optional<T> value;
    template<class U> void return_value(U&& result) {
        value.emplace(std::forward<U>(result));
    }
    T take() { return std::move(*value); }
};
template<> struct AsyncValue<void> {
    void return_void() noexcept {}
    void take() noexcept {}
};
}

class Context;

// Lazy, move-only, single-consumer operation. Never destroy a suspended operation.
template<class T = void> class [[nodiscard]] Async {
public:
    struct promise_type : detail::AsyncValue<T> {
        std::coroutine_handle<> continuation = std::noop_coroutine();
        bool started = false;
        std::coroutine_handle<> (*completion)(void*) noexcept = nullptr;
        void* completion_state = nullptr;
        Async get_return_object() noexcept {
            return Async{Handle::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().completion)
                    return h.promise().completion(h.promise().completion_state);
                return h.promise().continuation;
            }
            void await_resume() noexcept {}
        };
        Final final_suspend() noexcept { return {}; }
        void unhandled_exception() noexcept { std::abort(); }
    };
    using Handle = std::coroutine_handle<promise_type>;

    Async(Async&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;
    ~Async() {
        if (handle_) {
            detail::require(!handle_.promise().started || handle_.done());
            handle_.destroy();
        }
    }

    struct Awaiter {
        Handle handle;
        bool await_ready() noexcept { return false; }
        Handle await_suspend(std::coroutine_handle<> caller) noexcept {
            detail::require(handle && !handle.promise().started);
            handle.promise().started = true;
            handle.promise().continuation = caller;
            return handle;
        }
        T await_resume() { return handle.promise().take(); }
    };
    Awaiter operator co_await() && noexcept { return {handle_}; }

private:
    friend class Context;
    template<std::size_t N> friend struct detail::AllAwaiter;
    explicit Async(Handle handle) noexcept : handle_(handle) {}
    Handle handle_;
};

namespace detail {
template<std::size_t N> struct AllAwaiter {
    std::array<Async<void>, N> operations;
    std::size_t remaining = N;
    std::coroutine_handle<> continuation{};
    bool armed = false;
    bool await_ready() const noexcept { return N == 0; }
    static std::coroutine_handle<> complete(void* state) noexcept {
        auto& self = *static_cast<AllAwaiter*>(state);
        --self.remaining;
        if (self.remaining == 0 && self.armed) return self.continuation;
        return std::noop_coroutine();
    }
    bool await_suspend(std::coroutine_handle<> caller) noexcept {
        continuation = caller;
        for (auto& operation : operations) {
            auto h = operation.handle_;
            require(h && !h.promise().started);
            h.promise().started = true;
            h.promise().completion = complete;
            h.promise().completion_state = this;
            h.resume();
        }
        armed = true;
        return remaining != 0;
    }
    void await_resume() const noexcept {}
};
}

// Starts children together and retains every frame until all children finish.
// Child errors must be handled by each Async<void>; no implicit cancellation.
template<class... Operations>
Async<void> when_all(Operations... operations) {
    co_await detail::AllAwaiter<sizeof...(Operations)>{{std::move(operations)...}};
}
}
