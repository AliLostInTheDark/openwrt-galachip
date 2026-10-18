# Lightweight status tests

Run `node light-status.mjs` from this directory. It checks LuCI polling,
renders 8,640 simulated refreshes, and verifies the menus, ACLs and absence of
the history daemon. No live router or browser is used.

`node light-status.mjs --backend` emits a source-extracted ucode harness.
Run that output with a host `ucode` interpreter to check that live status
skips configuration/MIB reads, full SSH status remains available, and native
errors are preserved. These are mocked calls, not a 24-hour hardware test.
