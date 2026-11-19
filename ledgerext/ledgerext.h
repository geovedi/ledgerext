/*
 * ledgerext.h - shared declarations for the ledgerext extension.
 *
 * ledgerext is a double-entry ledger inside PostgreSQL.
 * The extension is split into:
 *   ledger_state.c - the grid (accounts, transfers) and the state machine
 *   ledger_wal.c   - WAL records, replay, snapshots, catch-up
 *   ledgerext.c    - module init, shared memory, SQL surface
 */
#ifndef LEDGEREXT_H
#define LEDGEREXT_H

#include "postgres.h"

#include "access/xlog.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/uuid.h"

/* ------------------------------------------------------------------ */
/* the amount                                                           */

/*
 * The quantity of money: 128-bit unsigned, as a transfer's amount and an
 * account's counters are.  PostgreSQL has no
 * 128-bit integer type, so the type is the compiler's, coerced to
 * MAXALIGN(8) the way PostgreSQL's own uint128 in c.h is — so an
 * 8-aligned palloc'ed buffer that holds these structs is enough, and no
 * buffer needs special care.  The SQL surface reports the amounts as
 * numeric, which holds every value of the range exactly.
 */
typedef uint128 ledger_amount;		/* c.h: pg_attribute_aligned(MAXALIGN) */

/* the largest amount the range holds: 2^128-1 */
#define LEDGER_AMOUNT_MAX	((ledger_amount) ~ (ledger_amount) 0)

/* add two amounts; true when the sum does not fit in the range */
static inline bool
ledger_amount_add(ledger_amount a, ledger_amount b, ledger_amount *out)
{
	return __builtin_add_overflow(a, b, out);
}

/* subtract; true when the result would be below zero */
static inline bool
ledger_amount_sub(ledger_amount a, ledger_amount b, ledger_amount *out)
{
	return __builtin_sub_overflow(a, b, out);
}

/*
 * The saturating difference the balancing transfer's cap uses:
 * credits_posted -| (debits_posted + debits_pending), stopping at zero
 * instead of wrapping.
 */
static inline ledger_amount
ledger_amount_sat_sub(ledger_amount a, ledger_amount b)
{
	ledger_amount r;

	if (ledger_amount_sub(a, b, &r))
		return 0;
	return r;
}

/* the 16 bytes of the wire format: the low half first */
static inline void
ledger_amount_to_bytes(ledger_amount a, unsigned char *out)
{
	uint64		lo = (uint64) a;
	uint64		hi = (uint64) (a >> 64);

	memcpy(out, &lo, 8);
	memcpy(out + 8, &hi, 8);
}

/* the decimal digits, most significant first; returns the number of them */
static inline int
ledger_amount_to_text(ledger_amount a, char *out, size_t n)
{
	char		buf[40];
	int			i = 0,
				j;

	if (a == 0)
	{
		if (n < 2)
			return -1;
		out[0] = '0';
		out[1] = '\0';
		return 1;
	}
	while (a > 0)
	{
		buf[i++] = (char) ('0' + (int) (a % 10));
		a /= 10;
	}
	if ((size_t) i + 1 > n)
		return -1;
	for (j = 0; j < i; j++)
		out[j] = buf[i - 1 - j];
	out[i] = '\0';
	return i;
}

/* the amount of a decimal string; false when it does not fit or is malformed */
static inline bool
ledger_amount_from_text(const char *s, ledger_amount *out)
{
	ledger_amount v = 0;

	if (*s == '\0')
		return false;
	for (const char *p = s; *p; p++)
	{
		if (*p < '0' || *p > '9')
			return false;
		if (__builtin_mul_overflow(v, (ledger_amount) 10, &v))
			return false;
		if (__builtin_add_overflow(v, (ledger_amount) (*p - '0'), &v))
			return false;
	}
	*out = v;
	return true;
}

/* ------------------------------------------------------------------ */
/* constants                                                           */

