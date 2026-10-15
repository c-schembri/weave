#pragma once

#include <weave/mysql/stream.hpp>

namespace weave::mysql {

class Connection;

/// Move-only server statement leasing its Connection. Methods retain state before initial suspension.
/// The connection must survive this handle and its deferred Tasks. Destruction queues bounded server cleanup;
/// close() sends that cleanup explicitly. No SQL interpolation, query retry or client-side placeholder rewriting.
class Statement {
  struct State;
  std::shared_ptr<State> state_;
  friend class Connection;
  friend class RowStream;

  explicit Statement(std::shared_ptr<State> state) noexcept;
  static Task<Results> run(std::shared_ptr<State> state, std::vector<Parameter> parameters);
  static Task<Results> run_view(std::shared_ptr<State> state, std::span<const ParameterView> parameters);
  static Task<RowStream> start(std::shared_ptr<State> state, std::vector<Parameter> parameters);
  static Task<RowStream> start_view(std::shared_ptr<State> state, std::span<const ParameterView> parameters);
  static Task<void> finish(std::shared_ptr<State> state);
  static Task<void> reset_impl(std::shared_ptr<State> state);
  static Task<void> long_data(std::shared_ptr<State> state, u16 parameter, std::span<const std::byte> bytes);

public:
  Statement(Statement &&) noexcept;
  Statement(const Statement &) = delete;
  ~Statement();
  /// Immutable prepare metadata; references borrow this statement handle.
  std::span<const Column> parameters() const noexcept;
  std::span<const Column> columns() const noexcept;
  /// Own parameters before suspension. Return one binary result, draining extra results before rejecting them.
  Task<ResultSet> execute(std::vector<Parameter> parameters = {});
  /// Borrow the array and referenced bytes through completion/cancellation cleanup; no input container copy.
  Task<ResultSet> execute_borrowed(std::span<const ParameterView> parameters = {});
  /// Own parameters and return all results from one execution, in wire order.
  Task<Results> query(std::vector<Parameter> parameters = {});
  /// Borrow parameter array/bytes through execution cleanup; return every owning binary result.
  Task<Results> query_borrowed(std::span<const ParameterView> parameters = {});
  /// Start incremental binary row delivery. Parameters are needed only through this start Task's cleanup.
  Task<RowStream> stream(std::vector<Parameter> parameters = {});
  /// Borrow parameters only through startup cleanup, then retain the statement/session stream lease.
  Task<RowStream> stream_borrowed(std::span<const ParameterView> parameters = {});
  /// Send one borrowed long-data fragment for a string/blob parameter; completion acknowledges transport only.
  /// Execute/reset clears accumulated fragments. Retain bytes through cancellation cleanup.
  Task<void> send_long_data(u16 parameter, std::span<const std::byte> bytes);
  /// Clear pending long data/cursor state; await the server's OK before reuse.
  Task<void> reset();
  /// Send COM_STMT_CLOSE (no server reply), invalidate the handle and release its session lease.
  Task<void> close();
};

} // namespace weave::mysql
