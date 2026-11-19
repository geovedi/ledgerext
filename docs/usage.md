# Using ledgerext

This is how you use the ledger in an application: the model, the API, the
statuses, and a worked example. For installing it see
[installation.md](installation.md); for running it see
[operations.md](operations.md).

## The model in one page

**Accounts** hold money. **Transfers** move it: a transfer takes an amount
from a debit account and gives it to a credit account, in one step, or
nothing happens at all. That is double entry — the money is never
created or destroyed by a transfer, it moves.

Three more ideas:

- A **ledger number** is *your* partition of the accounts: the accounts
  that may transfer among themselves. It is a `uint32` that your
  application chooses. The ledger number partitions the accounts
  that may transfer among themselves, and nothing else.
- A **code** is *your* classification of an account (customer, merchant,
  house, …). It is a `uint32` that your application chooses.
- A **book** (the ledger calls it a *tenant*) is ledgerext's own
  partition: the set of accounts that one PostgreSQL role owns. Two
  tenants cannot see each other and cannot move each other's money.

An account belongs to exactly one book. The book is the administrator's
choice, bound to a role in `ledger_tenants`; the client never sends it.

### The five kinds of account

Double-entry bookkeeping has five kinds of account, and the ledger
models all five as accounts — it does not know which is which, but two
of them need a flag, and getting the flag wrong is the classic
beginner's mistake:

| kind | what it holds | the balance it normally has | the flag |
|---|---|---|---|
| **asset** | what you own: cash, receivables | debits exceed credits | `DNC` if you do not want it to go below zero |
| **liability** | what you owe: payables, customer balances | credits exceed debits | `CND` if you do not want it to go above zero |
| **equity** | owners' stake | credits exceed debits | `CND` |
| **income** | money earned | credits exceed debits | `CND` |
| **expense** | money spent | debits exceed credits | `DNC` if it must not go negative |

The mapping to the flags is the thing worth remembering:

- **`DEBITS_MUST_NOT_EXCEED_CREDITS` (2) bounds a *debit-balance*
  account**: it may not pay out more than it holds. Asset and expense
  accounts. "The debits may not exceed the credits" — so its balance
  cannot go below zero.
- **`CREDITS_MUST_NOT_EXCEED_DEBITS` (4) bounds a *credit-balance*
  account**: it may not be credited past its own debit. Liability,
  equity and income accounts. "The credits may not exceed the debits" —
  so its balance cannot go above zero.

An account with **neither** flag has no bound in either direction. That
is what a control or clearing account wants, and it is why the operator's
account has none.

The statuses say the same thing from the transfer's point of view: a
debit on a `DNC` account that would overdraw it is `54 exceeds_credits`,
and a credit on a `CND` account that would overfill it is `55
exceeds_debits`.

## The books: a role owns one

A role with a row in `ledger_tenants` owns that book. Every write is
applied to the caller's book and every read shows the caller's book. A
role with no row has no book at all: it may neither move money nor read
the ledger.

```sql
insert into ledger_tenants values ('acme', 1);   -- the administrator's
```

Things worth knowing:

- **The client never sends a tenant.** There is nowhere in the wire
  format to put one, so it cannot be forged. The server stamps it when it
  applies the event.
- **A `SECURITY DEFINER` function acts on the caller's book**, never on
  its own. The tenant belongs to the session's role, not to the
  function's owner.
- **Two roles may share a book** (the same tenant number). That is how
  two services share one set of accounts.
- **A transfer naming another book's account is refused** with status
  `102 account_in_another_tenant`, and a transfer id held by another book
  with `103 id_in_another_tenant`. Nothing of the other book leaks
  through the status: the fields are not compared, so the refusal cannot
  tell you the other book's amount, code, ledger, pending id, timeout or
  accounts.

## Accounts

An account is an id, a set of flags, and two numbers of yours:

