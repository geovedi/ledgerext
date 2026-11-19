/*
 * ledgerext.c - the module: shared memory, the queue of pending work, and
 * the transaction hook that applies it.
 *
 * TRANSACTION MODEL
 * -----------------
 *  ledger_apply() and friends only QUEUE work.  Everything is validated,
 *  WAL-logged, journaled and applied at XACT_EVENT_PRE_COMMIT, under one
 *  exclusive lock, before the commit point: a failure there aborts the
 *  transaction cleanly, with nothing applied and nothing logged.  The queue
 *  is subtransaction-aware, so ROLLBACK TO SAVEPOINT discards queued work.
 *
 *  Every event of a batch gets its own status: a
 *  failing event does not fail the batch.  The statuses are computed at the
 *  commit point and are read with ledger_last_statuses().
 *
 *  The journal's records are written in the same protected section, from
 *  the same drafts the apply produced, so the journal and the balances
 *  cannot disagree.
 */
#include "postgres.h"

#include <string.h>
#include <sys/mman.h>

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "ledgerext.h"

PG_MODULE_MAGIC;

LedgerState *ledger_state = NULL;
LWLock	   *ledger_lock = NULL;
LWLock	   *ledger_snap_lock = NULL;
bool		ledger_shmem_ready = false;

int			ledger_max_accounts = 1048576;
int			ledger_max_transfers = 8388608;
int			ledger_snapshot_interval_ms = 5000;
int			ledger_snapshot_journal_bytes = 1024 * 1024 * 1024;
int			ledger_snapshot_body_multiple = 32;
int			ledger_expiry_interval_ms = 200;
int			ledger_replica_poll_ms = 200;
bool		ledger_replica_sync = true;
char	   *ledger_replica_of = NULL;

static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

static uint32
pow2_at_least(uint64 n)
{
	uint32		p = 1;

	while (p < n)
		p <<= 1;
	return p;
}

/* ------------------------------------------------------------------ */
/* shared memory                                                       */

/*
 * The grid's tables.
 *
 * They are the ledger's working set, they are read at random, and they
 * are read thousands of times a second: a lookup is a memory access, and
 * with 4 KB pages it is also a page walk - which is why the ledger was
 * 14% slower than it needed to be (research.md D5).  So the tables live
 * in a mapping of the extension's own: 2 MB aligned, and on the host's
 * explicit huge pages (MAP_HUGETLB) where the host has a pool for them.
 * PostgreSQL's shared memory cannot be asked for that - it is a mapping
 * of its own, its alignment is not ours to choose, and we cannot mark its
 * pages.
 */
#define LEDGER_HUGE_PAGE (2 * 1024 * 1024)

/* the mapping, and the space taken in it so far */
static char *ledger_grid = NULL;
static size_t ledger_grid_used = 0;

/*
 * A piece of the mapping.  Every table starts on a cache line: a slot is
 * 128 bytes, and every slot has to start at the same offset inside a
 * line, or a transfer would touch three lines of an account instead of
 * two.
 */
static void *
ledger_grid_take(size_t bytes)
{
	char	   *p = ledger_grid + ledger_grid_used;

	p = (char *) (((uintptr_t) p + 63) & ~(uintptr_t) 63);
	ledger_grid_used = (size_t) (p - ledger_grid) + bytes;
	return p;
}

static void
ledger_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();
	RequestNamedLWLockTranche(LEDGER_TRANCHE, 2);
	RequestAddinShmemSpace(sizeof(LedgerState) + 4096);
}

