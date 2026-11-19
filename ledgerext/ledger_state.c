/*
 * ledger_state.c - the grid of accounts and transfers, and the state
 * machine that moves money.
 *
 * The state machine is the double-entry one:
 *   - four counters per account (debits/credits x pending/posted),
 *   - a pending transfer reserves money, posting it releases the
 *     reservation and moves the money (a partial post is allowed), voiding
 *     it releases the reservation and moves nothing,
 *   - a closing transfer closes its account, voiding it reopens the account,
 *   - a balancing transfer moves at most the requested amount, capped by
 *     the balance available,
 *   - every event gets its own status; a failing event does not fail the
 *     batch, but events chained with the LINKED flag succeed or fail as a
 *     unit.
 */
#include "postgres.h"

#include <string.h>

#include "common/int.h"
#include "utils/memutils.h"

#include "ledgerext.h"

uint32
ledger_hash(const unsigned char *id)
{
	uint64		a,
				b,
				h;

	memcpy(&a, id, 8);
	memcpy(&b, id + 8, 8);
	h = a ^ (b * 0x9E3779B97F4A7C15ULL);
	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;
	return (uint32) h;
}

/* the id 2^128-1 is reserved */
static inline bool
is_int_max_id(const unsigned char *id)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (id[i] != 0xff)
			return false;
	return true;
}

static inline bool
is_zero_user_data(const unsigned char *ud)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (ud[i])
			return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* the tables                                                          */

/* find an account, skipping rows that a chain rollback deleted */
static int64
ledger_find_account_h(LedgerState *s, const unsigned char *id, uint32 h)
{
	uint32		b = h & s->amask;

	for (;;)
	{
		uint32		ent = s->abuckets[b];

		if (ent == 0)
			return -1;
		if (!s->slots[ent - 1].deleted &&
			memcmp(s->slots[ent - 1].id.data, id, 16) == 0)
			return (int64) (ent - 1);
		b = (b + 1) & s->amask;
	}
}

int64
ledger_find_account(LedgerState *s, const unsigned char *id)
{
	return ledger_find_account_h(s, id, ledger_hash(id));
}

/* find a transfer, skipping rows that a chain rollback deleted */
static int64
ledger_find_transfer_h(LedgerState *s, const unsigned char *id, uint32 h)
{
	uint32		b = h & s->tmask;

	for (;;)
	{
		uint32		ent = s->tbuckets[b];

		if (ent == 0)
			return -1;
		if (!s->transfers[ent - 1].deleted &&
			memcmp(s->transfers[ent - 1].id, id, 16) == 0)
			return (int64) (ent - 1);
		b = (b + 1) & s->tmask;
	}
}

int64
ledger_find_transfer(LedgerState *s, const unsigned char *id)
{
	return ledger_find_transfer_h(s, id, ledger_hash(id));
}

/* insert an account; the caller has checked that the id is free */
uint32
ledger_insert_account(LedgerState *s, const PackedAccount *a, uint32 h,
					  uint16 tenant)
{
	uint32		slot = (uint32) s->nslots++;
	uint32		b = h & s->amask;

	memset(&s->slots[slot], 0, sizeof(LedgerSlot));
	memcpy(s->slots[slot].id.data, a->id, 16);
	s->slots[slot].flags = a->flags;
	s->slots[slot].code = a->code;
	s->slots[slot].ledger = a->ledger;
	s->slots[slot].tenant = tenant;
	s->slots[slot].timestamp = a->timestamp;
	memcpy(s->slots[slot].user_data, a->user_data, 16);
	while (s->abuckets[b] != 0)
		b = (b + 1) & s->amask;
	s->abuckets[b] = slot + 1;
	return slot;
}

/* insert a transfer; the caller has checked that the id is free */
uint32
ledger_insert_transfer(LedgerState *s, const PackedTransfer *t,
					   uint32 debit_slot, uint32 credit_slot, uint16 state,
					   uint32 h, uint16 tenant)
{
	uint32		idx = (uint32) s->ntransfers++;
	uint32		b = h & s->tmask;

	memset(&s->transfers[idx], 0, sizeof(LedgerTransfer));
	memcpy(s->transfers[idx].id, t->id, 16);
	memcpy(s->transfers[idx].pending_id, t->pending_id, 16);
	memcpy(s->transfers[idx].user_data, t->user_data, 16);
	s->transfers[idx].amount = t->amount_actual;
	s->transfers[idx].timestamp = t->timestamp;
	s->transfers[idx].debit_slot = debit_slot;
	s->transfers[idx].credit_slot = credit_slot;
	s->transfers[idx].flags = t->flags;
	s->transfers[idx].code = t->code;
	s->transfers[idx].ledger = t->ledger;
	s->transfers[idx].tenant = tenant;
	s->transfers[idx].timeout = t->timeout;
	s->transfers[idx].state = state;
	while (s->tbuckets[b] != 0)
		b = (b + 1) & s->tmask;
	s->tbuckets[b] = idx + 1;
	return idx;
}

