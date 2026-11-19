# Running ledgerext

Housekeeping, administration and troubleshooting. For installing see
[installation.md](installation.md); for the API see
[usage.md](usage.md).

## Day to day

The ledger needs almost nothing. What you should look at:

| question | answer |
|---|---|
| how full is the grid? | `select ledger_capacity();` — `{accounts_used, accounts_max, transfers_used, transfers_max}` |
| how many records are in the journal? | `select ledger_journal_count();` |
| is anything due to expire? | `select ledger_expiry_due();` — how many pending transfers have expired and not been released yet; it falls back to zero once the server has caught up |
| is the grid on huge pages? | the server's log at startup: `ledgerext: the grid's 1192 MB is on huge pages` |

The journal's records are the ledger's history and they are append-only:
nothing ever removes or alters one. The reads
(`ledger_journal_lookup`, `ledger_journal_account`,
`ledger_journal_ledger`, `ledger_journal_range`, `ledger_journal_balances`)
are the way to query that history.

## Monitoring and alerting

The ledger needs almost no attention in normal operation, which makes
the few things that do matter easy to miss. Watch these six, in this
order:

| what | how | alert when |
|---|---|---|
| the journal's disk is running out | the filesystem's own free-space metric on `$PGDATA` | free space falls below the journal's growth rate times your alert window (an hour, say), or below 20% of the device |
| the journal is not growing when it should | `select ledger_journal_count();` polled over a window | the count is flat for longer than your peak-traffic window while your application is accepting writes |
| a standby is falling behind | the standby's `ledger_journal_count()` against the primary's | the gap is larger than one hour's worth of records |
| the grid is filling | `select ledger_capacity();` polled | transfers used above 80% of the max, or accounts used above 80% |
| the clock is wrong | the host's own time check | drift from an authoritative source exceeds a second |
| expiries are not being released | `select ledger_expiry_due();` polled | the value stays above zero for more than a few seconds |

None of these is a metric the extension emits: they are ordinary
PostgreSQL functions, and a poller that can `psql` the database can
read them. What the log adds is the two messages that mean something
has already gone wrong:

```
ledgerext: the grid's 1192 MB is on huge pages        -- a healthy start
ledgerext: the journal "$PGDATA/ledgerext.journal" was written by another version of ledgerext
ERROR:  ledgerext: could not ... "ledgerext.journal"
```

### The disk is the one that bites

The journal is never pruned and it is the ledger's only durable truth,
so the first alert that matters is **the filesystem the journal is
written to is running out of space**. Its growth rate is the transfer
rate times 176 bytes, which [the journal's table](#the-journal) gives
you. An alert threshold of one hour's growth is generous; a threshold
of 20% of the device is a backstop.

What happens if you miss it: the journal cannot be appended, so the
fsync that precedes every acknowledgement fails, so **every transfer
fails**. The ledger does not silently drop records — it refuses to
commit, and your application sees the error. It is loud, and it is
still the failure you least want to have.

### The clock

Every transfer's timestamp is the server's own clock, and a pending
transfer's `timeout` is measured against it. A clock that jumps will
release a reservation at the wrong moment or refuse to release it at
all, on every node at once.

Run NTP (or the equivalent) on every node. The primary's clock is the
one that matters; a standby's own clock decides nothing — expiries are
the primary's decision and travel as records — but a standby that is
drifted by hours will still be a confusing standby when you promote it.

### CPU and memory

The ledger's shared memory is fixed at startup: 1192 MB for the default
grid, and the log says whether the huge page pool covered it. Memory
does not grow with load. If a host is short of memory, the ledger is
not the thing to reclaim — the grid is either on huge pages or it is
not, and it is the same size either way.

CPU is the one resource the transfer rate actually consumes, at about
0.42-0.53 µs of server CPU a transfer. A single pinned core will do
about 1.7 million transfers a second. Watch the primary's CPU
utilisation; a sustained rise with a constant transfer rate means
something else on the host is competing, and the huge page pool is
worth checking first.

### What not to alert on

- **The journal's absolute size.** It grows forever by design. Alert on
  the *rate* and on the *free space*, not on the total.
- **A refused transfer.** A status is a normal outcome, not a failure:
  `46 exists` is a successful retry, `54 exceeds_credits` is a bound
  working as designed. Alert on a *rise in the error rate*, not on
  individual statuses.
- **The snapshot's age on its own.** The snapshot is a cache; a stale
  one only makes a restart longer. Alert on a restart that takes too
  long, or on a snapshot that never appears at all.



The journal's records are the ledger's history and they are append-only:
nothing ever removes or alters one. The reads
(`ledger_journal_lookup`, `ledger_journal_account`,
`ledger_journal_ledger`, `ledger_journal_range`, `ledger_journal_balances`)
are the way to query that history.

## Capacity

The grid's size is fixed when the server starts — the shared memory's
size is not something a running server can change. When it fills, the
ledger **refuses clearly**:

- a transfer that does not fit gets `101 no_transfer_slots`;
- an account that does not fit gets `100 no_account_slots`;
- nothing is lost: the refused event was never recorded, and the
  journal — which is unbounded — still holds everything that was.

Watch `ledger_capacity()` and raise the limits *before* it bites. Raising
them is a restart, and the whole ledger comes back from the journal:

```
# postgresql.conf
ledgerext.max_accounts = 2097152
ledgerext.max_transfers = 16777216
```

If you restart with a grid **smaller** than the ledger needs, the server
stops with a message that names the file, the counts and the setting to
raise. Nothing is lost; raise the setting and restart. The extension
knows the grid's size and whether the huge page pool covered it, and it
says so in the log.

A ledger whose journal is far larger than its grid will replay slowly at
startup: the snapshot (below) is what keeps that short.

## The journal

`$PGDATA/ledgerext.journal` is the ledger's only durable truth: one
176-byte record per committed transfer and per account creation, in
commit order, fsynced **before** the client is told the transaction
committed.

It is never pruned and it is unbounded — that is deliberate. Disk is
cheap compared with a ledger that cannot reconstruct its own history.
Plan for it:

| rate | journal growth |
|---|---|
| 1,000 transfers a second | 176 KB/s — about 15 GB a day |
| 100,000 transfers a second | 17.6 MB/s — about 1.5 TB a day |
| 1,700,000 transfers a second | 299 MB/s — the device's own rate |

The journal is also what a replica follows: a standby ships its records
to a second node, which applies them and appends them to its own journal.

## Backups

**The journal is the ledger.** A backup is a copy of
`$PGDATA/ledgerext.journal`, taken while the server is running — the
journal is only appended to, so a copy taken at any moment is a
consistent ledger's history.

`$PGDATA/ledgerext.snap` is a **cache**: the grid's contents at the
journal's position. It speeds up a restart and it is not needed for a
restore. It is safe to copy or to delete.

`pg_dump` does not carry any of it. The accounts and transfers are not
PostgreSQL relations; the ledger's state is those two files and the
shared memory they rebuild.

**Restoring a backup** — the journal is the only file that matters:

```sh
# stop the server
systemctl stop postgresql