static void
ledger_shmem_startup(void)
{
	bool		found;
	size_t		ab = pow2_at_least((uint64) ledger_max_accounts * 2);
	size_t		tb = pow2_at_least((uint64) ledger_max_transfers * 2);
	size_t		slotbytes = (size_t) ledger_max_accounts * sizeof(LedgerSlot);
	size_t		gridbytes;
	size_t		need;
	size_t		head;
	size_t		tabsize[5];
	int			i;
	char	   *base;
	LWLockPadded *tranche;

	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	tranche = GetNamedLWLockTranche(LEDGER_TRANCHE);
	ledger_lock = &tranche[0].lock;
	ledger_snap_lock = &tranche[1].lock;

	ledger_state = (LedgerState *) ShmemInitStruct("ledger_state", sizeof(LedgerState), &found);
	if (!found)
	{
		memset(ledger_state, 0, sizeof(LedgerState));
		ledger_state->max_accounts = ledger_max_accounts;
		ledger_state->max_transfers = ledger_max_transfers;
		ledger_state->amask = (uint32) (ab - 1);
		ledger_state->tmask = (uint32) (tb - 1);
		ledger_state->period = 1;
		ledger_state->journal_fd = -1;	/* 0 would be stdin */
	}

	/*
	 * The grid's tables, in a mapping of our own.  The pages are not
	 * touched: an anonymous page is zero when it is first read, so the
	 * tables start empty and cost nothing until they are used.
	 *
	 * The five tables' sizes are said once, here: the mapping is sized
	 * from them and the tables are taken from them, so the two cannot
	 * disagree about how much space the grid needs.
	 */
	tabsize[0] = slotbytes;						/* the account slots */
	tabsize[1] = ab * sizeof(uint32);			/* the account buckets */
	tabsize[2] = (size_t) ledger_max_transfers * sizeof(LedgerTransfer);
	tabsize[3] = tb * sizeof(uint32);			/* the transfer buckets */
	tabsize[4] = (size_t) ledger_max_transfers * sizeof(uint32);	/* the expiry heap */
	/*
	 * Every table starts on a cache line, so every table's size is
	 * rounded up to one: the mapping has to hold what the tables take,
	 * and what they take is the rounded-up sizes.
	 */
	need = 0;
	for (i = 0; i < 5; i++)
	{
		tabsize[i] = (tabsize[i] + 63) & ~(size_t) 63;
		need += tabsize[i];
	}
	/* the mapping is a whole number of huge pages */
	gridbytes = (need + LEDGER_HUGE_PAGE - 1) & ~(size_t) (LEDGER_HUGE_PAGE - 1);

	/*
	 * The explicit huge pages are asked for first: they are the ones
	 * that work whatever the host's transparent huge page policy is.
	 * A host with no pool reserved gives the ordinary mapping instead,
	 * and the pages are asked for there too - a kernel that will not
	 * give them gives 4 KB pages and a slower ledger, not a wrong one.
	 */
	base = mmap(NULL, gridbytes, PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_SHARED | MAP_HUGETLB, -1, 0);
	if (base != MAP_FAILED)
	{
		ledger_grid = base;
		head = 0;
		elog(LOG, "ledgerext: the grid's %zu MB is on huge pages",
			 gridbytes / (1024 * 1024));
	}
	else
	{
		base = mmap(NULL, gridbytes + LEDGER_HUGE_PAGE, PROT_READ | PROT_WRITE,
					MAP_ANONYMOUS | MAP_SHARED, -1, 0);
		if (base == MAP_FAILED)
			ereport(FATAL,
					(errcode_for_file_access(),
					 errmsg("ledgerext: could not map the grid: %m")));
		/* the mapping starts at a huge page boundary */
		ledger_grid = (char *) (((uintptr_t) base + LEDGER_HUGE_PAGE - 1) &
								~(uintptr_t) (LEDGER_HUGE_PAGE - 1));
		head = (size_t) (ledger_grid - base);
		if (head > 0)
			munmap(base, head);
#ifdef MADV_HUGEPAGE
		(void) madvise(ledger_grid, gridbytes - head, MADV_HUGEPAGE);
#endif
		/*
		 * The pool is the host's, and the host's is the kernel's: the
		 * grid could not be given the pages it asked for, so say what
		 * was needed and what the host has.  A ledger on ordinary pages
		 * is a correct ledger and a slower one.
		 */
		{
			long		free = -1,
						total = -1;
			FILE	   *f = fopen("/proc/meminfo", "r");
			char		line[256];

			if (f != NULL)
			{
				while (fgets(line, sizeof(line), f) != NULL)
				{
					if (sscanf(line, "HugePages_Free: %ld", &free) == 1)
						continue;
					if (sscanf(line, "HugePages_Total: %ld", &total) == 1)
						continue;
				}
				fclose(f);
			}
			elog(WARNING,
				 "ledgerext: the grid's %zu MB is not on explicit huge pages: "
				 "the host has %ld free of the %ld it reserves, and the grid "
				 "needs %zu of them.  Another node's grid may be holding the "
				 "rest: every node that preloads ledgerext maps its own grid.  "
				 "The ledger is correct and slower; reserve the pool with "
				 "'sysctl -w vm.nr_hugepages=%zu' and restart.",
				 gridbytes / (1024 * 1024), free, total,
				 gridbytes / LEDGER_HUGE_PAGE, gridbytes / LEDGER_HUGE_PAGE);
		}
	}
	/* and it ends where the tables end */
	if (gridbytes > head + need)
		munmap(ledger_grid + need, gridbytes - head - need);
	ledger_grid_used = 0;

	ledger_state->slots = (LedgerSlot *) ledger_grid_take(tabsize[0]);
	ledger_state->abuckets = (uint32 *) ledger_grid_take(tabsize[1]);
	ledger_state->transfers = (LedgerTransfer *) ledger_grid_take(tabsize[2]);
	ledger_state->tbuckets = (uint32 *) ledger_grid_take(tabsize[3]);

	/*
	 * The expiry heap.  It is not cleared: the count is zero, and an
	 * entry is always written before it is read, so the pages are only
	 * faulted in as transfers with a timeout are created.
	 */
	ledger_state->expiry = (uint32 *) ledger_grid_take(tabsize[4]);
	ledger_state->nexpiry = 0;

	/* shared memory is brand new on every (re)start: restore the snapshot */
	ledger_load_snapshot();

	LWLockRelease(AddinShmemInitLock);
	ledger_shmem_ready = true;
}