#define LEDGER_RMGR_ID		RM_EXPERIMENTAL_ID
#define LEDGER_RMGR_NAME	"ledgerext"

#define LEDGER_TRANCHE		"ledgerext"
#define LEDGER_SLOT_NAME	"ledgerext"
/* the id 2^128-1 is reserved, and a zero id is not an id */
static inline bool
is_zero_id(const unsigned char *id)
{
	int			i;

	for (i = 0; i < 16; i++)
		if (id[i])
			return false;
	return true;
}

#define LEDGER_SNAP_FILE	"ledgerext.snap"
#define LEDGER_JOURNAL_FILE	"ledgerext.journal"
#define LEDGER_SNAP_MAGIC	0x4C45444Du		/* "LEDM" */
/*
 * Both files carry a version, and a directory written by another version
 * is refused rather than read.  A version moves when the meaning of the
 * bytes moves without their size moving too - the tenant rides in the
 * journal record's flags' high half and in the grid rows' padding, so a
 * directory that predates the tenant would otherwise be read as one and
 * every account in it would be rebuilt in no book at all.  The journal's
 * version is what refuses an older directory; the snapshot's moves only
 * when SnapHeader, LedgerSlot or LedgerTransfer moves.
 */
#define LEDGER_SNAP_VERSION	1
#define LEDGER_JOURNAL_MAGIC	0x4C45444Au		/* "LEDJ" */
#define LEDGER_JOURNAL_VERSION	1

/* the account's flags (packed u16) */
#define ACCT_LINKED				0x0001
#define ACCT_DEBITS_MUST_NOT_EXCEED_CREDITS	0x0002
#define ACCT_CREDITS_MUST_NOT_EXCEED_DEBITS	0x0004
#define ACCT_HISTORY			0x0008	/* not supported yet */
#define ACCT_IMPORTED			0x0010	/* not supported yet */
#define ACCT_CLOSED				0x0020
#define ACCT_FLAGS_ALL			0x003f

/* the transfer's flags (packed u16) */
#define TR_LINKED				0x0001
#define TR_PENDING				0x0002
#define TR_POST				0x0004
#define TR_VOID				0x0008
#define TR_BALANCING_DEBIT		0x0010
#define TR_BALANCING_CREDIT		0x0020
#define TR_CLOSING_DEBIT		0x0040
#define TR_CLOSING_CREDIT		0x0080
#define TR_IMPORTED			0x0100	/* not supported yet */
#define TR_FLAGS_ALL			0x01ff

/*
 * The tenant: the book an account belongs to, and the partition that
 * keeps one user's money out of another's.
 *
 * It is 16 bits, and it travels in the space that already exists: the
 * grid's account row and transfer row have a byte or two spare, and the
 * journal's record - 176 bytes with no padding, which cannot grow - has
 * the high half of its flags field free (an account's flags use six bits,
 * a transfer's nine, and the bits in between stay reserved).
 */
#define LEDGER_TENANT_SHIFT		16
#define LEDGER_TENANT_MASK		0xffff0000u
#define LEDGER_TENANT_MAX		65535

/* the book a record belongs to: the high half of its flags */
#define LEDGER_RECORD_TENANT(rec)	((uint16) ((rec)->flags >> LEDGER_TENANT_SHIFT))

/* the transfer's statuses.  "created" is 0xFFFFFFFF, which
 * does not fit in int4, so the statuses are int8. */
