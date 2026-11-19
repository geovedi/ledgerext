# ledgerext

A double-entry ledger for PostgreSQL 18, as an extension. Balances live in
shared memory, every committed transfer is one fixed-size record in a
journal file, and the journal is the ledger's only durable truth: a
transfer a client has been told about is on disk before the transaction
is acknowledged.

This is the user manual. It is written for someone who has to **install,
configure, run and administer** ledgerext in a financial application —
not for the developer reading the source. The README at the repository
root is the short version of what it is and how fast it runs; these are
the instructions.

| document | what it answers |
|---|---|
| [installation.md](installation.md) | building and installing the extension, the first roles, the first binding, verifying it works |
| [configuration.md](configuration.md) | every setting, sizing the grid, hardware, the grants, the files on disk |
| [usage.md](usage.md) | the model and the API — accounts, transfers, statuses, pending/post/void, multi-tenancy, and a worked example |
| [recipes.md](recipes.md) | ten patterns: funding, control accounts, exchange, closing, reserving, correcting |
| [data-modeling.md](data-modeling.md) | how to lay the money out: ledgers, units, balance signs, codes, ids, and where the ledger sits |
| [operations.md](operations.md) | capacity, the journal and backups, monitoring, streaming out, snapshots, expiry, replication, upgrades, troubleshooting |
| [faq.md](faq.md) | the questions people actually ask |

## Five minutes

With Docker and the project's own harness (a container per node, both
bound to the loopback):

```sh
./scripts/setup.sh              # builds the extension, creates two nodes, installs
./scripts/test_ext.sh           # every check must pass
```

Then, as `postgres` in the `ledger` database on `127.0.0.1:55432`. (The
project's `setup.sh` already binds `postgres` to tenant 1; on your own
cluster you bind it yourself, and you bind every role that will use the
ledger — [installation.md](installation.md) has the whole procedure.)

```sql
insert into ledger_tenants values ('postgres', 1);
grant execute on function ledger_apply(bytea), ledger_create_accounts(bytea),
                 ledger_create_account(uuid,int,int,int), ledger_last_statuses(),
                 ledger_totals(), ledger_balance(uuid) to postgres;

select ledger_create_account('11111111-1111-1111-1111-111111111111'::uuid, 0, 1, 1);
select ledger_create_account('22222222-2222-2222-2222-222222222222'::uuid, 0, 1, 1);
select ledger_apply(decode(
  '01000000000000000000000000000000' -- header: count 1, kind 0, two pad fields
  '33333333333333333333333333333333' -- the transfer's id
  '11111111111111111111111111111111' -- the debit account
  '22222222222222222222222222222222' -- the credit account
  '00000000000000000000000000000000' -- pending_id: none
  '00000000000000000000000000000000' -- user_data
  '6400000000000000'                 -- amount, low half: 100
  '0000000000000000'                 -- amount, high half: 0
  '0000000000000000'                 -- amount_actual, low half: 0
  '0000000000000000'                 -- amount_actual, high half: 0
  '0000000000000000'                 -- timestamp: the server's clock
  '00000000'                         -- flags
  '01000000'                         -- code
  '01000000'                         -- ledger
  '00000000'                         -- timeout
  '0000000000000000',                -- pad
  'hex'));

select ledger_last_statuses();     -- {4294967295}  (4294967295 = created)
select ledger_balance('11111111-1111-1111-1111-111111111111'::uuid);
                                  -- {0,0,100,0,0,1,1}: 100 debits posted
```

The ids in a batch are **16 raw bytes**, not the text of the UUID: the
`3333…` above is the id of a transfer whose UUID is
`33333333-3333-3333-3333-333333333333`. [usage.md](usage.md) explains
every byte and gives a Python helper that builds the batch for you — you
should not be writing hex by hand.

## What this is not

- **Not a payment policy engine.** The ledger applies the transfer rules
  of the double-entry model and nothing else. Currency conversion, interest,
  rounding, limits, approvals and audit trails are your application's
  work.
- **Not a database of rows.** The accounts and transfers are not
  PostgreSQL tuples, so `pg_dump` does not carry them and MVCC does not
  apply to them. See [operations.md](operations.md#backups).
- **Not a multi-primary cluster.** One primary, one or more standbys that
  follow its journal. There is no quorum and the old primary is not
  fenced. See [operations.md](operations.md#replication-and-failover).
- **Not upgradeable in place across the version barrier.** PostgreSQL
  itself refuses `ALTER EXTENSION ledgerext UPDATE`. See
  [operations.md](operations.md#upgrades).