/* ------------------------------------------------------------------ */
/* module init                                                         */

void
_PG_init(void)
{
	BackgroundWorker bgw;

	if (!process_shared_preload_libraries_in_progress)
		return;

	DefineCustomIntVariable("ledgerext.max_accounts", "Capacity of the account table.", NULL,
							&ledger_max_accounts, 1048576, 16, 1 << 27,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("ledgerext.max_transfers", "Capacity of the transfer table.", NULL,
							&ledger_max_transfers, 8388608, 16, 1 << 28,
							PGC_POSTMASTER, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("ledgerext.snapshot_interval_ms", "Time between snapshots.", NULL,
							&ledger_snapshot_interval_ms, 5000, 100, 3600000,
							PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);
	DefineCustomIntVariable("ledgerext.snapshot_journal_bytes",
							"How much journal may pile up behind the snapshot before the grid is written out again.",
							NULL,
							&ledger_snapshot_journal_bytes, 1024 * 1024 * 1024, 1024 * 1024, INT_MAX,
							PGC_SIGHUP, GUC_UNIT_BYTE, NULL, NULL, NULL);
	DefineCustomIntVariable("ledgerext.snapshot_body_multiple",
							"How many times its own size the journal behind the snapshot may be before the grid is written out again.",
							NULL,
							&ledger_snapshot_body_multiple, 32, 1, 4096,
							PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("ledgerext.expiry_interval_ms", "How often the worker looks for due expiries.",
							NULL, &ledger_expiry_interval_ms, 200, 20, 60000,
							PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);
	DefineCustomStringVariable("ledgerext.replica_of",
							   "The primary this node follows.  Empty means this node is a primary.",
							   NULL, &ledger_replica_of, "",
							   PGC_SIGHUP, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("ledgerext.replica_poll_ms",
							"How often the replica asks the primary for the records it does not have.",
							NULL, &ledger_replica_poll_ms, 200, 1, 60000,
							PGC_SIGHUP, GUC_UNIT_MS, NULL, NULL, NULL);
	DefineCustomBoolVariable("ledgerext.replica_sync",
							 "Whether a commit waits for the replicas that have registered.",
							 NULL, &ledger_replica_sync, true,
							 PGC_SIGHUP, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("ledgerext");

	ledger_register_rmgr();

	prev_shmem_request_hook = shmem_request_hook;
	shmem_request_hook = ledger_shmem_request;
	prev_shmem_startup_hook = shmem_startup_hook;
	shmem_startup_hook = ledger_shmem_startup;

	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = 5;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "ledgerext");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ledger_snapshot_worker");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "ledgerext snapshotter");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "ledgerext snapshotter");
	RegisterBackgroundWorker(&bgw);

	/* the worker that releases the pending transfers that have expired */
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	bgw.bgw_start_time = BgWorkerStart_RecoveryFinished;
	bgw.bgw_restart_time = 5;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "ledgerext");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ledger_expiry_worker");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "ledgerext expiry");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "ledgerext expiry");
	RegisterBackgroundWorker(&bgw);

	/*
	 * The worker that follows a primary: it asks for the journal's records
	 * it does not have, applies them to its own grid and acknowledges the
	 * position.  It does nothing at all when this node is not a replica,
	 * so it is always registered.
	 */
	memset(&bgw, 0, sizeof(bgw));
	bgw.bgw_flags = BGWORKER_SHMEM_ACCESS;
	/*
	 * Not BgWorkerStart_RecoveryFinished: that never fires on a node that
	 * is a PostgreSQL standby, and a standby that follows a ledger
	 * primary has to be able to follow it.
	 */
	bgw.bgw_start_time = BgWorkerStart_ConsistentState;
	bgw.bgw_restart_time = 5;
	snprintf(bgw.bgw_library_name, BGW_MAXLEN, "ledgerext");
	snprintf(bgw.bgw_function_name, BGW_MAXLEN, "ledger_replica_worker");
	snprintf(bgw.bgw_name, BGW_MAXLEN, "ledgerext replica");
	snprintf(bgw.bgw_type, BGW_MAXLEN, "ledgerext replica");
	RegisterBackgroundWorker(&bgw);
}

/* ------------------------------------------------------------------ */
/* the queue of pending work                                           */

typedef enum PendingKind
{
	PEND_ACCOUNTS,
	PEND_TRANSFERS,
	PEND_CLOSE
} PendingKind;

typedef struct PendingOp
{
	struct PendingOp *next;
	SubTransactionId subid;
	PendingKind kind;
	uint32		count;
	uint32		status_at;		/* where this op's statuses go */
	/* payload follows, MAXALIGNed */
} PendingOp;

#define PENDING_DATA(op) ((char *) (op) + MAXALIGN(sizeof(PendingOp)))

static PendingOp *pending_head = NULL;
static PendingOp *pending_tail = NULL;
static bool callbacks_registered = false;

/* the statuses of the last committed batch (see ledger_last_statuses) */
static int64 *last_statuses = NULL;
static int	last_nstatuses = 0;

static void
ledger_xact_callback(XactEvent event, void *arg);
static void
ledger_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg);

static void
pending_free_all(void)
{
	while (pending_head)
	{
		PendingOp *n = pending_head->next;

		pfree(pending_head);
		pending_head = n;
	}
	pending_tail = NULL;
}

static void
pending_enqueue(PendingKind kind, uint32 count, const char *payload, size_t payload_len)
{
	PendingOp  *op;

	/*
	 * The ledger resolves its own conflicts, under its own lock.  A
	 * transaction that asked for serializable isolation would have its
	 * conflicts resolved by the database instead, and the database checks
	 * serializability only AFTER the commit hook has applied the work --
	 * so the work would be applied and the transaction would then fail.
	 * Refuse it here, where the transaction can still be abandoned.
	 */
	if (IsolationIsSerializable())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ledger: a serializable transaction cannot queue ledger work"),
				 errhint("The ledger resolves its own conflicts; use read committed "
						 "or repeatable read.")));

	op = (PendingOp *) MemoryContextAlloc(TopMemoryContext,
										  MAXALIGN(sizeof(PendingOp)) + payload_len);

	op->next = NULL;
	op->subid = GetCurrentSubTransactionId();
	op->kind = kind;
	op->count = count;
	op->status_at = 0;
	if (payload_len)
		memcpy(PENDING_DATA(op), payload, payload_len);
	if (pending_tail)
		pending_tail->next = op;
	else
		pending_head = op;
	pending_tail = op;

	if (!callbacks_registered)
	{
		RegisterXactCallback(ledger_xact_callback, NULL);
		RegisterSubXactCallback(ledger_subxact_callback, NULL);
		callbacks_registered = true;
	}
}

