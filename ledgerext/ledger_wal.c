/*
 * ledger_wal.c - recovery and snapshots.
 *
 * The grid lives in shared memory, which PostgreSQL throws away on every
 * restart, so the ledger keeps its own copy of the truth: the journal, an
 * append-only file of fixed-size records in $PGDATA.  The journal is the
 * ledger's log - it holds every committed event, it is made durable before
 * the transaction that wrote it is acknowledged, and it needs neither a
 * repair nor a copy anywhere else.
 *
 * The grid is a cache of the journal:
 *
 *   1. every applied batch is appended to the journal,
 *   2. a background worker periodically writes the grid into
 *      $PGDATA/ledgerext.snap, tagged with the journal position it covers,
 *   3. at startup the snapshot is loaded and the journal's records after
 *      that position are replayed into the grid.
 *
 * The snapshot is only a cache: it makes the replay short, and the journal
 * alone is enough to rebuild the grid.
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>

#include "access/rmgr.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogutils.h"
#include "common/file_perm.h"
#include "lib/stringinfo.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "replication/slot.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/spin.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#include "ledgerext.h"

/* the op code 0.7 used for a batch of records; 0.8 writes none */
#define XLOG_LEDGER_RECORDS	0x00

static void ledger_redo(XLogReaderState *record);
static void ledger_desc(StringInfo buf, XLogReaderState *record);
static const char *ledger_identify(uint8 info);

static const RmgrData ledger_rmgr = {
	.rm_name = LEDGER_RMGR_NAME,
	.rm_redo = ledger_redo,
	.rm_desc = ledger_desc,
	.rm_identify = ledger_identify
};

/* ------------------------------------------------------------------ */
/* the resource manager                                                */

void
ledger_register_rmgr(void)
{
	RegisterCustomRmgr(LEDGER_RMGR_ID, &ledger_rmgr);
}

/*
 * The resource manager stays registered, because the extension owns the id
 * and PostgreSQL refuses a log that names an id it does not know.  But the
 * ledger writes nothing to PostgreSQL's log: the journal is the log, so
 * there is no record here for the redo to apply.
 *
 * A log that holds one of our records was written by 0.7 or earlier.  The
 * journal of such a data directory is refused before the log is read, and
 * if the journal was removed anyway the records are ignored: a ledger that
 * starts empty is wrong, but a ledger that applies records it does not
 * have is worse.
 */
static void
ledger_redo(XLogReaderState *record)
{
	static bool warned = false;

	if (!warned)
	{
		warned = true;
		ereport(WARNING,
				(errmsg("ledgerext: ignoring a ledger record in PostgreSQL's log"),
				 errhint("The ledger's records live in $PGDATA/ledgerext.journal now. "
						 "A log that holds one was written by an older version of "
						 "ledgerext.")));
	}
}

static void
ledger_desc(StringInfo buf, XLogReaderState *record)
{
	appendStringInfo(buf, "journal records (ignored)");
}

static const char *
ledger_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_LEDGER_RECORDS)
		return "LEDGER_RECORDS";
	return NULL;
}

/* ------------------------------------------------------------------ */
/* replay: apply a record's effect, without validating it again        */

/*
 * A record carries the amount that actually moved, the reservation a post
 * or void released, and the flags that were in force, so applying it needs
 * nothing but the record.
 */
