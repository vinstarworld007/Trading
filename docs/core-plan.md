# Core plan

Every hot thread busy-polls on a core of its own. On an 8-core WSL2 VM the
default layout is:

| CPU | Runs | Phase |
| --- | --- | --- |
| 0-1 | Linux, tools, logger, slow-path services | 1 |
| 2 | Feed handler rx thread (`itch-fh --rx-cpu 2`): recvmmsg, retransmission requests | 3 |
| 3 | Feed handler book thread (`itch-fh --book-cpu 3`): sequencing, decode, books | 3 |
| 4 | Strategy engine | 5 |
| 5 | Pre-trade risk + order entry gateway | 5 |
| 6 | Exchange matching thread (`simex --matcher-cpu 6`) | 4 |
| 7 | Exchange market data thread (`simex --md-cpu 7`), or `itch-replay --cpu 7` | 3, 4 |
| 0-1 | Exchange gateway, journal and historical liquidity threads (`simex --gateway-cpu 1`) | 4 |

The feed handler keeps every syscall on its rx thread. The book thread only
reads packets from a lock-free ring, so a slow `recvmmsg` or a retransmission
request never stalls book updates, and a book thread that falls behind shows
up as "packet ring full" in the report (recovered like any other lost packet).

simex's matching thread makes no syscalls: the gateway thread owns every
socket and hands it commands through a ring, the matching thread hands back
responses, ITCH messages and journal records through three more. Firm orders
are taken ahead of historical liquidity, so a firm order never waits behind a
burst of replayed flow.

With fewer cores, put the exchange side (5, 6, 7) on shared cores first: its
latency matters less than the firm's.

WSL2 caveat: the Windows host schedules the VM's virtual CPUs, so a pinned
thread can still be paused by the host. Expect a long p99.9 tail and compare
latency as relative changes between builds, not as absolute production
numbers. `lab-check` measures a core-to-core round trip so you can see your
machine's baseline.
