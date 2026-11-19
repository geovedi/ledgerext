# Installing ledgerext

## What you need

| | |
|---|---|
| PostgreSQL | **18.x** (developed and measured against 18.6-18.7) |
| Toolchain | a C compiler, `make`, and PostgreSQL's server headers (`postgresql-server-dev-18` on Debian/Ubuntu, `postgresql18-devel` on RPM systems) |
| Memory | the grid is mapped in shared memory at startup: **1192 MB by default** (see [configuration.md](configuration.md#sizing-the-grid)) |
| Disk | for the journal, in `$PGDATA`: **176 bytes per committed transfer and per account creation**, and it is never pruned |
| Huge pages | optional but worth it: ~14% faster without them, and the extension says so in the log |

Nothing else. The extension is one shared library, one SQL script, and
one table.

## Build and install

```sh
git clone <your fork> noisycricket
cd noisycricket/ledgerext
make
sudo make install
```

`make install` copies `ledgerext.so` and `ledgerext--0.1.sql` into the
PostgreSQL installation. The SQL file is the extension's only script: no
upgrade scripts are shipped, so PostgreSQL refuses the upgrade rather than
reading a ledger written by another version (see
[operations.md](operations.md#upgrades)).

## Configure the server

The extension registers its own resource manager and maps its grid, so it
must be **preloaded**:

```
# postgresql.conf
shared_preload_libraries = 'ledgerext'
```

`ledgerext.max_accounts` and `ledgerext.max_transfers` are read once, when
the server starts, so they belong in `postgresql.conf` (or in the
container's command line) and not in `ALTER SYSTEM` — `ALTER SYSTEM`
writes `postgresql.auto.conf`, which is read after the shared memory is
set up. The rest of the settings can be changed with `ALTER SYSTEM` and a
reload; they are listed in [configuration.md](configuration.md).

The grid's size is the one number worth planning for. The default is
1,048,576 accounts and 8,388,608 transfers, which is **1192 MB** of
shared memory:

```
sudo sysctl -w vm.nr_hugepages=1200      # 2.4 GB; each node's grid needs 596
```

Without a reserved pool the ledger is still correct, but every lookup
into the grid pays a page walk. The extension says what it got at
startup:

```
LOG:  registered custom resource manager "ledgerext" with ID 128
LOG:  ledgerext: the grid's 1192 MB is on huge pages
```

If the line says the grid is **not** on huge pages, the pool was too
small; raise `vm.nr_hugepages` and restart. Every node that preloads
`ledgerext` maps its own grid, so the pool must cover them all.

## Start the server and create the extension

```sh
sudo systemctl restart postgresql
sudo -u postgres psql -c "create extension ledgerext;"
```

The extension's schema is where PostgreSQL puts it; the table
`ledger_tenants` is created there, and the extension's functions are
created in it too.

## Give the roles their books

The ledger is a set of books, one per tenant, and a role's book is a row
in the extension's own table. **Only the owner of that table writes it**
— the administrator, which is the role that ran `create extension`:

```sql
create role ledger_writer login;
create role ledger_reader login;

insert into ledger_tenants values ('ledger_writer', 1);
insert into ledger_tenants values ('ledger_reader', 1);
```

Two roles with the same tenant share that book; that is the
administrator's choice, and it is how a team of two services can share
one set of accounts.

Every role may **read** the table (the extension grants `SELECT` to
`PUBLIC`), so a role can look up its own binding. The rows hold role
names — which `pg_roles` already shows — and tenant numbers, which mean
nothing to a role that is not bound to them.

## Grant the functions

PostgreSQL grants `EXECUTE` on a new function to `PUBLIC`. That would
hand the whole ledger to every role that can connect, so the extension's
script **revokes** it from `PUBLIC` on every one of its functions. The
administrator grants what each role needs:

```sql
-- the role that moves money
grant execute on function ledger_apply(bytea),
                        ledger_create_accounts(bytea),
                        ledger_create_account(uuid, int, int, int),
                        ledger_last_statuses(),
                        ledger_amount(numeric)
  to ledger_writer;

-- the role that reads
grant execute on function ledger_balance(uuid),
                        ledger_totals(),
                        ledger_ledger_totals(int),
                        ledger_lookup_transfer(uuid),
                        ledger_pending_transfers(),
                        ledger_journal_lookup(uuid),
                        ledger_journal_account(uuid, int),
                        ledger_journal_ledger(int, int),
                        ledger_journal_range(bigint, bigint, int),
                        ledger_journal_balances(),
                        ledger_period_balances(int),
                        ledger_status_name(bigint),
                        ledger_account_status_name(bigint)
  to ledger_reader;
```

A role with no row in `ledger_tenants` may not use the ledger at all,
whatever it has been granted: the refusal names the role and the table.

```sql
ERROR:  ledgerext: role "ledger_reader" has no tenant
HINT:  The administrator binds a role to a book with
       INSERT INTO ledger_tenants VALUES ('ledger_reader', 1).
```

## Verify the install

```sql
select ledger_capacity();
-- {0,1048576,0,8388608}: accounts used/max, transfers used/max

select ledger_journal_count();   -- 0
```

and one transfer end to end:

```sql
select ledger_create_account('11111111-1111-1111-1111-111111111111'::uuid, 0, 1, 1);
select ledger_create_account('22222222-2222-2222-2222-222222222222'::uuid, 0, 1, 1);
select ledger_apply(decode('01000000000000000000000000000000'
  '33333333333333333333333333333333'
  '11111111111111111111111111111111'
  '22222222222222222222222222222222'
  '00000000000000000000000000000000'
  '00000000000000000000000000000000'
  '6400000000000000'
  '0000000000000000'
  '0000000000000000'
  '0000000000000000'
  '0000000000000000'
  '00000000'
  '01000000'
  '01000000'
  '00000000'
  '0000000000000000', 'hex'));
select ledger_last_statuses();   -- {4294967295} — 4294967295 is "created"
select ledger_balance('11111111-1111-1111-1111-111111111111'::uuid);
-- {0,0,100,0,0,1,1}
```

[usage.md](usage.md) explains every byte of that batch and gives you a
helper that builds it for you.

## Trying it without installing anything

The project ships a container harness that builds the extension, creates
two nodes (a primary and a standby) and loads everything:

```sh
./scripts/setup.sh              # builds, creates the nodes, installs
./scripts/test_ext.sh           # 62 groups, 415 checks, all must pass
```

Both nodes are bound to the loopback only (`127.0.0.1:55432` and
`:55433`) and are destroyed and recreated on every `setup.sh` run. The
benchmark needs a fresh cluster every time, so always run `setup.sh`
first.

## What the install leaves behind

| where | what |
|---|---|
| `$PGDATA/ledgerext.journal` | the ledger: every committed transfer and account creation, one 176-byte record each, fsynced before the client is told |
| `$PGDATA/ledgerext.snap` | a snapshot of the grid, written by a background worker; it is a **cache** — the journal alone rebuilds the ledger |
| `ledger_tenants` | the bindings, in the extension's schema |
| the shared memory | the grid: the balances, the account and transfer tables, and the hash tables |

Both files belong to the database cluster's own directory, and they are
the ledger's state — `pg_dump` does not carry them. See
[operations.md](operations.md#backups).
