/*
 * ledger_journal.c - the journal: an append-only file of fixed-size records
 * in $PGDATA, and the queries that read it.
 *
 * The journal is the ledger's record.  Every committed transfer and every
 * account creation is one record, in commit order, and nothing ever removes
 * or alters one.  The balances in shared memory are a cache of it: the
 * journal replayed from the beginning reproduces every account's four
 * counters exactly.
 *
 * The file is a plain array.  A record's position is its sequence number,
 * so its offset is arithmetic and there is no index to build, to grow or to
 * repair.  Each record also carries the sequence number of the previous
 * record that named each of its two accounts, and the grid holds the head
 * of each account's chain, so the by-account query is a walk of a chain
 * rather than a scan of the file.
 *
 * DURABILITY
 * ----------
 * The journal is fsynced before the transaction that wrote the records is
 * acknowledged, so a transfer a client has been told about is on disk.  It
 * used to be the other way round: the WAL held the same records and the
 * journal was a copy of it, fsynced by the snapshot worker.  The copy cost
 * 0.30 us of server CPU a transfer, and it bought nothing: the journal is
 * the ledger's log now, and the grid is a cache of it.
 */
#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"

#include "ledgerext.h"

/*
 * The journal's file descriptor lives in the shared state, so every backend
 * uses the one the postmaster opened.  It is NOT opened through
 * PostgreSQL's OpenTransientFile: that registers the descriptor with the
 * current subtransaction, which closes it at the end of the transaction.
 * A plain open() in the postmaster's startup gives a descriptor that
 * lasts as long as the server does.
 */
#define JFD (ledger_state->journal_fd)

static void
journal_path(char *out, const char *name)
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

/* the offset of a record in the file */
static off_t
journal_offset(uint64 seq)
{
	return (off_t) (sizeof(JournalHeader) +
					(seq - 1) * (off_t) sizeof(JournalRecord));
}

/*
 * Open the journal, and create it if it is not there.  A journal whose
 * header is not ours is refused: 0.4 had no journal at all, and a journal
 * from another version cannot be read.
 */
void
ledger_journal_open(void)
{
	char		path[MAXPGPATH];
	JournalHeader h;
	int			fd;

	if (JFD >= 0)
		return;

	journal_path(path, LEDGER_JOURNAL_FILE);
	fd = open(path, O_RDWR | PG_BINARY);
	if (fd < 0)
	{
		if (errno != ENOENT)
			ereport(FATAL, (errcode_for_file_access(),
							errmsg("ledgerext: could not open \"%s\": %m", path)));
		/* a new journal */
		fd = open(path, O_RDWR | O_CREAT | O_EXCL | PG_BINARY, pg_file_create_mode);
		if (fd < 0)
			ereport(FATAL, (errcode_for_file_access(),
							errmsg("ledgerext: could not create \"%s\": %m", path)));
		memset(&h, 0, sizeof(h));
		h.magic = LEDGER_JOURNAL_MAGIC;
		h.version = LEDGER_JOURNAL_VERSION;
		h.recsize = sizeof(JournalRecord);
		write_all(fd, (const char *) &h, sizeof(h), path);
		/*
		 * The journal is the ledger's only truth, so its creation is made
		 * durable before anything is acknowledged: the header, and the
		 * directory entry that names the file.
		 */
		if (pg_fsync(fd) != 0)
			ereport(FATAL, (errcode_for_file_access(),
							errmsg("ledgerext: could not fsync \"%s\": %m", path)));
		fsync_fname(DataDir, true);
		ledger_state->nrecords = 0;
		JFD = fd;
		return;
	}

	if (!read_all(fd, (char *) &h, sizeof(h)))
		ereport(FATAL, (errmsg("ledgerext: the journal \"%s\" has no header", path)));
	if (h.magic != LEDGER_JOURNAL_MAGIC || h.version != LEDGER_JOURNAL_VERSION ||
		h.recsize != sizeof(JournalRecord))
		ereport(FATAL,
				(errmsg("ledgerext: the journal \"%s\" was written by another version "
						"of ledgerext", path),
				 errhint("Remove the file to start with an empty ledger.")));

	/*
	 * The record count is the file's length: the header's count is only
	 * written at creation.  A torn tail (a record cut short) is dropped.
	 */
	{
		struct stat st;
		off_t		have;

		if (fstat(fd, &st) != 0)
			ereport(FATAL, (errcode_for_file_access(),
							errmsg("ledgerext: could not stat \"%s\": %m", path)));
		have = st.st_size - (off_t) sizeof(JournalHeader);
		ledger_state->nrecords = (uint64) (have / (off_t) sizeof(JournalRecord));
		if (have % (off_t) sizeof(JournalRecord) != 0 &&
			ftruncate(fd, (off_t) sizeof(JournalHeader) +
					  (off_t) ledger_state->nrecords * (off_t) sizeof(JournalRecord)) != 0)
			ereport(FATAL, (errcode_for_file_access(),
							errmsg("ledgerext: could not truncate the journal: %m")));
	}
	JFD = fd;
}

