# WayWin DVC v2 additions

The existing 24-byte little endian header and 2MiB payload bound are unchanged.
All requests use the existing target ID and channel epoch. Unknown or stale
target metadata is discarded. `waywin_policy.hpp` is shared by Qt, the engine,
the Windows Companion and contract tests, and is embedded in the product patch.

| Message | Payload |
| --- | --- |
| `Hello` (1) | Empty for legacy clients, or one `uint32` requested capability mask |
| `SurfaceState` (11) | `uint32` surface ID, `uint32` visibility (0 hidden, 1 background, 2 active) |
| `InputAck` (113) | Empty; header request ID identifies a completed successful input/state/close request |
| `Flow` (114, local IPC only) | `uint32` stage, `uint32` state (0 resumed, 1 blocked, 2 fatal, 3 above resume watermark) |

`Ready` retains its session ID, instance string and supported capability mask.
`0x40` advertises surface priority and `0x80` advertises input completion. New
clients request both (`0xc0`); the baseline capability mask remains `0x3f`.
Successful pointer, key, text, state and close requests are acknowledged only
when `0x80` was requested. Failures retain the existing `Result` response. An
acknowledgment does not change status text or trigger a full GUI refresh.
Coalesced, unsent motion requests are completed when their replacement takes
ownership of the queue slot. Presses, releases and closes remain FIFO.

An empty `Hello` requests neither addition: no new success responses and host
aggregate 4fps round robin capture. A new client with an old Companion disables
shell operations and asks for an update, while leaving apps, clipboard and
ordinary RDP available. State updates never focus a Windows window.

Local engine commands retain their bounded text framing. RAIL actions 1–6 use
the final payload token for a completion ID and receive a local `InputAck` after
engine dispatch, sharing the host's 64-request budget with DVC operations. The
legacy `-` token remains accepted without completion responses. `Flow` stages 1 and 2
report the session worker and engine queues. The surface worker reports its
output blockage to the GUI, which tracks it as stage 3. The GUI combines these
with its own queue and pending request budget before resuming ordinary input. State 3 reports a queue at or above the
128KiB resume watermark; it prevents early resume without blocking a previously
unblocked path by itself.
Control capacity is additional FIFO capacity, rather than a priority queue.
Generation changes discard unsent input; a partially written frame is completed
so it cannot corrupt the next generation's framing, and is then discarded by
the receiver's epoch check. Neither commands nor flow diagnostics store input
payloads in evidence files.