void
ledger_replay_records(const JournalRecord *recs, int n)
{
	LedgerState *s = ledger_state;
	int			i;

	for (i = 0; i < n; i++)
	{
		const JournalRecord *r = &recs[i];
		int64		ds,
					cs,
					pidx = -1,
					tidx;

		if (r->kind == JOURNAL_CLOSE)
		{
			/*
			 * A close that has been applied already is not applied
			 * again: the replay has to be idempotent, because a
			 * replica that fetched a record twice must not open two
			 * periods for it.
			 */
			if (r->seq == s->last_close_seq)
				continue;
			s->period++;
			s->last_close_seq = r->seq;
			if (r->timestamp >= s->clock)
				s->clock = r->timestamp + 1;
			s->gen++;
			continue;
		}

		if (r->kind == JOURNAL_ACCOUNT)
		{
			PackedAccount a;

			if (ledger_find_account(s, r->id) >= 0)
				continue;
			if (s->nslots >= s->max_accounts)
				ereport(FATAL,
						(errmsg("ledgerext: the grid's account table is full "
							 "(ledgerext.max_accounts = %zu) while the journal "
							 "\"%s\" is being replayed: the journal holds more "
							 "accounts than this node's grid can hold",
							 s->max_accounts, LEDGER_JOURNAL_FILE),
					  errdetail("The journal holds " UINT64_FORMAT " records. The "
								"ledger's data is not lost: it is in the journal.",
								s->nrecords),
					  errhint("Raise ledgerext.max_accounts above %zu and give the "
							  "node the memory for it, or restore the ledger on a "
							  "node with a larger grid.",
							  s->max_accounts)));
			memset(&a, 0, sizeof(a));
			memcpy(a.id, r->id, 16);
			memcpy(a.user_data, r->user_data, 16);
			/*
			 * The record's flags carry the tenant in their high half: the
			 * event's own flags are the low half, and the account goes
			 * back into the book it was created in.
			 */
			a.flags = r->flags & ~LEDGER_TENANT_MASK;
			a.code = r->code;
			a.ledger = r->ledger;
			a.timestamp = r->timestamp;
			ledger_insert_account(s, &a, ledger_hash(a.id),
								  LEDGER_RECORD_TENANT(r));
			if (r->timestamp >= s->clock)
				s->clock = r->timestamp + 1;
			s->gen++;
			continue;
		}

		if (r->kind == JOURNAL_EXPIRE)
		{
			/*
			 * The record of a release.  It names the expired transfer,
			 * so the check below would take it for a transfer that is
			 * already applied and drop the release: it has to come
			 * first.
			 */
			pidx = ledger_find_transfer(s, r->pending_id);
			if (pidx < 0)
				elog(PANIC, "ledgerext: the expiry of a missing pending transfer during replay");
			ds = ledger_find_account(s, r->debit);
			cs = ledger_find_account(s, r->credit);
			if (ds < 0 || cs < 0)
				elog(PANIC, "ledgerext: an expiry references a missing account during replay");
			/* the reservation goes back, and only once */
			if (s->transfers[pidx].state == TSTATE_PENDING)
			{
				ledger_release_pending(s, &s->transfers[pidx], true);
				s->transfers[pidx].state = TSTATE_EXPIRED;
				s->transfers[pidx].post_seq = r->seq;
			}
			s->slots[ds].last_seq = r->seq;
			s->slots[cs].last_seq = r->seq;
			if (r->timestamp >= s->clock)
				s->clock = r->timestamp + 1;
			s->gen++;
			continue;
		}

		/* a transfer */
		if (ledger_find_transfer(s, r->id) >= 0)
			continue;			/* already applied */

		ds = ledger_find_account(s, r->debit);
		cs = ledger_find_account(s, r->credit);
		if (ds < 0 || cs < 0)
			elog(PANIC, "ledgerext: transfer references a missing account during replay");
		if (s->ntransfers >= s->max_transfers)
			ereport(FATAL,
					(errmsg("ledgerext: the grid's transfer table is full "
						 "(ledgerext.max_transfers = %zu) while the journal "
						 "\"%s\" is being replayed: the journal holds more "
						 "transfers than this node's grid can hold",
						 s->max_transfers, LEDGER_JOURNAL_FILE),
				  errdetail("The journal holds " UINT64_FORMAT " records. The "
							"ledger's data is not lost: it is in the journal.",
							s->nrecords),
				  errhint("Raise ledgerext.max_transfers above %zu and give the "
						  "node the memory for it, or restore the ledger on a "
						  "node with a larger grid.",
						  s->max_transfers)));
		if (r->flags & (TR_POST | TR_VOID))
			pidx = ledger_find_transfer(s, r->pending_id);

		if (r->flags & (TR_POST | TR_VOID))
		{
			bool		is_void = (r->flags & TR_VOID) != 0;

			if (pidx < 0)
				elog(PANIC, "ledgerext: post/void of a missing pending transfer during replay");
			/* release the reservation the pending transfer holds */
			ledger_release_pending(s, &s->transfers[pidx], is_void);
			if (is_void)
				s->transfers[pidx].state = TSTATE_VOIDED;
			else
			{
				s->slots[ds].debits_posted += r->amount_actual;
				s->slots[cs].credits_posted += r->amount_actual;
				s->transfers[pidx].state = TSTATE_POSTED;
			}
		}
		else if (r->flags & TR_PENDING)
		{
			s->slots[ds].debits_pending += r->amount_actual;
			s->slots[cs].credits_pending += r->amount_actual;
		}
		else
		{
			s->slots[ds].debits_posted += r->amount_actual;
			s->slots[cs].credits_posted += r->amount_actual;
		}

		/* a closing transfer closes its account */
		if (r->flags & TR_CLOSING_DEBIT)
			s->slots[ds].flags |= ACCT_CLOSED;
		if (r->flags & TR_CLOSING_CREDIT)
			s->slots[cs].flags |= ACCT_CLOSED;

		{
			PackedTransfer t;

			memset(&t, 0, sizeof(t));
			memcpy(t.id, r->id, 16);
			memcpy(t.pending_id, r->pending_id, 16);
			memcpy(t.user_data, r->user_data, 16);
			memcpy(t.debit, r->debit, 16);
			memcpy(t.credit, r->credit, 16);
			t.amount = r->amount;
			t.amount_actual = r->amount_actual;
			t.timestamp = r->timestamp;
			t.flags = r->flags & ~LEDGER_TENANT_MASK;
			t.code = r->code;
			t.ledger = r->ledger;
			t.timeout = r->timeout;
			tidx = (int64) ledger_insert_transfer(s, &t, (uint32) ds, (uint32) cs,
													 (r->flags & TR_PENDING) ? TSTATE_PENDING : TSTATE_APPLIED,
												ledger_hash(t.id),
												LEDGER_RECORD_TENANT(r));
		}

		/* a pending transfer with a timeout is waiting to expire */
		ledger_expiry_arm(s, (uint32) tidx);

		/* the journal's positions: the chains and the transfer's own record */
		s->slots[ds].last_seq = r->seq;
		s->slots[cs].last_seq = r->seq;
		s->transfers[tidx].seq = r->seq;
		if (pidx >= 0)
			s->transfers[pidx].post_seq = r->seq;

		if (r->timestamp >= s->clock)
			s->clock = r->timestamp + 1;
		s->gen++;
	}
}