| field | meaning |
|---|---|
| `id` | 16 bytes, the account's identity; your UUID, or anything unique |
| `flags` | `LINKED` (1), `DEBITS_MUST_NOT_EXCEED_CREDITS` (2), `CREDITS_MUST_NOT_EXCEED_DEBITS` (4); `CLOSED` (32) is set by the ledger itself when the account is closed |
| `code` | your classification; **must not be 0** |
| `ledger` | your partition; **must not be 0** |
| `user_data` | 16 bytes of yours, carried through to the journal |

Two accounts may not transfer among themselves unless they share a
ledger: the debit and the credit account must have the same ledger, and
the transfer's own ledger must match theirs.

Creating an account that already exists is not an error — it returns
`21 exists` if the fields match, and the specific `exists_with_...`
status if they do not. An account id held by another book is refused
with `102`, and it is never overwritten.

## Transfers

| field | meaning |
|---|---|
| `id` | 16 bytes, the transfer's identity — **this is your idempotency key** |
| `debit`, `credit` | the accounts |
| `amount` | 128-bit unsigned, 0 to 2^128-1 |
| `flags` | see below |
| `code` | your classification of the *transfer*; must not be 0 |
| `ledger` | must match the accounts' ledger |
| `pending_id` | for a post or a void: the pending transfer it acts on |
| `timeout` | seconds, for a pending transfer |
| `user_data` | 16 bytes of yours |

Transfer flags:

| flag | value | meaning |
|---|---|---|
| `LINKED` | 1 | the chain fails as a unit: if any event of the chain fails, all of them do |
| `PENDING` | 2 | reserve the money instead of moving it |
| `POST` | 4 | post a pending transfer |
| `VOID` | 8 | void a pending transfer, giving the money back |
| `BALANCING_DEBIT` | 16 | move at most what the debit account has available |
| `BALANCING_CREDIT` | 32 | the same for the credit account |
| `CLOSING_DEBIT` | 64 | close the debit account (the account refuses further transfers) |
| `CLOSING_CREDIT` | 128 | close the credit account |

Three kinds of flag exist in the format and are **refused** by this
version with `4 reserved_flag`: `history` (8) and `imported` (16) on an
account, and `imported` (0x100) on a transfer. Bits 9-15 of a transfer's
flags and any bit above 0x1ff are reserved in the same way, and so are
the tenant's bits in a journal record — the client can never set them.

`user_data` is 16 bytes the ledger carries and never reads. It is the
place for your own cross-reference (an invoice id, a request id), and
the only field in a record that is *yours* alone.

## Amounts

An amount is a **128-bit unsigned integer**, from 0 to 2^128-1. It is not
a float and not a decimal: it is an exact count of your smallest unit
(a cent, a minor unit of a currency, whatever you choose). PostgreSQL has
no 128-bit integer type, so the SQL surface reports amounts as `numeric`,
which holds every value of the range exactly.

The unit is **yours**: the ledger has no currency. If you have several
currencies, give each one a different ledger number (or a different
code), and convert in your application — the ledger stores integers.

To build the 16 bytes of an amount on the client, ask the server:

```sql
select ledger_amount(12345678901234567890);
-- \xd20a1feb8ca954ab0000000000000000
```

The bytes are the low half first, then the high half, little-endian:
`0xab54ca8bef1b0ad2` is `12345678901234567890`, and it is written
backwards.

### `amount` and `amount_actual`

A transfer carries two amounts. **`amount` is what the event asked for;
`amount_actual` is what moved.** They are the same for an ordinary
transfer. They differ where the request is a ceiling rather than a
figure:

| the event | `amount` | `amount_actual` |
|---|---|---|
| a balancing transfer asking for 2^128-1 | 2^128-1 | what was available |
| a post asking for 2^128-1 (the whole pending) | 2^128-1 | the pending's amount |
| a post asking for 0 (the same thing) | 0 | the pending's amount |
| an ordinary transfer | the amount | the same |

You send `amount` and leave `amount_actual` zero; the server fills it in.
Read it back from the journal when you need to know what really moved —
`ledger_journal_lookup` and the journal reads report both.


## The batch

The API takes **one packed `bytea`** per call: a header and the events.

