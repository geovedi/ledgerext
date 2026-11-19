# Recipes

Ten patterns, each one a short batch you can copy. Every example here was
run against a real cluster and the output is the real output. The model
and the API are in [usage.md](usage.md); this is how you build the
things applications actually need out of it.

The account ids below are 16 raw bytes. In SQL they are `uuid`s. The
Python helper from [usage.md](usage.md#the-batch) is in every example.

## The cast

Ten accounts, two ledgers:

| id | what it is | flags |
|---|---|---|
| `f1f1f1f1-…-0001` | A, a customer with a debit balance | `DNC` |
| `f1f1f1f1-…-0002` | B, a merchant with a credit balance | — |
| `f1f1f1f1-…-0003` | C, a control account | — |
| `f1f1f1f1-…-0004` | D, a funded account, used for the two-phase examples | `DNC` |
| `f1f1f1f1-…-0005` | E, a credit balance, used to show the bound | `CND` |
| `f1f1f1f1-…-000f` | OP, the operator: the account you fund from | — |
| `f1f1f1f1-…-0011` / `…-0021` | USD and its liquidity, on ledger 1 | — |
| `f1f1f1f1-…-0012` / `…-0022` | INR and its liquidity, on ledger 2 | — |

`DNC` is `DEBITS_MUST_NOT_EXCEED_CREDITS`, `CND` is
`CREDITS_MUST_NOT_EXCEED_DEBITS`. [usage.md](usage.md#the-five-kinds-of-account)
says which balance each one is for.

## 1. Fund an account

The operator's account is the seed: everything else is fed from it, so
it has no balance bound and is never closed.

```python
apply("fund A", [transfer("a1a1a1a1-0000-0000-0000-000000000001",
                          OP, A, 100)])
```

```
statuses={4294967295}
balance A  {0,0,0,100,2,1,1}
-- debits_posted 0, credits_posted 100: A holds 100
```

## 2. Move exactly what an account has

`BALANCING_DEBIT` with the amount `2^128-1` ("the largest there is")
moves at most what the debit account has available. It is how you drain
an account without first reading its balance — one round trip, no
race.

```python
apply("A->C balancing_debit MAX",
      [transfer("a1a1a1a1-0000-0000-0000-000000000002", A, C, MAX,
                flags=BAL_DEBIT)])
```

```
statuses={4294967295}
balance A  {0,0,100,100,2,1,1}
balance C  {0,0,0,100,0,1,1}
```

A had 100 available, so 100 moved. The journal's record for it is
instructive — `amount` is what the event *asked* for, `amount_actual` is
what *moved*:

```sql
select id, amount, amount_actual, flags from ledger_journal_lookup(
  'a1a1a1a1-0000-0000-0000-000000000002'::uuid);
-- …0002 | 340282366920938463463374607431768211455 | 100 | 16
--         2^128-1: what it asked for                what moved
```

`BALANCING_CREDIT` does the same from the credit side.

## 3. Multi-credit through a control account

One debit into a control account, then the control pays everyone out,
all in one linked chain. The chain is what makes it atomic: if the last
payout fails, the first one is undone too.

```python
apply("OP->C, then C->A and C->B",
      [transfer("a1a1a1a1-0000-0000-0000-00000000000c", OP, C, 1000,
                flags=LINKED),
       transfer("a1a1a1a1-0000-0000-0000-00000000000d", C, A, 300,
                flags=LINKED),
       transfer("a1a1a1a1-0000-0000-0000-00000000000e", C, B, 700)])
```

```
statuses={4294967295,4294967295,4294967295}
balance C  {0,0,1000,1140,0,1,1}
balance A  {0,0,100,400,2,1,1}
balance B  {0,0,0,700,0,1,1}
```

The last event of a chain carries no `LINKED`. The rule is mechanical:
a chain is as long as you make it, and the last event of it is the one
that is not linked. Send a chain that ends on a `LINKED` event and the
whole batch is refused:

```
chain left open                         statuses={2}
-- 2 = linked_event_chain_open
```

## 4. Currency exchange

Two transfers, two ledgers, one chain: take the dollars, give the
rupees. The ledger numbers keep the two currencies apart — there is no
conversion anywhere in this code, only integers in two ledgers.

```python
apply("exchange USD->INR",
      [transfer("b1b1b1b1-0000-0000-0000-000000000001", USD, LIQ_USD,
                10000, flags=LINKED, ledger=1),
       transfer("b1b1b1b1-0000-0000-0000-000000000002", LIQ_INR, INR,
                824214, flags=0, ledger=2)])
```

```
statuses={4294967295,4294967295}
```

The rate (824214 = ₹8,242.14 for $100.00) is your number. The ledger
does not know it is a rate: it is two integers in two ledgers, moved
together because they are in one chain.

## 5. Balance bounds

An account with `DNC` refuses a debit that would push its debits past
its posted credits; an account with `CND` refuses a credit that would
push its credits past its posted debits. This is a *bound*, not a
policy: the ledger will not move money it does not see, and it will not
move money into an account that cannot hold it.

```python
apply("F->G 6 (available 5)", [transfer(..., F, G, 6)])
```

```
statuses={54}        -- 54 = exceeds_credits: F's debits may not exceed its credits
```

and on the other side:

```python
apply("OP->E 10 (E is CND)", [transfer(..., OP, E, 10)])
```

```
statuses={55}        -- 55 = exceeds_debits: E's credits may not exceed its debits
```

An account with neither flag has no bound: it can go negative on both
sides. That is what makes it a good control or clearing account.

## 6. Close an account, and reopen it

A closing transfer must be pending. Send it directly and the ledger
refuses:

```python
apply("A->C closing, not pending",
      [transfer("a1a1a1a1-0000-0000-0000-000000000003", A, C, 0,
                flags=CLOSE_DEBIT)])
```

```
statuses={64}        -- 64 = closing_transfer_must_be_pending
A flags = 2          -- still open
```

As a pending transfer with the amount 0, it closes immediately:

```python
apply("A->C closing_debit pending",
      [transfer("a1a1a1a1-0000-0000-0000-000000000004", A, C, 0,
                flags=CLOSE_DEBIT | PENDING)])
```

```
statuses={4294967295}
A flags = 34         -- 2|32: the flags as they were, plus CLOSED
```

A **closed account refuses every transfer that names it** — not only on
the closed side — with 65 when it is the debit side and 66 when it is
the credit side. The one exception is a **void**, which releases a
reservation and is always allowed:

```
OP->A after close     statuses={66}   -- 66 = credit_account_already_closed
```

and **voiding the closing transfer reopens it** — the flag goes back,
because a void is a cancellation of the reservation and the reservation
is what held the account shut:

```python
apply("void closing",
      [transfer("a1a1a1a1-0000-0000-0000-000000000006", A, C, 0,
                flags=VOID, pending_id=uid("a1a1a1a1-0000-0000-0000-000000000004"))])
```

```
statuses={4294967295}
A flags = 2          -- open again
```

`CLOSING_CREDIT` closes the credit side. Close both sides to freeze an
account in both directions.

## 7. Reserve, then post

The two-phase pattern: a pending transfer holds the money, and a post
moves it for real. Between the two the money is reserved, not moved.

```python
pending = transfer("a1a1a1a1-0000-0000-0000-000000000007", D, C, 40,
                   flags=PENDING, timeout=60)
apply("pending D->C 40", [pending])
```

```
statuses={4294967295}
pending rows = 1
balance D  {40,0,0,1000,2,1,1}
-- debits_pending 40, debits_posted 0: the 40 is held, not spent
```

Then the post:

```python
post = transfer("a1a1a1a1-0000-0000-0000-000000000008", D, C, 40,
                flags=POST, pending_id=uid("a1a1a1a1-0000-0000-0000-000000000007"))
apply("post it", [post])
```

```
statuses={4294967295}
pending rows = 0
balance D  {0,0,40,1000,2,1,1}
```

A post may be **partial** — a smaller amount moves that much and the
rest stays reserved. A void must release all of it. A post with the
amount `2^128-1` means "post the whole thing". A post with the amount 0
means the same for a void.

## 8. Rate limiting by reservation

Reserve against a balance you cannot see. If the reservation does not
fit, it is refused with `54`, and nothing is held. If it fits, the
balance is consumed by the reservation until the timeout releases it —
so a second request cannot spend the same money twice.

```python
apply("reserve 1 for 60s",
      [transfer("b1b1b1b1-0000-0000-0000-000000000003", D, C, 1,
                flags=PENDING, timeout=60)])
```

```
statuses={4294967295}
pending rows = 1
```

D has 960 available after the earlier post, so this fits. A reservation
for more than is available is `54` and holds nothing. With the timeout
the reservation gives itself back after 60 seconds — the server does it
and the release is in the journal like any other record.

## 9. Correct a transfer

The journal is append-only: you cannot un-post a transfer. You correct
one by adding another one in the other direction. That is not a
limitation, it is the point — the history is the history.

```python
apply("reverse 100",
      [transfer("b1b1b1b1-0000-0000-0000-000000000004", B, A, 100)])
```

```
statuses={4294967295}
```

Both transfers stay in the journal, in order, and the net balance is
what it should be. Give the reversal a `code` your reconciler can
recognise, and keep `user_data` for the reason.

## 10. A chain that fails rolls back as a unit

The chain's guarantee is not "all of these apply" — it is "all of these
apply **or none of them do**". A failing event takes its own status and
every other event in the chain takes `1 linked_event_failed`:

```python
apply("chain with a failing event",
      [transfer("a1a1a1a1-0000-0000-0000-00000000000a", D, C, 1,
                flags=LINKED),
       transfer("a1a1a1a1-0000-0000-0000-00000000000b", OP, C, MAX)])
```

```
statuses={1,49}      -- 49 = overflows_debits_posted, the second event's own status
balance D  {0,0,40,1000,2,1,1}
-- D's balance is exactly what it was: the first event was undone
```

The first event would have moved 1 out of D. It did not, because the
second event's failure undid it. Nothing was journalled for the chain.

## Retrying is free

The transfer's id is the idempotency key, so the retry story is one
line of your code:

```python
t = transfer("c2c2c2c2-0000-0000-0000-000000000014", OP, H, 1)
apply("first",  [t])    # {4294967295}
apply("again",  [t])    # {46}         exists: the same transfer, nothing moved
apply("different amount", [transfer(..., OP, H, 2)])
                        # {39}         exists_with_different_amount
```

Two rules for the ids: they are **stable** (derived from your own
request id, never random), and they are **persisted on your side before
you send**. If you generate an id and send it and lose it, you cannot
tell whether the money moved. Generate it, write it to your own table,
then send.

## What every example does not show

- **No currency.** Every amount is an integer in your own unit.
- **No policy.** Nothing here limits a user, checks a sanction, or
  asks for approval.
- **No rows.** None of these accounts or transfers is a PostgreSQL
  tuple; they live in the grid and the journal.

More on the model: [data-modeling.md](data-modeling.md) for how to lay
the accounts out, and [usage.md](usage.md#linked-events) for the chain
rules in full.
