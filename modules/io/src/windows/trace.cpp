#include <weave/io/detail/trace.hpp>
#include <windows.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <memory>

namespace weave {

namespace {

constexpr u32 trace_capacity = 1u << 18;

struct TraceHeader {
  std::array<char, 8> magic{'W', 'E', 'A', 'V', 'E', 'T', 'R', '1'};
  u64 frequency = 0;
  u64 committed = 0;
  u32 process = 0;
  u32 thread = 0;
  u32 capacity = trace_capacity;
  u32 entry_size = 40;
  std::array<std::byte, 24> reserved{};
};

struct TraceEntry {
  u64 ticks;
  u64 object;
  u64 value;
  u64 sequence;
  detail::TraceEvent event;
  u32 reserved;
};

static_assert(sizeof(TraceHeader) == 64);
static_assert(sizeof(TraceEntry) == 40);

struct TraceWriter {
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
  TraceHeader *header = nullptr;
  u64 sequence = 0;

  TraceWriter() noexcept
  {
    std::array<wchar_t, 4096> directory{};
    auto length = GetEnvironmentVariableW(
      L"WEAVE_TRACE_DIRECTORY",
      directory.data(),
      static_cast<DWORD>(directory.size()));
    if (!length || length >= directory.size())
      return;

    std::array<wchar_t, 4200> path{};
    swprintf_s(
      path.data(),
      path.size(),
      L"%s\\%lu-%lu.weavetrace",
      directory.data(),
      GetCurrentProcessId(),
      GetCurrentThreadId());
    file = CreateFileW(
      path.data(),
      GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
    constexpr DWORD size = sizeof(TraceHeader) + trace_capacity * sizeof(TraceEntry);
    if (file != INVALID_HANDLE_VALUE)
      mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, size, nullptr);
    if (mapping)
      header = static_cast<TraceHeader *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, size));
    if (!header) {
      std::fprintf(stderr, "Weave trace setup failed: %lu\n", GetLastError());
      return;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    std::construct_at(header);
    header->frequency = frequency.QuadPart;
    header->process = GetCurrentProcessId();
    header->thread = GetCurrentThreadId();
  }

  ~TraceWriter()
  {
    if (header)
      UnmapViewOfFile(header);
    if (mapping)
      CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
  }

  void write(detail::TraceEvent event, const void *object, u64 value) noexcept
  {
    if (!header)
      return;
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    auto *entries = reinterpret_cast<TraceEntry *>(header + 1);
    std::construct_at(
      &entries[sequence & (trace_capacity - 1)],
      TraceEntry{
        static_cast<u64>(ticks.QuadPart),
        reinterpret_cast<std::uintptr_t>(object),
        value,
        sequence,
        event,
        0});
    std::atomic_ref(header->committed).store(++sequence, std::memory_order_release);
  }
};

thread_local TraceWriter trace_writer;

} // namespace

void detail::trace(TraceEvent event, const void *object, u64 value) noexcept
{
  const auto error = GetLastError();
  trace_writer.write(event, object, value);
  SetLastError(error);
}

} // namespace weave
