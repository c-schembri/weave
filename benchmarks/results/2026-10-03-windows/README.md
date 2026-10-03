# Initial Windows baseline

Source: commit `0b060a1`, clean worktree. MSVC 19.44.35221.0, x64 Release,
exceptions disabled. See environment.json for OS/CPU and benchmark.json for
all raw results. Run with scripts/bench.ps1 defaults: five repetitions, 0.5s
minimum per repetition, randomized interleaving.

Mean elapsed microseconds per serial TCP echo roundtrip (lower is better):

| Payload | Weave | Asio |
| --- | ---: | ---: |
| 64 B | 17.8 | 18.2 |
| 1 KiB | 20.3 | 18.2 |
| 64 KiB | 36.8 | 38.3 |

Weave is slower on the 1 KiB case in this run. Variation across repetitions
is high (roughly 8-15% coefficient of variation); this is a baseline artifact,
not evidence of a general performance advantage or a reliable regression gate.
The earlier exploratory run was less noisy and also showed broadly comparable
performance. Both experiments used the same peer implementation for each client.

These are serial roundtrip measurements, not concurrent throughput or tail
latency. No affinity or power-plan tuning was applied. The shared development
machine was not isolated. Repeat on a controlled host before optimizing around
small differences.
