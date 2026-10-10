# PostgreSQL LDAP Service Lookup

LDAP is an optional **synchronous configuration source**, not a PostgreSQL
transport or authentication provider. Build with `WEAVE_POSTGRES_LDAP=ON`, then
explicitly enable it when loading a selected service:

```cpp
auto options = weave::pg::Options::load("service=application", {
  .service_file = "services.conf",
  .ldap = true,
  .ldap_timeout = std::chrono::milliseconds{2000},
});
if (!options)
  return weave::report_error(options.error());
```

Load during application setup, before driving latency-sensitive workers.
`Options::parse`, connect and reset never perform LDAP lookup implicitly.
Both the library build option and per-load opt-in are required; otherwise a
selected LDAP line returns `operation_not_supported`, without lookup.

## Service Syntax

```ini
[application]
ldap://directory.example:389/cn=application,dc=example,dc=com?description?base?(objectClass=*)
```

The URL identifies one attribute, a `base`, `one` or `sub` search, and a filter.
An omitted host means localhost; an omitted port means 389. Bracket IPv6
authorities. Percent-encode reserved characters and UTF8 DN/filter bytes;
unencoded URL text must be ASCII. Decoding preserves literal `+` characters.
The service-file classifier recognizes lowercase `ldap` prefixes. Userinfo,
fragments, multiple attributes, extensions and `ldaps://` are rejected.

Exactly one entry must supply at least one value of the requested attribute.
Values are concatenated with newlines and parsed as keyword/value settings:

```text
user=application
dbname=application
password='directory supplied secret'
```

Directory values support single quotes and quoted backslash escapes; unquoted
backslashes remain literal. Empty unquoted values end at whitespace instead of
consuming the next line. This grammar differs from literal ordinary service-file
`key=value` lines and preserves ordinary connection-string parsing.
Unknown keys, NUL, nested `service` settings and malformed values fail.

Earlier settings in the selected stanza win over directory duplicates; the
first directory value for a key wins. LDAP multi-value ordering is controlled
by the directory, so do not depend on duplicate values being returned in a
particular order. Explicit connection-string settings still override the
service, which overrides environment defaults.

A successful lookup ends the selected stanza; later lines are not loaded.
Native connection/anonymous-bind unavailability permits the next LDAP line or
ordinary fallback settings. Once the search begins, errors, referrals, missing/
multiple entries and malformed data are terminal, not fallback triggers.

## Security And Bounds

Only anonymous **plaintext `ldap://`** lookup is supported, as in libpq's
documented mechanism. There is no LDAP server authentication or encryption,
StartTLS, LDAPS, bind credential API, or automatic credential use. An on-path
attacker can alter supplied settings and read directory passwords. Opt in only
where the directory and network are trusted by deployment policy. PostgreSQL's
verified TLS default remains independent; loaded settings still undergo Weave's
normal security validation.

Referrals and alias dereferencing are disabled. The loader never follows a
directory-supplied URL or silently ignores URL extensions. These are intentional
differences from libpq.

The per-lookup timeout must be positive and at most one minute. Native connect/
bind and search calls receive timeout limits; search uses the remaining
steady-clock budget. This is not Task cancellation or a hard total wall-clock
guarantee: native DNS/provider calls, copying, parsing and cleanup are synchronous.
Each fallback lookup has its own budget; a stanza permits at most 16 attempts.

URL and decoded field limits are 64 KiB; returned values are capped at 1024,
with a 1 MiB raw aggregate and a 1 MiB combined-text limit including separators.
These are acceptance/copy bounds, **not a native LDAP PDU allocation limit**:
the native library receives and allocates a response before Weave can inspect it.
Owned copied values and native value-array copies are cleansed before release;
opaque native messages, library temporaries, caller copies and network buffers
are not guaranteed erased.

## Build And Qualification

Windows uses WinLDAP from the Windows SDK and privately links `wldap32`.
Linux uses OpenLDAP headers and its native library. Library-only builds never
fetch an LDAP server or libpq. Linux consumers of an LDAP-enabled installed
PostgreSQL component need OpenLDAP development files; the custom finder is
installed with Weave. Other components do not require them. Native headers are
absent from public Weave headers.

Private parser tests run with the normal correctness suite, including compiled-out
controls. The real-directory gate is an explicit Linux opt-in:

```text
-DWEAVE_POSTGRES_LDAP=ON
-DWEAVE_POSTGRES_LDAP_SERVER_TESTS=ON
-DWEAVE_POSTGRES_LDAP_TEST_TOOLS=/path/to/extracted-openldap-root
```

The tools root supplies `usr/sbin/slapd`, `usr/sbin/slapadd`, schema, modules and
their runtime dependencies. Tests run owned foreground servers and remove their
data; they never start or modify an installed system service. The same fixture
can drive a native Windows executable from WSL with `--windows-client`; optional
`--libpq-control` runs a separate native Linux functional baseline, not a
performance measurement or a suppression of Weave leak checking.

Development controls passed on native Windows and Linux, including sanitizer
checks, search timeout without fallback, referral target non-contact, Unicode,
multi-value/empty-value handling and exact size/count boundaries. One empty
binary value is injected by a test-only observer because slapadd rejects that
fixture at import time; other cases use a real OpenLDAP directory. Windows
domain LDAP and native IPv6 connectivity are not qualified by those checks.
Permanent Debug promotion passed 11 CTest executions across Windows/Linux,
including isolated/relocated component packaging, public-header probes and
missing Linux LDAP dependency rejection, plus a native Windows directory gate.
The subsequent frozen affected-source matrix passed 222 CTest executions across
Windows/Linux Debug, Release and ASan, including TLS, PostgreSQL, reduced-module
and packaging gates. Three native Windows directory gates and three native
Windows PostgreSQL client gates also passed against disposable Linux servers.
This closes promoted-test and affected-source qualification, not arbitrary
directory deployment or full PostgreSQL release qualification. See the
[qualification record](postgres-qualification.md).

Reference: [PostgreSQL 18 LDAP service lookup](https://www.postgresql.org/docs/18/libpq-ldap.html).