/* read a record by its sequence number */
bool
ledger_journal_read(uint64 seq, JournalRecord *out)
{
	off_t		off;
	ssize_t		r;

	if (seq == 0 || seq > ledger_state->nrecords)
		return false;
	if (JFD < 0)
		return false;
	off = journal_offset(seq);
	r = pread(JFD, out, sizeof(JournalRecord), off);
	if (r != (ssize_t) sizeof(JournalRecord))
	{
		return false;
	}
	return true;
}

/*
 * Read a block of records, starting at the given sequence number.  One read
 * serves every record in the block, so the replay of the journal's tail does
 * not pay a syscall a record.
 */
int
ledger_journal_read_block(uint64 seq, JournalRecord *buf, int max)
{
	size_t		blen = (size_t) max * sizeof(JournalRecord);
	ssize_t		got;

	if (seq == 0 || JFD < 0 || max <= 0)
		return 0;
	got = pread(JFD, buf, blen, journal_offset(seq));
	if (got <= 0)
		return 0;
	return (int) (got / (ssize_t) sizeof(JournalRecord));
}

/* append records at the positions their sequence numbers name */void
ledger_journal_append(const JournalRecord *recs, int n)
{
	char		path[MAXPGPATH];
	off_t		off;
	size_t		len;

	if (n <= 0)
		return;
	if (JFD < 0)
		ledger_journal_open();

	len = (size_t) n * sizeof(JournalRecord);
	off = journal_offset(recs[0].seq);	/* a record's position is its seq */

	journal_path(path, LEDGER_JOURNAL_FILE);

	/*
	 * pwrite, not write: the backends inherit the file descriptor from the
	 * postmaster and share its offset, so a plain write would interleave.
	 */
	{
		const char *buf = (const char *) recs;
		size_t		left = len;

		while (left > 0)
		{
			ssize_t		w = pwrite(JFD, buf, left, off);

			if (w < 0)
			{
				if (errno == EINTR)
					continue;
				ereport(ERROR, (errcode_for_file_access(),
								errmsg("ledgerext: could not write \"%s\": %m", path)));
			}
			buf += w;
			left -= (size_t) w;
			off += w;
		}
	}
	if (recs[n - 1].seq > ledger_state->nrecords)
		ledger_state->nrecords = recs[n - 1].seq;
}

/*
 * The journal's records after a position, for a replica to fetch.  Only
 * whole records are returned: a record that is being written is not a
 * record yet.  The records are the ones on disk, byte for byte, so a
 * replica's copy of them is a copy of the journal.
 */
int
ledger_journal_records_bytes(LedgerState *s, uint64 from_seq, int lim, uint8 *out)
{
	uint64		seq = from_seq + 1;
	int			n;

	if (seq == 0 || seq > s->nrecords || lim <= 0)
		return 0;

	n = ledger_journal_read_block(seq, (JournalRecord *) out, lim);
	if (n <= 0)
		return 0;
	return n * (int) sizeof(JournalRecord);
}

/* make the journal durable */
void
ledger_journal_fsync(void)
{
	char		path[MAXPGPATH];

	if (JFD < 0)
		return;
	journal_path(path, LEDGER_JOURNAL_FILE);
	if (pg_fsync(JFD) != 0)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("ledgerext: could not fsync \"%s\": %m", path)));
}