# put the journal where the data directory is
cp -a /path/to/backup/ledgerext.journal $PGDATA/ledgerext.journal
# the snapshot is optional; a fresh one is written after the start
rm -f $PGDATA/ledgerext.snap

# start; the grid is rebuilt by replaying the journal
systemctl start postgresql
```

Watch the log: `no snapshot; replayed N journal records`. With no
snapshot, the whole journal is replayed, so the restart is as long as
that takes. With a snapshot, only the records after its position are.

**A lost journal is a lost ledger.** If `$PGDATA/ledgerext.journal` is
deleted, the server starts a **new, empty** journal and the ledger begins
from nothing — silently. There is no other copy. This is the single most
common way to lose a ledgerext deployment, and it is why the journal
belongs in your backup.

## The snapshot

A background worker writes the grid to `$PGDATA/ledgerext.snap`,
tagged with the journal's position it covers. Startup loads the snapshot
and replays the journal's records **after** that position, so a short
journal behind the snapshot makes a short restart.

It is written when the journal behind it is large enough — either a
fixed amount (`ledgerext.snapshot_journal_bytes`, default 1 GB) or a
multiple of the snapshot's own body (`ledgerext.snapshot_body_multiple`,
default 32) — and at most once every
`ledgerext.snapshot_interval_ms` (default 5 s). The journal is fsynced
before the body is written, so the snapshot can never cover a record that
is not on disk.

The snapshot carries its own version. A snapshot from another version of
ledgerext is refused, like a journal from one, and the message says so.

## Expiry

A pending transfer with a `timeout` is released by the server itself
when the timeout passes — nobody has to void it. The worker runs every
`ledgerext.expiry_interval_ms` (default 200 ms), and the release is
journalled like any other record, so the balances stay reconstructible
from the journal alone. A reservation is never given back twice, not even
if a standby takes over.

`select ledger_expiry_due();` says how many are due. On a replica the
expiry worker does not run: expiries are the primary's decision and
travel to the replica as records.

## Closing periods

A **period** is the ledger's own bookkeeping boundary. `ledger_close_period()`
writes a close record; every book sees it (the record carries no book),
but a role that owns no book may not close a period.

```sql
select ledger_close_period();                 -- the period number
select * from ledger_journal_periods();       -- period | close_seq | records
select * from ledger_period_balances(3);      -- the balances frozen at period 3
```

The balances frozen at a close are the ones you close your books on. The
journal's records after the close belong to the next period.

## Streaming out: the journal as your change feed

`ledger_journal_records(from_seq, lim)` returns the journal's records
after a position, **as the raw 176-byte records they are on disk**, up
to 4,096 per call. That is the function a replica uses, and it is also
the change data capture (CDC) feed you can use yourself: a job that
polls it with the last sequence it has seen gets every change to every
account, in commit order, exactly once.

```sql
select ledger_journal_records(0, 100);
-- \x… 100 records, 176 bytes each
```

How to use it:

1. **Keep a position.** The position is a record's `seq`, monotonic and
   gap-free. Store it in your own database next to whatever you do with
   the records, in the same transaction, so a crash never loses a
   record or applies one twice.
2. **Poll from the position + 1.** A call returns an empty `bytea` when
   there is nothing yet; that is the normal quiet case. There is no
   notification and no waiting: poll on a timer.
3. **Decode with the record's layout**, which is the one in
   [usage.md](usage.md#the-batch)'s account and transfer tables plus the
   journal's own fields. `ledger_journal_range` gives you the same
   records *decoded* — `seq, timestamp, id, pending_id, debit, credit,
   amount, amount_actual, ledger, code, flags, state, period, kind` —
   which is the easier interface for a small feed. Use
   `ledger_journal_records` when you need `user_data` (the structured
   reads do not expose it) or when you want to move the bytes without
   decoding them on the server.

Two properties make this a safe feed:

- **A record is written before the transaction is acknowledged**, so a
  position you have recorded is a position whose effects are durable.
- **The journal is append-only and never pruned**, so a consumer that
  falls behind never loses a record: it just reads more when it catches
  up.

What it is not: **not a queue**. The journal keeps everything forever
and one consumer does not consume anything. Two consumers read the same
records independently, each from its own position, and neither affects
the other. Design your consumer to be replayable from any position —
that is the guarantee the journal actually gives you.

The records are **whole-cluster**: `ledger_journal_records` is one of
the reads that needs no tenant, and it returns every book's records.
If your feed is per book, filter on the flags' high half (the tenant)
yourself, or run the consumer as the role that owns the book and use
`ledger_journal_range`.

## Replication and failover

One primary, one or more standbys that follow its journal. A standby
decides nothing: it applies the records it is given with the same code
that rebuilds a grid from the journal at startup, and it applies no
transfers of its own until it is promoted.

To make a node a standby, set `ledgerext.replica_of` to the primary's
connection string and reload:

```sql
alter system set ledgerext.replica_of =
  'host=primary port=5432 dbname=ledger user=ledger password=...';
