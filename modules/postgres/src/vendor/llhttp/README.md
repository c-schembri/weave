# llhttp provenance

llhttp 9.4.3, MIT licensed. Generated release sources from
[nodejs/llhttp](https://github.com/nodejs/llhttp), commit
`0e815792b167a9bd8ace259b95b7da953776c288` (`release/v9.4.3`).

The only upstream change is an `#ifndef LLHTTP_EXPORT` guard around its existing
export definition. The private wrapper sets that macro empty, preventing unwanted
DLL exports on Windows. Parsing code and strict defaults are unchanged.
The first-party prefix header renames private C linkage symbols to `weave_pg_*`,
avoiding collisions with an application's independently linked llhttp.
No headers or parser types are installed as supported Weave API. Do not reformat
the upstream sources. The MIT license is installed with PostgreSQL.