void
ledger_queue_accounts(uint32 count, const char *payload)
{
	/*
	 * The tenant is resolved here, when the work is queued and not under
	 * the ledger's lock: the commit path uses the tenant this transaction
	 * resolved, and a role with no tenant is refused before it has
	 * queued anything.
	 */
	ledger_tenant_require();
	pending_enqueue(PEND_ACCOUNTS, count, payload, (size_t) count * sizeof(PackedAccount));
}

void
ledger_queue_transfers(uint32 count, const char *payload)
{
	ledger_tenant_require();
	pending_enqueue(PEND_TRANSFERS, count, payload, (size_t) count * sizeof(PackedTransfer));
}

void
ledger_queue_close(void)
{
	pending_enqueue(PEND_CLOSE, 0, NULL, 0);
}

/* how many closes this transaction has already queued */
int
ledger_queued_closes(void)
{
	PendingOp  *op;
	int			n = 0;

	for (op = pending_head; op; op = op->next)
		if (op->kind == PEND_CLOSE)
			n++;
	return n;
}

void
ledger_get_last_statuses(int64 **statuses, int *n)
{
	*statuses = last_statuses;
	*n = last_nstatuses;
}

/* ------------------------------------------------------------------ */
/* applying the queued work                                            */

/*
 * Compact the drafts of a batch: keep the ones whose FINAL status is
 * `created`, and drop the rest.  This is what keeps a rejected event and
 * every member in a broken linked chain out of the journal: a chain's
 * members are created first and their statuses are only overwritten after
 * the chain breaks, so a draft is not a record until the batch is over.
 *
 * The compaction runs over the drafts, not over the events.
 *
 * The references move with the records: a reference names the row of its
 * own record, so a reference left behind would name the wrong row.
 */
