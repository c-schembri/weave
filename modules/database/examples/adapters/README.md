# Database Driver Adapters

These examples are for implementing database drivers, not application queries.
They run in memory without a server. Real MySQL and PostgreSQL rows already
provide selection and conversion; applications use `row.as<Shipment>()`.

| Source / Target | Shows |
| --- | --- |
| [mapping.cpp](mapping.cpp) / `database_mapping` | A fake Row/Field adapter for shared struct decoding |
| [columns.cpp](columns.cpp) / `database_columns` | Column selection and binding for driver authors |

Build with `WEAVE_MODULES=database` and `WEAVE_BUILD_EXAMPLES=ON`.
For application code, start with the [MySQL](../../../mysql/README.md) or
[PostgreSQL](../../../postgres/README.md) struct query.
