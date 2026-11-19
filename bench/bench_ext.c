/* Benchmark client for ledgerext.ledger_apply(bytea).
 *
 * One batch = one packed bytea parameter, sent in BINARY format (no hex
 * escaping), one round trip.  The ledger's model of "one request
 * carries up to 8189 events".
 *
 * usage: bench_ext <batch> <iters> <naccounts> [clients]
 * env:   PGDSN, ACC (accounts to pre-create), RESET=1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <libpq-fe.h>

/* wire format, must match PackedTransfer/PackedAccount in ledgerext.h */
#define REC 144				/* 5x16 ids + amount16 + actual16 + ts8 + 4x4 + pad8 */
#define ACCREC 56			/* id16 + user_data16 + ts8 + 4x4 */
#define HDR 16				/* count4 + kind4 + pad8 */
#define KIND_TRANSFERS 0
#define KIND_ACCOUNTS 1

static long long now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int batch, iters, nacc;
static uint32_t bench_ledger = 1;
static const char *dsn;
static const char *acc_table = NULL;
static unsigned char *aids;		/* nacc * 16 */
static volatile long long grand_total = 0;

static void
fail(PGconn *cn, const char *what)
{
	fprintf(stderr, "%s: %s\n", what, PQerrorMessage(cn));
	exit(1);
}

static void *
worker(void *arg)
{
	long long id = (long long) arg;
	PGconn *cn = PQconnectdb(dsn);
	const char *paramValues[1];
	int paramLengths[1];
	int paramFormats[1];
	unsigned char *buf;
	PGresult *r;
	long long total = 0;
	int i, j;
	uint64_t base = ((uint64_t) getpid() << 24) | ((uint64_t) id << 12) | (uint64_t) time(NULL);

	if (PQstatus(cn) != CONNECTION_OK)
		fail(cn, "connect");

	buf = malloc(HDR + (size_t) batch * REC);
	*(uint32_t *) buf = batch;
	*(uint32_t *) (buf + 4) = KIND_TRANSFERS;
	for (i = 0; i < batch; i++)
	{
		unsigned char *p = buf + HDR + (size_t) i * REC;
		uint64_t h = (uint64_t) (id * 1000003LL + i) * 2654435761u + 12345;

		uint64_t d = h % (uint64_t) nacc;
		uint64_t c = (d + 1 + (h >> 17) % (uint64_t) (nacc - 1)) % (uint64_t) nacc;

		memset(p, 0, REC);
		memcpy(p, &base, 8);	/* transfer id */
		memcpy(p + 8, &base, 8);
		memcpy(p + 16, aids + d * 16, 16);	/* debit account */
		memcpy(p + 32, aids + c * 16, 16);	/* credit account */
		*(uint64_t *) (p + 80) = 100;		/* amount, the low half */
		*(uint64_t *) (p + 88) = 0;		/* amount, the high half */
		*(uint32_t *) (p + 124) = 1;	/* code */
		*(uint32_t *) (p + 128) = bench_ledger;	/* ledger */
	}
	paramValues[0] = (const char *) buf;
	paramLengths[0] = HDR + batch * REC;
	paramFormats[0] = 1;			/* binary */

	/*
	 * A prepared statement, not a one-shot: an unnamed statement is
	 * parsed and planned again for every call, and the planning is the
	 * server's CPU.  A named one is planned once and cached, so the
	 * server's time goes into the ledger and not into the planner.
	 */
	{
		PGresult *pr = PQprepare(cn, "apply", "select ledger_apply($1::bytea)",
								  0, NULL);

		if (!pr || PQresultStatus(pr) != PGRES_COMMAND_OK)
			fail(cn, "prepare");
		PQclear(pr);
	}

	for (j = 0; j < iters; j++)
	{
		/* vary the transfer id so a repeat would be detectable */
		for (i = 0; i < batch; i++)
		{
			unsigned char *p = buf + HDR + (size_t) i * REC;
			uint64_t tid = ((uint64_t) id << 40) | ((uint64_t) j * batch + i);

			memcpy(p + 8, &tid, 8);	/* the id varies per run */
		}
		r = PQexecPrepared(cn, "apply", 1, paramValues, paramLengths,
						   paramFormats, 0);
		if (!r || PQresultStatus(r) != PGRES_TUPLES_OK)
			fail(cn, "apply");
		PQclear(r);
		total += batch;
	}
	/* the threads add to the same counter: the addition has to be atomic,
	 * or a run loses a thread's transfers */
	__sync_fetch_and_add(&grand_total, total);
	PQfinish(cn);
	free(buf);
	return NULL;
}

