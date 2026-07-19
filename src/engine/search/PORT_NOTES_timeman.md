# timeman port notes

`timeman.c` needs no change to any existing header today: the option values and
the search start time it cannot read from `SearchLimits` are passed in as
parameters (`TimemanOptions`, `start_time`). Record the gaps here so the wiring
commit can close them deliberately rather than by accident.

## `SearchLimits` fields upstream carries and mcfish lacks

Requested only when the corresponding feature is wired; none is needed to compile
or to use this module.

- `npmsec` — upstream's `LimitsType::npmsec`, set from the `nodestime` UCI
  option and rewritten by `TimeManagement::init`. `timeman_init` returns the
  converted value in `TimemanLimits.npmsec` instead of writing it back, because
  `search_go` takes its limits by const pointer. Add the field only when
  `nodestime` becomes a real option.
- `start_time` — upstream stamps the clock when the `go` line is parsed, not
  when the search begins; `search.c` currently stamps it inside `search_go`.
  Until the UCI layer owns it, pass `now_ms()` for `start_time`.

## Widths

`SearchLimits.time_ms` / `.inc_ms` are `int`; upstream's `TimePoint` is
`int64_t`. This is safe for a wall clock but not for `nodes as time`, where the
same fields hold a node count that overflows `int` at roughly 2.1e9 nodes.
Widen both to `int64_t` when `nodestime` is wired.

## Not ported here

The score/bound/depth value model is not a table of time-management constants —
it belongs to whichever module owns the search value sentinels. No
`search_values.h` was created: every tuned constant in `TimeManagement::init` is
a literal at its use site upstream, and is kept that way.
