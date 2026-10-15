# JSON

Include `<weave/json.hpp>` and link `weave::json`. This optional, header-only module
uses pinned Glaze v9.0.0 for synchronous, exception-free JSON conversion.
Core/TCP do not depend on it. No consumer download or external JSON package is needed.

```cpp
struct Shipment {
  int id;
  double weight;
  bool delivered;
};

auto shipment = weave::json::read<Shipment>(
  R"({"id":42,"weight":3.5,"delivered":false})");
if (!shipment)
  return weave::report_error(shipment.error());

std::string output;
auto status = weave::json::write(*shipment, output); // Reuses capacity.
```

Ordinary aggregates need no metadata. Use native `glz::meta` specializations for
renamed/custom fields and `<glaze/json.hpp>` for advanced Glaze functionality.
Do not mix different Glaze versions in the same program.

| API | Result |
| --- | --- |
| `read<T>(text)` | New value-initialized `Result<T>` |
| `read(value, text)` | `Result<void>`; fills existing storage |
| `write(value)` | Owning `Result<std::string>` |
| `write(value, string)` | `Result<void>`; replaces output, reuses capacity |
| `write(value, span<char>)` | `Result<size_t>`; fixed capacity, bytes written, no terminating NUL |

Pass an optional `Failure *` as the final argument for error/zero-based byte offset/
message. Diagnostics omit source text. Reads consume one complete UTF-8 document,
including skipped values and trailing whitespace validation. Unknown struct keys
fail; absent keys retain existing/default values. Failures may leave partial updates
or output. Borrowed fields such as `string_view` retain Glaze's input-lifetime rules;
JSON conversion does not make them owning or decode escaped strings into stable views.
Input must not alias storage modified by decoding; output must not alias the value
being serialized or its borrowed data.

Typed conversion does not add schema validation, duplicate-key rejection or per-field
allocation limits. Apply protocol-specific admission/validation to untrusted input;
raw Glaze options can change these defaults. PostgreSQL OAuth separately enforces
its stricter duplicate/resource/secret-cleanup policy. Fixed output does not imply
all custom serializers allocate no memory internally. Allow spare buffer capacity
for Glaze's optimized writes; overflow is not an exact serialized-size preflight.

The [compiled example](examples/struct.cpp) uses caller-owned array output.
