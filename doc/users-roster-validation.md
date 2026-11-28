# Users roster validation

## Traffic usage and speed estimates

Two independent Chromium sessions watched one account on WaterWall 1.6.0.
Two producer reports raised combined **Traffic usage** from 100 to 142 to
226 B. The first traffic change established the timing baseline. After the
second, the two viewers displayed 11.3 and 10.6 B/s upload, and 33.9 and
31.7 B/s download, respectively. Each tab timed its own interval and showed
the producer's 1:3 upload-to-download ratio. Both received roster deltas and
made no extra account requests.

The browser fixtures check exact usage sorting across the 64-bit boundary,
independent sorting by each direction's average speed, and the difference
between a reported zero and an unavailable estimate.

## Original roster benchmarks

The initial live acceptance run used the release Wave builds, a WaterWall executable
that reported 1.6.0, and two Chromium contexts with independent administrator
sign-ins. Both received 1,000 accounts. One sorted by Total Download while the
other searched for Alice. An accepted producer report updated Alice, whose ID
was on the last WaterWall page. Both browsers received the changed summary,
and Alice moved to the first row in descending download order. Search,
sorting and scrolling sent no workspace actions. After stopping WaterWall,
both contexts loaded 1,000 saved accounts and the same local controls worked.

| Measurement | Observed |
| --- | --- |
| Live subscribe to committed baseline, real core | 486 and 669 ms, 295,713 payload bytes per viewer |
| Saved subscribe to committed baseline, release builds | 390 and 504 ms, 302,679 payload bytes per viewer |
| One live account's changed usage, real core | 667 payload bytes per viewer |
| Total Download header click to updated sort description | 24 ms in headless Chromium |
| Peak resident memory, real-core browser run | 84,320 KiB for the Wave backend and 252,428–281,204 KiB per Chromium renderer sampled |
| Idle list-only checks, controllable core fixture | One shared one-row probe per interval, 499 and 502 ms apart, with zero rows sent to browsers |
| Saved edit, two subscribed database bindings | 620 payload bytes for one changed summary. File access time stayed unchanged during 1.15 seconds of idle viewing. |
| Near-16 MiB saved database refresh racing with a write | 115,956 KiB peak Wave backend resident memory |

Payload counts include roster transfer envelopes. Resident memory is the
Linux process high-water mark and includes activity beyond roster handling.
These observations are from fixtures on one host. Workloads and browser
settings affect timing and memory use.

A separate debug Chromium fixture staged 50,000 saved summaries with about
17.9 MB of raw row JSON. The first load took 9.27 seconds. Staging and committing
a replacement while retaining the prior list took 7.99 seconds. The
WebAssembly linear memory buffer grew from 16 MiB to 40 MiB on the first load
and to approximately 69 MiB on the replacement. The workspace uses a
256 MiB WebAssembly memory ceiling. The backend caps each roster at
100,000 validated rows and 32 MiB of encoded summaries and emits chunks up to
128 KiB. Each watched target keeps its published version and most recent
delta. A socket holds its last delivered version and, during a transfer, its
destination version. Unchanged rows share their encoded storage.

The 1920 × 978 capture was compared with the reference panel. Wave keeps its
workspace styling and uses the reference table's separate ID and Name,
inline Enable and Options, striped single-line rows, frozen header and Logs
area. The 390 × 844 capture keeps ID, Name, Enable and Options visible. A
touch swipe brings the traffic totals into view. Starting from a tapped row,
the Down key and Enter selected the next account. Local captures are in
`build/browser-tests/roster-desktop.png`,
`build/browser-tests/roster-narrow.png` and
`build/browser-tests/roster-touch-totals.png`.

The native CTest fixtures cover shared polling, list and status probes,
malformed traversals, retained stale rows, saved write and refresh races,
independent sign-ins and remaps. The WebAssembly fixtures cover progressive
loads, local ordering and filtering, broken-chunk recovery, visibility and
editor state across changes. The opt-in real-core test checks a full-access
starter, a config with a monitoring-only grant, an applied account edit and
a producer usage report.
[Build and test commands](building.md#backend-tests)

## Catch-up and recovery regressions

The saved burst fixture loaded 1,000 accounts in two independent sign-ins.
Two edits delivered exactly two changed IDs to each viewer. The writer
received 1,294 WebSocket payload bytes across two deltas, and the passive viewer
received 934 bytes in one delta. Five edits delivered five changed IDs per
viewer, using 2,509 and 1,786 payload bytes in the release run. Neither burst sent
a full snapshot. When six writes raced a pinned three-chunk transfer, the
passive viewer received one changed ID and zero removals in 648 bytes.
Four cancelling writes advanced the sequence with a 253-byte empty delta.
These totals include the roster transfer envelopes. Peak backend resident
memory for the entire fixture was 84,404 KiB.

A fresh saved subscription on the same target saw an external rename on its
first baseline, and the first viewer received the change as a delta. An
unchanged re-entry still validated the file. Repeating the active subscription
and leaving it idle caused no disk read. A late entry validated the
replacement file after an earlier refresh opened its snapshot. One fresh subscribe recovered a
restored file while the other viewer stayed connected. The live transport
fixture exercised the same recovery after list permission returned.

The browser fixtures verified that an unreadable saved config preserved a
draft, a repair to the same binding let the operator finish it, and a remap
blocked writing the old draft to the replacement. An open live editor or
password form acquired a new token after list permission loss, then sent
one explicit write. The fixtures also checked future first-use deadlines,
lookup clock isolation, whitespace-normalized searches and toggles whose
account was removed before or after dispatch.

The two-viewer recovery check releases the passive browser's lost subscription
and retries from the other browser. Only the retrying viewer receives rows.
After that viewer leaves, the fixture observes zero further WaterWall probes.
The saved-config fixture exercises invalid node structures and verifies that
repair restores the original database binding. A valid empty node array removes
the tabs.

Browser regressions cover opening details directly by ID, including a lookup
that finishes after list permission is lost. They also cover a create whose
target becomes unverifiable before its reply, after an absent lookup, and
during reconnect. The original ID remains in Logs and the replacement
database receives no automatic lookup or repeated create.
