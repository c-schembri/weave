#pragma once

#include <weave/mysql/row.hpp>

namespace weave::mysql {

class Connection;
class Statement;

/// Move-only, single-reader session lease. Each yielded Row owns its bytes/schema.
/// Drain all results or finish(); abandoning unread data retires the connection, never silently reuses it.
/// Deferred methods own stream state and borrow the connection even if this handle is dropped.
class RowStream {
  struct State;
  std::shared_ptr<State> state_;
  friend class Connection;
  friend class Statement;

  explicit RowStream(std::shared_ptr<State> state) noexcept;
  static Task<std::optional<Row>> read(std::shared_ptr<State> state, u64 generation);
  static Task<bool> advance(std::shared_ptr<State> state, u64 generation);
  static Task<void> drain(std::shared_ptr<State> state);

public:
  RowStream(RowStream &&) noexcept;
  RowStream(const RowStream &) = delete;
  ~RowStream();
  /// Read one owning row; nullopt ends the current result, not necessarily the entire command.
  /// No competing reader/advance calls. Tasks capture the current result generation before suspension.
  Task<std::optional<Row>> next();
  /// Move to the next result only after current rows reach EOF; false means the command is drained.
  Task<bool> next_result();
  /// Copy current metadata/counts, excluding row data. Completion counts are final only after EOF.
  Result<ResultSet> summary() const;
  /// Drain unread rows/results before releasing the session. SQL errors still propagate.
  Task<void> finish();
  /// Whether the exchange lease was released, including on failure; not merely current-result EOF or proof of success.
  bool done() const noexcept;
};

} // namespace weave::mysql