/* rebuild the hash indexes, after a snapshot has been read into the arrays */
void
ledger_rebuild_indexes(LedgerState *s)
{
	size_t		i;

	memset(s->abuckets, 0, ((size_t) s->amask + 1) * sizeof(uint32));
	memset(s->tbuckets, 0, ((size_t) s->tmask + 1) * sizeof(uint32));
	for (i = 0; i < s->nslots; i++)
	{
		uint32		b;

		if (s->slots[i].deleted)
			continue;
		b = ledger_hash(s->slots[i].id.data) & s->amask;
		while (s->abuckets[b] != 0)
			b = (b + 1) & s->amask;
		s->abuckets[b] = (uint32) i + 1;
	}
	for (i = 0; i < s->ntransfers; i++)
	{
		uint32		b;

		if (s->transfers[i].deleted)
			continue;
		b = ledger_hash(s->transfers[i].id) & s->tmask;
		while (s->tbuckets[b] != 0)
			b = (b + 1) & s->tmask;
		s->tbuckets[b] = (uint32) i + 1;
	}
}

/* ------------------------------------------------------------------ */
/* undo of a linked chain                                              */

void
scope_init(LedgerScope *scope)
{
	scope->entries = NULL;
	scope->n = 0;
	scope->cap = 0;
}

void
scope_free(LedgerScope *scope)
{
	if (scope->entries)
		pfree(scope->entries);
	scope_init(scope);
}

static void
scope_push(LedgerScope *scope, const UndoEntry *u)
{
	if (scope->n == scope->cap)
	{
		int			newcap = scope->cap ? scope->cap * 2 : 32;
		UndoEntry  *ne = (UndoEntry *) palloc(newcap * sizeof(UndoEntry));

		if (scope->entries)
		{
			memcpy(ne, scope->entries, scope->n * sizeof(UndoEntry));
			pfree(scope->entries);
		}
		scope->entries = ne;
		scope->cap = newcap;
	}
	scope->entries[scope->n++] = *u;
}

/* remember an account's counters and flags before they change */
void
scope_save_account(LedgerScope *scope, LedgerState *s, uint32 slot)
{
	UndoEntry	u;

	memset(&u, 0, sizeof(u));
	u.kind = UNDO_ACCOUNT;
	u.idx = slot;
	u.debits_pending = s->slots[slot].debits_pending;
	u.credits_pending = s->slots[slot].credits_pending;
	u.debits_posted = s->slots[slot].debits_posted;
	u.credits_posted = s->slots[slot].credits_posted;
	u.flags = s->slots[slot].flags;
	scope_push(scope, &u);
}

/* remember that an account row was inserted, so a rollback can hide it */
void
scope_save_account_insert(LedgerScope *scope, uint32 slot)
{
	UndoEntry	u;

	memset(&u, 0, sizeof(u));
	u.kind = UNDO_ACCOUNT_INSERT;
	u.idx = slot;
	scope_push(scope, &u);
}

/* remember that a row was inserted, so a rollback can hide it */
void
scope_save_transfer(LedgerScope *scope, uint32 idx)
{
	UndoEntry	u;

	memset(&u, 0, sizeof(u));
	u.kind = UNDO_TRANSFER;
	u.idx = idx;
	scope_push(scope, &u);
}

/* remember a pending transfer's state before it changed */
void
scope_save_pending(LedgerScope *scope, uint32 idx, uint16 state)
{
	UndoEntry	u;

	memset(&u, 0, sizeof(u));
	u.kind = UNDO_PENDING;
	u.idx = idx;
	u.state = state;
	scope_push(scope, &u);
}