uint64
ledger_journal_nrecords(void)
{
	return ledger_state->nrecords;
}

/*
 * The position of the first record at or after the given timestamp.  The
 * commit clock only moves forward, so the timestamps are sorted and this is
 * a binary search over the file.
 */
uint64
ledger_journal_seek(uint64 timestamp)
{
	uint64		lo = 1,
				hi = ledger_state->nrecords;
	JournalRecord r;

	if (ledger_state->nrecords == 0)
		return 1;
	while (lo < hi)
	{
		uint64		mid = lo + (hi - lo) / 2;

		if (!ledger_journal_read(mid, &r))
			break;
		if (r.timestamp < timestamp)
			lo = mid + 1;
		else
			hi = mid;
	}
	/* every record is before the timestamp: the position past the end */
	if (ledger_journal_read(lo, &r) && r.timestamp < timestamp)
		lo++;
	return lo;
}

/* ------------------------------------------------------------------ */
/* the queries                                                         */

/*
 * The records that named an account, in commit order.  The account's chain
 * is walked backwards from the head in its row; the sequence numbers are
 * collected and then reversed.
 *
 * The account belongs to one book, so an account of another book is not
 * found: the chain of an account names only that account's records.
 */
int
ledger_journal_by_account(LedgerState *s, const unsigned char *id,
						  uint16 tenant, uint64 **seqs, int *n)
{
	int64		slot;
	uint64	   *list;
	int			count = 0,
				cap = 64;
	uint64		seq;

	*seqs = NULL;
	*n = 0;

	slot = ledger_find_account(s, id);
	if (slot < 0)
		return 0;
	if (s->slots[slot].tenant != tenant)
		return 0;			/* it belongs to another book */

	list = (uint64 *) palloc(cap * sizeof(uint64));
	seq = s->slots[slot].last_seq;
	while (seq != 0)
	{
		JournalRecord r;

		if (!ledger_journal_read(seq, &r))
			break;
		if (LEDGER_RECORD_TENANT(&r) != tenant)
			break;			/* the chain has left this book */
		if (count == cap)
		{
			cap *= 2;
			list = (uint64 *) repalloc(list, cap * sizeof(uint64));
		}
		list[count++] = seq;
		/* the record named the account as its debit or as its credit */
		seq = (memcmp(r.debit, id, 16) == 0) ? r.prev_debit_seq : r.prev_credit_seq;
	}

	/* the chain is walked backwards: put it back in commit order */
	{
		int			i;

		for (i = 0; i < count / 2; i++)
		{
			uint64		t = list[i];

			list[i] = list[count - 1 - i];
			list[count - 1 - i] = t;
		}
	}
	*seqs = list;
	*n = count;
	return count;
}

/*
 * A sequential reader over the journal.  The replay walks the file from the
 * beginning to the end, so it reads the records in order: one block read
 * serves every record in the block, instead of one read per record.
 */
typedef struct SeqReader
{
	char	   *buf;			/* the records of the current block */
	uint64		first;			/* the first sequence number in it */
	uint64		count;			/* how many records it holds */
	uint64		pos;			/* the next sequence number to return */
} SeqReader;

static void
seq_reader_init(SeqReader *sr)
{
	sr->buf = (char *) palloc(64 * 1024);
	sr->first = 1;
	sr->count = 0;
	sr->pos = 1;
}

static void
seq_reader_free(SeqReader *sr)
{
	pfree(sr->buf);
}

/* the next record, or false at the end of the journal */
static bool
seq_reader_next(SeqReader *sr, JournalRecord *out)
{
	size_t		recsize = sizeof(JournalRecord);

	while (sr->pos >= sr->first + sr->count)
	{
		size_t		want = 64 * 1024;
		size_t		blen;
		ssize_t		got;
		off_t		off;

		if (sr->pos > ledger_state->nrecords)
			return false;
		blen = want - (want % recsize);
		off = journal_offset(sr->pos);
		got = pread(JFD, sr->buf, blen, off);
		if (got <= 0)
			return false;
		sr->first = sr->pos;
		sr->count = (uint64) got / recsize;
		if (sr->count == 0)
			return false;
	}
	memcpy(out, sr->buf + (sr->pos - sr->first) * recsize, recsize);
	sr->pos++;
	return true;
}

