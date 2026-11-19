/*
 * ledger_expiry.c - the expiry of pending transfers.
 *
 * A pending transfer with a timeout is reserved until the timeout passes.
 * When it passes, the server releases the reservation itself: nobody has to
 * ask, and the release is a journaled fact, so the balances stay
 * reconstructible from the journal alone.
 *
 * The pending transfers that asked for a timeout are held in a binary
 * min-heap in shared memory, keyed on (expiry time, creation order).  The
 * heap is filled when a pending transfer is stamped or replayed, and it is
 * rebuilt from the grid at startup.  A transfer that is posted or voided
 * before it expires is left in the heap and skipped when it reaches the
 * top, so a post or a void pays nothing for the expiry.
 *
 * The worker wakes every ledgerext.expiry_interval_ms, looks at the top of
 * the heap, and runs a bounded round when something is due.  A round does
 * the same work as a client's commit -- the releases are applied under the
 * ledger lock, stamped, written to the WAL and appended to the journal --
 * but it does not run in a transaction: a background worker has no portal
 * manager, so PostgreSQL's own commit path cannot be used in one.
 */
#include "postgres.h"

#include <string.h>
#include <time.h>

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "port/atomics.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/latch.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#include "ledgerext.h"

/* ------------------------------------------------------------------ */
/* the clock                                                           */

/* the wall clock, in nanoseconds since the Unix epoch */
uint64
ledger_now_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return 0;
	return (uint64) ts.tv_sec * 1000000000ULL + (uint64) ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* the heap                                                            */

/*
 * The heap holds the index of a transfer, and nothing else: the expiry time
 * of a transfer is its own timestamp plus its timeout, so it is written down
 * exactly once, in the transfer's row.  The index grows with the creation
 * order, so it breaks the ties of two transfers that expire at the same
 * instant.
 */

/* the order of the heap: the expiry time first, the creation order second */
static inline bool
expiry_before(LedgerState *s, uint32 a, uint32 b)
{
	uint64		ta = ledger_expires_at(&s->transfers[a]);
	uint64		tb = ledger_expires_at(&s->transfers[b]);

	if (ta != tb)
		return ta < tb;
	return a < b;
}

static inline void
expiry_swap(LedgerState *s, uint32 a, uint32 b)
{
	uint32		t = s->expiry[a];

	s->expiry[a] = s->expiry[b];
	s->expiry[b] = t;
}

/*
 * Add a transfer to the heap.  A transfer enters it once, when its record
 * is stamped or replayed; the capacity is the number of transfers, so the
 * heap cannot overflow.
 */
void
ledger_expiry_push(LedgerState *s, uint32 idx)
{
	uint32		i;

	if (s->nexpiry >= s->max_transfers)
	{
		/* unreachable: a transfer enters the heap once */
		elog(WARNING, "ledgerext: the expiry heap is full");
		return;
	}

	i = s->nexpiry++;
	s->expiry[i] = idx;

	/* sift up */
	while (i > 0)
	{
		uint32		parent = (i - 1) / 2;

		if (!expiry_before(s, s->expiry[i], s->expiry[parent]))
			break;
		expiry_swap(s, i, parent);
		i = parent;
	}
}

/*
 * Restore the heap's order below the given position.  Also used to build
 * the heap in one pass: sifting down from the last parent up is O(n),
 * against O(n log n) for pushing the entries in one by one.
 */
static void
expiry_sift_down(LedgerState *s, uint32 i)
{
	for (;;)
	{
		uint32		l = 2 * i + 1,
					r = 2 * i + 2,
					m = i;

		if (l < s->nexpiry && expiry_before(s, s->expiry[l], s->expiry[m]))
			m = l;
		if (r < s->nexpiry && expiry_before(s, s->expiry[r], s->expiry[m]))
			m = r;
		if (m == i)
			break;
		expiry_swap(s, i, m);
		i = m;
	}
}

static uint32
expiry_pop(LedgerState *s)
{
	uint32		top = s->expiry[0];

	s->nexpiry--;
	if (s->nexpiry > 0)
	{
		s->expiry[0] = s->expiry[s->nexpiry];
		expiry_sift_down(s, 0);
	}
	return top;
}

/*
 * Put a transfer into the expiry heap, if it is one that can expire.  The
 * two paths that arm the heap -- a commit and a replay -- both call this,
 * so the rule of which transfers are armed lives in one place.
 */
void
ledger_expiry_arm(LedgerState *s, uint32 idx)
{
	const LedgerTransfer *p = &s->transfers[idx];

	if (!ledger_is_timed_pending(p))
		return;
	ledger_expiry_push(s, idx);
}

/*
 * The earliest expiry time in the heap, 0 if the heap is empty.  The caller
 * holds the ledger lock.
 */
uint64
ledger_expiry_earliest(LedgerState *s)
{
	if (s->nexpiry == 0)
		return 0;
	return ledger_expires_at(&s->transfers[s->expiry[0]]);
}

/*
 * Fill the heap from the grid.  Called once, after the snapshot has been
 * read: the pending transfers that asked for a timeout are the ones the
 * grid still holds as pending.
 *
 * The entries are put in the array as they are found and the heap is
 * restored afterwards, which is O(n) instead of the O(n log n) of pushing
 * them in one at a time.
 */
void
ledger_expiry_rebuild(LedgerState *s)
{
	size_t		i;
	uint32		n = 0;

	for (i = 0; i < s->ntransfers; i++)
	{
		const LedgerTransfer *p = &s->transfers[i];

		if (!ledger_is_timed_pending(p))
			continue;
		s->expiry[n++] = (uint32) i;
	}
	s->nexpiry = n;

	/* sift down from the last parent up: the heap is whole afterwards */
	for (i = n / 2; i > 0; i--)
		expiry_sift_down(s, (uint32) (i - 1));
}