/* undo everything done since the given mark */
void
scope_rollback(LedgerScope *scope, LedgerState *s, int mark)
{
	while (scope->n > mark)
	{
		UndoEntry  *u = &scope->entries[--scope->n];

		switch (u->kind)
		{
			case UNDO_ACCOUNT:
				s->slots[u->idx].debits_pending = u->debits_pending;
				s->slots[u->idx].credits_pending = u->credits_pending;
				s->slots[u->idx].debits_posted = u->debits_posted;
				s->slots[u->idx].credits_posted = u->credits_posted;
				s->slots[u->idx].flags = u->flags;
				break;
			case UNDO_TRANSFER:
				/* the row stays in the table but is invisible */
				s->transfers[u->idx].deleted = true;
				break;
			case UNDO_ACCOUNT_INSERT:
				s->slots[u->idx].deleted = true;
				break;
			case UNDO_PENDING:
				s->transfers[u->idx].state = u->state;
				break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* accounts                                                            */

/* the flags a client may set when creating an account */
#define ACCT_FLAGS_CREATEABLE	(ACCT_LINKED | ACCT_DEBITS_MUST_NOT_EXCEED_CREDITS | \
								 ACCT_CREDITS_MUST_NOT_EXCEED_DEBITS)

/*
 * Apply one account event.  Returns the status; on failure nothing has
 * been changed.
 */
static int64
apply_account(LedgerState *s, PackedAccount *a, LedgerScope *scope,
			  JournalRecord *rec, bool undo, uint16 tenant)
{
	int64		ex;
	uint32		ah;

	if (a->flags & ~ACCT_FLAGS_ALL)
		return AST_RESERVED_FLAG;
	if (is_zero_id(a->id))
		return AST_ID_MUST_NOT_BE_ZERO;
	if (is_int_max_id(a->id))
		return AST_ID_MUST_NOT_BE_INT_MAX;

	ah = ledger_hash(a->id);
	ex = ledger_find_account_h(s, a->id, ah);
	if (ex >= 0)
	{
		LedgerSlot *slot = &s->slots[ex];

		/*
		 * An account id belongs to one book.  Another tenant's account is
		 * not this one's to create, not even an identical one: the id
		 * would then answer two books at once.
		 */
		if (slot->tenant != tenant)
			return AST_ACCOUNT_IN_ANOTHER_TENANT;
		if (slot->flags != a->flags)
			return AST_EXISTS_WITH_DIFFERENT_FLAGS;
		if (memcmp(slot->user_data, a->user_data, 16) != 0)
			return AST_EXISTS_WITH_DIFFERENT_USER_DATA;
		if (slot->ledger != a->ledger)
			return AST_EXISTS_WITH_DIFFERENT_LEDGER;
		if (slot->code != a->code)
			return AST_EXISTS_WITH_DIFFERENT_CODE;
		return AST_EXISTS;
	}

	if (a->flags & ~(uint32) ACCT_FLAGS_CREATEABLE)
		return AST_RESERVED_FLAG;
	if ((a->flags & ACCT_DEBITS_MUST_NOT_EXCEED_CREDITS) &&
		(a->flags & ACCT_CREDITS_MUST_NOT_EXCEED_DEBITS))
		return AST_FLAGS_ARE_MUTUALLY_EXCLUSIVE;
	if (a->ledger == 0)
		return AST_LEDGER_MUST_NOT_BE_ZERO;
	if (a->code == 0)
		return AST_CODE_MUST_NOT_BE_ZERO;
	if (s->nslots >= s->max_accounts)
		return AST_NO_ACCOUNT_SLOTS;

	if (undo)
		scope_save_account_insert(scope, (uint32) s->nslots);	/* hide the row on rollback */
	ledger_insert_account(s, a, ah, tenant);

	/* the draft of the account's journal record */
	rec->kind = JOURNAL_ACCOUNT;
	memcpy(rec->id, a->id, 16);
	memcpy(rec->user_data, a->user_data, 16);
	rec->flags = a->flags | ((uint32) tenant << LEDGER_TENANT_SHIFT);
	rec->code = a->code;
	rec->ledger = a->ledger;
	rec->amount = 0;
	rec->amount_actual = 0;
	return AST_CREATED;
}

/* ------------------------------------------------------------------ */
/* transfers                                                           */

/*
 * Release a pending transfer's reservation.  The money goes back to the
 * accounts it was held from; a closing transfer's account is reopened when
 * the reservation is given up (a void, an expiry), and stays closed when
 * the transfer is posted.
 *
 * The void path, the expiry round and the WAL replay all call this, so the
 * three cannot drift apart.
 */
void
ledger_release_pending(LedgerState *s, const LedgerTransfer *p, bool reopen)
{
	uint32		ds = p->debit_slot;
	uint32		cs = p->credit_slot;

	s->slots[ds].debits_pending -= p->amount;
	s->slots[cs].credits_pending -= p->amount;

	if (reopen)
	{
		if (p->flags & TR_CLOSING_DEBIT)
			s->slots[ds].flags &= ~ACCT_CLOSED;
		if (p->flags & TR_CLOSING_CREDIT)
			s->slots[cs].flags &= ~ACCT_CLOSED;
	}
}

/*
 * The idempotency check: the same
 * id may be submitted again, but only with the same content.
 */
static int64
transfer_exists_status(LedgerState *s, const PackedTransfer *t,
					   const LedgerTransfer *e)
{
	int64		pidx;

	if (e->flags != t->flags)
		return ST_EXISTS_WITH_DIFFERENT_FLAGS;
	if (memcmp(e->pending_id, t->pending_id, 16) != 0)
		return ST_EXISTS_WITH_DIFFERENT_PENDING_ID;
	if (e->timeout != t->timeout)
		return ST_EXISTS_WITH_DIFFERENT_TIMEOUT;

	if (t->flags & (TR_POST | TR_VOID))
	{
		pidx = ledger_find_transfer(s, t->pending_id);
		if (pidx < 0)
			return ST_EXISTS;

		if (!is_zero_id(t->debit) &&
			memcmp(t->debit, s->slots[e->debit_slot].id.data, 16) != 0)
			return ST_EXISTS_WITH_DIFFERENT_DEBIT_ACCOUNT;
		if (!is_zero_id(t->credit) &&
			memcmp(t->credit, s->slots[e->credit_slot].id.data, 16) != 0)
			return ST_EXISTS_WITH_DIFFERENT_CREDIT_ACCOUNT;

		if (t->flags & TR_VOID)
		{
			if (t->amount == 0)
			{
				if (e->amount != s->transfers[pidx].amount)
					return ST_EXISTS_WITH_DIFFERENT_AMOUNT;
			}
			else if (t->amount != e->amount)
				return ST_EXISTS_WITH_DIFFERENT_AMOUNT;
		}
		else
		{
			if (t->amount == LEDGER_AMOUNT_MAX)
			{
				if (e->amount != s->transfers[pidx].amount)
					return ST_EXISTS_WITH_DIFFERENT_AMOUNT;
			}
			else if (t->amount != e->amount)
				return ST_EXISTS_WITH_DIFFERENT_AMOUNT;
		}

		if (is_zero_user_data(t->user_data))
		{
			if (memcmp(e->user_data, s->transfers[pidx].user_data, 16) != 0)
				return ST_EXISTS_WITH_DIFFERENT_USER_DATA;
		}
		else if (memcmp(e->user_data, t->user_data, 16) != 0)
			return ST_EXISTS_WITH_DIFFERENT_USER_DATA;

		if (t->ledger != 0 && t->ledger != e->ledger)
			return ST_EXISTS_WITH_DIFFERENT_LEDGER;
		if (t->code != 0 && t->code != e->code)
			return ST_EXISTS_WITH_DIFFERENT_CODE;
		return ST_EXISTS;
	}

	if (memcmp(s->slots[e->debit_slot].id.data, t->debit, 16) != 0)
		return ST_EXISTS_WITH_DIFFERENT_DEBIT_ACCOUNT;
	if (memcmp(s->slots[e->credit_slot].id.data, t->credit, 16) != 0)
		return ST_EXISTS_WITH_DIFFERENT_CREDIT_ACCOUNT;

	/* a balancing transfer records the amount it actually moved */
	if (t->flags & (TR_BALANCING_DEBIT | TR_BALANCING_CREDIT))
	{
		if (t->amount < e->amount)
			return ST_EXISTS_WITH_DIFFERENT_AMOUNT;
	}
	else if (t->amount != e->amount)
		return ST_EXISTS_WITH_DIFFERENT_AMOUNT;

	if (memcmp(e->user_data, t->user_data, 16) != 0)
		return ST_EXISTS_WITH_DIFFERENT_USER_DATA;
	if (e->ledger != t->ledger)
		return ST_EXISTS_WITH_DIFFERENT_LEDGER;
	if (e->code != t->code)
		return ST_EXISTS_WITH_DIFFERENT_CODE;
	return ST_EXISTS;
}

/* post or void a pending transfer */
static int64
apply_post_or_void(LedgerState *s, PackedTransfer *t, LedgerScope *scope,
				   JournalRecord *rec, DraftRef *ref, bool undo,
				   const EvHash *h, uint16 tenant)
{
	int64		pidx;
	LedgerTransfer *p;
	ledger_amount amount_actual;
	uint32		ds,
				cs;
	bool		is_void = (t->flags & TR_VOID) != 0;

	if ((t->flags & TR_POST) && (t->flags & TR_VOID))
		return ST_FLAGS_ARE_MUTUALLY_EXCLUSIVE;
	if (t->flags & TR_PENDING)
		return ST_FLAGS_ARE_MUTUALLY_EXCLUSIVE;
	if (t->flags & (TR_BALANCING_DEBIT | TR_BALANCING_CREDIT |
					TR_CLOSING_DEBIT | TR_CLOSING_CREDIT))
		return ST_FLAGS_ARE_MUTUALLY_EXCLUSIVE;

	if (is_zero_id(t->pending_id))
		return ST_PENDING_ID_MUST_NOT_BE_ZERO;
	if (is_int_max_id(t->pending_id))
		return ST_PENDING_ID_MUST_NOT_BE_INT_MAX;
	if (memcmp(t->pending_id, t->id, 16) == 0)
		return ST_PENDING_ID_MUST_BE_DIFFERENT;
	if (t->timeout != 0)
		return ST_TIMEOUT_RESERVED_FOR_PENDING;

	pidx = ledger_find_transfer(s, t->pending_id);
	if (pidx < 0)
		return ST_PENDING_TRANSFER_NOT_FOUND;
	p = &s->transfers[pidx];
	/* a pending transfer of another tenant is not this one's to post */
	if (p->tenant != tenant)
		return ST_ACCOUNT_IN_ANOTHER_TENANT;
	if (p->state == TSTATE_APPLIED)
		return ST_PENDING_TRANSFER_NOT_PENDING;

	/*
	 * A transfer that has expired cannot be posted or voided, whether or
	 * not the server has released the reservation yet.  The decision is
	 * made by the clock, so it does not depend on the release having run:
	 * s->clock is the time this event will be committed at.
	 */
	if (p->state == TSTATE_EXPIRED)
		return ST_PENDING_TRANSFER_EXPIRED;
	if (p->state == TSTATE_PENDING && p->timeout > 0 &&
		ledger_expires_at(p) <= s->clock)
		return ST_PENDING_TRANSFER_EXPIRED;

	ds = p->debit_slot;
	cs = p->credit_slot;

	if (!is_zero_id(t->debit) &&
		memcmp(t->debit, s->slots[ds].id.data, 16) != 0)
		return ST_PENDING_DIFFERENT_DEBIT_ACCOUNT;
	if (!is_zero_id(t->credit) &&
		memcmp(t->credit, s->slots[cs].id.data, 16) != 0)
		return ST_PENDING_DIFFERENT_CREDIT_ACCOUNT;
	if (t->ledger != 0 && t->ledger != p->ledger)
		return ST_PENDING_DIFFERENT_LEDGER;
	if (t->code != 0 && t->code != p->code)
		return ST_PENDING_DIFFERENT_CODE;

	/* a void voids the whole pending transfer, a post may post part of it */
	if (is_void)
		amount_actual = (t->amount == 0) ? p->amount : t->amount;
	else
		amount_actual = (t->amount == LEDGER_AMOUNT_MAX) ? p->amount : t->amount;

	if (amount_actual > p->amount)
		return ST_EXCEEDS_PENDING_AMOUNT;
	if (is_void && amount_actual < p->amount)
		return ST_PENDING_DIFFERENT_AMOUNT;

	if (p->state == TSTATE_POSTED)
		return ST_PENDING_ALREADY_POSTED;
	if (p->state == TSTATE_VOIDED)
		return ST_PENDING_ALREADY_VOIDED;

	/* voiding is the only movement allowed in a closed account */
	if (!is_void)
	{
		if (s->slots[ds].flags & ACCT_CLOSED)
			return ST_DEBIT_ACCOUNT_CLOSED;
		if (s->slots[cs].flags & ACCT_CLOSED)
			return ST_CREDIT_ACCOUNT_CLOSED;
	}

	if (s->ntransfers >= s->max_transfers)
		return ST_NO_TRANSFER_SLOTS;

	/* the post/void transfer inherits the pending transfer's accounts */
	memcpy(t->debit, s->slots[ds].id.data, 16);
	memcpy(t->credit, s->slots[cs].id.data, 16);
	t->ledger = p->ledger;
	t->code = p->code;
	if (is_zero_user_data(t->user_data))
		memcpy(t->user_data, p->user_data, 16);
	t->amount_actual = amount_actual;

	if (undo)
	{
		scope_save_account(scope, s, ds);
		scope_save_account(scope, s, cs);
		scope_save_pending(scope, (uint32) pidx, p->state);
	}

	/* release the reservation */
	ledger_release_pending(s, p, is_void);

	if (is_void)
	{
		p->state = TSTATE_VOIDED;
	}
	else
	{
		s->slots[ds].debits_posted += amount_actual;
		s->slots[cs].credits_posted += amount_actual;
		p->state = TSTATE_POSTED;
	}

	if (undo)
		scope_save_transfer(scope, (uint32) s->ntransfers);
	ref->row = (int32) ledger_insert_transfer(s, t, ds, cs, TSTATE_APPLIED,
											  h->t, tenant);
	ref->pending_row = (int32) pidx;

	/*
	 * The draft of the post's or the void's journal record.  It carries the
	 * pending transfer's flags, because the replay has to release the
	 * reservation and, for a void of a closing transfer, to reopen the
	 * account.  The amount the reservation held is the pending transfer's
	 * own amount, which the pending transfer's own record says.
	 */
	memset(rec, 0, sizeof(*rec));
	rec->kind = JOURNAL_TRANSFER;
	memcpy(rec->id, t->id, 16);
	memcpy(rec->pending_id, t->pending_id, 16);
	memcpy(rec->debit, s->slots[ds].id.data, 16);
	memcpy(rec->credit, s->slots[cs].id.data, 16);
	memcpy(rec->user_data, t->user_data, 16);
	rec->amount = t->amount;
	rec->amount_actual = amount_actual;
	rec->pending_flags = p->flags;
	rec->flags = t->flags | ((uint32) tenant << LEDGER_TENANT_SHIFT);
	rec->code = t->code;
	rec->ledger = t->ledger;
	rec->state = is_void ? TSTATE_VOIDED : TSTATE_POSTED;
	return ST_CREATED;
}

/* how far ahead the batch's lookups are prefetched */
#define PREFETCH_DIST 12

/*
 * The batch's hashes are kept from one batch to the next: a batch of
 * eight thousand events needs ninety kilobytes, and a fresh allocation of
 * that is a fault and a zeroing for every page of it.  The buffer is
 * this backend's own, and it is filled outside the ledger's lock.
 */
static EvHash *hash_cache = NULL;
static size_t hash_cap = 0;

/*
 * The hashes of a batch's ids.  The lookups of a batch are random reads
 * into two big arrays, and each one costs a cache miss that nothing else
 * overlaps with, so the hashes are computed up front and the buckets are
 * prefetched a few events ahead: by the time an event is applied, its
 * buckets are on their way in.
 *
 * The pass touches only the batch and this backend's own buffer, so it
 * runs before the ledger's lock: a commit that holds the lock makes
 * every other commit wait, and the hashing is not the ledger's work.
 */
EvHash *
ledger_batch_hashes(PackedTransfer *events, int n)
{
	void	   *hbuf = hash_cache;
	int			i;

	ledger_scratch_get(&hbuf, &hash_cap, (size_t) Max(n, 1), sizeof(EvHash));
	hash_cache = (EvHash *) hbuf;

	for (i = 0; i < n; i++)
	{
		hash_cache[i].t = ledger_hash(events[i].id);
		hash_cache[i].d = ledger_hash(events[i].debit);
		hash_cache[i].c = ledger_hash(events[i].credit);
	}
	return hash_cache;
}

/* a single phase or pending transfer */
static int64
apply_transfer(LedgerState *s, PackedTransfer *t, LedgerScope *scope,
			   JournalRecord *rec, DraftRef *ref, bool undo,
			   const EvHash *h, uint16 tenant)
{
	int64		ds,
				cs,
				ex;
	ledger_amount amount_actual;
	bool		is_pending = (t->flags & TR_PENDING) != 0;
	LedgerSlot *dr,
			   *cr;

	if (t->flags & ~TR_FLAGS_ALL)
		return ST_RESERVED_FLAG;
	if (t->flags & TR_IMPORTED)
		return ST_RESERVED_FLAG;
	if (is_zero_id(t->id))
		return ST_ID_MUST_NOT_BE_ZERO;
	if (is_int_max_id(t->id))
		return ST_ID_MUST_NOT_BE_INT_MAX;

	ex = ledger_find_transfer_h(s, t->id, h->t);
	if (ex >= 0)
	{
		/*
		 * The id belongs to another book's transfer, so the refusal says
		 * only that: the fields are not compared, or the status would
		 * tell the tenant what the other book's transfer holds.
		 */
		if (s->transfers[ex].tenant != tenant)
			return ST_ID_IN_ANOTHER_TENANT;
		return transfer_exists_status(s, t, &s->transfers[ex]);
	}

	/* posting or voiding a pending transfer is a different animal */
	if (t->flags & (TR_POST | TR_VOID))
		return apply_post_or_void(s, t, scope, rec, ref, undo, h, tenant);

	if (is_zero_id(t->debit))
		return ST_DEBIT_ACCOUNT_ID_MUST_NOT_BE_ZERO;
	if (is_int_max_id(t->debit))
		return ST_DEBIT_ACCOUNT_ID_MUST_NOT_BE_INT_MAX;
	if (is_zero_id(t->credit))
		return ST_CREDIT_ACCOUNT_ID_MUST_NOT_BE_ZERO;
	if (is_int_max_id(t->credit))
		return ST_CREDIT_ACCOUNT_ID_MUST_NOT_BE_INT_MAX;
	if (memcmp(t->debit, t->credit, 16) == 0)
		return ST_ACCOUNTS_MUST_BE_DIFFERENT;
	if (!is_zero_id(t->pending_id))
		return ST_PENDING_ID_MUST_BE_ZERO;

	if (!is_pending)
	{
		if (t->timeout != 0)
			return ST_TIMEOUT_RESERVED_FOR_PENDING;
		if (t->flags & (TR_CLOSING_DEBIT | TR_CLOSING_CREDIT))
			return ST_CLOSING_TRANSFER_MUST_BE_PENDING;
	}
	else if (t->timeout > 0)
	{
		/*
		 * The expiry time must fit in the ledger's clock: the transfer's
		 * own timestamp is the value s->clock holds now, and the expiry
		 * is that plus the timeout.  A 32-bit timeout of seconds always
		 * fits, but the guard is what keeps the promise.
		 */
		uint64		delta = (uint64) t->timeout * 1000000000ULL;

		if (s->clock > (uint64) INT64_MAX ||
			delta > (uint64) INT64_MAX - s->clock)
			return ST_OVERFLOWS_TIMEOUT;
	}

	if (t->ledger == 0)
		return ST_LEDGER_MUST_NOT_BE_ZERO;
	if (t->code == 0)
		return ST_CODE_MUST_NOT_BE_ZERO;

	ds = ledger_find_account_h(s, t->debit, h->d);
	if (ds < 0)
		return ST_DEBIT_ACCOUNT_NOT_FOUND;
	cs = ledger_find_account_h(s, t->credit, h->c);
	if (cs < 0)
		return ST_CREDIT_ACCOUNT_NOT_FOUND;

	dr = &s->slots[ds];
	cr = &s->slots[cs];
	/*
	 * The tenant is compared here, beside the ledger's own comparisons,
	 * because this is where the accounts have been found: an account of
	 * another tenant is not this transaction's to move money between, and
	 * the refusal says so rather than saying "no such account".
	 */
	if (dr->tenant != tenant || cr->tenant != tenant)
		return ST_ACCOUNT_IN_ANOTHER_TENANT;
	if (dr->ledger != cr->ledger)
		return ST_ACCOUNTS_MUST_HAVE_SAME_LEDGER;
	if (dr->ledger != t->ledger)
		return ST_TRANSFER_MUST_HAVE_SAME_LEDGER;
	if (dr->flags & ACCT_CLOSED)
		return ST_DEBIT_ACCOUNT_CLOSED;
	if (cr->flags & ACCT_CLOSED)
		return ST_CREDIT_ACCOUNT_CLOSED;

	/* how much actually moves */
	amount_actual = t->amount;
	if (t->flags & TR_BALANCING_DEBIT)
	{
		/*
		 * The balance the transfer may take is the saturating difference
		 * of the credits and the debits, so an account whose debits are
		 * larger than its credits leaves the amount that moves at zero
		 * rather than at a huge number.
		 */
		ledger_amount balance,
					available;

		if (ledger_amount_add(dr->debits_posted, dr->debits_pending, &balance))
			available = 0;			/* the balance is past the range */
		else
			available = ledger_amount_sat_sub(dr->credits_posted, balance);
		if (amount_actual > available)
			amount_actual = available;
	}
	if (t->flags & TR_BALANCING_CREDIT)
	{
		ledger_amount balance,
					available;

		if (ledger_amount_add(cr->credits_posted, cr->credits_pending, &balance))
			available = 0;
		else
			available = ledger_amount_sat_sub(cr->debits_posted, balance);
		if (amount_actual > available)
			amount_actual = available;
	}

	/*
	 * The overflow checks, in the order the limits are applied.  The sum of the pending
	 * and the posted counter is computed with an overflow check of its own,
	 * so that it cannot wrap before it is compared with the amount, and it
	 * is kept for the limit checks below, which compare the same sum.
	 */
	{
		ledger_amount tmp,
					debits_total,
					credits_total;

		if (is_pending)
		{
			if (ledger_amount_add(dr->debits_pending, amount_actual, &tmp))
				return ST_OVERFLOWS_DEBITS_PENDING;
			if (ledger_amount_add(cr->credits_pending, amount_actual, &tmp))
				return ST_OVERFLOWS_CREDITS_PENDING;
		}
		if (ledger_amount_add(dr->debits_posted, amount_actual, &tmp))
			return ST_OVERFLOWS_DEBITS_POSTED;
		if (ledger_amount_add(cr->credits_posted, amount_actual, &tmp))
			return ST_OVERFLOWS_CREDITS_POSTED;

		if (ledger_amount_add(dr->debits_pending, dr->debits_posted, &debits_total))
			return ST_OVERFLOWS_DEBITS;
		if (ledger_amount_add(debits_total, amount_actual, &debits_total))
			return ST_OVERFLOWS_DEBITS;
		if (ledger_amount_add(cr->credits_pending, cr->credits_posted, &credits_total))
			return ST_OVERFLOWS_CREDITS;
		if (ledger_amount_add(credits_total, amount_actual, &credits_total))
			return ST_OVERFLOWS_CREDITS;

		/* the limits of the accounts, against the same sums */
		if (dr->flags & ACCT_DEBITS_MUST_NOT_EXCEED_CREDITS)
		{
			if (debits_total > dr->credits_posted)
				return ST_EXCEEDS_CREDITS;
		}
		if (cr->flags & ACCT_CREDITS_MUST_NOT_EXCEED_DEBITS)
		{
			if (credits_total > cr->debits_posted)
				return ST_EXCEEDS_DEBITS;
		}
	}

	if (s->ntransfers >= s->max_transfers)
		return ST_NO_TRANSFER_SLOTS;

	if (undo)
	{
		scope_save_account(scope, s, (uint32) ds);
		scope_save_account(scope, s, (uint32) cs);
	}

	t->amount_actual = amount_actual;
	if (is_pending)
	{
		dr->debits_pending += amount_actual;
		cr->credits_pending += amount_actual;
	}
	else
	{
		dr->debits_posted += amount_actual;
		cr->credits_posted += amount_actual;
	}

	/* a closing transfer closes its account */
	if (t->flags & TR_CLOSING_DEBIT)
		dr->flags |= ACCT_CLOSED;
	if (t->flags & TR_CLOSING_CREDIT)
		cr->flags |= ACCT_CLOSED;

	if (undo)
		scope_save_transfer(scope, (uint32) s->ntransfers);
	ref->row = (int32) ledger_insert_transfer(s, t, (uint32) ds, (uint32) cs,
											  is_pending ? TSTATE_PENDING : TSTATE_APPLIED,
											  h->t, tenant);
	ref->pending_row = -1;

	/* the draft of the transfer's journal record */
	rec->kind = JOURNAL_TRANSFER;
	memcpy(rec->id, t->id, 16);
	memcpy(rec->debit, dr->id.data, 16);
	memcpy(rec->credit, cr->id.data, 16);
	memcpy(rec->user_data, t->user_data, 16);
	rec->amount = t->amount;
	rec->amount_actual = amount_actual;
	rec->flags = t->flags | ((uint32) tenant << LEDGER_TENANT_SHIFT);
	rec->code = t->code;
	rec->ledger = t->ledger;
	rec->timeout = t->timeout;
	rec->state = is_pending ? TSTATE_PENDING : TSTATE_APPLIED;
	return ST_CREATED;
}

/* ------------------------------------------------------------------ */
/* batches                                                             */

/*
 * A chain of linked events must succeed or fail as a unit.  The chain
 * starts at the first event with the LINKED flag and ends at the first
 * event without it.  If an event of the chain fails, the whole chain is
 * rolled back: every event of the chain that was applied reports
 * linked_event_failed, and the event that broke the chain keeps its own
 * status.  A chain left open at the end of the batch is rolled back and
 * reports linked_event_chain_open.
 */
typedef struct ChainState
{
	bool		open;			/* a chain is running */
	bool		broken;			/* the chain has failed */
	int			mark;			/* undo mark when the chain started */
	int			start;			/* index of the chain's first event */
} ChainState;

/*
 * Apply a batch of transfers.  statuses[] receives one status per event.
 * Returns the number of events.
 */
int
ledger_apply_transfers(LedgerState *s, PackedTransfer *events, int n,
					   int64 *statuses, JournalRecord *recs, DraftRef *refs,
					   EvHash *hashes, uint16 tenant)
{
	LedgerScope scope;
	ChainState	chain = {false, false, 0, 0};
	int			i;

	scope_init(&scope);

	for (i = 0; i < n; i++)
	{
		PackedTransfer *t = &events[i];
		bool		linked = (t->flags & TR_LINKED) != 0;
		bool		undo;			/* this event may have to be rolled back */
		int64		st;

		if (i + PREFETCH_DIST < n)
		{
			EvHash *hp = &hashes[i + PREFETCH_DIST];

			__builtin_prefetch(&s->tbuckets[hp->t & s->tmask], 0, 1);
			__builtin_prefetch(&s->abuckets[hp->d & s->amask], 0, 1);
			__builtin_prefetch(&s->abuckets[hp->c & s->amask], 0, 1);
		}

		/* the draft is only a draft until the batch's statuses are final */
		memset(&recs[i], 0, sizeof(JournalRecord));
		refs[i].row = -1;
		refs[i].pending_row = -1;

		/*
		 * Only an event of a linked chain can be rolled back: the chain
		 * starts at a linked event and ends at the first event without the
		 * flag.  So the undo is kept for the chain's events and for nobody
		 * else, and a batch of ordinary transfers does not pay for a copy
		 * of the counters that nobody reads.
		 */
		undo = linked || chain.open;

		if (linked)
		{
			if (!chain.open)
			{
				chain.open = true;
				chain.broken = false;
				chain.mark = scope.n;
				chain.start = i;
			}
			if (i == n - 1)
			{
				/* the chain is left open at the end of the batch */
				st = ST_LINKED_EVENT_CHAIN_OPEN;
				goto have_status;
			}
		}
		if (chain.broken)
		{
			st = ST_LINKED_EVENT_FAILED;
			goto have_status;
		}

		/*
		 * The commit time is the clock's next value.  It is assigned
		 * before the event is applied, so the row the state machine
		 * inserts already carries it; a row created pending keeps the
		 * time its expiry is measured from.
		 */
		t->timestamp = s->clock;

		st = apply_transfer(s, t, &scope, &recs[i], &refs[i], undo, &hashes[i],
							tenant);
		if (st == ST_CREATED)
		{
			s->clock++;
			recs[i].timestamp = t->timestamp;
		}

have_status:
		if (st != ST_CREATED && chain.open && !chain.broken)
		{
			/* the chain is broken: roll it back */
			chain.broken = true;
			scope_rollback(&scope, s, chain.mark);
			for (int j = chain.start; j < i; j++)
				statuses[j] = ST_LINKED_EVENT_FAILED;
		}
		statuses[i] = st;

		if (chain.open && (!linked || st == ST_LINKED_EVENT_CHAIN_OPEN))
		{
			/* the chain is complete */
			chain.open = false;
			chain.broken = false;
		}
	}
	scope_free(&scope);
	return n;
}

/* Apply a batch of accounts. */
int
ledger_apply_accounts(LedgerState *s, PackedAccount *events, int n,
					  int64 *statuses, JournalRecord *recs, uint16 tenant)
{
	LedgerScope scope;
	ChainState	chain = {false, false, 0, 0};
	int			i;

	scope_init(&scope);
	for (i = 0; i < n; i++)
	{
		PackedAccount *a = &events[i];
		bool		linked = (a->flags & ACCT_LINKED) != 0;
		bool		undo;			/* this event may have to be rolled back */
		int64		st;

		/* the draft is only a draft until the batch's statuses are final */
		memset(&recs[i], 0, sizeof(JournalRecord));

		/* the same rule as the transfers' */
		undo = linked || chain.open;

		if (linked)
		{
			if (!chain.open)
			{
				chain.open = true;
				chain.broken = false;
				chain.mark = scope.n;
				chain.start = i;
			}
			if (i == n - 1)
			{
				st = AST_LINKED_EVENT_CHAIN_OPEN;
				goto have_status;
			}
		}
		if (chain.broken)
		{
			st = AST_LINKED_EVENT_FAILED;
			goto have_status;
		}

		/* the commit time is the clock's next value, assigned before the
		 * event is applied so the row the state machine inserts already
		 * carries it */
		a->timestamp = s->clock;

		st = apply_account(s, a, &scope, &recs[i], undo, tenant);
		if (st == AST_CREATED)
		{
			s->clock++;
			recs[i].timestamp = a->timestamp;
		}

have_status:
		if (st != AST_CREATED && chain.open && !chain.broken)
		{
			chain.broken = true;
			scope_rollback(&scope, s, chain.mark);
			for (int j = chain.start; j < i; j++)
				statuses[j] = AST_LINKED_EVENT_FAILED;
		}
		statuses[i] = st;

		if (chain.open && (!linked || st == AST_LINKED_EVENT_CHAIN_OPEN))
		{
			chain.open = false;
			chain.broken = false;
		}
	}
	scope_free(&scope);
	return n;
}