/*
 * The records of a ledger.  The journal is in commit order and every record
 * carries the ledger it belongs to, so the records of a ledger are found by
 * one pass over the file, in order, with no sort and no de-duplication: a
 * transfer's two accounts always share a ledger (the engine refuses a
 * transfer whose accounts do not), so a record's own ledger is the ledger
 * of every account it names.
 *
 * The pass reads the file in blocks, so it costs one read per block rather
 * than one per record.
 */
int
ledger_journal_by_ledger(LedgerState *s, uint32 ledger,
						 uint16 tenant, uint64 **seqs, int *n)
{
	uint64	   *list;
	int			count = 0,
				cap = 1024;
	uint64		seq;
	SeqReader	sr;

	*seqs = NULL;
	*n = 0;

	list = (uint64 *) palloc(cap * sizeof(uint64));

	seq_reader_init(&sr);
	for (seq = 1; seq <= ledger_state->nrecords; seq++)
	{
		JournalRecord r;

		if (!seq_reader_next(&sr, &r))
			break;
		if (r.ledger != ledger)
			continue;
		if (!ledger_record_visible(&r, tenant))
			continue;		/* it belongs to another book */
		if (count == cap)
		{
			cap *= 2;
			list = (uint64 *) repalloc(list, cap * sizeof(uint64));
		}
		list[count++] = seq;
	}
	seq_reader_free(&sr);

	*seqs = list;
	*n = count;
	return count;
}

/* ------------------------------------------------------------------ */
/* the replay                                                          */

/*
 * An account of the replay: the four counters and the flags, built from
 * the journal alone.
 */
typedef struct ReplayAccount
{
	unsigned char id[16];
	ledger_amount dp,
				cp,
				d,
				c;
	uint32		flags,
				code,
				ledger;
} ReplayAccount;

/*
 * The index of a replay table: the buckets hold the index of an entry
 * plus one, so that zero means "no entry".  The table is hashed, like the
 * grid's, so that a replay of a million records does not do a million
 * linear scans over the accounts.
 *
 * The index knows only the ids, and an entry's id is at its front, so one
 * index serves every table of the replay.
 */
typedef struct ReplayMap
{
	uint32	   *buckets;
	uint32		bcap;
	uint32		mask;
} ReplayMap;

/* the index of the entry with this id, or -1 */
static int
replay_map_find(const ReplayMap *m, const void *entries, size_t stride,
				const unsigned char *id)
{
	uint32		b;

	if (m->buckets == NULL)
		return -1;			/* the table is empty */
	b = ledger_hash(id) & m->mask;

	for (;;)
	{
		uint32		ent = m->buckets[b];

		if (ent == 0)
			return -1;
		if (memcmp((const unsigned char *) entries + (size_t) (ent - 1) * stride,
				   id, 16) == 0)
			return (int) (ent - 1);
		b = (b + 1) & m->mask;
	}
}

/* index the entry at the end of the table, which the caller has just added */
static void
replay_map_add(ReplayMap *m, const void *entries, size_t stride, int n)
{
	uint32		b;

	/*
	 * The index grows with the table, so it stays half empty.  A rebuild
	 * indexes every entry, including the one just added, so it must not
	 * be indexed again afterwards.
	 */
	if ((uint32) n * 2 > m->bcap)
	{
		uint32		nb = m->bcap * 2;
		uint32	   *nbuckets;
		int			i;

		if (nb == 0)
			nb = 1024;
		nbuckets = (uint32 *) palloc0(nb * sizeof(uint32));
		for (i = 0; i < n; i++)
		{
			b = ledger_hash((const unsigned char *) entries + (size_t) i * stride) & (nb - 1);
			while (nbuckets[b] != 0)
				b = (b + 1) & (nb - 1);
			nbuckets[b] = (uint32) (i + 1);
		}
		if (m->buckets)
			pfree(m->buckets);
		m->buckets = nbuckets;
		m->bcap = nb;
		m->mask = nb - 1;
		return;
	}
	b = ledger_hash((const unsigned char *) entries + (size_t) (n - 1) * stride) & m->mask;
	while (m->buckets[b] != 0)
		b = (b + 1) & m->mask;
	m->buckets[b] = (uint32) n;
}

