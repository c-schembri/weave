# yyjson provenance

Unmodified yyjson 0.12.0, MIT licensed. Upstream:
[ibireme/yyjson](https://github.com/ibireme/yyjson), commit
`8b4a38dc994a110abaec8a400615567bd996105f` (peeled tag `0.12.0`).

| File | SHA256 |
| --- | --- |
| yyjson.c | ac2e9bbb2e2d9149d90878d40506a1d624fa0b33c979a11b61075c54782c6d6a |
| yyjson.h | 175867c5493a5df648cec566717fa1c29aa2f6096f5f0cf1efad0b65e1f6d7b3 |
| LICENSE | 45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947 |

The private OAuth JSON wrapper includes the upstream C implementation as C++,
with TU-local API linkage. It disables writing, incremental reading, utilities
and non-standard JSON. Strict UTF8 validation stays enabled. A bounded cleansing
pool owns all parsing allocations. Nothing is fetched at consumer configure time,
no yyjson symbols are part of Weave's public API, and no native JSON types leak
through installed headers. Do not reformat or patch these upstream files.