#define ST_CREATED				4294967295LL
#define ST_LINKED_EVENT_FAILED		1
#define ST_LINKED_EVENT_CHAIN_OPEN	2
#define ST_RESERVED_FLAG		4
#define ST_ID_MUST_NOT_BE_ZERO		5
#define ST_ID_MUST_NOT_BE_INT_MAX	6
#define ST_FLAGS_ARE_MUTUALLY_EXCLUSIVE	7
#define ST_DEBIT_ACCOUNT_ID_MUST_NOT_BE_ZERO	8
#define ST_DEBIT_ACCOUNT_ID_MUST_NOT_BE_INT_MAX	9
#define ST_CREDIT_ACCOUNT_ID_MUST_NOT_BE_ZERO	10
#define ST_CREDIT_ACCOUNT_ID_MUST_NOT_BE_INT_MAX	11
#define ST_ACCOUNTS_MUST_BE_DIFFERENT	12
#define ST_PENDING_ID_MUST_BE_ZERO	13
#define ST_PENDING_ID_MUST_NOT_BE_ZERO	14
#define ST_PENDING_ID_MUST_NOT_BE_INT_MAX	15
#define ST_PENDING_ID_MUST_BE_DIFFERENT	16
#define ST_TIMEOUT_RESERVED_FOR_PENDING	17
#define ST_LEDGER_MUST_NOT_BE_ZERO	19
#define ST_CODE_MUST_NOT_BE_ZERO	20
#define ST_DEBIT_ACCOUNT_NOT_FOUND	21
#define ST_CREDIT_ACCOUNT_NOT_FOUND	22
#define ST_ACCOUNTS_MUST_HAVE_SAME_LEDGER	23
#define ST_TRANSFER_MUST_HAVE_SAME_LEDGER	24
#define ST_PENDING_TRANSFER_NOT_FOUND	25
#define ST_PENDING_TRANSFER_NOT_PENDING	26
#define ST_PENDING_DIFFERENT_DEBIT_ACCOUNT	27
#define ST_PENDING_DIFFERENT_CREDIT_ACCOUNT	28
#define ST_PENDING_DIFFERENT_LEDGER	29
#define ST_PENDING_DIFFERENT_CODE	30
#define ST_EXCEEDS_PENDING_AMOUNT	31
#define ST_PENDING_DIFFERENT_AMOUNT	32
#define ST_PENDING_ALREADY_POSTED	33
#define ST_PENDING_ALREADY_VOIDED	34
#define ST_PENDING_TRANSFER_EXPIRED	35
#define ST_EXISTS_WITH_DIFFERENT_FLAGS	36
#define ST_EXISTS_WITH_DIFFERENT_DEBIT_ACCOUNT	37
#define ST_EXISTS_WITH_DIFFERENT_CREDIT_ACCOUNT	38
#define ST_EXISTS_WITH_DIFFERENT_AMOUNT	39
#define ST_EXISTS_WITH_DIFFERENT_PENDING_ID	40
#define ST_EXISTS_WITH_DIFFERENT_USER_DATA	41
#define ST_EXISTS_WITH_DIFFERENT_TIMEOUT	44
#define ST_EXISTS_WITH_DIFFERENT_CODE	45
#define ST_EXISTS			46
#define ST_OVERFLOWS_DEBITS_PENDING	47
#define ST_OVERFLOWS_CREDITS_PENDING	48
#define ST_OVERFLOWS_DEBITS_POSTED	49
#define ST_OVERFLOWS_CREDITS_POSTED	50
#define ST_OVERFLOWS_DEBITS		51
#define ST_OVERFLOWS_CREDITS		52
#define ST_OVERFLOWS_TIMEOUT		53
#define ST_EXCEEDS_CREDITS		54
#define ST_EXCEEDS_DEBITS		55
#define ST_CLOSING_TRANSFER_MUST_BE_PENDING	64
#define ST_DEBIT_ACCOUNT_CLOSED		65
#define ST_CREDIT_ACCOUNT_CLOSED	66
#define ST_EXISTS_WITH_DIFFERENT_LEDGER	67
/* extensions beyond the standard list */
#define ST_NO_TRANSFER_SLOTS		101
#define ST_ACCOUNT_IN_ANOTHER_TENANT	102
#define ST_ID_IN_ANOTHER_TENANT	103

