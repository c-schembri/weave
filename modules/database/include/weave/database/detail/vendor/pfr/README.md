# Bundled Boost.PFR

Source: https://github.com/boostorg/pfr/tree/boost-1.89.0

Archive: https://github.com/boostorg/pfr/archive/refs/tags/boost-1.89.0.tar.gz

SHA256: `749e15549f4bda76cbca2106e0f5c4864bbbc89c9680a64a4183cc24a2b886b3`

This directory contains the transitive header dependencies of `core.hpp` and
`core_name.hpp`, plus the upstream Boost Software License. No Boost installation
or build-time download is required. This is internal template machinery, not
a supported Weave API.

Only mechanical isolation changes are applied:

- `boost/pfr/` include paths become `weave/database/detail/vendor/pfr/`.
- `namespace boost` and `boost::` become `weave_db_vendor`.
- `BOOST_PFR` macros/guards become `WEAVE_DB_PFR`.
- `BOOST_USE_MODULES` becomes `WEAVE_DB_PFR_USE_MODULES`.

The namespace substitution also updates compiler-signature parsing literals.
This prevents the bundled backend from colliding with a consumer's Boost.PFR.
Preserve upstream formatting and copyright notices.