/* add an account to the replay's table and its index */
static int
replay_add(ReplayAccount **accts, int *naccts, int *cap, ReplayMap *m,
		   const unsigned char *id)
{
	if (*naccts == *cap)
	{
		*cap *= 2;
		*accts = (ReplayAccount *) repalloc(*accts, *cap * sizeof(ReplayAccount));
		memset(*accts + *naccts, 0, (size_t) (*cap - *naccts) * sizeof(ReplayAccount));
	}
	memcpy((*accts)[*naccts].id, id, 16);
	(*naccts)++;
	replay_map_add(m, *accts, sizeof(ReplayAccount), *naccts);
	return *naccts - 1;
}

/*
 * A pending transfer the replay has seen: the amount it reserved.  A post
 * or a void releases a reservation, and the record does not carry the
 * amount it releases - that amount is the pending transfer's own amount,
 * and the pending transfer's own record said so.  The replay remembers it,
 * so that it releases the right one.
 */
typedef struct ReplayPending
{
	unsigned char id[16];
	ledger_amount amount;
} ReplayPending;

/* the pending transfers the replay has seen, and the index into them */
typedef struct ReplayPendings
{
	ReplayPending *ents;
	int			n;
	int			cap;
	ReplayMap	map;
} ReplayPendings;

static void
replay_pending_put(ReplayPendings *ps, const unsigned char *id,
					ledger_amount amount)
{
	int			e = replay_map_find(&ps->map, ps->ents, sizeof(ReplayPending), id);

	if (e >= 0)
	{
		ps->ents[e].amount = amount;
		return;
	}

	if (ps->n == ps->cap)
	{
		if (ps->cap == 0)
		{
			ps->cap = 64;
			ps->ents = (ReplayPending *) palloc0(ps->cap * sizeof(ReplayPending));
		}
		else
		{
			ps->cap *= 2;
			ps->ents = (ReplayPending *) repalloc(ps->ents,
												  ps->cap * sizeof(ReplayPending));
		}
	}

	memcpy(ps->ents[ps->n].id, id, 16);
	ps->ents[ps->n].amount = amount;
	ps->n++;
	replay_map_add(&ps->map, ps->ents, sizeof(ReplayPending), ps->n);
}

static bool
replay_pending_get(ReplayPendings *ps, const unsigned char *id,
					ledger_amount *amount)
{
	int			e = replay_map_find(&ps->map, ps->ents, sizeof(ReplayPending), id);

	if (e < 0)
		return false;
	*amount = ps->ents[e].amount;
	return true;
}

/*
 * Release a reservation in the replay: the money goes back to the two
 * accounts it was held from, and a closing transfer's account is reopened
 * when the reservation is given up.
 */
static void
replay_release(ReplayAccount *accts, int di, int ci, ledger_amount amount,
			   uint32 pending_flags, bool reopen)
{
	accts[di].dp -= amount;
	accts[ci].cp -= amount;
	if (reopen)
	{
		if (pending_flags & TR_CLOSING_DEBIT)
			accts[di].flags &= ~ACCT_CLOSED;
		if (pending_flags & TR_CLOSING_CREDIT)
			accts[ci].flags &= ~ACCT_CLOSED;
	}
}

/*
 * The balances the journal implies.  Every record is read from the
 * beginning and its effect applied to a fresh set of accounts; the grid is
 * never consulted.  The counters are returned per account, in the order
 * the accounts first appear.
 *
 * The replay is of one book: a record of another book is not applied, so
 * the balances of another book do not reach this one.  A record with no
 * book is the cluster's own, and every book sees it.
 */