/* the account's statuses */
#define AST_CREATED			4294967295LL	/* the same value as ST_CREATED */
#define AST_LINKED_EVENT_FAILED		1
#define AST_LINKED_EVENT_CHAIN_OPEN	2
#define AST_RESERVED_FIELD		4
#define AST_RESERVED_FLAG		5
#define AST_ID_MUST_NOT_BE_ZERO		6
#define AST_ID_MUST_NOT_BE_INT_MAX	7
#define AST_FLAGS_ARE_MUTUALLY_EXCLUSIVE	8
#define AST_LEDGER_MUST_NOT_BE_ZERO	13
#define AST_CODE_MUST_NOT_BE_ZERO	14
#define AST_EXISTS_WITH_DIFFERENT_FLAGS	15
#define AST_EXISTS_WITH_DIFFERENT_USER_DATA	16
#define AST_EXISTS_WITH_DIFFERENT_LEDGER	19
#define AST_EXISTS_WITH_DIFFERENT_CODE	20
#define AST_EXISTS			21
#define AST_NO_ACCOUNT_SLOTS		100
#define AST_ACCOUNT_IN_ANOTHER_TENANT	102

/* the state of a transfer that was created pending */
#define TSTATE_APPLIED	0
#define TSTATE_PENDING	1
#define TSTATE_POSTED	2
#define TSTATE_VOIDED	3
#define TSTATE_EXPIRED	4

/* the status of a reset, which is not a ledger event */
#define LEDGER_STATUS_OK	0

/* at most this many events in one WAL record */
#define WAL_CHUNK	1024

/* ------------------------------------------------------------------ */
/* wire formats (also used for WAL)                                    */

/*
 * One transfer.  The client fills in the ids, amount, flags, code, ledger,
 * timeout and user_data; the server fills in amount_actual, the resolved
 * account ids, the ledger and code actually used, and the timestamp.
 */
typedef struct PackedTransfer
{
	unsigned char id[16];
	unsigned char debit[16];
	unsigned char credit[16];
	unsigned char pending_id[16];
	unsigned char user_data[16];
	ledger_amount amount;
	ledger_amount amount_actual;
	uint64		timestamp;
	uint32		flags;
	uint32		code;
	uint32		ledger;
	uint32		timeout;
	uint64		pad;				/* the event is 144 bytes by construction */
} PackedTransfer;

/* One account. */
typedef struct PackedAccount
{
	unsigned char id[16];
	unsigned char user_data[16];
	uint64		timestamp;
	uint32		flags;
	uint32		code;
	uint32		ledger;
	uint32		pad;
} PackedAccount;

/* the header of a batch: a count, then that many entries */
typedef struct PackedBatch
{
	uint32		count;
	uint32		kind;			/* 0 = transfers, 1 = accounts */
	uint32		pad[2];
} PackedBatch;

#define LEDGER_KIND_TRANSFERS	0
#define LEDGER_KIND_ACCOUNTS	1

/* ------------------------------------------------------------------ */
/* the journal record                                                  */

/*
 * One committed fact: a transfer, an account creation, or the close of a
 * period.  The record is fixed size (176 bytes), so the journal file is a
 * plain array and the position of a record is its sequence number.
 *
 * The record is also the WAL payload, byte for byte.  Every field of it -
 * the sequence number, the chain links and the period included - is final
 * BEFORE the WAL record is inserted, so that the journal can be repaired
 * from the WAL by copying.  The writer holds the exclusive ledger lock
 * across the stamping and the insert, so the counter and the chain heads
 * cannot move in between.
 */
typedef struct JournalRecord
{
	unsigned char id[16];			/* the transfer's id, or the account's */
	unsigned char pending_id[16];	/* the pending transfer a post/void resolves */
	unsigned char debit[16];		/* the resolved debit account */
	unsigned char credit[16];		/* the resolved credit account */
	unsigned char user_data[16];
	ledger_amount amount;			/* the amount the event asked for */
	ledger_amount amount_actual;	/* the amount that moved */
	uint64		timestamp;			/* the commit clock, monotonic */
	uint64		seq;				/* the position in the journal, from 1 */
	uint64		prev_debit_seq;		/* the previous record naming the debit account */
	uint64		prev_credit_seq;	/* the previous record naming the credit account */
	uint32		kind;				/* JOURNAL_TRANSFER / ACCOUNT / CLOSE */
	uint32		flags;				/* the event's flags, as committed,
									   with the book in the high half */
	uint32		pending_flags;		/* the pending transfer's flags */
	uint32		code;
	uint32		ledger;
	uint32		period;				/* the period the event belongs to */
	uint32		state;				/* the state the event was committed in */
	uint32		timeout;			/* the pending timeout, kept for idempotency */
} JournalRecord;

