# Data modeling

How to lay out your money in ledgerext. The model itself is in
[usage.md](usage.md); this is the design work around it — what a ledger
number is for, what a unit is, what to put in `user_data`, and where the
ledger sits relative to your own database.

## The ledger is one thing; your application is another

Decide this first, because everything else follows from it:

**The ledger is the system of record for money. Your application's own
tables are the system of record for everything else.**

They are not alternatives and they are not a cache of each other. The
ledger holds balances and the journal; your database holds customers,
invoices, orders, sessions, permissions, and the reasons money moved.
Neither reads the other's data directly, and they are not in one
transaction.

```
your app's database                 ledgerext
┌──────────────────────┐            ┌──────────────────────────┐
│ customers            │            │ accounts                 │
│ invoices             │            │ transfers (the journal)  │
│ the reason money moved│  ───────► │ 176 bytes each, appended │
│ your id ↔ ledger id  │  ◄─────── │ the balances             │
└──────────────────────┘            └──────────────────────────┘
```

The two connections between them:

- **Outward, on every write:** your application builds the transfer,
  sends it, reads the status, and updates its own tables with the
  result. The ledger's id is your idempotency key.
- **Outward, for history:** your application reads the journal's records
  (`ledger_journal_records()`, [operations.md](operations.md#streaming-out-the-journal-as-your-change-feed))
  and joins them to its own rows.

There is no third channel. The ledger never calls into your application,
and your application never asks the ledger to interpret a field it
carries for you.

### Why not store everything in the ledger?

Because the ledger has **no rows**. An account is an id and two
counters; a transfer is a 176-byte record. There is no column for an
invoice number, no index you can add, no join you can write. It is
deliberately not a database, and the moment you need a query over
customers, invoices or reasons, you are in your own tables.

The corollary that matters operationally: **`pg_dump` does not carry the
money.** A backup of your database is not a backup of your ledger, and
[operations.md](operations.md#backups) explains why the journal is.

## A ledger is a currency, or an asset

A **ledger number** is your partition of the accounts — the accounts
that may transfer among themselves. The natural use of it is one number
per asset or currency:

| ledger | what it is |
|---|---|
| 1 | USD |
| 2 | INR |
| 3 | points |
| 4 | receivables |

The reasons to make it one ledger per asset:

- **Transfers are checked against the ledger.** Two accounts in
  different ledgers cannot transfer among themselves, and a transfer
  must carry the accounts' ledger. So a dollar account cannot be credited
  with a rupee amount by accident — the transfer is refused with
  `24 transfer_must_have_same_ledger_as_accounts`, not silently applied.
- **You can total a ledger in one call:** `ledger_ledger_totals(int)`.
- **Exchange is explicit.** A currency trade is two transfers in two
  ledgers, in one linked chain — [recipes.md](recipes.md#4-currency-exchange).

The rule of thumb: **give each thing that has its own exchange rate, its
own rounding rule, or its own lifecycle its own ledger number.** If two
things never trade against each other, they are two ledgers.

What a ledger is *not*: not a tenant, not a `CODE`, not a date. A
tenant is ledgerext's partition ([usage.md](usage.md#the-books-a-role-owns-one))
and a code is your classification of an account or a transfer. A
ledger, a code and a tenant are three orthogonal axes, and an account
has one of each.

## The unit is yours: asset scale

An amount is an integer count of your smallest unit. Choose the unit
once, globally, for each ledger, and never change it:

| the currency | the unit | the amount for $1.00 | the amount for $12,345.67 |
|---|---|---|---|
| USD | cents | 100 | 1234567 |
| GBP | pennies | 100 | 1234567 |
| JPY | yen | 1 | 1234567 |
| BTC | satoshi | 100000000 | 123456700000000 |
| a share | shares (as a fraction) | 10000 | 123456700 |

**Store the scale, not the scale's exponent, and never divide to get
one.** A division is where rounding appears. Convert a money amount to
a string exactly once, on the way out to a human, and convert a
human's figure into an integer exactly once, on the way in:

```python
def to_ledger(amount_cents: int) -> bytes:
    # 1234567 -> the 16 bytes
    ...

def to_display(amount_cents: int, scale: int = 2) -> str:
    # 1234567 -> "12345.67"; the only place a dot is written
    sign = "-" if amount_cents < 0 else ""
    d, r = divmod(abs(amount_cents), 10 ** scale)
    return f"{sign}{d}.{r:0{scale}d}"
```

Two consequences worth stating:

- **Never store a float.** Not in the ledger, not beside it. `1.1` is
  not representable and two floats that "look equal" are not equal.
- **Never let the ledger do arithmetic on a scale you have not chosen.**
  It has no notion of a currency, a decimal place, or a rate. The
  128-bit range is generous — 2^128-1 satoshi is about 3.4e38, which
  is more than enough for any money in any unit for any purpose you
  will live with — but it does not make rounding safe.

Negative amounts are impossible: the range is 0 to 2^128-1. A credit
balance or a refund is not a negative transfer; it is a transfer in the
other direction. That is why the balance signs in the next section
matter.

## Debit and credit are directions, not signs

Double entry means every transfer names a debit account and a credit
account. It never means a negative amount. What you get is two
counters per account:

| counter | what it counts |
|---|---|
| `debits_posted` | money taken out of this account |
| `credits_posted` | money put into this account |
| `debits_pending` | reservations against this account |
| `credits_pending` | reservations in this account's favour |

The account's **balance** is the pair, not the difference:

```
balance = (credits_posted - debits_posted)      the sign depends on the kind
```

And which of the two you *mean* depends on what the account is:

| the account | what its balance is | so it is | the flag |
|---|---|---|---|
| cash | credits − debits | debit-balance: money is *in* it | `DNC` |
| payable | debits − credits | credit-balance: money is *owed* | `CND` |

This is why the flags are called what they are: `DEBITS_MUST_NOT_EXCEED_CREDITS`
says "the account's own debits may not exceed its credits", which for a
debit-balance account means it cannot go negative, and for a
credit-balance account means it cannot be credited past its own debit.
[usage.md](usage.md#the-five-kinds-of-account) has the table.

Two mistakes this model prevents, if you use the flags:

- **A customer with a negative balance.** Give the customer a
  credit-balance account (`CND`); a transfer that would credit them past
  their own debit is refused with `55 exceeds_debits`.
- **Paying out of an account with no money.** Give it a debit-balance
  account (`DNC`); a transfer that would overdraw it is refused with
  `54 exceeds_credits`.

A control, clearing or settlement account wants **neither** flag: it
must be able to go negative in both directions. The operator's account
is one, and so is any account that acts as a bucket for a payment
flow.

## `code` is an enum, not a string

`code` is a `uint32` on both accounts and transfers, and it must not be
zero. It is *your* classification — the ledger does not interpret it.
Use it as a stable, small, enumerable set:

```
account codes
  1  customer
  2  merchant
  3  house
  4  clearing
  5  revenue
  6  expense

transfer codes
  1  purchase
  2  refund
  3  fee
  4  transfer
  5  adjustment
```

Keep the numbers in a single place in your codebase, and never renumber
them. A `code` is stored in the journal, so a renumbering would make
your old records unreadable in your own terms.

It is not a lookup table: `ledger_journal_ledger(int, int)` and
`ledger_journal_account(uuid, int)` filter by ledger and account, not
by code. If you need to query by code, keep an index on your own side,
or read `ledger_journal_records()` and filter in your application.

## `user_data` is yours: 16 bytes, and a privacy decision

Sixteen bytes the ledger carries and never reads. It is the only field
in a record that is entirely your own. Put in it a cross-reference that
makes a ledger record traceable to your world:

```
an invoice id          16 bytes, your own uuid
a request id           16 bytes
a session id           16 bytes
a padded, stable hash  of an email address
```

The privacy decision is not about the ledger's capacity — 16 bytes is
not much and the journal holds it forever, for every book, and
`ledger_journal_records()` returns it whole-cluster. It is about what
you are willing to have in a file that is copied on every backup and
shipped to every standby.

- **A random id is safe.** An invoice id, a request id, a session id:
  they are pseudonymous and carry nothing about the person.
- **A hash of personal data is *not* automatically safe.** A short hash
  of an email is reversible for a domain where the candidate set is
  small (`@gmail.com`), and it is a personal-data-processing decision
  your legal team has to make, not one the ledger makes for you.
- **Never put a name, an email, a card number, a phone number or a
  balance in `user_data`.** It is not encrypted and it is not
  redacted: it is in the journal, in the snapshot, and in every backup.

There is no per-field encryption and no field-level redaction in the
ledger. If a field has to be there and has to be private, the only
answer is to put a non-private reference in `user_data` and keep the
private thing on your side of the boundary.

## Ids: time-ordered, or derived

Every account and every transfer is an id of 16 bytes that you choose.
There are two schemes that work, and they fail in different ways:

### Time-ordered ids (recommended)

A UUIDv7, a ULID, or a `(timestamp, counter)` packed into 16 bytes. The
first few bytes are monotonic, so the ids sort in the order you created
them, and they are unguessable in the part that is random.

What you get:

- **You can sweep by time.** "Everything I sent after 14:00" is a
  prefix, and your own index on it is a range scan.
- **You can see the order** of your attempts from the ids alone, which
  is what a reconciliation job wants.
- **The grid indexes them well**: the ledger's hash is on the whole
  16 bytes, but your own index on the first eight is a B-tree range.

What you do not get: a way to recompute an id from the request. If you
lose your record of the id, a retry generates a new one and the ledger
will move the money twice — unless you persist the id first (see
[usage.md](usage.md#generating-the-ids)).

### Deterministic ids

A hash of your own business key: `id = H(request_id)` or
`id = H(invoice_id, line_number)`. The same request always produces the
same id, so a retry recomputes it even if your record of the first
attempt is gone.

This is the strongest scheme for idempotency and the reason it is worth
the cost:

- No persistence step before the first send: the id is a function of
  the request, so there is no window in which an id exists that you do
  not know.
- Two services, or a service and its reconciliation job, can derive the
  same id for the same business event independently.

The cost is your problem, not the ledger's: **a collision is two
different business events that hash to the same id**, and the ledger
will report `46 exists` and refuse to move the second one. Use a hash
long enough that you will never see one, and never reuse a key.

### Never

- **A counter.** `1, 2, 3, …` is guessable by anyone with a query
  surface and by any client that can send an id. Do not build one.
- **The database's own row id.** It leaks your row count and your
  insert order into a field that other people can see.
- **A UUID in the text form** (`"33333333-…"` as 36 characters). The
  ids are 16 raw bytes; a text id would be 36 of them, and the wire
  format has no room for it. Convert at the edge: the Python helper in
  [usage.md](usage.md#the-batch) does it in one line.

## Where the ledger sits in your stack

The ledger is **a data plane**, not a control plane. Your application's
API, your admin console, your billing engine are the control plane. The
distinction is who is allowed to talk to the ledger:

**No untrusted client connects to it.** Not a browser, not a mobile
app, not a partner's server. Every write is a request from your
application, with its own id, its own validation and its own audit
trail. The ledger will happily move money for a client that asks for
something stupid; it has no way to know it is stupid.

That is a security boundary, not a performance one:

- **A role that may write may move money** — that is what the `EXECUTE`
  grant is ([configuration.md](configuration.md#the-grants)). Give it
  only to the services that move money, never to an interface.
- **A role that may read sees its book** and nothing else. Give read
  grants to reporting jobs, and never to a client-facing service.
- **`ledger_tenants` is the administrator's table.** No application
  role writes it. A role with no row has no book at all, which is the
  fail-closed behaviour you want.

The usual shape is:

```
a partner's request
   └─► your API (validates, checks limits, records the reason)
          └─► your payment service
                 ├─► the ledger (move the money)
                 └─► your tables (record why)
```

A reporting job is a different role, reads only, and cannot write to
the ledger at all.

## A worked layout

The usual product has one ledger per currency and a small set of
accounts per customer:

```
ledger 1 (USD)
  account 11111111-…  customer A, DNC        credits - debits = the balance
  account 22222222-…  customer B, DNC
  account 99999999-…  house, no flags
  account 88888888-…  clearing, no flags

ledger 2 (EUR)
  …
```

A purchase is one transfer, `customer A → house`, code 1. A fee is a
second transfer, `house → revenue`, code 3. A refund is a transfer in the
other direction. An exchange is two transfers in two ledgers in one
chain.

The application's tables hold the rest:

```sql
create table payments (
    id            uuid primary key,        -- the transfer's id: your idempotency key
    ledger        int      not null,
    amount        numeric  not null,       -- in the ledger's unit
    code          int      not null,
    reason        text     not null,       -- why the money moved: yours
    created_at    timestamptz not null default now(),
    statuses      bigint[]                 -- what the ledger said
);
```

`id` is the join key both ways: it is what you send to the ledger, and
what the journal's records carry back. `reason` is the field the ledger
does not have, and it is the one you must keep.

## What not to model

- **Balances.** Never store a balance in your own tables as a number
  you update. `ledger_balance()` is the balance; the journal is how you
  audit it. A second, separately maintained balance is a second thing
  that can disagree.
- **A "ledger table" in PostgreSQL.** `pg_dump` will not carry it, MVCC
  will not apply to it, and the two copies will drift. The journal and
  the grid *are* the ledger's tables.
- **Time in a transfer.** The server stamps it. Sending a timestamp is
  meaningless and the field is ignored.
- **A rate, an exchange, or an interest accrual.** The ledger stores
  integers. A rate belongs in your application, next to the trade it
  came from.