static int
ledger_compact(const JournalRecord *drafts, const DraftRef *refs,
			   const int64 *statuses, int n,
			   JournalRecord *out, DraftRef *out_refs)
{
	int			i,
				k = 0;

	for (i = 0; i < n; i++)
	{
		if (statuses[i] != ST_CREATED)
			continue;			/* only the events that took effect */
		out[k] = drafts[i];
		out_refs[k] = refs[i];
		k++;
	}
	return k;
}

/*
 * Stamp the records that survived: give each its sequence number, the
 * previous record that named each of its accounts, and the period it
 * belongs to, and tell the grid about them.  Every field is final BEFORE
 * the record is appended, so that the record in the journal and the change
 * in the grid are the same fact.
 *
 * The stamp looks nothing up.  The apply already resolved the accounts and
 * the transfer the record is about, and handed the row over in the
 * reference beside the record; looking them up again would be a cold miss
 * each, because the stamp runs after the whole batch has been applied and
 * every line the apply touched has been evicted by then.
 */
void
ledger_stamp_records(LedgerState *s, JournalRecord *recs, DraftRef *refs, int n)
{
	int			i;

	for (i = 0; i < n; i++)
	{
		JournalRecord *r = &recs[i];
		int64		row = refs[i].row;
		int64		prow = refs[i].pending_row;
		uint32		ds,
					cs;

		r->seq = ++s->nrecords;
		r->period = s->period;

		if (r->kind == JOURNAL_ACCOUNT)
			continue;		/* the account is already in the grid */

		if (r->kind == JOURNAL_CLOSE)
		{
			s->last_close_seq = r->seq;
			s->period++;
			continue;
		}

		if (row < 0)
			continue;			/* there is no transfer to stamp */

		/*
		 * The row is the transfer the record is about.  The stamp writes
		 * the record's position into it, so it is a line the stamp has to
		 * touch anyway, and the two accounts it names come with it.
		 */
		ds = s->transfers[row].debit_slot;
		cs = s->transfers[row].credit_slot;

		/* the chain of the debit account */
		r->prev_debit_seq = s->slots[ds].last_seq;
		s->slots[ds].last_seq = r->seq;
		/* the chain of the credit account */
		r->prev_credit_seq = s->slots[cs].last_seq;
		s->slots[cs].last_seq = r->seq;

		if (r->kind == JOURNAL_EXPIRE)
		{
			/*
			 * The record of a release names the transfer it released,
			 * so it is that transfer's post_seq and not a position of
			 * its own.
			 */
			s->transfers[row].post_seq = r->seq;
			continue;
		}

		/* the transfer's own record */
		s->transfers[row].seq = r->seq;

		/* a pending transfer with a timeout is waiting to expire */
		ledger_expiry_arm(s, (uint32) row);

		/* the record of the post or the void that resolved it */
		if (prow >= 0)
			s->transfers[prow].post_seq = r->seq;
	}
}

/*
 * Write the records to the journal, and to nowhere else.
 *
 * The journal is the ledger's log: it holds every committed event, it is
 * append-only, and the grid is a cache of it.  It used to be written to
 * PostgreSQL's log as well, so that the log could rebuild the grid and
 * repair the journal's tail - but the copy cost 0.30 us of server CPU per
 * transfer, and the journal does not need repairing: it is made durable
 * before the transaction is acknowledged (ledger_flush_journal).
 */
void
ledger_write_records(LedgerState *s, const JournalRecord *recs, int n)
{
	int			i;

	for (i = 0; i < n; i += WAL_CHUNK)
	{
		uint32		n2 = (uint32) Min(n - i, WAL_CHUNK);

		ledger_journal_append(recs + i, (int) n2);
	}
	s->nrecords = ledger_journal_nrecords();
}