/*
 * The record's size is the disk's cost of a transfer, and a record is read
 * at a fixed position, so a size that changed is a version that changed.
 */
StaticAssertDecl(sizeof(JournalRecord) == 176, "the journal's record is 176 bytes");

/*
 * A record belongs to the book in the high half of its flags.  A record
 * with no book is the cluster's own - the close of a period - and every
 * book sees it.
 */
static inline bool
ledger_record_visible(const JournalRecord *rec, uint16 tenant)
{
	uint16		book = LEDGER_RECORD_TENANT(rec);

	return book == tenant || book == 0;
}

/*
 * The record does not carry the reservation a post or a void released.
 * The reservation is always the pending transfer's own amount, and the
 * pending transfer's own record says so: a reader that has seen that
 * record knows what was reserved.  The record is 176 bytes, which is the
 * disk's cost of a transfer.
 */

#define JOURNAL_TRANSFER	0
#define JOURNAL_ACCOUNT		1
#define JOURNAL_CLOSE		2
#define JOURNAL_EXPIRE		3

/*
 * What the stamp needs to know about a draft.
 *
 * The apply resolves the accounts and the transfer it touches, and the
 * stamp needs those resolutions again: the row that names the accounts,
 * the position in the journal, and the pending transfer a post or a void
 * resolved.  Looking them up a second time would cost a cold miss each,
 * because the stamp runs after the whole batch has been applied.  So the
 * apply hands them over instead.
 */
typedef struct DraftRef
{
	int32		row;			/* the transfer's own row, or -1 */
	int32		pending_row;	/* the pending transfer a post or void resolved, or -1 */
} DraftRef;

/* the header of the journal file */
typedef struct JournalHeader
{
	uint32		magic;
	uint32		version;
	uint32		recsize;
	uint32		pad;
	uint64		nrecords;
	uint64		pad2[2];
	/*
	 * The header fills a whole page, so that the records start at a
	 * page boundary.  A record is 176 bytes, so a write is page-aligned
	 * only when it starts at a multiple of 256 records: the header
	 * aligns the file's start and the whole-chunk writes that begin at
	 * one, not every commit's.
	 */
	uint8		fill[4096 - 40];
} JournalHeader;

StaticAssertDecl(sizeof(JournalHeader) == 4096, "the journal's header is a page");

/* the hashes of an event's three ids, so the lookups can be prefetched */
typedef struct EvHash
{
	uint32		t;				/* the transfer's own id */
	uint32		d;				/* the debit account's id */
	uint32		c;				/* the credit account's id */
} EvHash;

/* ------------------------------------------------------------------ */
/* shared memory                                                       */

typedef struct LedgerSlot
{
	/*
	 * The layout is two cache lines and no more, because a transfer
	 * touches two accounts and the lines it misses are what a transfer
	 * costs.  The four counters fill the first line; the second holds
	 * everything else a transfer reads - the id, the head of the chain,
	 * the flags, the code and the ledger - plus the two fields only the
	 * creation path writes, because a line is loaded whole anyway.
	 *
	 * The struct is 128 bytes on purpose: a power of two keeps every
	 * slot on the same offset inside its cache line, so the two lines a
	 * slot occupies are always the same two.
	 */
	ledger_amount debits_pending;
	ledger_amount credits_pending;
	ledger_amount debits_posted;
	ledger_amount credits_posted;
	pg_uuid_t	id;
	uint64		last_seq;			/* the head of the account's chain */
	uint32		flags;
	uint32		code;
	uint32		ledger;
	uint16		tenant;			/* the book this account belongs to */
	char		pad1[1];
	bool		deleted;		/* a chain rollback hid this row */
	uint64		timestamp;
	unsigned char user_data[16];
} LedgerSlot;