/* ------------------------------------------------------------------ */
/* the replay from the journal                                         */

/*
 * The journal is the log, so the recovery reads it: the snapshot gives the
 * starting point, and the records after it are applied to the grid in
 * order.  It is the same code that applied them when they were committed,
 * so the grid that comes back is the grid that was there.
 */
static void
ledger_journal_replay_tail(uint64 from_seq)
{
	LedgerState *s = ledger_state;
	JournalRecord *buf;
	uint64		seq = from_seq + 1;

	if (seq > s->nrecords)
		return;

	buf = (JournalRecord *) palloc(WAL_CHUNK * sizeof(JournalRecord));
	while (seq <= s->nrecords)
	{
		int			n = ledger_journal_read_block(seq, buf, WAL_CHUNK);
		int			i;

		if (n <= 0)
			break;
		/*
		 * A record's position is its sequence number, so a record that is
		 * not where it says it is means the file is damaged.  The journal
		 * is the ledger's only truth now, so there is nothing to repair it
		 * from: the ledger refuses to start rather than apply a fact that
		 * may not be one.
		 */
		for (i = 0; i < n; i++)
			if (buf[i].seq != seq + (uint64) i)
				ereport(FATAL,
						(errmsg("ledgerext: the journal is damaged: the record at position "
								"%llu claims to be %llu",
								(unsigned long long) (seq + (uint64) i),
								(unsigned long long) buf[i].seq),
						 errhint("The ledger's records are in $PGDATA/ledgerext.journal. "
								 "Remove it to start with an empty ledger.")));
		ledger_replay_records(buf, n);
		seq += (uint64) n;
	}
	pfree(buf);

	s->applied_seq = s->nrecords;
}

