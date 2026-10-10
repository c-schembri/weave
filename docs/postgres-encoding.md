# PostgreSQL encodings and quoting

Weave handles PostgreSQL's 42 named encodings without linking libpq. Connection
text remains bytes in the selected client encoding; Weave does not silently
transcode application strings. PostgreSQL performs supported client/server
conversions and reports unsupported conversions as SQL errors.

```cpp
co_await connection.set_client_encoding(weave::pg::Encoding::latin1);

auto encoding = connection.client_encoding(); // synchronous Result<Encoding>
if (!encoding)
  co_await weave::fail(encoding.error());
```

`BlockingConnection` exposes the same getter and a synchronous
`Result<void> set_client_encoding(Encoding)`. Startup still accepts
`Options::client_encoding` by name. Ordinary SQL `SET`, `RESET` and transaction
rollback updates are observed through PostgreSQL's ParameterStatus messages;
there is no separate local encoding override.

The setter sends a real SQL command, drains ReadyForQuery and checks that the
reported encoding agrees before succeeding. Invalid enum values fail before
network writes. SQL errors preserve a usable session; contradictory or missing
required metadata is a terminal protocol error. Its Task owns a session borrow
before initial suspension, so an unstarted setter prevents reset/replacement.
Pipeline/COPY ownership rejects concurrent setters with `Error::busy`.

The getter returns `Error::closed` on a closed connection, `Error::protocol` for
missing metadata and `operation_not_supported` for an unrecognized encoding.
Synchronous quoting fails with the same errors instead of guessing an encoding.
Connection APIs retain the normal single-execution-graph synchronization contract.

## Utilities

Include `<weave/postgres/encoding.hpp>` for standalone helpers:

```cpp
auto encoding = weave::pg::parse_encoding("ShiftJIS");
if (!encoding)
  return weave::report_error(encoding.error());

auto info = weave::pg::encoding_info(*encoding);
auto valid = weave::pg::validate_text(bytes, *encoding);
auto first_bytes = weave::pg::character_size(bytes, *encoding);
auto first_columns = weave::pg::character_width(bytes, *encoding);
```

`parse_encoding` accepts canonical PostgreSQL names and their documented aliases,
case-insensitively and ignoring ASCII punctuation/spacing. Names are bounded to
63 bytes; embedded NUL, non-ASCII names and unknown names are invalid arguments.
`EncodingInfo` provides a static canonical `name`, `max_bytes` per character and
`server` availability. Enum numeric values are not a libpq ABI contract.

All scanners use bounded string views. Empty text validates successfully and has
first-character size/width zero. Embedded NUL, truncated characters and invalid
framing return `illegal_byte_sequence`. `character_size` and `character_width`
inspect only the first character, not the entire suffix; use `validate_text` for
whole-buffer validation.

Legacy validation deliberately follows PostgreSQL's framing rules. In particular,
BIG5, GBK and UHC framing is permissive, and JOHAB follows PostgreSQL's historical
validator rather than a general codec. Successful validation does **not** prove
that bytes map to assigned characters or that the server can convert them.

UTF8 display width uses the installed ICU Unicode properties: controls are -1,
combining/enclosing/format characters and trailing Hangul Jamo are zero,
wide/fullwidth characters are two and others are one. This is an approximate
single-code-point column count, not grapheme segmentation, emoji layout or a
font measurement. ICU and PostgreSQL Unicode table versions can differ; exact
`PQdsplen` equality for every Unicode version is not promised. Legacy widths use
PostgreSQL's conventions, including MULE_INTERNAL's control-character quirk.

## SQL Quoting

Prefer bound parameters for values. When SQL construction needs quoting,
`Connection::escape_literal` and `escape_identifier` use the current client
encoding and connection message-size bound. The standalone equivalents accept an
explicit encoding (default UTF8) and complete-output limit (default 16 MiB).

Both return owning, fully quoted strings. Literals always use `E'...'`, double
single quotes and escape standalone backslashes, independently of
`standard_conforming_strings`. Identifiers use double quotes and double embedded
double quotes. Complete multibyte characters are copied without escaping their
continuation bytes: Shift JIS `83 5c`, for example, must not become `83 5c 5c`.
NUL and malformed framing are rejected rather than truncated; output bounds are
checked before producing any result. `encode_bytea`/`decode_bytea` remain separate
owning byte helpers, not SQL literal quoting or text conversion.

For a deliberately stricter security policy, quoting also rejects either quote
character in a multibyte continuation, even where a permissive legacy framing
validator would accept it. This differs from libpq's permissive cases. Validation
and SQL quoting therefore intentionally have different acceptance sets.

Do not change the client encoding between quoting and executing SQL. Standalone
helpers must be given the actual PostgreSQL client encoding; they are not generic
escapers for another database or arbitrary SQL dialect. A successful quote does
not guarantee that PostgreSQL can convert or store the data. SQL_ASCII disables
conversion, and PostgreSQL rejects non-ASCII input from client-only encodings
against SQL_ASCII databases before parsing it. Weave preserves that SQL error;
it does not pretend the server can accept the character set.

Reference: [PostgreSQL encoding support](https://www.postgresql.org/docs/18/multibyte.html),
[libpq encoding control](https://www.postgresql.org/docs/18/libpq-control.html),
[libpq quoting](https://www.postgresql.org/docs/18/libpq-exec.html#LIBPQ-EXEC-ESCAPE-STRING).
The server's SQL_ASCII safety restriction is implemented in
[PostgreSQL's conversion boundary](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/backend/utils/mb/mbutils.c).