/*
 * Make the journal durable, so that a transfer the client has been told
 * about is on disk.  This is where the commit's durability is decided, and
 * it is once per commit and not once per transfer.
 *
 * The fsync is outside the ledger's lock, so the commits do not wait for
 * each other's disk.  A failure of it is still a failure of the commit: the
 * grid holds the change and the journal may not, so the ledger is marked
 * broken and refuses to work until the server is restarted.
 */
void
ledger_flush_journal(void)
{
	if (synchronous_commit <= SYNCHRONOUS_COMMIT_OFF)
		return;

	PG_TRY();
	{
		ledger_journal_fsync();
	}
	PG_CATCH();
	{
		ledger_state->broken = true;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * The commit path's working buffers.
 *
 * A commit of a big batch needs a few megabytes of scratch, and a fresh
 * allocation of that size costs a page fault and a zeroing for every 4 KB
 * of it - so the buffers are kept from one commit to the next and only
 * ever grow.  They live in TopMemoryContext because they must outlive
 * the transaction that filled them.
 *
 * The buffers are not zeroed: a consumer has to write every element it
 * reads.
 */
typedef struct LedgerScratch
{
	void	   *accs;
	void	   *xfers;
	void	   *arecs;
	void	   *trecs;
	void	   *crecs;
	void	   *refs;
	void	   *srefs;
	void	   *acc_st;
	void	   *tr_st;
	void	   *survivors;
	size_t		cap_accs;
	size_t		cap_xfers;
	size_t		cap_arecs;
	size_t		cap_trecs;
	size_t		cap_crecs;
	size_t		cap_refs;
	size_t		cap_srefs;
	size_t		cap_acc_st;
	size_t		cap_tr_st;
	size_t		cap_survivors;
} LedgerScratch;

static LedgerScratch ledger_scratch;

/*
 * A buffer that grows and is never given back.  The buffer belongs to the
 * caller, and so does the capacity beside it: the pair grows together.
 */
void *
ledger_scratch_get(void **buf, size_t *cap, size_t need, size_t elem)
{
	if (need > *cap)
	{
		size_t		want = Max(need, *cap + *cap / 2);

		if (*buf == NULL)
			*buf = MemoryContextAlloc(TopMemoryContext, want * elem);
		else
			*buf = repalloc(*buf, want * elem);
		*cap = want;
	}
	return *buf;
}

/* ------------------------------------------------------------------ */

/*
 * Apply the queued work.  The accounts are applied first, then the
 * transfers, so that a transfer can use an account created in the same
 * transaction, and the closes last.  The journal's records come in that
 * same order: the accounts, then the transfers, then the closes.
 */
static void
ledger_commit_pending(void)
{
	LedgerState *s = ledger_state;
	PendingOp  *op;
	size_t		nev = 0;
	int64	   *sts;
	EvHash    *hashes = NULL;
	uint64		now;

	/*
	 * A replica decides nothing: it follows another node's records and
	 * applies them.  A transfer applied here would fork the ledger, so it
	 * is refused before anything is touched.
	 */
	if (ledger_is_replica())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("ledgerext: this node is a replica and applies no transfers of its own"),
				 errhint("Clear ledgerext.replica_of to make this node a primary.")));

	/* where the statuses of every op go */
	for (op = pending_head; op; op = op->next)
	{
		op->status_at = (uint32) nev;
		nev += (op->kind == PEND_CLOSE) ? 1 : op->count;
	}
	/*
	 * The statuses are not scratch: they are handed to the transaction and
	 * read after the commit, so they get their own memory and the next
	 * commit gives the old one back.
	 */
	sts = (int64 *) MemoryContextAlloc(TopMemoryContext, Max(nev, 1) * sizeof(int64));
	memset(sts, 0, Max(nev, 1) * sizeof(int64));

	/* gather the queue */
	{
		size_t		nacc = 0,
					ntr = 0,
					nclose = 0;
		PackedAccount *accs;
		PackedTransfer *xfers;
		JournalRecord *arecs,
				   *trecs,
				   *crecs;
		DraftRef  *refs,
				   *srefs;
		int64	   *acc_st = NULL,
				   *tr_st = NULL;
		JournalRecord *survivors;
		size_t		iacc = 0,
					itr = 0,
					iclose = 0;
		uint16		tenant;
		int			kacc,
					ktr,
					ntotal,
					i;

		for (op = pending_head; op; op = op->next)
		{
			if (op->kind == PEND_ACCOUNTS)
				nacc += op->count;
			else if (op->kind == PEND_TRANSFERS)
				ntr += op->count;
			else if (op->kind == PEND_CLOSE)
				nclose++;
		}

		/*
		 * The tenant of this transaction, resolved before the ledger's
		 * lock is taken: the commit path must not read a table under it.
		 * The queue resolved it already, so this is the cached value; a
		 * transaction that only closed a period has no tenant and needs
		 * none.
		 */
		tenant = (nacc > 0 || ntr > 0) ? ledger_tenant_current() : 0;

		accs = (PackedAccount *) ledger_scratch_get(&ledger_scratch.accs,
											&ledger_scratch.cap_accs,
											Max(nacc, 1), sizeof(PackedAccount));
		xfers = (PackedTransfer *) ledger_scratch_get(&ledger_scratch.xfers,
											  &ledger_scratch.cap_xfers,
											  Max(ntr, 1), sizeof(PackedTransfer));
		arecs = (JournalRecord *) ledger_scratch_get(&ledger_scratch.arecs,
											  &ledger_scratch.cap_arecs,
											  Max(nacc, 1), sizeof(JournalRecord));
		trecs = (JournalRecord *) ledger_scratch_get(&ledger_scratch.trecs,
											  &ledger_scratch.cap_trecs,
											  Max(ntr, 1), sizeof(JournalRecord));
		crecs = (JournalRecord *) ledger_scratch_get(&ledger_scratch.crecs,
											  &ledger_scratch.cap_crecs,
											  Max(nclose, 1), sizeof(JournalRecord));

		/*
		 * The references the stamp will need, beside the records: the
		 * transfers' apply fills its own, and the accounts' and the
		 * closes' have none.
		 */
		refs = (DraftRef *) ledger_scratch_get(&ledger_scratch.refs,
									   &ledger_scratch.cap_refs,
									   Max(nacc + ntr + nclose, 1), sizeof(DraftRef));
		srefs = (DraftRef *) ledger_scratch_get(&ledger_scratch.srefs,
										 &ledger_scratch.cap_srefs,
										 Max(nacc + ntr + nclose, 1), sizeof(DraftRef));
		for (i = 0; i < (int) (nacc + ntr + nclose); i++)
		{
			refs[i].row = -1;
			refs[i].pending_row = -1;
			srefs[i].row = -1;
			srefs[i].pending_row = -1;
		}

		for (op = pending_head; op; op = op->next)
		{
			if (op->kind == PEND_ACCOUNTS)
			{
				memcpy(accs + iacc, PENDING_DATA(op),
					   (size_t) op->count * sizeof(PackedAccount));
				iacc += op->count;
			}
			else if (op->kind == PEND_TRANSFERS)
			{
				memcpy(xfers + itr, PENDING_DATA(op),
					   (size_t) op->count * sizeof(PackedTransfer));
				itr += op->count;
			}
		}

		/*
		 * The clock is the wall clock, read once per commit: every event
		 * of the batch takes the next nanosecond from here.  The read is
		 * outside the critical section, so it does not add to the time
		 * every committing backend spends waiting for the lock.
		 */
		now = ledger_now_ns();

		/*
		 * The batch's hashes are computed here, outside the lock: the
		 * hashing is this backend's own work, and a commit that holds the
		 * lock makes every other commit wait for it.
		 */
		hashes = ledger_batch_hashes(xfers, (int) ntr);

		LWLockAcquire(ledger_lock, LW_EXCLUSIVE);
		PG_TRY();
		{
			/*
			 * The clock never moves backwards, so a step of the system
			 * clock leaves the ledger's time where it was.
			 */
			ledger_advance_clock(s, now);

			if (nacc > 0)
			{
				acc_st = (int64 *) ledger_scratch_get(&ledger_scratch.acc_st,
											  &ledger_scratch.cap_acc_st,
											  Max(nacc, 1), sizeof(int64));
				ledger_apply_accounts(s, accs, (int) nacc, acc_st, arecs,
									   tenant);
			}
			if (ntr > 0)
			{
				tr_st = (int64 *) ledger_scratch_get(&ledger_scratch.tr_st,
											 &ledger_scratch.cap_tr_st,
											 Max(ntr, 1), sizeof(int64));
				ledger_apply_transfers(s, xfers, (int) ntr, tr_st, trecs,
									   refs + nacc, hashes, tenant);
			}

			/* the closes */
			for (op = pending_head; op; op = op->next)
			{
				if (op->kind != PEND_CLOSE)
					continue;
				memset(&crecs[iclose], 0, sizeof(JournalRecord));
				crecs[iclose].kind = JOURNAL_CLOSE;
				crecs[iclose].timestamp = s->clock++;
				iclose++;
			}

			/*
			 * Compact the drafts: only the events whose FINAL status is
			 * `created` are facts, so a rejected event and every member
			 * of a broken linked chain are dropped here.
			 */
			survivors = (JournalRecord *) ledger_scratch_get(&ledger_scratch.survivors,
										 &ledger_scratch.cap_survivors,
										 Max(nacc + ntr + nclose, 1),
										 sizeof(JournalRecord));
			kacc = ledger_compact(arecs, refs, acc_st, (int) nacc, survivors,
								   srefs);
			ktr = ledger_compact(trecs, refs + nacc, tr_st, (int) ntr,
								  survivors + kacc, srefs + kacc);
			memcpy(survivors + kacc + ktr, crecs, (size_t) nclose * sizeof(JournalRecord));
			ntotal = kacc + ktr + (int) nclose;

			/* stamp them, and tell the grid about them */
			ledger_stamp_records(s, survivors, srefs, ntotal);

			/* the journal: the ledger's log, and the only copy */
			ledger_write_records(s, survivors, ntotal);

			/* the statuses go back to the queue order of the events */
			iacc = 0;
			itr = 0;
			for (op = pending_head; op; op = op->next)
			{
				if (op->kind == PEND_ACCOUNTS)
				{
					memcpy(sts + op->status_at, acc_st + iacc,
						   (size_t) op->count * sizeof(int64));
					iacc += op->count;
				}
				else if (op->kind == PEND_TRANSFERS)
				{
					memcpy(sts + op->status_at, tr_st + itr,
						   (size_t) op->count * sizeof(int64));
					itr += op->count;
				}
				else
					sts[op->status_at] = LEDGER_STATUS_OK;
			}
			s->gen++;
		}
		PG_CATCH();
		{
			/*
			 * Something went wrong after the grid was touched: the grid and
			 * the journal may now disagree, so refuse to do anything else
			 * until the server is restarted (the grid is then rebuilt from
			 * the snapshot and the journal).
			 */
			s->broken = true;
			LWLockRelease(ledger_lock);
			PG_RE_THROW();
		}
		PG_END_TRY();
		LWLockRelease(ledger_lock);

	}

	ledger_flush_journal();

	/*
	 * The ledger's records are on disk.  If a replica is following this
	 * node, the commit waits here until the replica has applied them, so
	 * that a failover cannot lose a transfer a client was told about.
	 * The wait is outside the ledger's lock and after the fsync, so it
	 * costs no CPU on the path and no other commit waits for this one.
	 */
	ledger_replica_wait(s->nrecords);

	pending_free_all();

	/* the statuses of this transaction, for ledger_last_statuses() */
	if (last_statuses)
		pfree(last_statuses);
	last_statuses = sts;
	last_nstatuses = (int) nev;
}

