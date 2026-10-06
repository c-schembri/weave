#include <weave/sync/detail/wait.hpp>

namespace weave {

namespace {

class SyncCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.sync";
  }

  std::string message(int error) const override
  {
    return error == static_cast<int>(SyncError::closed) ? "Synchronization primitive closed" : "Unknown sync error";
  }
};

} // namespace

Error make_error_code(SyncError error) noexcept
{
  static SyncCategory category;

  return {static_cast<int>(error), category};
}

detail::SyncWait::~SyncWait()
{
  disarm();

  require(phase != Phase::queued);
}

void detail::SyncWait::disarm() noexcept
{
  cancellation.reset();
  shutdown.reset();
}

void detail::CancelWait::operator()() const noexcept
{
  std::lock_guard lock(wait->domain.mutex);
  if (wait->phase == SyncWait::Phase::done)
    return;

  const auto error = std::make_error_code(std::errc::operation_canceled);
  if (wait->phase == SyncWait::Phase::queued)
    wait->finish(error);
  else
    wait->error = error;
}

void detail::SyncWait::finish(Error result) noexcept
{
  const bool pending = phase == Phase::queued;
  if (pending)
    queue->remove(*this);

  error = result;
  phase = Phase::done;

  if (pending)
    post(*event.target, event);
}

void detail::WaitQueue::push(SyncWait &wait) noexcept
{
  require(wait.phase == SyncWait::Phase::initial && !wait.queue);

  wait.queue = this;
  wait.previous = last;
  if (last)
    last->next = &wait;
  else
    first = &wait;

  last = &wait;
  wait.phase = SyncWait::Phase::queued;
}

void detail::WaitQueue::remove(SyncWait &wait) noexcept
{
  require(wait.queue == this);

  if (wait.previous)
    wait.previous->next = wait.next;
  else
    first = wait.next;

  if (wait.next)
    wait.next->previous = wait.previous;
  else
    last = wait.previous;

  wait.queue = nullptr;
  wait.previous = wait.next = nullptr;
}

} // namespace weave