```
batch header (16 bytes)
  uint32 count        how many events follow
  uint32 kind         0 = transfers, 1 = accounts
  uint32 pad
  uint32 pad

transfer (144 bytes)
  id[16] debit[16] credit[16] pending_id[16] user_data[16]
  uint128 amount      low 64 bits first, then the high 64
  uint128 amount_actual
  uint64  timestamp   the server's clock: send 0
  uint32  flags
  uint32  code
  uint32  ledger
  uint32  timeout     seconds
  uint64  pad

account (56 bytes)
  id[16] user_data[16]
  uint64  timestamp   the server's clock: send 0
  uint32  flags
  uint32  code
  uint32  ledger
  uint32  pad
```

Everything is little-endian. An id of all zeros means "none"; an id of
all `0xff` bytes (2^128-1) is reserved by the ledger's rules and is
refused.

**Do not write this by hand.** The project's `bench/mkbatch.py` builds
batches from a readable spec, and the example below has a Python helper
you can copy.

## Writing

Three functions queue work:

```sql
select ledger_apply(bytea);                       -- a batch of transfers
select ledger_create_accounts(bytea);             -- a batch of accounts
select ledger_create_account(uuid, int, int, int);-- one account
```

They return the number of events queued. The work is applied **when the
transaction commits**, and it is applied to the caller's book.

### Reading the statuses

Every event gets a status, and the statuses of the last batch are read
from the same session:

```sql
select ledger_apply(decode('...','hex'));
select ledger_last_statuses();
```

Two statements, **one session**. The statuses live in the backend that
applied them, so a second connection will not see them. With a driver
that opens a connection per statement, hold the connection.

`4294967295` is "created" — it does not fit in an `int4`, so the
statuses are `bigint`.

If you queue two batches in one transaction, the statuses of the last
one are what you read; the first batch's statuses are gone.

### What the batch guarantees

**Events apply in order, and each sees the ones before it.** The fifth
event of a batch sees the four before it as if they had already
committed: if move 1 credits the account that move 5 debits, move 5
can spend it. This is per *session and transaction*: two transactions
interleave in the order in which they commit, not in the order in which
they were sent.

**The commit is all-or-nothing; the events are not.** At commit time
every event is applied under the ledger's lock and every event gets its
own status. An event that would break a rule is **refused with a status
and the rest of the batch still applies**. So a batch is not a
transaction in the SQL sense — it is a *stream of independent
decisions*, applied atomically with respect to the ledger, where a
refusal is a normal outcome and not an error.

If you need "all of these or none", use a **linked chain** (below): a
chain is the unit that fails together.

**Two ways to lose a batch.** A PostgreSQL `ROLLBACK`, or a `ROLLBACK TO
SAVEPOINT`, discards the queued work — nothing is applied and nothing is
journalled. A crash before the commit does the same. A batch that
committed is on disk before you were told so.

**Three things are refused before any event is looked at**, as SQL
errors rather than statuses, because they are malformed requests and not
refused events:

| the request | the error |
|---|---|
| a batch shorter than its 16-byte header | `ledger: batch too short` |
| a header whose kind is not the one the function expects | `ledger: batch is of kind 1, expected 0` |
| a header with no events | `ledger: batch holds no events` |
| a length that does not match the header's count | `ledger: batch size 16 does not match 1 transfers` |
| more than 1,000,000 transfers, or 4,000,000 accounts, in one batch | `ledger: a batch holds at most 1000000 transfers` |

The caps exist because a batch is parsed and applied in one pass under
the lock; the numbers are well above what a real client sends. A
1,000,001-transfer batch is a client bug, not a workload.

### The statuses of a transfer