/*
 * The row is two cache lines and no more, and the tenant sits in the
 * bytes that were already padding: a byte here and a byte there, with
 * the flags' high half in the record.  Nothing grows, because the grid's
 * memory is the wall and the host's huge page pool is finite.
 */
StaticAssertDecl(sizeof(LedgerSlot) == 128, "the account row is two cache lines");

typedef struct LedgerTransfer
{
	unsigned char id[16];
	unsigned char pending_id[16];
	unsigned char user_data[16];
	ledger_amount amount;		/* the amount that moved */
	uint64		timestamp;
	uint64		seq;				/* this transfer's own journal record */
	uint64		post_seq;			/* the record that posted or voided it */
	uint32		debit_slot;
	uint32		credit_slot;
	uint32		flags;
	uint32		code;
	uint32		ledger;
	uint32		timeout;
	uint16		state;			/* TSTATE_* */
	bool		deleted;		/* a chain rollback hid this row */
	uint16		tenant;			/* the book this transfer belongs to */
} LedgerTransfer;

StaticAssertDecl(sizeof(LedgerTransfer) == 120, "the transfer row is 120 bytes");

/* how many replicas a primary can track at once */
#define LEDGER_MAX_REPLICAS	8

/*
 * A replica of this ledger: the position it has applied and made durable,
 * and when it was last heard from.  The position is the whole protocol -
 * "give me the records after N", "I have M" - and it only moves forward.
 */
typedef struct LedgerReplica
{
	char		name[64];
	uint64		ack_seq;			/* the last position it has applied */
	uint64		last_seen;			/* the clock when it last spoke */
	bool		used;
} LedgerReplica;

typedef struct LedgerState
{
	size_t		max_accounts;
	size_t		max_transfers;
	size_t		nslots;
	size_t		ntransfers;
	uint64		nrecords;			/* the journal's record count */
	uint64		applied_seq;		/* the journal's records up to here are
								 * reflected in the grid */
	int			journal_fd;		/* the journal's file descriptor */
	uint64		period;				/* the period the ledger is in */
	uint64		last_close_seq;		/* the record that closed the last period */
	uint32		amask;			/* account bucket mask */
	uint32		tmask;			/* transfer bucket mask */
	uint32		nexpiry;			/* how many entries the expiry heap holds */
	bool		broken;			/* the grid and the journal may disagree */
	uint64		gen;			/* bumped on every change */
	uint64		clock;			/* cluster-assigned timestamps */
	uint64		snap_gen;		/* gen captured by the body on disk */
	uint64		snap_body_seq;	/* the journal's records the body covers, 0 = none */
	LedgerSlot *slots;
	uint32	   *abuckets;
	LedgerTransfer *transfers;
	uint32	   *tbuckets;
	uint32	   *expiry;			/* the heap: the indexes of the transfers */
	LedgerReplica replicas[LEDGER_MAX_REPLICAS];
} LedgerState;

/*
 * When a pending transfer expires.  The timeout is in seconds and the
 * timestamp is the commit clock, so both are in the ledger's own time.
 */
static inline uint64
ledger_expires_at(const LedgerTransfer *p)
{
	return p->timestamp + (uint64) p->timeout * 1000000000ULL;
}

/*
 * Is this a pending transfer that asked for a timeout?  Only those can
 * expire, and only those are in the expiry heap.
 */
static inline bool
ledger_is_timed_pending(const LedgerTransfer *p)
{
	return !p->deleted && p->state == TSTATE_PENDING && p->timeout > 0;
}

/*
 * The commit clock is real time, and it never moves backwards: a step of
 * the system clock leaves the ledger's time where it was.
 */