BalanceRow *
ledger_journal_replay(LedgerState *s, uint64 upto, uint16 tenant, int *nrows)
{
	ReplayAccount *accts;
	ReplayPendings pendings;
	ReplayMap	map = {NULL, 0, 0};
	int			naccts = 0,
				cap = 256;
	uint64		seq;
	SeqReader	sr;

	if (upto == 0)
		upto = ledger_state->nrecords;

	memset(&pendings, 0, sizeof(pendings));

	accts = (ReplayAccount *) palloc(cap * sizeof(ReplayAccount));
	memset(accts, 0, cap * sizeof(ReplayAccount));

	seq_reader_init(&sr);
	for (seq = 1; seq <= upto; seq++)
	{
		JournalRecord r;
		int			di,
					ci;
		bool		is_post,
					is_void,
					is_pending;

		if (!seq_reader_next(&sr, &r))
			break;

		if (!ledger_record_visible(&r, tenant))
			continue;		/* it belongs to another book */

		if (r.kind == JOURNAL_ACCOUNT)
		{
			di = replay_map_find(&map, accts, sizeof(ReplayAccount), r.id);
			if (di < 0)
				di = replay_add(&accts, &naccts, &cap, &map, r.id);
			accts[di].flags = r.flags & ~LEDGER_TENANT_MASK;
			accts[di].code = r.code;
			accts[di].ledger = r.ledger;
			continue;
		}

		if (r.kind == JOURNAL_CLOSE)
			continue;

		/* the two accounts of the transfer */
		di = replay_map_find(&map, accts, sizeof(ReplayAccount), r.debit);
		if (di < 0)
			di = replay_add(&accts, &naccts, &cap, &map, r.debit);
		ci = replay_map_find(&map, accts, sizeof(ReplayAccount), r.credit);
		if (ci < 0)
			ci = replay_add(&accts, &naccts, &cap, &map, r.credit);

		is_post = (r.flags & (TR_POST | TR_VOID)) != 0;
		is_void = (r.flags & TR_VOID) != 0;
		is_pending = (r.flags & TR_PENDING) != 0;

		if (r.kind == JOURNAL_EXPIRE)
		{
			/*
			 * The record of a release.  The reservation goes back, and a
			 * closing transfer's account is reopened.  It has to come
			 * before the transfer case below, which would read the
			 * record as a movement of money.  The record carries the
			 * amount it releases: it is the pending transfer's own.
			 */
			replay_release(accts, di, ci, r.amount, r.pending_flags, true);
			continue;
		}

		if (is_post)
		{
			/*
			 * A post or a void releases the reservation first.  The
			 * amount it releases is the pending transfer's own amount,
			 * which the pending transfer's own record said - so the
			 * replay remembers it.
			 */
			ledger_amount reserved;

			if (!replay_pending_get(&pendings, r.pending_id, &reserved))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("ledgerext: the journal posts a pending transfer "
								"that is not in it"),
						 errhint("The journal is damaged.")));
			replay_release(accts, di, ci, reserved, r.pending_flags, is_void);
			if (!is_void)
			{
				accts[di].d += r.amount_actual;
				accts[ci].c += r.amount_actual;
			}
		}
		else if (is_pending)
		{
			accts[di].dp += r.amount_actual;
			accts[ci].cp += r.amount_actual;
			/* the reservation this pending transfer holds */
			replay_pending_put(&pendings, r.id, r.amount_actual);
		}
		else
		{
			accts[di].d += r.amount_actual;
			accts[ci].c += r.amount_actual;
		}

		/* a closing transfer closes its account */
		if (r.flags & TR_CLOSING_DEBIT)
			accts[di].flags |= ACCT_CLOSED;
		if (r.flags & TR_CLOSING_CREDIT)
			accts[ci].flags |= ACCT_CLOSED;
	}

	/* the rows are copied out, so that the table can be freed */
	{
		BalanceRow *rows = (BalanceRow *) palloc(Max(naccts, 1) * sizeof(BalanceRow));
		int			i;

		for (i = 0; i < naccts; i++)
		{
			memcpy(rows[i].id.data, accts[i].id, 16);
			rows[i].dp = accts[i].dp;
			rows[i].cp = accts[i].cp;
			rows[i].d = accts[i].d;
			rows[i].c = accts[i].c;
		}
		*nrows = naccts;
		seq_reader_free(&sr);
		pfree(accts);
		if (map.buckets)
			pfree(map.buckets);
		if (pendings.map.buckets)
			pfree(pendings.map.buckets);
		if (pendings.ents)
			pfree(pendings.ents);
		return rows;
	}
}