| value | name | meaning |
|---|---|---|
| 4294967295 | created | the transfer was created |
| 46 | exists | the same transfer was already there, with the same fields |
| 39 | exists_with_different_amount | the id is taken, but the amount differs |
| 37 | exists_with_different_debit_account_id | the id is taken, the debit differs |
| 38 | exists_with_different_credit_account_id | the id is taken, the credit differs |
| 40 | exists_with_different_pending_id | the id is taken, the pending id differs |
| 41 | exists_with_different_user_data | the id is taken, the user data differs |
| 44 | exists_with_different_timeout | the id is taken, the timeout differs |
| 45 | exists_with_different_code | the id is taken, the code differs |
| 67 | exists_with_different_ledger | the id is taken, the ledger differs |
| 36 | exists_with_different_flags | the id is taken, the flags differ |
| 1 | linked_event_failed | the chain this event belongs to failed |
| 2 | linked_event_chain_open | the chain is not complete yet |
| 4 | reserved_flag | a flag bit that is reserved was set |
| 5 | id_must_not_be_zero | the transfer's id was zero |
| 6 | id_must_not_be_int_max | the transfer's id was the reserved maximum |
| 7 | flags_are_mutually_exclusive | two flags that may not go together were both set |
| 8 | debit_account_id_must_not_be_zero | the debit account's id was zero |
| 9 | debit_account_id_must_not_be_int_max | the debit account's id was the reserved maximum |
| 10 | credit_account_id_must_not_be_zero | the credit account's id was zero |
| 11 | credit_account_id_must_not_be_int_max | the credit account's id was the reserved maximum |
| 12 | accounts_must_be_different | the debit and the credit are the same account |
| 13 | pending_id_must_be_zero | a pending id was given where none is allowed |
| 14 | pending_id_must_not_be_zero | a post or a void named no pending transfer |
| 15 | pending_id_must_not_be_int_max | the pending id was the reserved maximum |
| 16 | pending_id_must_be_different | the pending id is the transfer's own id |
| 17 | timeout_reserved_for_pending_transfer | a timeout was given for a transfer that is not pending |
| 19 | ledger_must_not_be_zero | the ledger was 0 |
| 20 | code_must_not_be_zero | the code was 0 |
| 21 | debit_account_not_found | the debit account does not exist |
| 22 | credit_account_not_found | the credit account does not exist |
| 23 | accounts_must_have_the_same_ledger | the two accounts are in different ledgers |
| 24 | transfer_must_have_the_same_ledger_as_accounts | the transfer's ledger differs from the accounts' |
| 25 | pending_transfer_not_found | no pending transfer with that id |
| 26 | pending_transfer_not_pending | the pending transfer is already posted or voided |
| 27 | pending_transfer_has_different_debit_account | the post names a different debit account |
| 28 | pending_transfer_has_different_credit_account | the post names a different credit account |
| 29 | pending_transfer_has_different_ledger | the post's ledger differs from the pending's |
| 30 | pending_transfer_has_different_code | the post's code differs from the pending's |
| 31 | exceeds_pending_transfer_amount | the post is larger than the pending transfer |
| 32 | pending_transfer_has_different_amount | a void may not void part of a pending transfer: the void must release all of it |
| 33 | pending_transfer_already_posted | the pending transfer is already posted |
| 34 | pending_transfer_already_voided | the pending transfer is already voided |
| 35 | pending_transfer_expired | the pending transfer's timeout has passed |
| 47-52 | overflows_* | a counter would pass 2^128-1 (debits/credits, pending/posted/total) |
| 53 | overflows_timeout | the timeout does not fit the ledger's clock |
| 54 | exceeds_credits | the debit account's flag says its debits may not exceed its posted credits |
| 55 | exceeds_debits | the credit account's flag says its credits may not exceed its posted debits |
| 64 | closing_transfer_must_be_pending | a closing transfer was not pending |
| 65 | debit_account_already_closed | the debit account is closed |
| 66 | credit_account_already_closed | the credit account is closed |
| 101 | no_transfer_slots | the grid is full of transfers |
| 102 | account_in_another_tenant | the account belongs to another book |
| 103 | id_in_another_tenant | the transfer id belongs to another book |

### The statuses of an account

