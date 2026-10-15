# Database migrations

Describe schema changes with `weave::db::schema`, not application structs.
There is no ORM, schema discovery, query builder or automatic startup migration.
Include `<weave/postgres.hpp>` and link `weave::postgres` for the currently
implemented PostgreSQL adapter. Shared descriptions need only `weave::database`.

## One file per migration

```text
migrations/
  0001_create_shipments.cpp
  0002_add_shipment_weight.cpp
  0003_add_shipment_tracking.cpp
```

`0001_create_shipments.cpp` defines a non-static function matching its version digits:

```cpp
#include <weave/database/migrations.hpp>

namespace db = weave::db;
namespace schema = weave::db::schema;

db::Migration migration_0001()
{
  return db::migration(
    1, "create_shipments",
    schema::create_table(
      "shipments",
      schema::identity("id").primary_key(),
      schema::text("recipient").not_null()
    )
  ).rollback(schema::drop_table("shipments"));
}
```

For larger migrations, named `up` and `down` sections keep the directions separate.
Both lists execute in written order; rollback order is explicit, not automatically reversed.
The [third migration](../../postgres/examples/migrations/changes/0003_add_shipment_tracking.cpp)
demonstrates this alternative alongside the fluent examples:

```cpp
db::Migration migration_0003()
{
  return {
    .version = 3,
    .name = "add_shipment_tracking",

    .up =
      {
        schema::add_column("shipments", schema::text("tracking_code").nullable()),
        schema::create_index("shipments_tracking_idx", "shipments", "tracking_code"),
      },

    .down =
      {
        schema::drop_index("shipments_tracking_idx"),
        schema::drop_column("shipments", "tracking_code"),
      },
  };
}
```

```cmake
find_package(weave REQUIRED COMPONENTS postgres)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE weave::postgres)

weave_add_migrations(TARGET my_app
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/migrations" NAME app_migrations)
```

The CMake helper also works with `add_subdirectory(weave)`. No Python, runtime
file discovery or external generator is needed. It validates filenames and unique,
positive versions, sorts numerically regardless of padding, and generates
`app_migrations.hpp/.cpp` in the build directory. Source definitions are checked
against generated declarations. Returned version/name must match the filename;
mismatches fail migration validation. Adding/removing files triggers reconfiguration.
Use one version-function set per linked application; separate applications can
use the same filenames. Without CMake, build an ordered `db::migrations(...)` array.

```cpp
#include <weave/postgres.hpp>
#include "app_migrations.hpp"

auto migrations = app_migrations();
auto report = co_await weave::db::migrate(connection, migrations);
```

Keep the manifest and connection alive and unchanged until completion, including
cancellation cleanup. Temporary owning collections are rejected. Descriptions own
their strings and operation lists; this is not an allocation-free execution API.

## Operations

- Columns: `identity`, `int16/int32/int64`, `float32/float64`, `boolean`, `text`,
  `varchar`, `binary`, `date`, `timestamp`, `decimal`.
- Modifiers: `not_null`, `nullable`, `primary_key`, `unique`, `default_value`.
  Strings are quoted values, never expressions. Decimal defaults use exact strings.
- Tables: `create_table`, `drop_table`, `rename_table`; columns: `add_column`,
  `drop_column`, `rename_column`.
- Constraints: ordered composite `primary_key`, `unique`,
  `foreign_key(...).references(table, ...).on_delete(action).on_update(action)`.
- Indexes: `create_index(name, table, columns...).unique()`, `drop_index`.
- Names: `schema::qualified("app", "shipments")`; a dot in an ordinary name is literal.

Types describe intent, not identical vendor semantics. Unsupported guarantees or
descriptions fail before schema/history changes; no implicit CASCADE, table rebuild,
unsigned-integer approximation, or transaction downgrade. PostgreSQL requires
UTF-8 client encoding and server version 10 or later. Unqualified DSL objects bind
to the namespace resolved at the start of the run, not a later search_path.

## Execution and recovery