/* ------------------------------------------------------------------ */
/* snapshots                                                           */

typedef struct SnapHeader
{
	uint32		magic;
	uint32		version;
	uint64		nslots;
	uint64		ntransfers;
	uint64		max_accounts;
	uint64		max_transfers;
	uint64		clock;
	uint64		nrecords;		/* the journal's records the body covers */
	uint64		period;			/* the period the ledger is in */
	uint64		last_close_seq;	/* the record that closed the last period */
	uint32		body_crc;
	uint32		header_crc;		/* over everything above */
} SnapHeader;

static void
snap_path(char *out, const char *name)
{
	snprintf(out, MAXPGPATH, "%s/%s", DataDir, name);
}

static void
write_all(int fd, const char *buf, size_t len, const char *path)
{
	while (len > 0)
	{
		size_t		chunk = len > (1 << 20) ? (1 << 20) : len;
		ssize_t		w = write(fd, buf, chunk);

		if (w < 0)
		{
			if (errno == EINTR)
				continue;
			ereport(ERROR, (errcode_for_file_access(),
							errmsg("ledgerext: could not write \"%s\": %m", path)));
		}
		buf += w;
		len -= (size_t) w;
	}
}

static bool
read_all(int fd, char *buf, size_t len)
{
	while (len > 0)
	{
		size_t		chunk = len > (1 << 20) ? (1 << 20) : len;
		ssize_t		r = read(fd, buf, chunk);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		buf += r;
		len -= (size_t) r;
	}
	return true;
}

/* Called in the postmaster with AddinShmemInitLock held, before any backend. */
void
ledger_load_snapshot(void)
{
	LedgerState *s = ledger_state;
	char		path[MAXPGPATH];
	SnapHeader	h;
	pg_crc32c	crc;
	int			fd;

	/* the journal exists with or without a snapshot */
	ledger_journal_open();
	s->nrecords = ledger_journal_nrecords();

	snap_path(path, LEDGER_SNAP_FILE);
	fd = OpenTransientFile(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(FATAL, (errcode_for_file_access(),
							errmsg("ledgerext: could not open \"%s\": %m", path)));
		/* no snapshot yet: the journal alone rebuilds the grid */
		ledger_journal_replay_tail(0);
		ledger_expiry_rebuild(s);
		ereport(LOG, (errmsg("ledgerext: no snapshot; replayed %llu journal records",
							 (unsigned long long) s->nrecords)));
		return;
	}

	if (!read_all(fd, (char *) &h, sizeof(h)))
		ereport(FATAL, (errmsg("ledgerext: snapshot header truncated")));
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &h, offsetof(SnapHeader, header_crc));
	FIN_CRC32C(crc);
	if (h.magic != LEDGER_SNAP_MAGIC || crc != h.header_crc)
		ereport(FATAL, (errmsg("ledgerext: snapshot header is invalid")));
	if (h.version != LEDGER_SNAP_VERSION)
		ereport(FATAL,
				(errmsg("ledgerext: the snapshot in \"%s\" was written by another version "
						"of ledgerext (version %u, expected %u)",
						path, h.version, LEDGER_SNAP_VERSION),
				 errhint("Remove the file to start with an empty ledger.")));
	if (h.nslots > s->max_accounts || h.ntransfers > s->max_transfers)
		ereport(FATAL, (errmsg("ledgerext: snapshot holds " UINT64_FORMAT " accounts and "
							   UINT64_FORMAT " transfers; raise ledgerext.max_accounts/max_transfers",
							   h.nslots, h.ntransfers)));

	if (!read_all(fd, (char *) s->slots, h.nslots * sizeof(LedgerSlot)) ||
		!read_all(fd, (char *) s->transfers, h.ntransfers * sizeof(LedgerTransfer)))
		ereport(FATAL, (errmsg("ledgerext: snapshot body truncated")));
	CloseTransientFile(fd);

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, s->slots, h.nslots * sizeof(LedgerSlot));
	COMP_CRC32C(crc, s->transfers, h.ntransfers * sizeof(LedgerTransfer));
	FIN_CRC32C(crc);
	if (crc != h.body_crc)
		ereport(FATAL, (errmsg("ledgerext: snapshot body checksum mismatch")));

	/* the body covers the journal's records up to its position, so the
	 * journal cannot be shorter than that */
	if (h.nrecords > s->nrecords)
		ereport(FATAL, (errmsg("ledgerext: the journal holds " UINT64_FORMAT " records but "
							   "the snapshot covers " UINT64_FORMAT " of them",
							   s->nrecords, h.nrecords)));

	/* rebuild the hash indexes */
	s->nslots = h.nslots;
	s->ntransfers = h.ntransfers;
	ledger_rebuild_indexes(s);
	s->clock = h.clock;
	s->period = h.period;
	s->last_close_seq = h.last_close_seq;
	s->snap_body_seq = h.nrecords;
	s->snap_gen = s->gen;

	/* the journal's records after the body's position are the log's tail */
	ledger_journal_replay_tail(h.nrecords);

	/* and the expiry heap: the pending transfers that asked for a timeout */
	ledger_expiry_rebuild(s);

	ereport(LOG, (errmsg("ledgerext: loaded snapshot (%zu accounts, %zu transfers) covering "
						 "%llu journal records, and replayed the rest",
						 s->nslots, s->ntransfers,
						 (unsigned long long) h.nrecords)));
}

