# Proposal for WaterWall per-user traffic rates

WaterWall 1.6.0 exposes cumulative charged byte counters. A producer can
report usage after a delay and apply charge multipliers. Wave displays these
counters as combined **Traffic usage**. Its **Avg upload speed** and **Avg
download speed** columns estimate charged bytes per second over the last
two traffic updates observed by the open panel. Measured network speeds
require a new read operation and producer reports.

## Measurements

Measure bytes where the producer observes the traffic, before applying quota
multipliers. Upload means traffic from the account toward the remote peer.
Download is traffic in the reverse direction. Report the raw bytes in each
direction, the window duration, and the window end so the server can report
bytes per second and sample age. Use a monotonic timer for measurement windows.
Identify each report by its AuthenticationServer target, producer identity,
reporting session, and monotonically increasing sequence. Within each report,
identify the account with its durable unsigned 64-bit ID.

Align producers on a common configurable window before aggregating their
rates. Define allowed window skew, reporting delay and retention with upstream.
Producers with different windows need a server-side alignment rule before
their values can be combined. Bound the producer queue and server cache by
bytes and by the number of producer/account entries. Expire old samples
automatically. A reporting session replacement invalidates that producer's
prior samples. Discard duplicates and samples older than the most recent
accepted sequence for the applicable window.

## Management API

Add an independently granted read-only operation for recent per-user rates.
Scope its authorization to the exact config and AuthenticationServer node.
The result should support bounded reads and include the ID, direction rates,
window length, age, and coverage. Define complete measured zero, missing
samples, stale samples and partial producer coverage as distinct cases.
Agree how the server establishes which producers can contribute to an
account's window before claiming full coverage. Keep reporting and retention
independent of accounting acknowledgements, quota enforcement and full
account pulls.

## Wave integration and acceptance

Once a supporting WaterWall release defines the contract, add a version-specific
adapter and schematic capability. Join authorized samples by target and
account ID inside the shared roster reader. Publish changed samples and remove
them when the target, run, permission or freshness changes. Add sortable
**Current Upload Speed** and **Current Download Speed** columns. Place unavailable
and stale rates after available values. Indicate partial coverage in the UI.

Test idle and busy accounts, two producers, non-unit multipliers, dropped,
replayed and out-of-order reports, stale or stalled producers, producer
restart, partial coverage and an absent rate grant. Send a known volume over
a known window and check both directions against the agreed tolerance. Keep
the existing roster available with WaterWall 1.6.0.
