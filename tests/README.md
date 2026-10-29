# Logic tests

Seven small programs, each one a copy of a piece of real decision logic from the
service with the hardware replaced by a script. They exist because every one of
them caught a bug that reading the code had not, including bugs in the fix being
written at the time.

They are deliberately **copies, not links**. Each file restates the function it
tests, so changing the service does not change the test, and the two drifting
apart is itself the signal to look. Where a test disagrees with the service,
read both before deciding which is wrong.

## Running them

`run-tests.cmd` builds and runs all seven, finding Visual Studio itself, and
exits non-zero if any check fails. Each test prints one line per check.

To build one by hand, from a Visual Studio developer prompt:

```
cl /nologo /EHsc /std:c++17 /W4 alloc_test.cpp /Fe:alloc_test.exe && alloc_test.exe
```

`pipe_test.cpp` is the exception: it links the real `PipeServer.cpp` and so
needs its include path, and it opens two named pipes under test names, so it
must not run while something else holds them:

```
cl /nologo /EHsc /std:c++17 /W3 pipe_test.cpp ..\CecService\PipeServer.cpp ^
   /I ..\CecService /Fe:pipe_test.exe && pipe_test.exe
```

## What each one covers

- **alloc_test** - the candidate selection inside `AllocateLogicalAddress`: the
  avoid mask, the two-poll confirmation, the mask being a preference rather than
  a cage, and the difference between a full bus and one that could not be
  probed. Caught a real flaw in the anti-deadlock guard: counting this device's
  own address as usable made the fallback dead code. Also the address a
  rebuilt device starts on (`set_logicalAddress` and the order `AcquireDevice`
  seeds it in), which is the address a wake keeps, and why the seed matters to
  the mask: unseeded, the own-address exemption fell on 4 whoever held it.
- **addressmove_test** - every path through `MoveIfAddressTaken`: keeping an
  address that turned out free, a genuine move, a move onto Free Use, the
  retry leash and its limit, and the dangerous case where an address is chosen
  but the mask cannot be restored, which must never be reported as a move.
- **port_test** - `ClaimPort` and the `ListDevices` deduplication, against the
  field cases: a soundbar moved onto the port an Apple TV held, the swap
  completing, a bare announcement failing to take a port from a PC whose claim
  carried this app's vendor ID, and the address mask built by
  `TidyPeerAddresses` including a sleeping PC. Also `NotePeer`'s rule that a
  new address is asked directly before it counts as a PC running this app,
  against the torn frames of 2026-10-06 that made a Chromecast into "DESK";
  the minute of silence after which a "PC" that answers polls without a hello
  is taken for an ordinary device, against the Apple TV taking a sleeping
  PC's address on the Vizio; and a device on a new input being asked its name
  again.
- **devices_test** - the `DEVICES` line end to end, the service's exact reply
  builder fed to the tray's exact parser. The field positions there have broken
  twice; this is the cheap way to check them after touching either end.
- **recovery_test** - three decisions that keep a power path honest after
  something underneath it went wrong: which frame is the television's power
  answer (not another PC's hello behind a stale 0x90), when a standby verify's
  answer is too late to act on, and the search for a port after a display
  arrives with none attached - spaced, bounded, kept while the screen is off,
  closed by any attach, and safe across the tick count wrapping. Caught the
  test's own model calling the search after it had attached, and an end tick
  that gave twelve or thirteen tries depending on whether it was odd. Also a
  poll's three outcomes - acknowledged, refused, and no status at all, which
  is neither - and the soundbar probe that read the third as a soundbar. And
  which restarts were Windows Update's own, against real events from the
  desktop. And which devices are asked their names, one a round, and when a
  remote button is a press rather than a held one's repeat, against the LG's
  Select that went unreleased on 2026-09-30.
- **standby_test** - `WaitForStandby` against a scripted television, which is
  hard rule 1 as code: standby believed whenever reported, "on" only from a
  question asked 2.5 seconds or more after the command. The suspend path's
  2.5-second wait therefore never says "on"; ten and five second waits still
  see a set that ignored the command; an early "on" followed by silence is
  unknown; and the deadline survives the tick count wrapping. Fails three ways
  against the old rule, which is the bug it was written for. Also
  `TvNeedsStandby`, whether a sleep or shutdown sends `<Standby>` at all: the
  set's own answer over this service's record either way, the record when the
  set will not say, and this service's own `<Standby>` from the last ten
  seconds trusted without asking.
- **pipe_test** - that a question on the read-only pipe is answered while a
  command is still running on the command pipe. Links the real `PipeServer`.
