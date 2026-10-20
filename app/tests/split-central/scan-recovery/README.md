# Split central scan recovery

Run from `app/` after West initialization/update:

```sh
west test tests/split-central/scan-recovery
```

Or build the standalone fixture from the repository root:

```sh
west build -s app/tests/split-central/scan-recovery -b native_sim/native/64 -d build/split-central-scan-recovery
build/split-central-scan-recovery/zephyr/zephyr.exe
```

The fixture compiles production `central.c` directly. Bluetooth scan start/stop,
connection creation/unref and peripheral address storage are stubbed to inject
errors and observe reference ownership. Radio initialization and transport
registration are disabled, and the final peripheral-event handler is stubbed.
The production advertisement parser, Zephyr data/UUID helpers, slot reservation,
cleanup, queues, work and spinlocks run unchanged.

Tests cover a failed scan start followed by a successful retry, repeated errors
without immediate retry loops, host/local already-running state, failed stop and
slot cleanup, failed restart followed by later recovery, already-stopped errors,
reservation rejection, connection-create failure recovery and balanced connection
references. A callback during scan start guards against overwriting its stop
state when the start API completes. An advertisement containing the split UUID
exercises discovery, failed-stop cleanup, rescan and a later successful connection.

On the preceding F3b-only commit, 8 of 15 cases fail; with scan recovery, all
15 pass. No periodic recovery policy, radio behavior or hardware sleep/wake is
simulated. A scan-start failure permits later ordinary calls to retry; a failed
stop releases its reservation before one recovery scan attempt. These conditional
source defects are not evidence of a particular hardware outage's cause.
