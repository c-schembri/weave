# Glaze

Unmodified JSON header dependency closure from Glaze v9.0.0,
commit `d78832c82289c61a9315bfbc35332cec9f4e93ca`:
https://github.com/stephenberry/glaze/tree/d78832c82289c61a9315bfbc35332cec9f4e93ca

Roots: `glaze/json.hpp`, `glaze/version.hpp`, `glaze/core/error_category.hpp`.
Copy their transitive upstream quoted/local include dependencies without edits.
Retain `LICENSE` and requalify Windows/Linux, exception-disabled builds and OAuth
validation/cleansing tests on upgrades. Networking and other format frontends
are not included. Consumers do not fetch dependencies or need Python.

Glaze owns the `glz` namespace and `<glaze/...>` include paths. Do not combine
different Glaze versions in one program or mix Weave's headers with another copy.
