# ledgerext

A double-entry financial ledger as a PostgreSQL 18 extension. Balances live
in a dense array in shared memory; every committed transfer is one
fixed-size record in `$PGDATA/ledgerext.journal`; the journal is the
ledger's only durable truth and is fsynced before the transaction is
acknowledged. PostgreSQL's write-ahead log carries no ledger payload — 0
bytes a transfer.

Version 0.1. The user manual is under [`docs/`](docs/README.md).

## Measured

Batch of 8189 events, client pinned to the server's CPUs, a fresh cluster
each run:

| shape | transfers/s | server CPU/transfer | WAL/transfer |
|---|---|---|---|
| 2 clients on 2 CPUs (`bench/ext_bench.sh 8189 2 400`) | 1.30M | 0.527 us | 0 B |
| 1 server CPU + 1 client CPU (`bench/goal_bench.sh 8189 200 4`) | 1.69M | 0.435 us | 0 B |

Against a per-transfer budget of 2.2-2.5 us on the same two CPUs, and a
vanilla PostgreSQL table baseline of 5.25-10.0 us.

In the one-core shape the server is disk-bound, not CPU-bound: it uses
about 0.43 of the core it has, and the journal's 176 bytes a transfer is
~300 MB/s of the device.

## Install

```bash
cd ledgerext && make && sudo make install
```

`shared_preload_libraries = 'ledgerext'` in `postgresql.conf`, then
`CREATE EXTENSION ledgerext`. The grid asks for explicit huge pages, which
need a pool on the host — without one the ledger is correct and about 14%
slower:

```bash
sudo sysctl -w vm.nr_hugepages=1200     # each node's grid needs 596
```

Full procedure: [docs/installation.md](docs/installation.md).

## Use

A role needs a binding, a row in the extension's own table, before it can
use the ledger:

```sql
insert into ledger_tenants values ('acme', 1);
grant execute on function ledger_apply(bytea), ledger_create_accounts(bytea),
                        ledger_last_statuses(), ledger_balance(uuid) to acme;
```

```sql
select ledger_create_account('11111111-1111-1111-1111-111111111111'::uuid, 0, 1, 1);
select ledger_apply($batch);            -- one packed bytea, queued until commit
select ledger_last_statuses();          -- one status per event
select ledger_balance('1111...'::uuid); -- the caller's own book
```

The API, the batch format, the statuses and worked examples:
[docs/usage.md](docs/usage.md).

## Design

- **The grid.** Accounts and transfers live in dense arrays in shared
  memory, with open-addressed uuid indexes, mapped by the extension itself
  2 MB aligned. No MVCC, no tuples, no btree descent — PostgreSQL's storage
  engine is not in the per-transfer path. `ledgerext.max_accounts`
  (1048576) and `max_transfers` (8388608) fix the size at startup.
- **The journal.** One 176-byte record per committed transfer and per
  account creation, written in the same locked section that applies the
  events, fsynced before the acknowledgement. The grid is a cache of it:
  startup loads the snapshot and replays the records after it.
- **The snapshot.** The whole grid, written when the journal behind the
  last body reaches `ledgerext.snapshot_body_multiple` (32) times the
  body's size, so its cost is a fixed fraction of the journal's.
- **The commit point.** Work is applied at `XACT_EVENT_PRE_COMMIT`, under
  one exclusive LWLock, after every event is validated: a failing event
  returns its own status, a failing system aborts the transaction with
  nothing applied and nothing written. `ROLLBACK` and `ROLLBACK TO
  SAVEPOINT` discard queued work.
- **The books.** Every account belongs to one of up to 65535 tenants. A
  tenant rides in the grid rows' padding and the record's flags' high half
  — nothing grows. The commit path compares it where it already compares
  the ledger; the reads are scoped to the caller's book.
- **Replication.** A standby asks the primary for the journal's records
  after a position (`ledger_journal_records`), applies them with the same
  replay code, appends them to its own journal and acknowledges. One
  implementation of the rules, so the two grids cannot disagree. It is not
  a quorum protocol, and an old primary is not fenced.

Design notes that go deeper: [docs/operations.md](docs/operations.md) for
capacity, backups, replication and upgrades; [docs/data-modeling.md](docs/data-modeling.md)
for how to lay the money out.

## Repository layout

```
ledgerext/
  ledgerext.h        constants, the wire formats, the grid's structs
  ledger_state.c     the grid and the double-entry state machine
  ledger_journal.c   the journal file, its queries, its replay
  ledger_wal.c       the custom rmgr, the startup replay, the snapshot
  ledgerext.c        shared memory, the work queue, the commit hook
  ledger_expiry.c    the expiry heap, the release rounds, the worker
  ledger_replica.c   the replica's worker: follow, apply, journal, ack
  ledger_sql.c       the SQL surface
  ledger_tenant.c    the tenant's resolution, once per transaction
  ledgerext--0.1.sql the extension's SQL: the tables and the functions
bench/
  bench_ext.c        C client: one packed bytea per batch
  bench_uuid.c       C client for the vanilla-tables baseline
  mkbatch.py         builds a batch from a readable spec
  final_matrix.sh    head-to-head: vanilla vs ledgerext, same container
  ext_bench.sh       sustained run, client pinned to 2 CPUs
  goal_bench.sh      one server CPU, one client CPU
  vanilla.sql        the baseline schema
scripts/
  setup.sh           builds everything, creates two nodes, installs
  test_ext.sh        62 check groups, 415 checks
docs/                the user manual
```

Both benchmarks need a fresh cluster: `./scripts/setup.sh` before each.

```bash
./scripts/setup.sh
./scripts/test_ext.sh
```

## What is not built

Imported (backdated) events; query filters; multi-node quorum replication;
fencing of an old primary after a promotion; spilling the working set to
disk — the grid is RAM-resident, refuses clearly at its limit with statuses
100 and 101, and the history stays on disk in the journal. Writers
serialize on one global LWLock: fine at 2 CPUs, untested beyond. There is
no `ledger_reset()`; nothing in the interface removes or alters a recorded
transfer.

Upgrades between versions are refused rather than migrated, and no upgrade
scripts are shipped. See [docs/operations.md](docs/operations.md#upgrades).

## License

MIT
