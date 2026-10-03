#include <weave/weave.hpp>
#include <array>
#include <cstdio>

// First slice: one connection at a time. No pretend task-group/shutdown API.
weave::Async<weave::Result<void>> serve(weave::TcpListener& listener) {
    for (;;) {
        auto client = co_await listener.accept();
        if (!client) co_return std::unexpected(client.error());
        std::array<std::byte, 4096> buffer;
        for (;;) {
            auto read = co_await client->read(buffer);
            if (!read) {
                std::fprintf(stderr, "Read: %s\n", read.error().message().c_str());
                break;
            }
            if (*read == 0) break;
            auto written = co_await client->write_all(std::span(buffer).first(*read));
            if (!written) {
                std::fprintf(stderr, "Write: %s\n", written.error().message().c_str());
                break;
            }
        }
    }
}

int main() {
    weave::Context ctx;
    auto ready = ctx.status();
    if (!ready) { std::fprintf(stderr, "%s\n", ready.error().message().c_str()); return 1; }
    auto listener = ctx.listen("127.0.0.1", 8080);
    if (!listener) { std::fprintf(stderr, "%s\n", listener.error().message().c_str()); return 1; }
    std::puts("Weave echo: 127.0.0.1:8080 (one client at a time; Ctrl+C terminates)");
    auto result = ctx.block_on(serve(*listener));
    if (!result) { std::fprintf(stderr, "%s\n", result.error().message().c_str()); return 1; }
}