/* ------------------------------------------------------------------ */
/* the round                                                           */

/*
 * Release the transfers whose expiry time has passed and draft the journal
 * records that say so.  At most `max` transfers are released, so the round
 * is bounded; the caller holds the ledger lock exclusively and writes the
 * records to the journal.
 */
int
ledger_apply_expiries(LedgerState *s, JournalRecord *recs, DraftRef *refs,
					   int max, uint64 now)
{
	int			n = 0;

	ledger_advance_clock(s, now);

	while (n < max && s->nexpiry > 0)
	{
		LedgerTransfer *p;
		uint32		idx;

		if (ledger_expires_at(&s->transfers[s->expiry[0]]) > s->clock)
			break;			/* nothing more is due */
		idx = expiry_pop(s);
		p = &s->transfers[idx];
		if (!ledger_is_timed_pending(p))
			continue;		/* it was resolved before it expired */

		/* the money goes back, and a closed account is reopened */
		ledger_release_pending(s, p, true);
		p->state = TSTATE_EXPIRED;

		/*
		 * The record of the release.  It carries the expired transfer's
		 * id in both id and pending_id, so every reader has to look at
		 * the kind before it treats the record as a transfer.
		 */
		memset(&recs[n], 0, sizeof(JournalRecord));
		recs[n].kind = JOURNAL_EXPIRE;
		/*
		 * The release names the transfer it released, so the stamp links
		 * it to that transfer's row - and not to a row of its own: the
		 * release is the transfer's post_seq, not its own position.
		 */
		refs[n].row = (int32) idx;
		refs[n].pending_row = -1;
		memcpy(recs[n].id, p->id, 16);
		memcpy(recs[n].pending_id, p->id, 16);
		memcpy(recs[n].debit, s->slots[p->debit_slot].id.data, 16);
		memcpy(recs[n].credit, s->slots[p->credit_slot].id.data, 16);
		recs[n].amount = p->amount;
		recs[n].amount_actual = 0;
		recs[n].pending_flags = p->flags;
		/*
		 * The release has no flags of its own, but it names a transfer,
		 * and the transfer names a book: the tenant rides in the record's
		 * flags, so the replay puts the release in the same book.
		 */
		recs[n].flags = ((uint32) p->tenant) << LEDGER_TENANT_SHIFT;
		recs[n].code = p->code;
		recs[n].ledger = p->ledger;
		recs[n].state = TSTATE_EXPIRED;
		recs[n].timestamp = s->clock++;
		n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* the worker                                                          */

/*
 * One round: release what is due, write the records and flush them.
 *
 * The round does not run in a transaction.  A background worker has no
 * portal manager, so PostgreSQL's own commit path cannot be used in one,
 * and the ledger does not need it: the work is applied under the ledger
 * lock, stamped, written to the WAL and appended to the journal, exactly
 * as a client's commit does it, and the WAL is flushed before the round
 * returns.
 */
static void
ledger_expiry_round(JournalRecord *recs, DraftRef *refs, uint64 now)
{
	LedgerState *s = ledger_state;
	int			n;

	LWLockAcquire(ledger_lock, LW_EXCLUSIVE);
	PG_TRY();
	{
		n = ledger_apply_expiries(s, recs, refs, WAL_CHUNK, now);
		if (n > 0)
		{
			ledger_stamp_records(s, recs, refs, n);
			ledger_write_records(s, recs, n);
			s->gen++;
		}
	}
	PG_CATCH();
	{
		/*
		 * Something went wrong after the grid was touched: the grid, the
		 * WAL and the journal may now disagree, so refuse to do anything
		 * else until the server is restarted.
		 */
		s->broken = true;
		LWLockRelease(ledger_lock);
		PG_RE_THROW();
	}
	PG_END_TRY();
	LWLockRelease(ledger_lock);

	ledger_flush_journal();
}

/*
 * One tick: release what is due.  The rounds keep coming until nothing is
 * due any more, and each round is bounded.
 */
static void
ledger_expiry_tick(JournalRecord *recs, DraftRef *refs)
{
	LedgerState *s = ledger_state;
	uint64		now,
				due;

	if (!ledger_shmem_ready || s == NULL)
		return;

	for (;;)
	{
		now = ledger_now_ns();

		LWLockAcquire(ledger_lock, LW_SHARED);
		due = ledger_expiry_earliest(s);
		LWLockRelease(ledger_lock);

		if (due == 0 || due > now)
			return;				/* nothing is due */
		if (s->broken)
			return;				/* the grid and the WAL disagree */

		PG_TRY();
		{
			ledger_expiry_round(recs, refs, now);
		}
		PG_CATCH();
		{
			/*
			 * The round failed.  The ledger has already been marked
			 * broken, so it refuses to work until the server is
			 * restarted; the worker stays up to say so once more.
			 */
			EmitErrorReport();
			FlushErrorState();
			return;
		}
		PG_END_TRY();
	}
}

PGDLLEXPORT void
ledger_expiry_worker(Datum arg)
{
	JournalRecord *recs;
	DraftRef  *refs;

	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();

	recs = (JournalRecord *) palloc(WAL_CHUNK * sizeof(JournalRecord));
	refs = (DraftRef *) palloc(WAL_CHUNK * sizeof(DraftRef));

	while (!ShutdownRequestPending)
	{
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		/*
		 * A replica decides nothing: the primary decides when a pending
		 * transfer expires, and the release travels to the replica as a
		 * record like any other.
		 */
		if (!ledger_is_replica())
			ledger_expiry_tick(recs, refs);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ledger_expiry_interval_ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	proc_exit(0);
}
