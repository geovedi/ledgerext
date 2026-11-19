/*
 * ledger_replica.c - the replication: a standby that carries the same ledger.
 *
 * The ledger's truth is the journal (see ledger_wal.c): it is made durable
 * before a commit is acknowledged, and the grid in shared memory is a cache
 * of it.  So a standby does not need a protocol for the balances, or for
 * the events, or for the state machine's rules: it needs the journal's
 * records, and it applies them with the same function that rebuilds a grid
 * from the journal at startup (ledger_replay_records()).
 *
 * The protocol is one number.  The primary's journal has a position - the
 * sequence number of its last record - and a replica has the position it
 * has applied.  The replica asks for the records after its position and
 * acknowledges the position it has applied; the primary waits for that
 * acknowledgement before it acknowledges a commit, so a failover cannot lose
 * a transfer a client was told about.
 *
 * The wait is outside the ledger's lock and after the journal's fsync, so
 * it costs no CPU on the commit path and no other commit waits for it.  A
 * node with no replica registered pays nothing at all.
 */
#include "postgres.h"

#include <libpq-fe.h>

#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#include "ledgerext.h"

/* how many records a replica asks for at a time */
#define REPLICA_CHUNK		1024

/* is this node a replica of another one? */
bool
ledger_is_replica(void)
{
	return ledger_replica_of != NULL && ledger_replica_of[0] != '\0';
}

/*
 * A replica's acknowledgement: the position it has applied and made
 * durable.  The first acknowledgement registers the replica, so the
 * primary knows it has to wait for it from then on.
 *
 * An acknowledgement that goes backwards, or that is beyond the journal's
 * last record, is ignored: the replica is behind or confused, and the
 * primary's own records are not in doubt.
 */
void
ledger_replica_ack(const char *name, uint64 seq)
{
	LedgerState *s = ledger_state;
	int			i;
	LedgerReplica *r = NULL;

	if (s == NULL)
		return;

	for (i = 0; i < LEDGER_MAX_REPLICAS; i++)
	{
		if (s->replicas[i].used && strcmp(s->replicas[i].name, name) == 0)
		{
			r = &s->replicas[i];
			break;
		}
	}

	if (r == NULL)
	{
		for (i = 0; i < LEDGER_MAX_REPLICAS; i++)
			if (!s->replicas[i].used)
			{
				r = &s->replicas[i];
				break;
			}
		if (r == NULL)
		{
			ereport(WARNING,
					(errmsg("ledgerext: no room for the replica \"%s\"", name)));
			return;
		}
		memset(r, 0, sizeof(*r));
		snprintf(r->name, sizeof(r->name), "%s", name);
		r->used = true;
		ereport(LOG, (errmsg("ledgerext: the replica \"%s\" is following this node", name)));
	}

	if (seq > s->nrecords)
	{
		ereport(WARNING,
				(errmsg("ledgerext: the replica \"%s\" acknowledged position " UINT64_FORMAT
						", which is beyond this node's last record (" UINT64_FORMAT ")",
						name, seq, s->nrecords)));
		return;
	}
	if (seq < r->ack_seq)
		return;				/* a stale acknowledgement: nothing to do */

	r->ack_seq = seq;
	r->last_seen = s->clock;
}

/*
 * The primary's side of the wait: until every replica that has registered
 * has applied this commit's records.
 *
 * The wait cannot be cancelled.  The grid has already been touched when
 * this is called - the records are in the journal and the balances have
 * moved - so a cancel that arrives now must not abort the transaction: the
 * transfer is applied, and the client is told so.  The pending interrupt is
 * dropped, and the signal that set it has already been delivered, so
 * nothing acts on it later.
 */
void
ledger_replica_wait(uint64 seq)
{
	LedgerState *s = ledger_state;
	bool		waiting;

	if (s == NULL || ledger_is_replica() || !ledger_replica_sync)
		return;				/* a replica waits for nobody: it is not a primary */

	HOLD_INTERRUPTS();
	for (;;)
	{
		int			i;

		waiting = false;
		for (i = 0; i < LEDGER_MAX_REPLICAS; i++)
			if (s->replicas[i].used && s->replicas[i].ack_seq < seq)
			{
				waiting = true;
				break;
			}
		if (!waiting)
			break;

		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}
	RESUME_INTERRUPTS();

	/* the cancel lost the race: the commit is going through */
	InterruptPending = false;
}

/* ------------------------------------------------------------------ */
/* the replica's side                                                 */

/*
 * Ask the primary for the records after a position.  The answer is the
 * records as they are on disk, back to back.
 */
static bool
replica_fetch(PGconn *conn, uint64 from_seq, JournalRecord *buf, int *nrecs)
{
	const char *values[2];
	char		v0[32],
				v1[32];
	PGresult   *res;
	bool		ok = false;

	*nrecs = 0;

	snprintf(v0, sizeof(v0), UINT64_FORMAT, from_seq);
	snprintf(v1, sizeof(v1), "%d", REPLICA_CHUNK);
	values[0] = v0;
	values[1] = v1;

	res = PQexecParams(conn,
					   "select ledger_journal_records($1::bigint, $2::int)",
					   2, NULL, values, NULL, NULL, 1);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		ereport(LOG, (errmsg("ledgerext: the primary refused the records: %s",
							  PQerrorMessage(conn))));
		PQclear(res);
		return false;
	}

	if (PQntuples(res) == 1 && !PQgetisnull(res, 0, 0))
	{
		int			len = PQgetlength(res, 0, 0);
		const char *data = PQgetvalue(res, 0, 0);

		if (len % (int) sizeof(JournalRecord) != 0)
			ereport(LOG, (errmsg("ledgerext: the primary sent %d bytes, which is not a whole "
								  "number of records", len)));
		else
		{
			*nrecs = len / (int) sizeof(JournalRecord);
			memcpy(buf, data, len);
			ok = true;
		}
	}
	PQclear(res);
	return ok;
}