| value | name |
|---|---|
| 4294967295 | created |
| 1 | linked_event_failed (the chain this event belongs to failed) |
| 2 | linked_event_chain_open (the chain is not complete yet) |
| 21 | exists (the same account, with the same fields) |
| 15 | exists_with_different_flags |
| 16 | exists_with_different_user_data |
| 19 | exists_with_different_ledger |
| 20 | exists_with_different_code |
| 5 | reserved_flag |
| 6 | id_must_not_be_zero |
| 7 | id_must_not_be_int_max |
| 8 | flags_are_mutually_exclusive |
| 13 | ledger_must_not_be_zero |
| 14 | code_must_not_be_zero |
| 100 | no_account_slots (the grid is full of accounts) |
| 102 | account_in_another_tenant |

`ledger_status_name(bigint)` gives the transfer's name and
`ledger_account_status_name(bigint)` the account's. 102 and 103 are
ledgerext's own; the rest of the statuses, their values and their order
are unchanged.

## Idempotency and retries

**The transfer's id is the idempotency key.** Sending the same transfer
twice is safe: the second one gets `46 exists` if the fields match, or
the specific `exists_with_different_...` if they do not. Nothing is
moved twice.

This is the whole retry story:

1. Build the transfer with a **stable id** — derived from your own
   request id, not random.
2. Send it.
3. If the transaction committed, the transfer exists; re-sending it
   returns `exists` and changes nothing.
4. If the transaction did not commit, nothing was applied and nothing
   was journalled; re-sending it is a fresh transfer.

An id that was **refused** is not burnt: it was never recorded, so the
same id may be used again once the cause is fixed.

### Generating the ids

The ledger does not generate ids — you do — and the server's idempotency
only works if the id survives a retry. So the id has to be **on your
disk before the first send**:

```
your request  ──>  write (request_id, transfer_id) to your own table
               ──>  commit that
               ──>  send the transfer
               ──>  retry: send the same transfer_id
```

If you generate an id, send it, and crash before you record it, you
cannot tell whether the money moved: a retry with a fresh id would move
it twice. Persisting first is what makes `46 exists` the answer to every
retry.

Two properties are worth designing for:

- **Time-ordered ids** (a ULID, a UUIDv7, a `(timestamp, counter)`
  packed into 16 bytes) give you a cheap way to sweep "everything I sent
  after time *t*" and to see the order in which you attempted them.
  They are also unguessable enough for the ledger's purposes given the
  random part, and they index well on your side.
- **Deterministic ids** (a hash of your own request id and business key)
  make the id derive from the request itself, so a retry that recomputes
  it gets the same id even if your record of the first attempt is lost.
  This is the strongest option, at the cost of a hash whose collisions
  are your problem, not the ledger's.

A random UUIDv4 is fine *if* you persist it first. What is never fine is
an id generated per attempt.

The ledger treats an id of all zeros as "none" and an id of all `0xff`
bytes as reserved, so neither is usable. Do not build ids by
incrementing an integer: they would be trivially guessable to anyone
with a query surface, and if a client sends another book's id it gets
`103 id_in_another_tenant` — a signal you do not want to hand out.

## Pending transfers, posts and voids

A **pending** transfer reserves money instead of moving it: the amount
appears in the account's `debits_pending` / `credits_pending`, not in the
posted counters. It is how you hold funds for a day, and then:

- **post** it: a transfer with the `POST` flag and the pending's id in
  `pending_id`. The reservation becomes a posted movement. A post may be
  **partial** — a smaller amount than the pending's moves that much and
  the rest stays reserved. The amount 2^128-1 (the largest there is)
  means "post the whole pending transfer".
- **void** it: a transfer with the `VOID` flag and the pending's id. The
  reservation is released and the money goes back.
- **let it expire**: a pending transfer with a `timeout` (in seconds) is
  released by the server itself when the timeout passes. Nobody has to
  void it, and the release is in the journal like any other record, so
  the balances stay reconstructible.

The expiry is the server's decision and it travels as a record of its
own, so a reservation is never given back twice — not even if a standby
takes over.

What it looks like. After the pending transfer is queued:

```sql
select * from ledger_pending_transfers();
-- 44444444-…-444444444444 | 11111111-… | 22222222-… | 50 | 60
--   id                        debit        credit      amount  timeout

select ledger_balance('11111111-1111-1111-1111-111111111111'::uuid);
-- {50,0,0,0,0,1,1}: 50 debits *pending*, nothing posted
```

and after its post:

```sql
select ledger_balance('11111111-1111-1111-1111-111111111111'::uuid);
-- {0,0,50,0,0,1,1}: 50 debits posted
select ledger_balance('22222222-2222-2222-2222-222222222222'::uuid);
-- {0,0,0,50,0,1,1}: 50 credits posted
select count(*) from ledger_pending_transfers();
-- 0: the reservation is gone
```

## Linked events

The `LINKED` flag makes a set of events fail as a unit. The rule is
mechanical, and it is the whole feature:

**An event that is `LINKED` joins it to the next event.** So a chain is
a run of consecutive events where every one but the last carries
`LINKED`:

```
e1 LINKED   ─┐
e2 LINKED   ─┼─ one chain of three
e3          ─┘
```

Four rules follow from that:

1. **The last event of a chain must not be linked.** A batch whose
   last event carries `LINKED` ends with an open chain: every event in it
   gets `2 linked_event_chain_open` and none of them is applied. The
   chain does **not** span batches — it ends at the end of the batch,
   open or not.
2. **A chain is undone as a unit.** When one event of a chain fails, it
   keeps its own status and every earlier event of the chain is changed
   to `1 linked_event_failed` and undone. The accounts are exactly as
   they were before the chain, and nothing was journalled for it.
3. **A batch may hold several chains.** `LINKED LINKED — LINKED —` is
   two chains; the second is independent of the first, and one failing
   does not undo the other.
4. **The chain is not persisted.** The journal records the transfers,
   not the fact that they were linked: an event's record has the
   `LINKED` bit set in its flags and nothing more. The chain exists to
   decide one commit; after that the records are individual records.

What a chain does *not* do: it does not make the batch atomic. A batch
with a chain and a separate, unlinked, failing event applies the chain
and refuses the failing event. Use one chain around everything you need
together.

## The reads

| the caller's book | the whole cluster |
|---|---|
| `ledger_balance(uuid)` | `ledger_journal_records(from_seq, lim)` |
| `ledger_lookup_transfer(uuid, ...)` | `ledger_snapshot()` |
| `ledger_totals()` | `ledger_journal_periods()` |
| `ledger_ledger_totals(int)` | `ledger_journal_count()` |
| `ledger_pending_transfers()` | `ledger_capacity()` |
| `ledger_journal_lookup(uuid)` | `ledger_account_count()` |
| `ledger_journal_account(uuid, int)` | `ledger_transfer_count()` |
| `ledger_journal_ledger(int, int)` | `ledger_expiry_due()` |
| `ledger_journal_range(bigint, bigint, int)` | |
| `ledger_journal_balances()` | |
| `ledger_period_balances(int)` | |

**The `int` is a limit, and it defaults to 1,000.** `ledger_journal_account`,
`ledger_journal_ledger` and `ledger_journal_range` stop at 1,000 records
unless you say otherwise, and **a negative limit means no limit**:

```sql
select ledger_journal_count();                                         -- 40
select count(*) from ledger_journal_range(0, 9223372036854775807, 5);   -- 5
select count(*) from ledger_journal_range(0, 9223372036854775807);     -- 40
select count(*) from ledger_journal_range(0, 9223372036854775807, -1); -- 40
```

The journal here holds 40 records, so the default's 1,000 is not yet
reached and the first and last lines agree. On an account with a long
history the default is a **silent truncation**: pass `-1`, or a limit
you will notice.

The journal reads are also narrow: they filter by **ledger** and by
**time**, and `ledger_journal_account` filters by account. There is no
filter on `code` or on `user_data`. If you need those, keep your own
index, or read `ledger_journal_records()` and filter in your
application.