int
main(int argc, char **argv)
{
	PGconn *cn;
	pthread_t *th;
	long long i, t0, t1;
	int nclients = 4;

	batch = argc > 1 ? atoi(argv[1]) : 8189;
	iters = argc > 2 ? atoi(argv[2]) : 40;
	nacc = argc > 3 ? atoi(argv[3]) : 200000;
	if (argc > 4)
		nclients = atoi(argv[4]);
	dsn = getenv("PGDSN") ? getenv("PGDSN")
		: "host=127.0.0.1 port=55432 dbname=ledger user=postgres";
	acc_table = getenv("ACC");
	{
		const char *led = getenv("LEDGER");

		if (led)
			bench_ledger = (uint32_t) atoi(led);
	}

	cn = PQconnectdb(dsn);
	if (PQstatus(cn) != CONNECTION_OK)
		fail(cn, "connect");

	aids = malloc((size_t) nacc * 16);
	for (i = 0; i < nacc; i++)
	{
		unsigned char *p = aids + i * 16;
		uint64_t h = (uint64_t) i * 2654435761u + 999;

		memset(p, 0, 16);
		memcpy(p, &h, 8);
		memcpy(p + 8, &h, 8);
	}

	if (!acc_table)
	{
		/* the ledger needs accounts to exist; creation is idempotent */
		int n = nacc, off = 0;
		const int chunk = 100000;

		while (off < n)
		{
			int m = n - off < chunk ? n - off : chunk;
			size_t len = HDR + (size_t) m * ACCREC;
			unsigned char *ab = malloc(len);
			const char *pv[1];
			int pl[1], pf[1];
			PGresult *r;
			int k;

			*(uint32_t *) ab = m;
			*(uint32_t *) (ab + 4) = KIND_ACCOUNTS;
			for (k = 0; k < m; k++)
			{
				unsigned char *e = ab + HDR + (size_t) k * ACCREC;

				memcpy(e, aids + (size_t) (off + k) * 16, 16);
				*(uint32_t *) (e + 44) = 1;	/* code */
				*(uint32_t *) (e + 48) = bench_ledger;	/* ledger */
			}
			pv[0] = (const char *) ab; pl[0] = (int) len; pf[0] = 1;
			r = PQexecParams(cn, "select ledger_create_accounts($1::bytea)", 1, NULL, pv, pl, pf, 0);
			if (!r || PQresultStatus(r) != PGRES_TUPLES_OK)
				fail(cn, "create accounts");
			PQclear(r);
			free(ab);
			off += m;
		}
	}
	if (acc_table)
	{
		PGresult *r;
		char *q = malloc(64 + strlen(acc_table));

		sprintf(q, "truncate %s", acc_table);
		r = PQexec(cn, q);
		PQclear(r);
		sprintf(q, "insert into %s(id) select gen_random_uuid() from generate_series(1,%d)", acc_table, nacc);
		r = PQexec(cn, q);
		if (PQresultStatus(r) != PGRES_COMMAND_OK)
			fail(cn, "seed accounts");
		PQclear(r);
		free(q);
	}

	th = malloc(nclients * sizeof(pthread_t));
	t0 = now_us();
	for (i = 0; i < nclients; i++)
		pthread_create(&th[i], NULL, worker, (void *) i);
	for (i = 0; i < nclients; i++)
		pthread_join(th[i], NULL);
	t1 = now_us();

	printf("ext batch=%d clients=%d accs=%d transfers=%lld wall=%.3fs tps=%.0f\n",
		   batch, nclients, nacc, grand_total, (t1 - t0) / 1e6,
		   grand_total / ((t1 - t0) / 1e6));
	PQfinish(cn);
	return 0;
}