static void
ledger_xact_callback(XactEvent event, void *arg)
{
	switch (event)
	{
		case XACT_EVENT_PRE_COMMIT:
			if (pending_head)
				ledger_commit_pending();
			break;
		case XACT_EVENT_PRE_PREPARE:
			if (pending_head)
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("ledger: cannot PREPARE a transaction that queued ledger work")));
			break;
		case XACT_EVENT_COMMIT:
			pending_free_all();
			ledger_tenant_reset();
			break;
		case XACT_EVENT_ABORT:
			pending_free_all();
			ledger_tenant_reset();
			if (last_statuses)
			{
				pfree(last_statuses);
				last_statuses = NULL;
				last_nstatuses = 0;
			}
			break;
		default:
			break;
	}
}

static void
ledger_subxact_callback(SubXactEvent event, SubTransactionId mySubid,
						SubTransactionId parentSubid, void *arg)
{
	PendingOp  *op;
	PendingOp **link;

	switch (event)
	{
		case SUBXACT_EVENT_COMMIT_SUB:
			for (op = pending_head; op; op = op->next)
				if (op->subid == mySubid)
					op->subid = parentSubid;
			break;
		case SUBXACT_EVENT_ABORT_SUB:
			link = &pending_head;
			pending_tail = NULL;
			while (*link)
			{
				op = *link;
				if (op->subid == mySubid)
				{
					*link = op->next;
					pfree(op);
				}
				else
				{
					pending_tail = op;
					link = &op->next;
				}
			}
			break;
		default:
			break;
	}
}
