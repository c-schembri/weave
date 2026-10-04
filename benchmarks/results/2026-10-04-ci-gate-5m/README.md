# Invalid Setup Attempt

> Historical prototype evidence. Labels, commands, and raw hashes describe the
> original code. That source has since been replaced by native `weave::Task`;
> linked methodology documents now describe the current gate.

No benchmark trials ran. The driver forwarded the single CPU-isolation argument
as individual characters after PowerShell unwrapped a one-element array.
The native executable rejected the flags and the gate returned invalid (exit 3)
in 1.7 seconds. See [run.log](run.log).

The driver was fixed to use an explicitly typed `string[]`. This attempt is kept
for provenance; the [complete measured run](../2026-10-04-ci-gate-5m-run2/README.md)
is separate. No performance samples were discarded or retried based on results.