/*
 * Tell the primary how far this replica has got.  The acknowledgement is
 * sent after the replica's journal has been made durable, so the primary
 * knows the records are on the standby's disk.
 */
static bool
replica_ack(PGconn *conn, const char *name, uint64 seq)
{
	const char *values[2];
	char		v0[32],
				v1[32];
	PGresult   *res;
	bool		ok;

	snprintf(v0, sizeof(v0), "%s", name);
	snprintf(v1, sizeof(v1), UINT64_FORMAT, seq);
	values[0] = v0;
	values[1] = v1;

	res = PQexecParams(conn, "select ledger_replica_ack($1::text, $2::bigint)",
					   2, NULL, values, NULL, NULL, 0);
	/* a select of a void function answers with a row, not with a command */
	ok = (PQresultStatus(res) == PGRES_TUPLES_OK ||
		  PQresultStatus(res) == PGRES_COMMAND_OK);
	if (!ok)
		ereport(LOG, (errmsg("ledgerext: the primary refused the acknowledgement: %s",
							  PQerrorMessage(conn))));
	PQclear(res);
	return ok;
}

/*
 * The replica's worker: follow the primary, apply its records, and
 * acknowledge the position.
 */
PGDLLEXPORT void
ledger_replica_worker(Datum arg)
{
	PGconn	   *conn = NULL;
	JournalRecord *buf;
	const char *name;

	pqsignal(SIGTERM, SignalHandlerForShutdownRequest);
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	BackgroundWorkerUnblockSignals();

	/* the shared memory has to be there before anything is applied */
	while (!ledger_shmem_ready)
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 100, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}

	buf = (JournalRecord *) palloc(REPLICA_CHUNK * sizeof(JournalRecord));

	/*
	 * The loop ends when the server is shutting down: the worker is a
	 * child of the postmaster, and a postmaster that waits for a child
	 * that does not exit is a server that does not shut down.
	 */
	while (!ShutdownRequestPending)
	{
		uint64		pos;
		int			nrecs = 0;

		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		if (!ledger_is_replica())
		{
			/*
			 * This node is a primary: it follows nobody.  It may be
			 * promoted later - or made a replica - by changing the
			 * setting, so the worker waits and looks again.
			 */
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 500, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
			continue;
		}

		name = "replica";

		if (conn == NULL)
		{
			/*
			 * The connect timeout comes first, so that the operator's
			 * own setting wins: a worker that cannot reach the primary
			 * must not block the server's shutdown.
			 */
			conn = PQconnectdb(psprintf("connect_timeout=5 %s", ledger_replica_of));
			if (PQstatus(conn) != CONNECTION_OK)
			{
				ereport(LOG, (errmsg("ledgerext: could not reach the primary: %s",
									  PQerrorMessage(conn))));
				PQfinish(conn);
				conn = NULL;
				(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
								 ledger_replica_poll_ms, PG_WAIT_EXTENSION);
				ResetLatch(MyLatch);
				continue;
			}
			ereport(LOG, (errmsg("ledgerext: following the primary at %s", ledger_replica_of)));
		}

		/* where this replica has got to: its own journal's last record */
		pos = ledger_state->nrecords;

		/* register, so that the primary waits for this replica */
		if (!replica_ack(conn, name, pos))
		{
			PQfinish(conn);
			conn = NULL;
			continue;
		}

		if (!replica_fetch(conn, pos, buf, &nrecs))
		{
			PQfinish(conn);
			conn = NULL;
			continue;
		}

		if (nrecs > 0)
		{
			uint64		last;

			/*
			 * The records name their own positions, so the first one must
			 * be the one after this replica's.  A stream that is not is
			 * damaged, and a ledger that is applied out of order is worse
			 * than a ledger that is down.
			 */
			if (buf[0].seq != pos + 1)
				ereport(ERROR,
						(errmsg("ledgerext: the primary sent the record " UINT64_FORMAT
								", but this replica's position is " UINT64_FORMAT,
								buf[0].seq, pos),
						 errhint("The ledger's records are in $PGDATA/ledgerext.journal. "
								 "The replica refuses a stream it cannot account for.")));

			last = buf[nrecs - 1].seq;

			LWLockAcquire(ledger_lock, LW_EXCLUSIVE);
			PG_TRY();
			{
				/*
				 * The grid first, then the journal.  The other way round
				 * would leave the journal ahead of the grid if this
				 * process died in between, and the worker would then
				 * fetch nothing: its position is the journal's, so the
				 * grid would stay behind for good.  This way round a
				 * death in between leaves the grid ahead, and the
				 * records are fetched again and replayed over the
				 * transfers that are already there - which the replay
				 * skips, because it applies a transfer once.
				 */
				ledger_replay_records(buf, nrecs);
				ledger_journal_append(buf, nrecs);
			}
			PG_CATCH();
			{
				ledger_state->broken = true;
				LWLockRelease(ledger_lock);
				PG_RE_THROW();
			}
			PG_END_TRY();
			LWLockRelease(ledger_lock);

			/* the records are on this node's disk before the primary is told */
			ledger_journal_fsync();

			if (!replica_ack(conn, name, last))
			{
				PQfinish(conn);
				conn = NULL;
				continue;
			}
			continue;			/* there may be more waiting */
		}

		/* nothing new: wait for the next poll */
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 ledger_replica_poll_ms, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
	}

	proc_exit(0);
}