An account of another book is **not found, not refused**: the read
returns nothing rather than an error, so a client cannot tell an
account that does not exist from one that belongs to someone else.

The shapes:

```sql
select ledger_balance('11111111-1111-1111-1111-111111111111'::uuid);
-- {debits_pending, credits_pending, debits_posted, credits_posted,
--  flags, code, ledger}

select ledger_totals();
-- {naccounts, debits_pending, credits_pending, debits_posted, credits_posted}

select ledger_ledger_totals(61);
-- the same five, for one of your ledgers

select ledger_lookup_transfer('33333333-3333-3333-3333-333333333333'::uuid);
-- debit | credit | amount | flags | code | ledger | state

select * from ledger_pending_transfers();
-- id | debit | credit | amount | code | ledger | timeout
```

## A worked example

The batch builder, in Python — no hex by hand:

```python
import struct, uuid

LINKED, PENDING, POST, VOID = 1, 2, 4, 8
BAL_DEBIT, BAL_CREDIT, CLOSE_DEBIT, CLOSE_CREDIT = 16, 32, 64, 128

def uid(u):                      # a uuid as the 16 raw bytes the wire wants
    return uuid.UUID(u).bytes

def transfer(tid, debit, credit, amount, flags=0, code=1, ledger=1,
             pending_id=b"\0" * 16, timeout=0, user_data=b"\0" * 16):
    return (uid(tid) + uid(debit) + uid(credit) + pending_id + user_data
            + struct.pack("<QQ", amount & ((1 << 64) - 1), amount >> 64)   # amount
            + struct.pack("<QQ", 0, 0)                                     # amount_actual
            + struct.pack("<Q", 0)                                         # timestamp
            + struct.pack("<IIII", flags, code, ledger, timeout)
            + struct.pack("<Q", 0))                                        # pad

def account(aid, flags=0, code=1, ledger=1, user_data=b"\0" * 16):
    return (uid(aid) + user_data
            + struct.pack("<Q", 0)                                         # timestamp
            + struct.pack("<IIII", flags, code, ledger, 0))

def batch(events, kind=0):        # kind 0 = transfers, 1 = accounts
    return struct.pack("<IIII", len(events), kind, 0, 0) + b"".join(events)
```

And the transaction:

```python
import psycopg

with psycopg.connect(dsn) as conn:
    with conn.cursor() as cur:
        cur.execute("select ledger_apply(%s::bytea)", (batch([
            transfer("33333333-3333-3333-3333-333333333333",
                     "11111111-1111-1111-1111-111111111111",
                     "22222222-2222-2222-2222-222222222222",
                     100),
        ]),))
        cur.execute("select ledger_last_statuses()")
        status = cur.fetchone()[0]      # [4294967295] — created
```

Note that the two statements run on the **same connection**: the statuses
live in the backend that applied them.

A pending transfer, and its post, a second later:

```python
pending = transfer("44444444-4444-4444-4444-444444444444",
                   "11111111-1111-1111-1111-111111111111",
                   "22222222-2222-2222-2222-222222222222",
                   50, flags=PENDING, timeout=60)

post = transfer("55555555-5555-5555-5555-555555555555",
                "11111111-1111-1111-1111-111111111111",
                "22222222-2222-2222-2222-222222222222",
                50, flags=POST,
                pending_id=uid("44444444-4444-4444-4444-444444444444"))
```

## What the ledger does not do

- **No currency, no conversion, no interest, no rounding.** The unit is
  yours and the amount is an integer. Anything more is your
  application's work.
- **No policies.** Limits, approvals, sanctions checks and audit trails
  are yours. The ledger applies the transfer rules and nothing else.
- **No rows.** The accounts and transfers are not PostgreSQL tuples:
  they are not in `pg_dump`, and PostgreSQL's MVCC does not apply to
  them. Your own tables hold whatever you need to keep beside the
  ledger.
- **No multi-currency accounting, no balance snapshots in the
  application.** The journal's records are the history; the journal's
  reads are the queries.
- **No clustering.** One primary, standbys that follow it. No quorum,
  no fencing.