static void
write_file_atomic(const char *name, const void *a, size_t alen, const void *b, size_t blen,
				  const void *c, size_t clen, const void *d, size_t dlen)
{
	char		final[MAXPGPATH];
	char		tmp[MAXPGPATH];
	int			fd;

	snap_path(final, name);
	snprintf(tmp, MAXPGPATH, "%s.tmp", final);
	fd = OpenTransientFilePerm(tmp, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY, pg_file_create_mode);
	if (fd < 0)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("ledgerext: could not create \"%s\": %m", tmp)));
	write_all(fd, a, alen, tmp);
	if (blen)
		write_all(fd, b, blen, tmp);
	if (clen)
		write_all(fd, c, clen, tmp);
	if (dlen)
		write_all(fd, d, dlen, tmp);
	if (pg_fsync(fd) != 0)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("ledgerext: could not fsync \"%s\": %m", tmp)));
	CloseTransientFile(fd);
	durable_rename(tmp, final, ERROR);
}

/*
 * Write a snapshot: the grid, and the journal position it covers.
 *
 * Writing the body costs the size of the whole grid, so the body is only
 * written when enough journal has piled up behind it.  A timer would make
 * every transfer pay for the whole ledger every few seconds, and the more
 * the ledger grows the more it pays; a volume of journal makes the cost of
 * a transfer the same whatever the ledger's size.  Until then the body
 * stays where it is, and the journal's records after it are replayed at the
 * next start.
 *
 * The journal is made durable BEFORE the body is written, so the journal
 * can never lose a record the body covers.
 */
