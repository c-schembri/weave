# Addresses And DNS

Include `<weave/address.hpp>` for `IpAddress`, `Endpoint`, and `AddressFamily`.
Include `<weave/resolve.hpp>` for async resolution. Both belong to `weave::io`,
so DNS and address handling do not require linking TCP or Runtime.

## Numeric Addresses

```cpp
auto address = weave::IpAddress::parse("fe80::1%12");
auto endpoint = weave::Endpoint::parse("::1", 8080);
weave::Endpoint loopback{weave::IpAddress::loopback_v6(), 8080};
```

Parsing is synchronous and returns `Result<T>`. IPv4 uses full dotted decimal;
IPv6 accepts numeric scope IDs (`%12`), not interface names. Brackets and ports
are not part of the address input. `Endpoint::to_string()` adds the brackets
when formatting IPv6, for example `[::1]:8080`. Values own their address bytes
and do not borrow text. Parsing and formatting do not require a Context.

## Listening

```cpp
// Inside a Task; IPv6-only by default:
auto listener = co_await weave::tcp::listen(
  weave::Endpoint{weave::IpAddress::loopback_v6(), 8080});

// Opt into IPv4 and IPv6 on a wildcard IPv6 socket:
auto dual = co_await weave::tcp::listen(
  weave::Endpoint{weave::IpAddress::any_v6(), 8081},
  {.backlog = 512, .ipv6_only = false});
```

The explicit-Context overload is synchronous:
`tcp::listen(ctx, endpoint, options)` returns `Result<TcpListener>`.
Existing `listen(ctx, "127.0.0.1", port, backlog)` and contextless string calls
remain valid, and now accept numeric IPv6 too. Listening never resolves names.
String inputs are borrowed until setup finishes; an owned `Endpoint` avoids
that lifetime requirement. `tcp::serve(endpoint, options, handler)` supports
the same IPv6 policy through `ServeOptions::ipv6_only`.

`listener.local_endpoint()` and `local_port()` are cached synchronous values,
retained after close. A moved-from listener reports the default endpoint/zero
port. `stream.local_endpoint()` and `peer_endpoint()` query the socket and
return synchronous `Result<Endpoint>`; closed sockets can fail. Dual-stack
accepted sockets expose IPv4 peers as IPv4-mapped IPv6 addresses, as Windows does.

## Resolution And Connection

```cpp
// Inside a Task:
auto client = co_await weave::tcp::connect("localhost", 8080);

// Own dynamic host strings across lazy execution:
auto other = co_await weave::tcp::connect(std::string("localhost"), 8081);

// Resolve separately and choose a family:
auto endpoints = co_await weave::resolve(
  "localhost", 8082, {.family = weave::AddressFamily::v6});
auto third = co_await weave::tcp::connect(std::move(endpoints));
```

`resolve` owns its UTF-8 hostname, returns `Task<std::vector<Endpoint>>`, and
supports explicit-Context and contextless overloads. Numeric literals bypass
DNS and honor the family filter. Results preserve OS order and remove duplicate
endpoints. Empty results fail rather than reporting an empty successful lookup.
Invalid input, embedded NULs, URL syntax, and malformed numeric literals fail.
Port values are numeric; service-name lookup is not provided.

`connect(endpoint)` also bypasses DNS. `connect(vector<Endpoint>)` owns its list
and tries each address sequentially, closing failed attempts before advancing.
All failures return the last connection error; an empty list is invalid.
Hostname connections resolve first and then use this same fallback. Cancellation
stops the sequence. There is no parallel Happy Eyeballs racing or DNS cache owned
by Weave; system resolver behavior still applies.

The `const char *` connection overload borrows its string until setup completes.
Use `std::string` by value when constructing tasks from temporary/dynamic strings.
Both the vector and string overloads own their input in their coroutine frame.
Contextless calls select the active Context when executed, not when constructed.

## Cancellation And Completion

Windows resolution uses the native overlapped
[GetAddrInfoExW API](https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfoexw),
not blocking `getaddrinfo` on I/O workers or a Weave helper-thread pool.
Completion is posted through the existing Context/executor routing, preserving
affinity and work-stealing serialization. The query's Winsock reference, result
list, strings, and native record remain alive until native completion drains.

Task cancellation and Context shutdown request
[GetAddrInfoExCancel](https://learn.microsoft.com/en-us/windows/win32/api/ws2tcpip/nf-ws2tcpip-getaddrinfoexcancel).
Cancellation is not permission to destroy the query: Weave waits for its callback,
then synchronizes cancellation callbacks and frees results before returning.
Cancelled operations report `std::errc::operation_canceled`; completion can win
a cancellation race. `weave::timeout` can bound a lookup cooperatively, but must
still drain cancellation and is not a hard native teardown deadline.

Linux uses glibc's asynchronous `getaddrinfo_a` notification API, with the same
Context/executor routing and retained query lifetimes. Numeric addresses bypass
it. Already-running lookups may resist native cancellation; Weave still waits
for notification before returning cancellation. glibc owns its resolver threads;
Weave does not run blocking DNS on I/O workers. [Linux details](linux.md).

Public headers contain no native OS types.