alter system set ledgerext.replica_poll_ms = 50;   -- 50 ms is fine
select pg_reload_conf();
```

While `ledgerext.replica_of` is set the node is a replica: it follows the
primary, it refuses writes of its own, and it does not run the expiry
worker. With `ledgerext.replica_sync = on` (the default) the primary's
commit **waits** for the registered standbys' acknowledgements, so what
the client was told is on both nodes. Set it to `off` to stop waiting —
then a failover can lose what was only acknowledged on the primary.

The standby's own books are its own: it has its own `ledger_tenants`,
with the **same** tenant numbers. The numbers are what the journal's
records carry, so a record of tenant 2's book is tenant 2's book on every
node; the rows are what say which role may use it there.

**Failover** is a `docker kill`-equivalent of the primary and a setting
change on the standby:

```sql
-- on the standby
alter system set ledgerext.replica_of = '';
select pg_reload_conf();
```

The promoted node keeps its grid and its journal, its journal continues
where the replicated records left off, and from then on it decides for
itself.

**What this is not: a quorum protocol.** There is no quorum and the old
primary is not fenced. After a promotion the operator must not let the
old primary accept writes: it would fork the ledger. Use a single
primary, keep the standby in sync, and take the promotion as an
operator's action — not as a system's automatic decision.

## Cluster sizing and fault domains

How many standbys, where, and what each one is worth.

**The count.** One primary plus one standby is the minimum that
survives the loss of a node. Two standbys is the usual production shape
— one for the fast failover and one for a slow, off-site one — and it
costs you a second copy of the journal and a second commit wait while
`ledgerext.replica_sync` is on. Three or more is rarely worth it
without a quorum, because the replication here is a chain of
acknowledgements and not a vote.

**The domains.** A standby that shares the primary's failure domain —
same rack, same power supply, same region, same account with the same
cloud provider — is a copy, not a recovery. Put them apart: different
host, different rack, different region. The journal is small relative
to the ledger's importance, and its cost is bandwidth and a commit
wait, not capacity.

**The roles.** Every node needs the same administrator and the same
binding table, with the **same tenant numbers**. The numbers ride in the
journal's records, so a record of tenant 2's book is tenant 2's book
everywhere; the rows in `ledger_tenants` say which role may use it
there, and they are per node.

**What the standby is worth.** It follows the primary's journal and it
does not decide anything, so:

- It can be **read** as a reporting node, in the ordinary way, as long
  as you remember that its ledger lags by up to `replica_poll_ms`
  (50-200 ms is typical).
- It cannot **take over automatically**. Promotion is an operator's
  action, and the operator must then not let the old primary accept
  writes.
- Its journal is its own, and it is a full copy. A standby's
  `$PGDATA/ledgerext.journal` is a complete backup of the ledger's
  history up to the position it has applied.

**What to do about split-brain, since there is no fencing.** After a
promotion the old primary is still able to accept writes, and if it
does the ledger forks into two histories. Three things prevent it in
practice:

1. **Promote deliberately.** One operator, one action, one node.
2. **Take the old primary's ability to write away at the same time.**
   Stop the server, or take the network away, or both. Do not leave it
   as "a node that could come back".
3. **Reconcile from the journal.** If both nodes have taken writes,
   both journals are readable and the histories are comparable. The
   one you keep is the primary's; the other's records are
   exportable as [operation 2 of the upgrades section](#upgrades).

There is no automatic way to do this safely, and pretending otherwise
is how ledgers fork. The honest design is one primary, standbys that
follow it, and an operator who knows that a promotion is a decision
about which of two nodes becomes the truth.

## Rebuilding a lost standby

A standby is **derived**. Everything it holds came from the primary's
journal, so a lost or broken standby is rebuilt by making it empty and
letting it follow. There is nothing to restore, nothing to migrate, and
no step that could leave it in a state it cannot explain.

```sh
# 1. on the standby: stop it and clear the data directory's ledger state
systemctl stop postgresql
rm -f $PGDATA/ledgerext.journal $PGDATA/ledgerext.snap
systemctl start postgresql
```

With no journal and no snapshot the node starts **empty** — an empty
grid and an empty journal — and registers with the primary at position
0. The primary sends it every record from the beginning, it applies
them the same way a startup replay does, and its own journal is
recreated as it goes.

Two things to expect, both normal:

- **The catch-up is the whole ledger.** A node that starts from zero
  replays every record the primary has. At 176 bytes each, a billion
  transfers is 176 GB of records to ship; this is why a fresh standby
  is a task, not an instant. Keep the standby's data directory (or a
  cheap copy of the journal) somewhere and copy it back to make the
  rebuild short.
- **The replica refuses a stream it cannot account for.** If the
  primary sends a record whose sequence is not the next one the
  standby expects, the standby stops with a message naming both
  positions. That is the protection against applying a damaged or
  out-of-order stream, and it means a half-built standby is visibly
  broken rather than subtly wrong. Rebuild it again.

While the standby is rebuilding, `ledgerext.replica_sync` is still on,
so the primary's commits wait for this node's acknowledgements. That
is the point of the wait — but it also means a rebuilding standby
slows the primary down to the rate at which it can ship the whole
ledger. If that is a problem, take the standby out of the
`replica_sync` group (it has no other job) until it is caught up, or
rebuild it from a copy of the journal on a quiet path.

The other things a rebuilt standby needs, because they are its own and
not the primary's:

- **its own `ledger_tenants` rows**, with the same tenant numbers;
- **its own grants**, per role, the same as the primary's;
- **its own `shared_preload_libraries` and its own GUCs**, including
  `ledgerext.replica_of`.


## Upgrades

PostgreSQL refuses the upgrade:

```sql
alter extension ledgerext update to '0.2';
-- ERROR:  extension "ledgerext" has no update path from version "0.1"
```

That is deliberate. A version can change the *meaning* of the journal's
record or of the grid's rows without changing their size — the tenant, for
instance, rides in the record's flags' high half and in the rows' padding
— so a data directory written by another version would be read as this
one and every account in it rebuilt in **no book at all**. The journal's
and the snapshot's version bytes are the barrier: a directory written by
another version is refused rather than misread.

```
FATAL:  ledgerext: the journal "$PGDATA/ledgerext.journal" was written by another version of ledgerext
HINT:  Remove the file to start with an empty ledger.
```

There is no in-place migration between versions that change the meaning
of the records. The honest options are:

1. **Run the old version's node until your own records have been
   re-imported** into a new node — the application's own database is the
   second source of truth; the journal is the ledger's.
2. **Export from the old node and rebuild.** The journal's records are
   readable — in fields with `ledger_journal_lookup`,
   `ledger_journal_account`, `ledger_journal_ledger` and
   `ledger_journal_range`, and as the raw bytes they are on disk with
   `ledger_journal_records()`. A batch of transfers can be rebuilt from
   those fields and applied to the new node with `ledger_apply()`, with
   fresh ids. Note that the structured reads do not expose `user_data`;
   the raw records do.
3. **Do not upgrade.** Nothing forces it.

Deleting the journal to clear a version mismatch starts the ledger from
**nothing**. That is a decision to make on purpose, never by following a
hint.

## Troubleshooting

| symptom | cause | what to do |
|---|---|---|
| `ERROR: ledgerext: role "x" has no tenant` | the role has no row in `ledger_tenants` | bind it: `insert into ledger_tenants values ('x', 1);` |
| `ERROR: ledgerext: could not ... "ledgerext.journal"` | the file is missing or unreadable | check the data directory's ownership; **a missing journal starts an empty ledger** — restore it from backup |
| `FATAL: ... was written by another version` | the data directory is from another version of ledgerext | see [Upgrades](#upgrades); do not delete the file casually |
| `permission denied for table ledger_tenants` | a role tried to write the binding table | only the table's owner writes it; the administrator binds |
| `permission denied for function ledger_apply` | the role has no `EXECUTE` grant | grant it, per role: see [configuration.md](configuration.md#the-grants) |
| the ledger is slower than it should be | the huge page pool did not cover the grid | `sysctl -w vm.nr_hugepages=...`, restart, and look for `the grid's ... is on huge pages` in the log |
| a transfer gets `101 no_transfer_slots` or `100 no_account_slots` | the grid is full | raise `ledgerext.max_transfers` / `ledgerext.max_accounts` and restart; watch `ledger_capacity()` |
| a commit waits a long time | a replica is registered and `ledgerext.replica_sync` is on | check the standby's log; set `replica_sync = off` if you do not want the wait |
| the server stops at startup with a "grid does not fit" message | the journal is larger than the grid you configured | raise the limits and restart; nothing is lost |
| a pending transfer was released twice | it cannot be: the release is a journal record and the expiry worker does not run on a replica | if you see it, the node's journal and its grid disagree — restore from the journal |