static inline void
ledger_advance_clock(LedgerState *s, uint64 now)
{
	if (now > s->clock)
		s->clock = now;
}

extern LedgerState *ledger_state;
extern LWLock *ledger_lock;
extern LWLock *ledger_snap_lock;
extern bool ledger_shmem_ready;

/* ------------------------------------------------------------------ */
/* undo of a chain of linked events                                    */

/*
 * A linked chain either succeeds or fails as a unit.  To roll a chain back
 * we remember what each event changed.
 */
#define UNDO_ACCOUNT	1		/* an account's counters and flags */
#define UNDO_TRANSFER	2		/* a transfer row was inserted */
#define UNDO_PENDING	3		/* a pending transfer's state changed */
#define UNDO_ACCOUNT_INSERT 4	/* an account row was inserted */

typedef struct UndoEntry
{
	ledger_amount debits_pending;
	ledger_amount credits_pending;
	ledger_amount debits_posted;
	ledger_amount credits_posted;
	uint32		idx;
	uint32		flags;
	uint16		state;
	uint8		kind;
} UndoEntry;

typedef struct LedgerScope
{
	UndoEntry  *entries;
	int			n;
	int			cap;
} LedgerScope;

extern void scope_init(LedgerScope *scope);
extern void scope_free(LedgerScope *scope);
extern void scope_save_account(LedgerScope *scope, LedgerState *s, uint32 slot);
extern void scope_save_account_insert(LedgerScope *scope, uint32 slot);
extern void scope_save_transfer(LedgerScope *scope, uint32 idx);
extern void scope_save_pending(LedgerScope *scope, uint32 idx, uint16 state);
extern void scope_rollback(LedgerScope *scope, LedgerState *s, int mark);

/* ------------------------------------------------------------------ */
/* the state machine (ledger_state.c)                                  */

extern uint32 ledger_hash(const unsigned char *id);
extern void *ledger_scratch_get(void **buf, size_t *cap, size_t need, size_t elem);
extern int64 ledger_find_account(LedgerState *s, const unsigned char *id);
extern int64 ledger_find_transfer(LedgerState *s, const unsigned char *id);
extern uint32 ledger_insert_account(LedgerState *s, const PackedAccount *a,
									uint32 h, uint16 tenant);
extern uint32 ledger_insert_transfer(LedgerState *s, const PackedTransfer *t,
									 uint32 debit_slot, uint32 credit_slot,
									 uint16 state, uint32 h, uint16 tenant);
extern void ledger_rebuild_indexes(LedgerState *s);

/*
 * Apply a batch of events.  Every event gets its own status: a failing event
 * does not fail the batch, but events chained
 * with the LINKED flag succeed or fail as a unit.  The events[] array is
 * updated in place with the values the server resolved.
 */
extern struct EvHash *ledger_batch_hashes(PackedTransfer *events, int n);
extern int ledger_apply_transfers(LedgerState *s, PackedTransfer *events, int n,
								  int64 *statuses, JournalRecord *recs,
								  DraftRef *refs, EvHash *hashes,
								  uint16 tenant);
extern int ledger_apply_accounts(LedgerState *s, PackedAccount *events, int n,
								 int64 *statuses, JournalRecord *recs,
								 uint16 tenant);

/*
 * The tenant: the book a role owns.  It is resolved once per transaction
 * from the binding table and held until the transaction ends, so the
 * commit path never reads a table.
 *
 * Resolve it BEFORE taking the ledger's lock: the resolution reads the
 * table, and the lock is the ledger's own.
 */
extern uint16 ledger_tenant_current(void);
extern void ledger_tenant_reset(void);

/* the role must own a book: the resolution raises if it does not */
extern void ledger_tenant_require(void);

/*
 * Release a pending transfer's reservation: the money goes back to the
 * accounts it was held from, and a closing transfer's account is reopened.
 * The void path, the expiry round and the WAL replay all call it, so the
 * three cannot drift apart.
 */