| API | Behavior |
| --- | --- |
| `migrate(connection, manifest)` | Apply pending versions; repeat runs are no-ops |
| `migrate(..., {.target = 2})` | Apply through an existing version; zero means none, never downgrade |
| `migration_status(connection, manifest)` | Validate history and return applied/pending entries; does not create a missing history table |
| `rollback(connection, manifest, {.steps = 1})` | Execute explicit down operations for the latest versions; never infer reversibility |

Use an idle connection exclusively for the whole run, or retain one pool lease.
Never share it with independent query tasks. Existing transactions are rejected,
not committed or rolled back. Options select the default schema, history schema/table
and lock deadline. Empty schema selects the driver's current namespace; empty
history schema follows it. Schemas must exist and the role needs the relevant DDL
permissions. Use the same namespace/history configuration on every deployment.
PostgreSQL accepts lock deadlines from zero through 24 hours; zero makes one
immediate attempt rather than waiting. This is not an overall migration timeout.
Temporary namespaces and nonpermanent/non-table history relations are rejected;
the ledger must survive session exit and server recovery.

PostgreSQL uses a session advisory lock keyed to the qualified history table,
with cancellable asynchronous waits. This serializes cooperating runners using
that history, not arbitrary DDL or different histories targeting the same tables.
History must be an exact manifest prefix. SHA-256 checksums cover a versioned
description including both directions, transaction policy and resolved default
schema, not generated SQL. Never edit applied migrations or insert older versions.

Each migration runs in its own transaction by default, committing its SQL and
history record together. A later failure does not undo earlier versions. Cleanup
is shielded from task cancellation; failed cleanup retires the connection.
Transport loss can leave commit outcome unknown. Reconnect, reacquire the lock
and inspect history before deciding what to do; there are no blind retries.
Closing a connection does not prove that the server has already finished rollback.

For one trusted vendor-specific statement, use `db::sql(db::Vendor::postgres, sql)`.
No SQL splitting or translation is performed. Never include transaction control,
manipulate migration locks/history, or change client encoding. Arbitrary SQL effects
cannot be prevalidated; misusing this escape hatch can violate atomicity guarantees.
Use `.nontransactional()` explicitly for operations such as concurrent index creation.
The runner persists an applying/reverting marker first. Failure or interruption
blocks later runs: inspect the actual schema, reconcile it manually, then repair
the history row under the same lock. Do not simply delete a marker and retry partial
DDL. No automatic repair or rollback of nontransactional partial work is provided.

`MigrationOptions::failure` and `RollbackOptions::failure` optionally receive an
owning version/phase/error/message snapshot, including an optional separate cleanup error.
Keep that destination alive until completion. SQL and transport error codes retain
their driver categories; history/validation errors use `MigrationError`.

The [complete example](../../postgres/examples/migrations/main.cpp) has
[three separate migration files](../../postgres/examples/migrations/changes).
MySQL supports async and blocking execution through `<weave/mysql/migrations.hpp>`;
its [example](../../mysql/examples/migrations/main.cpp) uses the same file generator.
Set `.transaction = db::MigrationTransaction::none` in **every** entry. DDL implicitly
commits; requesting atomic schema/history execution fails before changes. The adapter
uses GET_LOCK, qualified InnoDB history and durable applying/reverting markers.
Autocommit must stay enabled with no active transaction. Lock deadlines accept zero
through 24 hours; unlock cleanup is shielded and bounded. Server identifier folding
is respected for lock/history identity and reserved-history protection.

Portable types map to SMALLINT/INT/BIGINT, FLOAT/DOUBLE, checked BOOLEAN, LONGTEXT/
LONGBLOB, DATE/DATETIME(6), VARCHAR and DECIMAL. Identity needs an indexed key.
Conservative limits: identifiers 64 UTF-8 bytes in the Unicode BMP (no trailing spaces), VARCHAR 16,383 characters, decimal
precision 65 and scale 30. Invalid/unsupported descriptions fail, not truncate.
Use `schema::drop_index(table, index)`, not the PostgreSQL index-only form. Vendor
SQL selects `db::Vendor::mysql`; transaction/lock/history manipulation stays forbidden.
Dirty history blocks migration/status until an operator reconciles actual schema
and repairs history under the same lock. Other vendors are not implemented.
Dialect preflight checks descriptions, not every server SQL-mode/value/engine limit;
native rejection can still occur after a nontransactional marker is written.
