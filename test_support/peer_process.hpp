#pragma once
#include "async_echo_peer.hpp"
#include <array>
#include <cstdint>
#include <string>

namespace experiment {

inline constexpr std::size_t peer_workers = 4;

struct Handle {
  HANDLE value = nullptr;
  Handle() = default;
  Handle(const Handle &) = delete;

  ~Handle()
  {
    reset();
  }

  void reset()
  {
    if (value)
      CloseHandle(std::exchange(value, nullptr));
  }
};

struct PeerReady {
  std::uint32_t magic = 0x57455632;
  std::uint32_t error = 0;
  std::array<std::uint16_t, peer_workers> ports{};
};

inline int serve_peer()
{
  support::AsyncEchoPeer peer(peer_workers);
  PeerReady ready;
  ready.error = static_cast<std::uint32_t>(peer.error().value());
  if (!ready.error) {
    for (std::size_t i = 0; i < peer_workers; ++i)
      ready.ports[i] = peer.port(i);
  }
  DWORD written = 0;
  if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), &ready, sizeof(ready), &written, nullptr) || written != sizeof(ready))
    return 1;
  if (ready.error)
    return 1;
  char stop;
  DWORD read = 0;
  ReadFile(GetStdHandle(STD_INPUT_HANDLE), &stop, 1, &read, nullptr);
  peer.stop();
  return peer.wait_idle() && !peer.error() ? 0 : 1;
}

// A separate process keeps echo-peer CPU out of the client's process counters.
// The job also reclaims the peer if a test runner terminates this executable.
class PeerProcess {
  Handle process_, control_, output_, job_;
  PeerReady ready_;
  bool valid_ = false, stopped_ = false, clean_ = false;

public:
  explicit PeerProcess(DWORD_PTR cpu_mask = 0)
  {
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input, output;
    if (!CreatePipe(&input.value, &control_.value, &security, 0) ||
      !CreatePipe(&output_.value, &output.value, &security, 0) ||
      !SetHandleInformation(control_.value, HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(output_.value, HANDLE_FLAG_INHERIT, 0))
      return;
    job_.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_.value || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
      return;
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size())
      return;
    std::wstring command = L"\"" + std::wstring(path.data()) + L"\" --weave-peer";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.value;
    startup.hStdOutput = output.value;
    startup.hStdError = output.value;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(
          path.data(),
          command.data(),
          nullptr,
          nullptr,
          TRUE,
          CREATE_NO_WINDOW | CREATE_SUSPENDED,
          nullptr,
          nullptr,
          &startup,
          &child))
      return;
    process_.value = child.hProcess;
    Handle thread;
    thread.value = child.hThread;
    if (!AssignProcessToJobObject(job_.value, process_.value) ||
      (cpu_mask && !SetProcessAffinityMask(process_.value, cpu_mask)) ||
      ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
      TerminateProcess(process_.value, 2);
      WaitForSingleObject(process_.value, INFINITE);
      return;
    }
    input.reset();
    output.reset();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
      DWORD available = 0;
      if (!PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr))
        return;
      if (available >= sizeof(ready_)) {
        DWORD read = 0;
        valid_ = ReadFile(output_.value, &ready_, sizeof(ready_), &read, nullptr) && read == sizeof(ready_) &&
          ready_.magic == 0x57455632 && ready_.error == 0;
        return;
      }
      if (WaitForSingleObject(process_.value, 0) != WAIT_TIMEOUT)
        return;
      Sleep(1);
    }
  }

  ~PeerProcess()
  {
    stop();
  }

  bool valid() const
  {
    return valid_;
  }

  HANDLE process() const
  {
    return process_.value;
  }

  std::uint16_t port(std::size_t connection) const
  {
    return ready_.ports[connection % peer_workers];
  }

  bool stop()
  {
    if (stopped_)
      return clean_;
    stopped_ = true;
    control_.reset();
    if (!process_.value)
      return false;
    if (WaitForSingleObject(process_.value, 15000) != WAIT_OBJECT_0) {
      TerminateProcess(process_.value, 2);
      WaitForSingleObject(process_.value, INFINITE);
      return false;
    }
    DWORD result = 1;
    clean_ = GetExitCodeProcess(process_.value, &result) && result == 0;
    return clean_;
  }
};

} // namespace experiment
