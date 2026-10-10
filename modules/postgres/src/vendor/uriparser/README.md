# uriparser provenance

Unmodified parsing-only sources from uriparser 1.0.2, commit
`9b2bed92f5deecf740819f9bf27724bee2fe9c12` (peeled `uriparser-1.0.2`),
[uriparser/uriparser](https://github.com/uriparser/uriparser).
These library sources are BSD-3-Clause licensed; upstream's differently licensed
tests and fuzzers are not imported.

The private build enables only the `char` parser and never normalizes input URI
components. A first-party prefix header renames C linkage to `weave_pg_*`, avoiding
collisions with another uriparser in a consumer. No native types or headers are
installed. Library-only consumers do not fetch dependencies. Do not reformat or
patch the upstream code; its license is installed with PostgreSQL.