uint64
ledger_do_snapshot(bool force)
{
	LedgerState *s = ledger_state;
	LedgerSlot *sbuf = NULL;
	LedgerTransfer *tbuf = NULL;
	SnapHeader	h;
	pg_crc32c	crc;
	uint64		pos;
	uint64		seq;
	uint64		threshold;
	uint64		bodybytes;
	bool		defer;

	if (RecoveryInProgress())
		ereport(ERROR, (errmsg("ledgerext: cannot snapshot during recovery")));

	LWLockAcquire(ledger_snap_lock, LW_EXCLUSIVE);
	LWLockAcquire(ledger_lock, LW_SHARED);
	/*
	 * Writers hold the lock exclusively across the journal's append, so
	 * every record before this position is applied and none after it is.
	 */
	seq = s->nrecords;
	pos = GetXLogInsertRecPtr();
	/*
	 * The body is the grid, so it grows with the ledger.  A threshold
	 * that counted only the journal would make the snapshot's amortized
	 * cost grow with the ledger too, and the snapshot would cost more
	 * than the journal it saves.  So the threshold is also a multiple of
	 * the body's own size: the snapshot then costs a fixed fraction of
	 * the journal's, whatever the ledger's size is, and the startup
	 * replays at most that multiple of the body.
	 */
	threshold = (uint64) ledger_snapshot_journal_bytes;
	bodybytes = (uint64) s->nslots * sizeof(LedgerSlot) +
		(uint64) s->ntransfers * sizeof(LedgerTransfer);
	if (bodybytes * (uint64) ledger_snapshot_body_multiple > threshold)
		threshold = bodybytes * (uint64) ledger_snapshot_body_multiple;
	if (seq <= s->snap_body_seq)
		defer = !force && s->snap_body_seq != 0;
	else
		defer = !force &&
			(seq - s->snap_body_seq) * sizeof(JournalRecord) < threshold;

	if (!defer)
	{
		memset(&h, 0, sizeof(h));
		h.nslots = s->nslots;
		h.ntransfers = s->ntransfers;
		h.clock = s->clock;
		h.nrecords = seq;
		h.period = s->period;
		h.last_close_seq = s->last_close_seq;
		/* the copies can pass palloc's size limit, hence the huge ones */
		sbuf = (LedgerSlot *) MemoryContextAllocHuge(CurrentMemoryContext,
													 Max(1, h.nslots) * sizeof(LedgerSlot));
		tbuf = (LedgerTransfer *) MemoryContextAllocHuge(CurrentMemoryContext,
														 Max(1, h.ntransfers) * sizeof(LedgerTransfer));
		memcpy(sbuf, s->slots, h.nslots * sizeof(LedgerSlot));
		memcpy(tbuf, s->transfers, h.ntransfers * sizeof(LedgerTransfer));
		s->snap_gen = s->gen;
	}
	LWLockRelease(ledger_lock);

	if (defer)
	{
		LWLockRelease(ledger_snap_lock);
		return pos;
	}

	/*
	 * The journal is made durable before the body is written: the body
	 * covers the records up to `seq`, so they have to be on disk before the
	 * body that claims them exists.
	 */
	ledger_journal_fsync();

	h.magic = LEDGER_SNAP_MAGIC;
	h.version = LEDGER_SNAP_VERSION;
	h.max_accounts = s->max_accounts;
	h.max_transfers = s->max_transfers;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, sbuf, h.nslots * sizeof(LedgerSlot));
	COMP_CRC32C(crc, tbuf, h.ntransfers * sizeof(LedgerTransfer));
	FIN_CRC32C(crc);
	h.body_crc = crc;
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, &h, offsetof(SnapHeader, header_crc));
	FIN_CRC32C(crc);
	h.header_crc = crc;
	write_file_atomic(LEDGER_SNAP_FILE, &h, sizeof(h),
					  sbuf, h.nslots * sizeof(LedgerSlot),
					  tbuf, h.ntransfers * sizeof(LedgerTransfer),
					  NULL, 0);
	s->snap_body_seq = seq;
	pfree(sbuf);
	pfree(tbuf);

	LWLockRelease(ledger_snap_lock);
	return pos;
}

/* ------------------------------------------------------------------ */
/* the background worker                                               */

/*
 * The slot that used to pin PostgreSQL's log is not used any more: the
 * ledger's records are not in the log.  It is dropped, because a slot that
 * is never advanced pins the log forever and the log would never be
 * recycled.
 */
static void
ledger_drop_slot(void)
{
	if (SearchNamedReplicationSlot(LEDGER_SLOT_NAME, true) == NULL)
		return;

	PG_TRY();
	{
		ReplicationSlotDrop(LEDGER_SLOT_NAME, true);
		ereport(LOG, (errmsg("ledgerext: dropped the replication slot \"%s\", which this "
							 "version does not use", LEDGER_SLOT_NAME)));
	}
	PG_CATCH();
	{
		/* a standby is using it: it is not ours to drop */
		EmitErrorReport();
		FlushErrorState();
	}
	PG_END_TRY();
}

PGDLLEXPORT void
ledger_snapshot_worker(Datum arg)
{
	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();

	ledger_drop_slot();

	while (!ShutdownRequestPending)
	{
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		ledger_do_snapshot(false);
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ledger_snapshot_interval_ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();
	}
	proc_exit(0);
}
