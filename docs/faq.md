# Frequently asked questions

## The model

### What is a tenant, in one sentence?

A 16-bit number that partitions the ledger into books: every account
belongs to exactly one tenant, and a role's tenant says which book it
owns. It is chosen by the administrator, never by the client, and it never
changes.

### Why is it called a tenant and not a schema or a database?

Because it is a partition *inside* one ledger, not a separate ledger.
The books share the journal, the grid and the transaction: a transfer
cannot cross a book, but the operator sees one system, one journal and
one set of balances per book.

### Which `must_not_exceed` flag goes with which balance?

| the account's balance | the flag |
|---|---|
| debits exceed credits: assets, expenses | `DEBITS_MUST_NOT_EXCEED_CREDITS` (2) |
| credits exceed debits: liabilities, equity, income | `CREDITS_MUST_NOT_EXCEED_DEBITS` (4) |
| neither: a control or clearing account | none |

A debit that would overdraw a `DNC` account is `54 exceeds_credits`; a
credit that would overfill a `CND` account is `55 exceeds_debits`.
[usage.md](usage.md#the-five-kinds-of-account) has the full table.

### The client never sends a tenant. Isn't that a limitation?

It is the point. There is nowhere in the wire format to put one, so a
client cannot claim to be a tenant it is not. The server stamps the
transaction's tenant when it applies the event. A client that wants to
act as another book must be *another role*, with its own binding and its
own grant.

### Can two roles share a book?

Yes. Give them the same tenant number:

```sql
insert into ledger_tenants values ('payments_service', 1);
insert into ledger_tenants values ('payments_reconciler', 1);
```

They then share the accounts, the balances and the journal's view. That
is the administrator's choice, and it is the usual way to let a service
and a reconciliation job work on the same book.

### Can one role own several books?

No. A role has one tenant, so it has one book. Give the second book to a
second role.

### What is the difference between a tenant and a ledger number?

The ledger number is **yours**: it partitions the accounts that may
transfer among themselves. The tenant is **ledgerext's**: it partitions
the accounts a *role* may touch. They are orthogonal — two books may use
the same ledger number, and the suite checks exactly that.

## Durability and correctness

### If I commit, is the money really moved?

Yes. The transfer is applied under the ledger's lock, written to the
journal, and the journal is **fsynced before the transaction is
acknowledged**. A transfer a client was told about is on disk.

### Is it ACID?

The transfer is atomic and durable. It is **not** isolated in the
PostgreSQL sense, and the difference matters:

- The ledger resolves its own conflicts, under its own lock, at commit
  time. A transfer that would violate the ledger's rules is **refused
  with a status**, and the other events in the same batch still apply —
  each one gets its own status.
- A `SERIALIZABLE` transaction is **refused** up front: PostgreSQL
  checks serializability *after* the commit hook has applied the work, so
  the work would be applied and the transaction would then fail. Use
  `READ COMMITTED` or `REPEATABLE READ`.
- `ROLLBACK` and `ROLLBACK TO SAVEPOINT` discard the queued work.

### What happens if the server crashes mid-transfer?

The journal is fsynced before the acknowledgement, so a transfer that was
acknowledged is on disk. A transfer that was not acknowledged was never
written and never applied. On restart the grid is rebuilt from the
snapshot and the journal, so the balances and the journal always agree.

### What if I send the same transfer twice?

The second one is refused with `46 exists` (or the specific
`exists_with_different_...` if a field differs), and nothing moves
twice. **The transfer's id is the idempotency key** — use a stable one,
derived from your own request id.

### Is an id burnt when a transfer is refused?

No. A refused transfer was never recorded, so the same id may be used
again once the cause is fixed.

## Reading and writing

### How do I get the status of a transfer I just sent?

In the **same session**:

```sql
select ledger_apply(decode('...','hex'));
select ledger_last_statuses();
```

The statuses live in the backend that applied them. A second connection
will not see them, and if you queue two batches in one transaction, the
statuses of the last one are what you read.

### The statuses are bigints and 4294967295 is "created". Why?

The `created` status is `0xFFFFFFFF`, which does not fit in an
`int4`. The statuses are `bigint` so the value is not truncated.

### How do I look up an account's balance?

```sql
select ledger_balance('11111111-1111-1111-1111-111111111111'::uuid);
-- {debits_pending, credits_pending, debits_posted, credits_posted,
--  flags, code, ledger}
```

An account that does not exist **and** an account that belongs to another
book both return nothing. A client cannot tell them apart, which is the
point.

### Why is my amount a `numeric` and not a bigint?

The ledger's amounts are 128-bit unsigned, from 0 to 2^128-1.
PostgreSQL has no 128-bit integer type, and `numeric` holds every value
of the range exactly. A `bigint` would not.

### Can I have floats or decimals?

No. An amount is an integer count of **your** smallest unit. Choose the
unit — a cent, a minor unit — and keep everything else in your
application's own tables.

### What is `user_data` for?

Sixteen bytes of yours, carried through to the journal's records. The
ledger does not read it.

### What is the `timestamp` in a batch?

Nothing: send zero. The server stamps the record with its own clock when
it applies the event, which is why a pending transfer's timeout means the
same thing on every node.

### Can I post part of a pending transfer?

Yes. A post with a smaller amount than the pending's moves that much and
the rest stays reserved. The amount `2^128-1` — the largest there is —
means "post the whole pending transfer". A **void**, on the other hand,
must void all of it.

### What does a linked chain do?

The events of a linked chain fail as a unit: if one of them fails, all
of them do, and none of them is applied. It is how you make a set of
transfers atomic with respect to the ledger's rules.

### How long can a chain be, and can it span batches?

As long as you like, and no. A chain is a run of consecutive events
where every one but the last carries `LINKED`; it ends at the end of the
batch. A batch whose last event is linked ends with an open chain and
every event in it gets `2 linked_event_chain_open`.

### Is the batch atomic?

The **commit** is: every event is applied under the ledger's lock, and
the journal is fsynced before you are told. The **events** are not: each
gets its own status, and a refused event does not stop the rest. If you
need all-or-nothing, put the events in one linked chain.

### How many events can one batch hold?

1,000,000 transfers or 4,000,000 accounts. More than that is a SQL
error, not a status — it is a malformed request, not a refused event.

### Why did my journal query stop at 1,000 rows?

The `lim` argument defaults to 1,000 on `ledger_journal_account`,
`ledger_journal_ledger` and `ledger_journal_range`. Pass `-1` for no
limit, or a limit you will notice.

### Can I query the journal by `code` or `user_data`?

Not through the SQL functions: they filter by ledger, by account and by
time. Read `ledger_journal_records()` and filter in your application, or
keep your own index.

### What is `amount_actual`?

What actually moved, as opposed to `amount`, which is what the event
asked for. They differ for a balancing transfer and for a post of the
whole pending transfer. [usage.md](usage.md#amount-and-amount_actual)
has the table.

### Can I use the journal as a change feed?

Yes. `ledger_journal_records(from_seq, lim)` returns the raw records
after a position, up to 4,096 at a time, and the journal is append-only
and never pruned, so a consumer that falls behind never loses a record.
It is not a queue: two consumers read independently and neither consumes
anything. [operations.md](operations.md#streaming-out-the-journal-as-your-change-feed)
has the pattern.

## Operations

### How much disk does the journal need?

176 bytes per committed transfer and per account creation, and it is
never pruned. At 100,000 transfers a second that is 1.5 TB a day.

### Can I prune the journal?

No. The journal is the ledger's only durable truth: the balances, the
history and the replica's stream all come from it. The snapshot is the
cache; the journal is not a log you may rotate.

### How do I back it up?

Copy `$PGDATA/ledgerext.journal`. It is only appended to, so a copy
taken while the server runs is consistent. `pg_dump` does not carry it —
the accounts and transfers are not PostgreSQL relations.

**A deleted journal is a deleted ledger:** the server starts a new, empty
one without a word. This is the most common way to lose a ledgerext
deployment.

### What happens if the grid fills up?

The events that do not fit are refused with `100 no_account_slots` or
`101 no_transfer_slots`. Nothing is lost — they were never recorded — and
the journal is unbounded. Raise `ledgerext.max_accounts` /
`ledgerext.max_transfers` and restart.

### Why is the server slow without huge pages?

Every lookup into the grid pays a TLB miss and a page walk: about 14%
slower. The grid is 1192 MB by default and asks the host for its
**explicit** huge pages, so the host needs a pool:

```
sudo sysctl -w vm.nr_hugepages=1200
```

The server's log says whether the pool covered it.

### Can I run two primaries?

Not safely. There is no quorum and the old primary is not fenced: after a
promotion the operator must not let the old primary accept writes, or
the ledger forks. One primary, standbys that follow it.

### What does the standby need from me?

Its own `ledger_tenants` rows, with the **same** tenant numbers, and its
own grants. The numbers are what the journal's records carry, so a
record of tenant 2's book is tenant 2's book on every node; the rows are
what say which role may use it there.

### How do I upgrade?

PostgreSQL refuses it, on purpose:

```
ERROR:  extension "ledgerext" has no update path from version "0.1"
```

No upgrade script is shipped, so a data directory written by another
version of ledgerext would be read as a new one and every account in it
rebuilt in no book at all. A directory written by another version is
refused rather than misread. See [operations.md](operations.md#upgrades)
for the ways forward.

### Can two versions run side by side?

Yes — a different `major_version` in the extension's directory, or a
different PostgreSQL installation. What cannot happen is one data
directory read by two versions.

## Performance

### How fast is it?

About **1.67 million transfers a second** on one pinned core, at 0.42 µs
of server CPU a transfer, with **0 bytes** of PostgreSQL's write-ahead
log per transfer (the journal is the log). On the project's own 2-CPU
measurement, where the client competes for the same CPUs, it is
0.51-0.53 µs a transfer.

### Why is the write-ahead log empty?

The ledger's records live in the journal, which is made durable before
the transaction is acknowledged. The extension registers a resource
manager but writes no ledger payload to PostgreSQL's log: the journal
*is* the log.

### Does the tenant cost anything?

Nothing the budget can see. The tenant rides in bytes that already
existed — the grid's account row, the grid's transfer row's padding, and
the high half of the journal record's flags — so nothing grows, the wire
does not change, and the commit path pays one comparison beside the
ledger's own. The binding is one index lookup per transaction, not per
transfer.

### What is the bottleneck?

The disk, in the one-core shape: the journal's 176 bytes a transfer at
1.7 million transfers a second is about 299 MB/s, which is the device's
own rate. The server uses a third of the core it has.

## The last word

### Where is the truth?

The journal. The grid in shared memory is a cache of it, rebuilt at
startup by replaying the journal's records; the snapshot is a faster way
to do the same thing. If the two ever disagree, the journal is right.

### What is not implemented?

- Currency conversion, interest, rounding — the ledger counts integers.
- Approval workflows, limits, sanctions checks, audit trails — your
  application's tables.
- Multi-currency balances and the reporting over them.
- A quorum protocol: the replication is a primary and its standbys, with
  an operator's promotion.
- An in-place upgrade path between versions that change the meaning of
  the records.