extern void ledger_release_pending(LedgerState *s, const LedgerTransfer *p,
								   bool reopen);

/*
 * The records of a batch: they are stamped, written to the WAL and
 * appended to the journal.  A client's commit and the expiry round both
 * use them.
 */
extern void ledger_stamp_records(LedgerState *s, JournalRecord *recs,
								 DraftRef *refs, int n);
extern void ledger_write_records(LedgerState *s, const JournalRecord *recs, int n);
extern void ledger_flush_journal(void);

/* ------------------------------------------------------------------ */
/* the expiry (ledger_expiry.c)                                        */

extern uint64 ledger_now_ns(void);
extern void ledger_expiry_push(LedgerState *s, uint32 idx);
extern void ledger_expiry_arm(LedgerState *s, uint32 idx);
extern uint64 ledger_expiry_earliest(LedgerState *s);
extern void ledger_expiry_rebuild(LedgerState *s);
extern int ledger_apply_expiries(LedgerState *s, JournalRecord *recs,
								  DraftRef *refs, int max, uint64 now);
PGDLLEXPORT void ledger_expiry_worker(Datum arg);

/* ------------------------------------------------------------------ */
/* WAL and snapshots (ledger_wal.c)                                    */

/* the replication (ledger_replica.c) */
extern void ledger_replica_worker(Datum arg);
extern void ledger_replica_wait(uint64 seq);
extern void ledger_replica_ack(const char *name, uint64 seq);
extern bool ledger_is_replica(void);

extern void ledger_register_rmgr(void);
extern void ledger_replay_records(const JournalRecord *recs, int n);
extern uint64 ledger_do_snapshot(bool force);
extern void ledger_load_snapshot(void);
PGDLLEXPORT void ledger_snapshot_worker(Datum arg);
/* ------------------------------------------------------------------ */
/* the queued work and the module (ledgerext.c)                        */

extern int	ledger_max_accounts;
extern int	ledger_max_transfers;
extern int	ledger_snapshot_interval_ms;
extern int	ledger_snapshot_journal_bytes;
extern int	ledger_snapshot_body_multiple;
extern int	ledger_expiry_interval_ms;
extern int	ledger_replica_poll_ms;
extern bool ledger_replica_sync;
extern char *ledger_replica_of;

extern void ledger_queue_accounts(uint32 count, const char *payload);
extern void ledger_queue_transfers(uint32 count, const char *payload);
extern void ledger_queue_close(void);
extern int ledger_queued_closes(void);
extern void ledger_get_last_statuses(int64 **statuses, int *n);

/* ------------------------------------------------------------------ */
/* the journal (ledger_journal.c)                                      */

/* the file */
extern void ledger_journal_open(void);
extern int ledger_journal_records_bytes(LedgerState *s, uint64 from_seq, int lim,
										uint8 *out);
extern void ledger_journal_append(const JournalRecord *recs, int n);
extern void ledger_journal_fsync(void);
extern bool ledger_journal_read(uint64 seq, JournalRecord *out);
extern int ledger_journal_read_block(uint64 seq, JournalRecord *buf, int max);
extern uint64 ledger_journal_nrecords(void);
/* the position of the first record at or after the given timestamp */
extern uint64 ledger_journal_seek(uint64 timestamp);

/* the queries */
extern int ledger_journal_by_account(LedgerState *s, const unsigned char *id,
									 uint16 tenant, uint64 **seqs, int *n);
extern int ledger_journal_by_ledger(LedgerState *s, uint32 ledger,
									uint16 tenant, uint64 **seqs, int *n);

/* the replay */
/* the balances of the replay: an account and its four counters */
typedef struct BalanceRow
{
	pg_uuid_t	id;
	ledger_amount dp,
				cp,
				d,
				c;
} BalanceRow;

extern BalanceRow *ledger_journal_replay(LedgerState *s, uint64 upto,
										  uint16 tenant, int *nrows);

#endif							/* LEDGEREXT_H */
