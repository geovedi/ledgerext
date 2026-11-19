# Configuring ledgerext

## The settings

All of them are in the `ledgerext` prefix, which is reserved: a setting
outside that prefix will be refused.

| setting | type | default | range | when it takes effect | what it does |
|---|---|---|---|---|---|
| `ledgerext.max_accounts` | integer | `1048576` | 16 – 134217728 | **restart** | how many accounts the grid holds |
| `ledgerext.max_transfers` | integer | `8388608` | 16 – 268435456 | **restart** | how many transfers the grid holds |
| `ledgerext.snapshot_interval_ms` | integer | `5000` | 100 – 3600000 | reload | how often the snapshot worker looks |
| `ledgerext.snapshot_journal_bytes` | integer | `1073741824` (1 GB) | 1 MB – 2 GB | reload | how much journal may pile up behind the snapshot before the grid is written out again |
| `ledgerext.snapshot_body_multiple` | integer | `32` | 1 – 4096 | reload | how many times its own size the journal behind the snapshot may be before the grid is written out again |
| `ledgerext.expiry_interval_ms` | integer | `200` | 20 – 60000 | reload | how often the worker looks for due expiries |
| `ledgerext.replica_of` | string | `''` | | reload | the primary this node follows; empty means this node is a primary |
| `ledgerext.replica_poll_ms` | integer | `200` | 1 – 60000 | reload | how often a replica asks the primary for the records it does not have |
| `ledgerext.replica_sync` | boolean | `true` | | reload | whether a commit waits for the replicas that have registered |

