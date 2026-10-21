# Core plan

Every hot thread busy-polls on a core of its own. On an 8-core WSL2 VM the
default layout is:

| CPU | Runs | Phase |
| --- | --- | --- |
| 0-1 | Linux, tools, logger, slow-path services | 1 |
| 2 | Feed handler (`itch-fh`) | 3 |
| 3 | Strategy engine | 5 |
| 4 | Pre-trade risk + order entry gateway | 5 |
| 5 | Exchange matching engine (`simex`) | 4 |
| 6 | Exchange market data publisher | 4 |
| 7 | ITCH replayer (`itch-replay`) | 3 |

With fewer cores, put the exchange side (5, 6, 7) on shared cores first: its
latency matters less than the firm's.

WSL2 caveat: the Windows host schedules the VM's virtual CPUs, so a pinned
thread can still be paused by the host. Expect a long p99.9 tail and compare
latency as relative changes between builds, not as absolute production
numbers. `lab-check` measures a core-to-core round trip so you can see your
machine's baseline.