`max_accounts` and `max_transfers` are read when the server starts, because
the grid's size is the shared memory's size and shared memory cannot be
resized under a running server. They belong in `postgresql.conf` (or in
the container's command line), **not** in `ALTER SYSTEM`: that writes
`postgresql.auto.conf`, which is read after the grid is mapped. The other
seven can be changed with `ALTER SYSTEM ... ; SELECT pg_reload_conf();`.

```sql
alter system set ledgerext.replica_of = 'host=primary port=5432 dbname=ledger user=ledger password=...';
alter system set ledgerext.replica_poll_ms = 50;
select pg_reload_conf();
```

## Sizing the grid

The grid is five tables in one mapping, sized from the two limits:

| table | size |
|---|---|
| the account slots | `max_accounts` × 128 bytes |
| the account hash buckets | next power of two ≥ `max_accounts × 2`, × 4 bytes |
| the transfer rows | `max_transfers` × 120 bytes |
| the transfer hash buckets | next power of two ≥ `max_transfers × 2`, × 4 bytes |
| the expiry heap | `max_transfers` × 4 bytes |

With the defaults (1,048,576 accounts and 8,388,608 transfers) that is
128 MB + 8 MB + 960 MB + 64 MB + 32 MB = **1192 MB**, which is 596 huge
pages of 2 MB.

```
sudo sysctl -w vm.nr_hugepages=1200
```

Every node that preloads `ledgerext` maps its own grid, so the pool has
to cover all of them. Without the pool the ledger is correct and about
14% slower — every lookup into the grid pays a TLB miss and a page walk —
and the server says so at startup:

```
LOG:  ledgerext: the grid's 1192 MB is on huge pages
```

The grid's pages are not touched at startup: an anonymous page is zero
when it is first read, so the tables cost nothing until they are used.
The mapping is reserved in full, though, so the memory must be there.

**Raising the limits** is a restart, and the whole ledger comes back:

```
# postgresql.conf
ledgerext.max_accounts = 2097152
ledgerext.max_transfers = 16777216
```

A ledger that does not fit a *smaller* grid stops the server with a
message that names the file, the counts and the setting to raise; nothing
is lost, because the journal is the truth and it is unbounded. See
[operations.md](operations.md#capacity).

## The bindings

`ledger_tenants` maps a role to a book:

| column | type | meaning |
|---|---|---|
| `role_name` | `name`, primary key | the PostgreSQL role |
| `tenant` | `int`, `check (tenant between 1 and 65535)` | the book |

Only the **owner of the table** may write it — the role that ran
`create extension ledgerext`, normally `postgres`. Every role may read
it. The administrator binds:

```sql
insert into ledger_tenants values ('acme', 1);
delete from ledger_tenants where role_name = 'acme';
```

A tenant number is permanent: it is in the journal's records, and a
number that changed would move an account from one book to another. To
give a role a different book, give it a **different role** — or move the
accounts by transferring them.

A role with no row has no book at all. It may neither move money nor
read the ledger, and the refusal names the role and the table:

```
ERROR:  ledgerext: role "ledger_reader" has no tenant
HINT:  The administrator binds a role to a book with
       INSERT INTO ledger_tenants VALUES ('ledger_reader', 1).
```

Two roles may share a tenant, and then they share the book. That is the
administrator's choice.

**Per node.** The bindings live in the database, so every node has its
own. A standby that is promoted needs its own rows, with the **same
numbers**: the numbers are what the journal's records carry, so a record
of tenant 2's book is tenant 2's book on every node.

## The grants

The extension's script revokes `EXECUTE` from `PUBLIC` on every one of
its functions, because PostgreSQL's default would hand the ledger to
every role that can connect. Grant per role:

| function | what it does | typical role |
|---|---|---|
| `ledger_apply(bytea)` | queue a batch of transfers | writer |
| `ledger_create_accounts(bytea)` | queue a batch of account creations | writer |
| `ledger_create_account(uuid,int,int,int)` | create one account | writer |
| `ledger_last_statuses()` | the statuses of the last batch, in this session | writer |
| `ledger_amount(numeric)` | the 16 bytes of an amount, for building a batch | writer |
| `ledger_balance(uuid)` | one account's counters | reader |
| `ledger_totals()` | the caller's book's totals | reader |
| `ledger_ledger_totals(int)` | the totals of one of the client's ledgers | reader |
| `ledger_lookup_transfer(uuid,...)` | one transfer | reader |
| `ledger_pending_transfers()` | the caller's pending transfers | reader |
| `ledger_journal_lookup(uuid)` | the journal's records for one id | reader |
| `ledger_journal_account(uuid,int)` | the journal's records for one account | reader |
| `ledger_journal_ledger(int,int)` | the journal's records for one ledger | reader |
| `ledger_journal_range(bigint,bigint,int)` | the journal's records in a time range | reader |
| `ledger_journal_balances()` | the journal's balances | reader |
| `ledger_period_balances(int)` | the balances frozen at a period close | reader |
| `ledger_status_name(bigint)` | the name of a transfer's status | writer, reader |
| `ledger_account_status_name(bigint)` | the name of an account's status | writer, reader |

The cluster's own functions — `ledger_journal_records`, `ledger_snapshot`,
`ledger_journal_count`, `ledger_journal_periods`, `ledger_capacity`,
`ledger_account_count`, `ledger_transfer_count`, `ledger_expiry_due` —
are for the operator, not for the application. `ledger_close_period()`
writes to the ledger, so it needs a binding, and its result is
whole-cluster.

A grant does not give access to *another* book: the reads are scoped to
the caller's own tenant, whatever the grant says.

## The files

| file | what |
|---|---|
| `$PGDATA/ledgerext.journal` | the ledger's durable truth: one 176-byte record per committed transfer or account creation, in commit order |
| `$PGDATA/ledgerext.snap` | a snapshot of the grid, written by a background worker; a cache — the journal alone rebuilds the ledger |

Both are in the data directory, with the data directory's own
permissions. They are not PostgreSQL relation files: `pg_dump` does not
carry them, and they are not in `base/`. See
[operations.md](operations.md#backups).

## Hardware

The ledger's resource needs are unusually lopsided: **one fast core, a
modest amount of RAM, and a disk that can take a sustained write.**

### The disk is the real limit

Every committed transfer is one 176-byte record, appended to
`$PGDATA/ledgerext.journal` and **fsynced before the client is told the
transfer committed**. So the ledger's sustained write rate *is* the
transfer rate times 176 bytes, and the disk has to make that durable:

| transfers a second | the journal's write rate |
|---|---|
| 1,000 | 176 KB/s |
| 10,000 | 1.8 MB/s |
| 100,000 | 17.6 MB/s |
| 1,700,000 | 299 MB/s — the device's own ceiling |

What that means in practice:

- **An SSD, and an enterprise one.** 299 MB/s of *sustained* writes is
  past what a consumer drive does once its SLC cache is exhausted, and
  the journal never stops. Look at the drive's sustained-write figure,
  not its burst figure.
- **The fsync latency is on the critical path.** Every commit waits for
  it. A drive with a deep write cache and no power-loss protection
  will lose acknowledged transfers on a power cut; one with
  protection (or a battery-backed controller) will not. This is the one
  hardware decision that can lose money.
- **Separate the journal from PostgreSQL's own files if you can.** The
  ledger writes 176 bytes and fsyncs; PostgreSQL's WAL writes nothing
  for a ledger transfer (`0 WAL bytes`, by design) but has its own
  traffic for everything else. Two devices remove the contention
  between them.
- **Size the device by the growth rate, not by the ledger's size.**
  The journal is unbounded: at 100,000 transfers a second it is 1.5 TB
  a day. [operations.md](operations.md#the-journal) has the table, and
  [monitoring](operations.md#monitoring-and-alerting) says what to
  alert on.

### Cores

One pinned core does about **1.7 million transfers a second** at
0.42-0.43 µs of server CPU a transfer. Beyond that, throughput is a
matter of how many connections you run and how the OS schedules them,
not of the ledger: the work under the ledger's lock is a hash lookup,
two counter updates and the append.

What the extra cores buy:

- **the client.** The project's own two-CPU measurement is the server
  and its client competing for the same CPUs, and it reads 0.51-0.53 µs
  a transfer. On a real deployment the client is another host.
- **the snapshot worker and the expiry worker.** They are background
  workers and they need a core between them in a busy deployment.
- **PostgreSQL's own backends**, for everything that is not a ledger
  transfer.

A four-core host with one pinned core for the ledger's backends is a
comfortable production shape. A one-core host will work: it just has
nothing left for anything else.

### Memory

Beyond the grid, the ledger's memory does not grow with load:

| what | how much |
|---|---|
| the grid | `max_accounts` × 128 B + `max_transfers` × 120 B + the hash tables and the expiry heap (1192 MB at the defaults) |
| PostgreSQL's own shared memory | `shared_buffers` and the rest, as any server |
| each backend | a small stack and the batch it is parsing |

The grid is the dominant term and it is fixed at startup. The tuning
question is not "how much memory does the ledger need" but "how many
accounts and transfers do I need in the grid", which is the sizing
section above.

### The network

A commit with `ledgerext.replica_sync = on` waits for every registered
standby's acknowledgement, so the **round trip to the furthest standby is
on the critical path of every commit**. Put the standbys somewhere the
round trip is small: same region, low latency, and one slow link is a
slow ledger.

The replication traffic itself is the journal's rate, so the same
arithmetic as the disk applies: 17.6 MB/s from the primary to each
standby at 100,000 transfers a second.

### A short shopping list

For a deployment at a few thousand transfers a second:

- 4 cores, one of them for the ledger's backends;
- 16 GB of RAM, with 1.2 GB of it on huge pages for the default grid;
- an enterprise SSD with power-loss-protected writes, sized for the
  journal's growth;
- a second standby host in another rack, and a network round trip
  between them you would be happy to pay on every commit.

## The clock

The ledger stamps its own records with the server's clock, in
nanoseconds. The client does not send a timestamp — it cannot, and a
pending transfer's timeout is counted from the server's clock, so every
node agrees on when a reservation expires.

Timezone and clock skew are the operator's problem, as they are for any
database: the server's clock should be NTP-disciplined, and a pending
transfer's timeout is only as good as that clock.
