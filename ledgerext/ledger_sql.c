/*
 * ledger_sql.c - the SQL surface of the ledger.
 *
 * A batch is a PackedBatch header (count, kind) followed by that many
 * events, exactly as they are written to the WAL.
 */
#include "postgres.h"

#include <string.h>

#include "access/htup_details.h"
#include "fmgr.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"
#include "utils/uuid.h"

#include "ledgerext.h"

static void
require_ready(void)
{
	if (!ledger_shmem_ready || ledger_state == NULL)
		ereport(ERROR, (errmsg("ledger: shared memory not initialized; "
							   "ledgerext must be in shared_preload_libraries")));
	if (ledger_state->broken)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("ledger: the ledger is inconsistent and cannot be used; "
							   "restart the server to rebuild it from the WAL")));
}

/* ------------------------------------------------------------------ */
/* the amounts                                                         */

/*
 * The amount as a numeric.  PostgreSQL has no 128-bit integer, so the read
 * path formats the digits and lets PostgreSQL parse them: numeric holds
 * every value of the range exactly.  An amount that fits in 64 bits takes
 * PostgreSQL's own int8 -> numeric, which is cheaper than the digits.
 */
static Datum
ledger_amount_to_numeric(ledger_amount a)
{
	char		buf[40];

	if (a <= (ledger_amount) INT64_MAX)
		return DirectFunctionCall1(int8_numeric, Int64GetDatum((int64) a));

	if (ledger_amount_to_text(a, buf, sizeof(buf)) < 0)
		elog(ERROR, "ledger: the amount does not fit its decimal form");
	return DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
							   ObjectIdGetDatum(NUMERICOID),
							   Int32GetDatum(-1));
}

/*
 * The sum of a counter over the whole grid.  One account's counter is at
 * most 2^128-1, so the sum over the grid is wider than a counter: the
 * 2^128s it carries are counted apart, and the total is reported as a
 * numeric, which holds it exactly.
 */
typedef struct LedgerSum
{
	ledger_amount lo;
	uint64		hi;			/* how many 2^128 the sum carries */
} LedgerSum;

static void
ledger_sum_add(LedgerSum *s, ledger_amount v)
{
	ledger_amount t = s->lo + v;

	if (t < s->lo)
		s->hi++;
	s->lo = t;
}

static Numeric
ledger_sum_to_numeric(const LedgerSum *s)
{
	Numeric		lo = DatumGetNumeric(ledger_amount_to_numeric(s->lo));
	bool		have_error;
	char		hitext[24];
	Numeric		hi,
				two128,
				res;

	if (s->hi == 0)
		return lo;

	/* the sum is wider than a counter: numeric does the wide arithmetic */
	snprintf(hitext, sizeof(hitext), "%llu", (unsigned long long) s->hi);
	hi = DatumGetNumeric(DirectFunctionCall3(numeric_in,
											  CStringGetDatum(hitext),
											  ObjectIdGetDatum(NUMERICOID),
											  Int32GetDatum(-1)));
	two128 = DatumGetNumeric(DirectFunctionCall3(numeric_in,
												 CStringGetDatum("340282366920938463463374607431768211456"),
												 ObjectIdGetDatum(NUMERICOID),
												 Int32GetDatum(-1)));
	res = numeric_mul_opt_error(hi, two128, &have_error);
	return numeric_add_opt_error(res, lo, &have_error);
}

/* the amount of a numeric: the decimal digits, exactly */
static ledger_amount
ledger_numeric_to_amount(Numeric n)
{
	char	   *text = DatumGetCString(DirectFunctionCall1(numeric_out,
														  NumericGetDatum(n)));
	ledger_amount v;

	if (!ledger_amount_from_text(text, &v))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("ledger: \"%s\" is not an amount between 0 and 2^128-1",
						text)));
	return v;
}

/*
 * ledger_amount(numeric) -> bytea: the 16 bytes of a decimal amount, the
 * low half first, so a client can build a batch without writing bytes by
 * hand.  A negative amount or one above 2^128-1 is an error.
 *
 * The C function is named sql_ledger_amount, because the SQL name is the
 * name of the type.
 */
PG_FUNCTION_INFO_V1(sql_ledger_amount);
Datum
sql_ledger_amount(PG_FUNCTION_ARGS)
{
	ledger_amount v = ledger_numeric_to_amount(PG_GETARG_NUMERIC(0));
	unsigned char buf[16];
	bytea	   *result;

	ledger_amount_to_bytes(v, buf);
	result = (bytea *) palloc(VARHDRSZ + 16);
	SET_VARSIZE(result, VARHDRSZ + 16);
	memcpy(VARDATA(result), buf, 16);
	PG_RETURN_BYTEA_P(result);
}

/* ------------------------------------------------------------------ */
/* status names                                                        */

static const char *
ledger_status_text(int64 st)
{
	switch (st)
	{
		case ST_CREATED:
			return "created";
		case ST_LINKED_EVENT_FAILED:
			return "linked_event_failed";
		case ST_LINKED_EVENT_CHAIN_OPEN:
			return "linked_event_chain_open";
		case ST_RESERVED_FLAG:
			return "reserved_flag";
		case ST_ID_MUST_NOT_BE_ZERO:
			return "id_must_not_be_zero";
		case ST_ID_MUST_NOT_BE_INT_MAX:
			return "id_must_not_be_int_max";
		case ST_FLAGS_ARE_MUTUALLY_EXCLUSIVE:
			return "flags_are_mutually_exclusive";
		case ST_DEBIT_ACCOUNT_ID_MUST_NOT_BE_ZERO:
			return "debit_account_id_must_not_be_zero";
		case ST_DEBIT_ACCOUNT_ID_MUST_NOT_BE_INT_MAX:
			return "debit_account_id_must_not_be_int_max";
		case ST_CREDIT_ACCOUNT_ID_MUST_NOT_BE_ZERO:
			return "credit_account_id_must_not_be_zero";
		case ST_CREDIT_ACCOUNT_ID_MUST_NOT_BE_INT_MAX:
			return "credit_account_id_must_not_be_int_max";
		case ST_ACCOUNTS_MUST_BE_DIFFERENT:
			return "accounts_must_be_different";
		case ST_PENDING_ID_MUST_BE_ZERO:
			return "pending_id_must_be_zero";
		case ST_PENDING_ID_MUST_NOT_BE_ZERO:
			return "pending_id_must_not_be_zero";
		case ST_PENDING_ID_MUST_NOT_BE_INT_MAX:
			return "pending_id_must_not_be_int_max";
		case ST_PENDING_ID_MUST_BE_DIFFERENT:
			return "pending_id_must_be_different";
		case ST_TIMEOUT_RESERVED_FOR_PENDING:
			return "timeout_reserved_for_pending_transfer";
		case ST_LEDGER_MUST_NOT_BE_ZERO:
			return "ledger_must_not_be_zero";
		case ST_CODE_MUST_NOT_BE_ZERO:
			return "code_must_not_be_zero";
		case ST_DEBIT_ACCOUNT_NOT_FOUND:
			return "debit_account_not_found";
		case ST_CREDIT_ACCOUNT_NOT_FOUND:
			return "credit_account_not_found";
		case ST_ACCOUNTS_MUST_HAVE_SAME_LEDGER:
			return "accounts_must_have_the_same_ledger";
		case ST_TRANSFER_MUST_HAVE_SAME_LEDGER:
			return "transfer_must_have_the_same_ledger_as_accounts";
		case ST_PENDING_TRANSFER_NOT_FOUND:
			return "pending_transfer_not_found";
		case ST_PENDING_TRANSFER_NOT_PENDING:
			return "pending_transfer_not_pending";
		case ST_PENDING_DIFFERENT_DEBIT_ACCOUNT:
			return "pending_transfer_has_different_debit_account";
		case ST_PENDING_DIFFERENT_CREDIT_ACCOUNT:
			return "pending_transfer_has_different_credit_account";
		case ST_PENDING_DIFFERENT_LEDGER:
			return "pending_transfer_has_different_ledger";
		case ST_PENDING_DIFFERENT_CODE:
			return "pending_transfer_has_different_code";
		case ST_EXCEEDS_PENDING_AMOUNT:
			return "exceeds_pending_transfer_amount";
		case ST_PENDING_DIFFERENT_AMOUNT:
			return "pending_transfer_has_different_amount";
		case ST_PENDING_ALREADY_POSTED:
			return "pending_transfer_already_posted";
		case ST_PENDING_ALREADY_VOIDED:
			return "pending_transfer_already_voided";
		case ST_PENDING_TRANSFER_EXPIRED:
			return "pending_transfer_expired";
		case ST_EXISTS_WITH_DIFFERENT_FLAGS:
			return "exists_with_different_flags";
		case ST_EXISTS_WITH_DIFFERENT_DEBIT_ACCOUNT:
			return "exists_with_different_debit_account_id";
		case ST_EXISTS_WITH_DIFFERENT_CREDIT_ACCOUNT:
			return "exists_with_different_credit_account_id";
		case ST_EXISTS_WITH_DIFFERENT_AMOUNT:
			return "exists_with_different_amount";
		case ST_EXISTS_WITH_DIFFERENT_PENDING_ID:
			return "exists_with_different_pending_id";
		case ST_EXISTS_WITH_DIFFERENT_USER_DATA:
			return "exists_with_different_user_data";
		case ST_EXISTS_WITH_DIFFERENT_TIMEOUT:
			return "exists_with_different_timeout";
		case ST_EXISTS_WITH_DIFFERENT_CODE:
			return "exists_with_different_code";
		case ST_EXISTS_WITH_DIFFERENT_LEDGER:
			return "exists_with_different_ledger";
		case ST_EXISTS:
			return "exists";
		case ST_OVERFLOWS_DEBITS_PENDING:
			return "overflows_debits_pending";
		case ST_OVERFLOWS_CREDITS_PENDING:
			return "overflows_credits_pending";
		case ST_OVERFLOWS_DEBITS_POSTED:
			return "overflows_debits_posted";
		case ST_OVERFLOWS_CREDITS_POSTED:
			return "overflows_credits_posted";
		case ST_OVERFLOWS_DEBITS:
			return "overflows_debits";
		case ST_OVERFLOWS_CREDITS:
			return "overflows_credits";
		case ST_OVERFLOWS_TIMEOUT:
			return "overflows_timeout";
		case ST_EXCEEDS_CREDITS:
			return "exceeds_credits";
		case ST_EXCEEDS_DEBITS:
			return "exceeds_debits";
		case ST_CLOSING_TRANSFER_MUST_BE_PENDING:
			return "closing_transfer_must_be_pending";
		case ST_DEBIT_ACCOUNT_CLOSED:
			return "debit_account_already_closed";
		case ST_CREDIT_ACCOUNT_CLOSED:
			return "credit_account_already_closed";
		case ST_NO_TRANSFER_SLOTS:
			return "no_transfer_slots";
		case ST_ACCOUNT_IN_ANOTHER_TENANT:
			return "account_in_another_tenant";
		case ST_ID_IN_ANOTHER_TENANT:
			return "id_in_another_tenant";
	}
	return NULL;
}

static const char *
ledger_account_status_text(int64 st)
{
	switch (st)
	{
		case AST_CREATED:
			return "created";
		case AST_LINKED_EVENT_FAILED:
			return "linked_event_failed";
		case AST_LINKED_EVENT_CHAIN_OPEN:
			return "linked_event_chain_open";
		case AST_RESERVED_FLAG:
			return "reserved_flag";
		case AST_ID_MUST_NOT_BE_ZERO:
			return "id_must_not_be_zero";
		case AST_ID_MUST_NOT_BE_INT_MAX:
			return "id_must_not_be_int_max";
		case AST_FLAGS_ARE_MUTUALLY_EXCLUSIVE:
			return "flags_are_mutually_exclusive";
		case AST_LEDGER_MUST_NOT_BE_ZERO:
			return "ledger_must_not_be_zero";
		case AST_CODE_MUST_NOT_BE_ZERO:
			return "code_must_not_be_zero";
		case AST_EXISTS_WITH_DIFFERENT_FLAGS:
			return "exists_with_different_flags";
		case AST_EXISTS_WITH_DIFFERENT_USER_DATA:
			return "exists_with_different_user_data";
		case AST_EXISTS_WITH_DIFFERENT_LEDGER:
			return "exists_with_different_ledger";
		case AST_EXISTS_WITH_DIFFERENT_CODE:
			return "exists_with_different_code";
		case AST_EXISTS:
			return "exists";
		case AST_NO_ACCOUNT_SLOTS:
			return "no_account_slots";
		case AST_ACCOUNT_IN_ANOTHER_TENANT:
			return "account_in_another_tenant";
	}
	return NULL;
}

/* ledger_status_name(bigint) -> text */
PG_FUNCTION_INFO_V1(ledger_status_name);
Datum
ledger_status_name(PG_FUNCTION_ARGS)
{
	int64		st = PG_GETARG_INT64(0);
	const char *name = ledger_status_text(st);

	if (name == NULL)
		name = ledger_account_status_text(st);
	if (name == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(name));
}

/* ledger_account_status_name(bigint) -> text */
PG_FUNCTION_INFO_V1(ledger_account_status_name);
Datum
ledger_account_status_name(PG_FUNCTION_ARGS)
{
	int64		st = PG_GETARG_INT64(0);
	const char *name = ledger_account_status_text(st);

	if (name == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(cstring_to_text(name));
}

/* ------------------------------------------------------------------ */
/* queueing work                                                       */

/* parse the batch header and check that the payload matches it */
static void
parse_batch(bytea *raw, uint32 kind, uint32 *count, const char **entries)
{
	char	   *base = VARDATA_ANY(raw);
	Size		len = VARSIZE_ANY_EXHDR(raw);
	PackedBatch	hdr;

	if (len < sizeof(PackedBatch))
		ereport(ERROR, (errmsg("ledger: batch too short")));
	memcpy(&hdr, base, sizeof(hdr));
	if (hdr.kind != kind)
		ereport(ERROR, (errmsg("ledger: batch is of kind %u, expected %u", hdr.kind, kind)));
	if (hdr.count == 0)
		ereport(ERROR, (errmsg("ledger: batch holds no events")));
	*count = hdr.count;
	*entries = base + sizeof(hdr);
}

/* ledger_apply(batch bytea) -> bigint (the number of events queued) */
PG_FUNCTION_INFO_V1(ledger_apply);
Datum
ledger_apply(PG_FUNCTION_ARGS)
{
	bytea	   *raw;
	uint32		count;
	const char *entries;
	Size		len;

	raw = PG_GETARG_BYTEA_PP(0);
	len = VARSIZE_ANY_EXHDR(raw);

	require_ready();
	parse_batch(raw, LEDGER_KIND_TRANSFERS, &count, &entries);
	if (count > 1000000)
		ereport(ERROR, (errmsg("ledger: a batch holds at most 1000000 transfers")));
	if (len != (Size) sizeof(PackedBatch) + (Size) count * sizeof(PackedTransfer))
		ereport(ERROR, (errmsg("ledger: batch size %zu does not match %u transfers",
							   len, count)));
	ledger_queue_transfers(count, entries);
	PG_RETURN_INT64((int64) count);
}

/* ledger_create_accounts(batch bytea) -> bigint (the number of accounts queued) */
PG_FUNCTION_INFO_V1(ledger_create_accounts);
Datum
ledger_create_accounts(PG_FUNCTION_ARGS)
{
	bytea	   *raw;
	uint32		count;
	const char *entries;
	Size		len;

	raw = PG_GETARG_BYTEA_PP(0);
	len = VARSIZE_ANY_EXHDR(raw);

	require_ready();
	parse_batch(raw, LEDGER_KIND_ACCOUNTS, &count, &entries);
	if (count > 4000000)
		ereport(ERROR, (errmsg("ledger: a batch holds at most 4000000 accounts")));
	if (len != (Size) sizeof(PackedBatch) + (Size) count * sizeof(PackedAccount))
		ereport(ERROR, (errmsg("ledger: batch size %zu does not match %u accounts",
							   len, count)));
	ledger_queue_accounts(count, entries);
	PG_RETURN_INT64((int64) count);
}

/* ledger_create_account(id uuid, flags int, code int, ledger int) -> void */
PG_FUNCTION_INFO_V1(ledger_create_account);
Datum
ledger_create_account(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *id;
	uint32		flags;
	uint32		code;
	uint32		ledger;
	PackedAccount a;

	id = PG_GETARG_UUID_P(0);
	flags = (uint32) PG_GETARG_INT32(1);
	code = (uint32) PG_GETARG_INT32(2);
	ledger = (uint32) PG_GETARG_INT32(3);

	require_ready();
	memset(&a, 0, sizeof(a));
	memcpy(a.id, id->data, 16);
	a.flags = flags;
	a.code = code;
	a.ledger = ledger;
	ledger_queue_accounts(1, (const char *) &a);
	PG_RETURN_VOID();
}

/* ledger_last_statuses() -> bigint[] (one status per event of the last batch) */
PG_FUNCTION_INFO_V1(ledger_last_statuses);
Datum
ledger_last_statuses(PG_FUNCTION_ARGS)
{
	int64	   *statuses;
	int			n,
				i;
	Datum	   *elems;

	require_ready();
	ledger_get_last_statuses(&statuses, &n);
	elems = (Datum *) palloc(Max(n, 1) * sizeof(Datum));
	for (i = 0; i < n; i++)
		elems[i] = Int64GetDatum(statuses[i]);
	PG_RETURN_ARRAYTYPE_P(construct_array(elems, n, INT8OID, 8, true, TYPALIGN_INT));
}

/* ------------------------------------------------------------------ */
/* reading the ledger                                                  */

PG_FUNCTION_INFO_V1(ledger_account_count);
Datum
ledger_account_count(PG_FUNCTION_ARGS)
{
	require_ready();
	PG_RETURN_INT64((int64) ledger_state->nslots);
}

PG_FUNCTION_INFO_V1(ledger_transfer_count);
Datum
ledger_transfer_count(PG_FUNCTION_ARGS)
{
	require_ready();
	PG_RETURN_INT64((int64) ledger_state->ntransfers);
}

/*
 * ledger_capacity() -> bigint[] {accounts_used, accounts_max,
 *                                 transfers_used, transfers_max}
 *
 * How close the ledger is to the size this node's memory holds.  The grid
 * is fixed when the server starts, so the limit is known and the refusal
 * that comes with it is not a surprise: this is where an operator looks
 * before it happens.
 */
PG_FUNCTION_INFO_V1(ledger_capacity);
Datum
ledger_capacity(PG_FUNCTION_ARGS)
{
	LedgerState *s = ledger_state;
	Datum		elems[4];

	require_ready();

	LWLockAcquire(ledger_lock, LW_SHARED);
	elems[0] = Int64GetDatum((int64) s->nslots);
	elems[1] = Int64GetDatum((int64) s->max_accounts);
	elems[2] = Int64GetDatum((int64) s->ntransfers);
	elems[3] = Int64GetDatum((int64) s->max_transfers);
	LWLockRelease(ledger_lock);

	PG_RETURN_ARRAYTYPE_P(construct_array(elems, 4, INT8OID, 8, true, TYPALIGN_INT));
}

/*
 * ledger_totals() -> bigint[] {naccounts, debits_pending, credits_pending,
 *                               debits_posted, credits_posted}
 */
PG_FUNCTION_INFO_V1(ledger_totals);
Datum
ledger_totals(PG_FUNCTION_ARGS)
{
	LedgerSum	sum_dp = {0, 0},
				sum_cp = {0, 0},
				sum_d = {0, 0},
				sum_c = {0, 0};
	Datum		elems[5];
	size_t		i;
	int64		nacc = 0;
	uint16		tenant;

	require_ready();
	tenant = ledger_tenant_current();
	LWLockAcquire(ledger_lock, LW_SHARED);
	for (i = 0; i < ledger_state->nslots; i++)
	{
		if (ledger_state->slots[i].tenant != tenant)
			continue;
		nacc++;
		ledger_sum_add(&sum_dp, ledger_state->slots[i].debits_pending);
		ledger_sum_add(&sum_cp, ledger_state->slots[i].credits_pending);
		ledger_sum_add(&sum_d, ledger_state->slots[i].debits_posted);
		ledger_sum_add(&sum_c, ledger_state->slots[i].credits_posted);
	}
	elems[0] = ledger_amount_to_numeric((ledger_amount) nacc);
	elems[1] = PointerGetDatum(ledger_sum_to_numeric(&sum_dp));
	elems[2] = PointerGetDatum(ledger_sum_to_numeric(&sum_cp));
	elems[3] = PointerGetDatum(ledger_sum_to_numeric(&sum_d));
	elems[4] = PointerGetDatum(ledger_sum_to_numeric(&sum_c));
	LWLockRelease(ledger_lock);
	PG_RETURN_ARRAYTYPE_P(construct_array(elems, 5, NUMERICOID, -1, false, TYPALIGN_INT));
}

/*
 * ledger_ledger_totals(ledger int) -> bigint[]
 *     {naccounts, debits_pending, credits_pending, debits_posted, credits_posted}
 */
PG_FUNCTION_INFO_V1(ledger_ledger_totals);
Datum
ledger_ledger_totals(PG_FUNCTION_ARGS)
{
	int32		led = PG_GETARG_INT32(0);
	LedgerSum	sum_dp = {0, 0},
				sum_cp = {0, 0},
				sum_d = {0, 0},
				sum_c = {0, 0};
	int64		nacc = 0;
	Datum		elems[5];
	size_t		i;
	uint16		tenant;

	require_ready();
	tenant = ledger_tenant_current();
	LWLockAcquire(ledger_lock, LW_SHARED);
	for (i = 0; i < ledger_state->nslots; i++)
	{
		if (ledger_state->slots[i].deleted)
			continue;
		if (ledger_state->slots[i].tenant != tenant)
			continue;
		if (ledger_state->slots[i].ledger != (uint32) led)
			continue;
		nacc++;
		ledger_sum_add(&sum_dp, ledger_state->slots[i].debits_pending);
		ledger_sum_add(&sum_cp, ledger_state->slots[i].credits_pending);
		ledger_sum_add(&sum_d, ledger_state->slots[i].debits_posted);
		ledger_sum_add(&sum_c, ledger_state->slots[i].credits_posted);
	}
	LWLockRelease(ledger_lock);
	elems[0] = ledger_amount_to_numeric((ledger_amount) nacc);
	elems[1] = PointerGetDatum(ledger_sum_to_numeric(&sum_dp));
	elems[2] = PointerGetDatum(ledger_sum_to_numeric(&sum_cp));
	elems[3] = PointerGetDatum(ledger_sum_to_numeric(&sum_d));
	elems[4] = PointerGetDatum(ledger_sum_to_numeric(&sum_c));
	PG_RETURN_ARRAYTYPE_P(construct_array(elems, 5, NUMERICOID, -1, false, TYPALIGN_INT));
}

/*
 * ledger_balance(id uuid) -> bigint[] {debits_pending, credits_pending,
 *                                       debits_posted, credits_posted,
 *                                       flags, code, ledger}
 */
PG_FUNCTION_INFO_V1(ledger_balance);
Datum
ledger_balance(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *id;
	int64		slot;
	Datum		elems[7];
	uint16		tenant;

	id = PG_GETARG_UUID_P(0);

	require_ready();
	tenant = ledger_tenant_current();
	LWLockAcquire(ledger_lock, LW_SHARED);
	slot = ledger_find_account(ledger_state, id->data);
	if (slot < 0 || ledger_state->slots[slot].tenant != tenant)
	{
		LWLockRelease(ledger_lock);
		PG_RETURN_NULL();
	}
	elems[0] = ledger_amount_to_numeric(ledger_state->slots[slot].debits_pending);
	elems[1] = ledger_amount_to_numeric(ledger_state->slots[slot].credits_pending);
	elems[2] = ledger_amount_to_numeric(ledger_state->slots[slot].debits_posted);
	elems[3] = ledger_amount_to_numeric(ledger_state->slots[slot].credits_posted);
	elems[4] = ledger_amount_to_numeric((ledger_amount) ledger_state->slots[slot].flags);
	elems[5] = ledger_amount_to_numeric((ledger_amount) ledger_state->slots[slot].code);
	elems[6] = ledger_amount_to_numeric((ledger_amount) ledger_state->slots[slot].ledger);
	LWLockRelease(ledger_lock);
	PG_RETURN_ARRAYTYPE_P(construct_array(elems, 7, NUMERICOID, -1, false, TYPALIGN_INT));
}

/*
 * ledger_lookup_transfer(id uuid, OUT debit uuid, OUT credit uuid, OUT amount
 *                        bigint, OUT flags int, OUT code int, OUT ledger int,
 *                        OUT state int)
 */
PG_FUNCTION_INFO_V1(ledger_lookup_transfer);
Datum
ledger_lookup_transfer(PG_FUNCTION_ARGS)
{
	pg_uuid_t  *id;
	TupleDesc	tupdesc;
	int64		idx;
	Datum		values[7];
	bool		nulls[7] = {false, false, false, false, false, false, false};
	pg_uuid_t  *du,
			   *cu;
	uint16		tenant;

	id = PG_GETARG_UUID_P(0);

	require_ready();
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	du = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
	cu = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
	tenant = ledger_tenant_current();
	LWLockAcquire(ledger_lock, LW_SHARED);
	idx = ledger_find_transfer(ledger_state, id->data);
	if (idx < 0 || ledger_state->transfers[idx].tenant != tenant)
	{
		LWLockRelease(ledger_lock);
		PG_RETURN_NULL();
	}
	memcpy(du->data, ledger_state->slots[ledger_state->transfers[idx].debit_slot].id.data, 16);
	memcpy(cu->data, ledger_state->slots[ledger_state->transfers[idx].credit_slot].id.data, 16);
	values[0] = UUIDPGetDatum(du);
	values[1] = UUIDPGetDatum(cu);
	values[2] = ledger_amount_to_numeric(ledger_state->transfers[idx].amount);
	values[3] = Int32GetDatum((int32) ledger_state->transfers[idx].flags);
	values[4] = Int32GetDatum((int32) ledger_state->transfers[idx].code);
	values[5] = Int32GetDatum((int32) ledger_state->transfers[idx].ledger);
	values[6] = Int32GetDatum((int32) ledger_state->transfers[idx].state);
	LWLockRelease(ledger_lock);
	PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(tupdesc, values, nulls)));
}

/*
 * ledger_pending_transfers() -> setof (id uuid, debit uuid, credit uuid,
 *                                      amount bigint, code int, ledger int,
 *                                      timeout int)
 */
typedef struct PendingRow
{
	pg_uuid_t	id;
	pg_uuid_t	debit;
	pg_uuid_t	credit;
	ledger_amount amount;
	int32		code;
	int32		ledger;
	int32		timeout;
} PendingRow;

PG_FUNCTION_INFO_V1(ledger_pending_transfers);
Datum
ledger_pending_transfers(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	require_ready();

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		PendingRow *rows;
		size_t		i,
					n = 0;
		uint16		tenant;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		(void) get_call_result_type(fcinfo, NULL, &tupdesc);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		tenant = ledger_tenant_current();

		LWLockAcquire(ledger_lock, LW_SHARED);
		for (i = 0; i < ledger_state->ntransfers; i++)
			if (ledger_state->transfers[i].state == TSTATE_PENDING &&
				ledger_state->transfers[i].tenant == tenant)
				n++;
		rows = (PendingRow *) palloc0(Max(n, 1) * sizeof(PendingRow));
		n = 0;
		for (i = 0; i < ledger_state->ntransfers; i++)
		{
			LedgerTransfer *t = &ledger_state->transfers[i];

			if (t->state != TSTATE_PENDING || t->tenant != tenant)
				continue;
			memcpy(rows[n].id.data, t->id, 16);
			memcpy(rows[n].debit.data, ledger_state->slots[t->debit_slot].id.data, 16);
			memcpy(rows[n].credit.data, ledger_state->slots[t->credit_slot].id.data, 16);
			rows[n].amount = t->amount;
			rows[n].code = (int32) t->code;
			rows[n].ledger = (int32) t->ledger;
			rows[n].timeout = (int32) t->timeout;
			n++;
		}
		LWLockRelease(ledger_lock);
		funcctx->user_fctx = (void *) rows;
		funcctx->max_calls = n;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	if (funcctx->call_cntr < funcctx->max_calls)
	{
		PendingRow *rows = (PendingRow *) funcctx->user_fctx;
		PendingRow *r = &rows[funcctx->call_cntr];
		Datum		values[7];
		bool		nulls[7] = {false, false, false, false, false, false, false};

		values[0] = UUIDPGetDatum(&r->id);
		values[1] = UUIDPGetDatum(&r->debit);
		values[2] = UUIDPGetDatum(&r->credit);
		values[3] = ledger_amount_to_numeric(r->amount);
		values[4] = Int32GetDatum(r->code);
		values[5] = Int32GetDatum(r->ledger);
		values[6] = Int32GetDatum(r->timeout);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
																	 values, nulls)));
	}
	SRF_RETURN_DONE(funcctx);
}

/*
 * ledger_expiry_due() -> bigint: how many pending transfers have expired
 * and have not been released yet.  It is a reading aid: the number falls to
 * zero once the server has caught up.
 *
 * The count comes from the expiry heap, which holds only the pending
 * transfers that asked for a timeout, so it does not walk the whole
 * transfer table.
 */
PG_FUNCTION_INFO_V1(ledger_expiry_due);
Datum
ledger_expiry_due(PG_FUNCTION_ARGS)
{
	LedgerState *s = ledger_state;
	uint64		now;
	int64		n = 0;
	uint32		i;

	require_ready();

	now = ledger_now_ns();
	LWLockAcquire(ledger_lock, LW_SHARED);
	for (i = 0; i < s->nexpiry; i++)
	{
		const LedgerTransfer *t = &s->transfers[s->expiry[i]];

		if (!ledger_is_timed_pending(t))
			continue;		/* it was resolved before it expired */
		if (ledger_expires_at(t) <= now)
			n++;
	}
	LWLockRelease(ledger_lock);
	PG_RETURN_INT64(n);
}

/* ledger_snapshot() -> pg_lsn: writes a snapshot of the grid to disk */
PG_FUNCTION_INFO_V1(ledger_snapshot);
Datum
ledger_snapshot(PG_FUNCTION_ARGS)
{
	require_ready();
	PG_RETURN_LSN(ledger_do_snapshot(true));
}

/* ------------------------------------------------------------------ */
/* the journal                                                         */

/*
 * The row every journal query returns:
 *   seq, timestamp, id, pending_id, debit, credit, amount, amount_actual,
 *   ledger, code, flags, state, period, kind
 */
/* a period of the journal */
typedef struct PeriodRow
{
	int32		period;
	int64		close_seq;
	int64		records;
} PeriodRow;

typedef struct JournalRow
{
	uint64		seq;
	int64		timestamp;
	pg_uuid_t	id;
	pg_uuid_t	pending_id;
	pg_uuid_t	debit;
	pg_uuid_t	credit;
	ledger_amount amount;
	ledger_amount amount_actual;
	int32		ledger;
	int32		code;
	int32		flags;
	int32		state;
	int32		period;
	int32		kind;
} JournalRow;

#define JOURNAL_NCOLS 14

static void
journal_row_from(JournalRow *r, const JournalRecord *rec)
{
	memset(r, 0, sizeof(*r));
	r->seq = rec->seq;
	r->timestamp = (int64) rec->timestamp;
	memcpy(r->id.data, rec->id, 16);
	memcpy(r->pending_id.data, rec->pending_id, 16);
	memcpy(r->debit.data, rec->debit, 16);
	memcpy(r->credit.data, rec->credit, 16);
	r->amount = rec->amount;
	r->amount_actual = rec->amount_actual;
	r->ledger = (int32) rec->ledger;
	r->code = (int32) rec->code;
	/* the book's number is not the client's business: it is masked off */
	r->flags = (int32) (rec->flags & ~LEDGER_TENANT_MASK);
	r->state = (int32) rec->state;
	r->period = (int32) rec->period;
	r->kind = (int32) rec->kind;
}

/*
 * The shared body of every set-returning journal function.
 *
 * A set-returning function's body runs ONCE PER ROW: the executor calls it
 * again for the next row.  So the work of finding the rows -- walking the
 * chains, scanning the accounts -- must happen in the first call only.  If
 * it did not, a thousand-row answer would walk every chain a thousand
 * times, and a by-ledger query over a large ledger would take minutes.
 *
 * So the shape is always the same: the first call gathers the sequence
 * numbers and turns them into rows, in the multi-call context, and the
 * later calls just hand out the rows.
 */
typedef void (*JournalGatherFn)(FunctionCallInfo fcinfo, void *arg,
								uint64 **seqs, int *n);

/* the first call: gather the rows, once */
static FuncCallContext *
journal_firstcall(FunctionCallInfo fcinfo, JournalGatherFn gather, void *arg)
{
	FuncCallContext *funcctx;
	MemoryContext oldcontext;
	TupleDesc	tupdesc;
	uint64	   *seqs = NULL;
	int			n = 0;

	funcctx = SRF_FIRSTCALL_INIT();
	oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
	gather(fcinfo, arg, &seqs, &n);
	(void) get_call_result_type(fcinfo, NULL, &tupdesc);
	funcctx->tuple_desc = BlessTupleDesc(tupdesc);
	funcctx->user_fctx = seqs;
	funcctx->max_calls = n;
	MemoryContextSwitchTo(oldcontext);
	return funcctx;
}

/* the later calls: hand out the rows that were gathered */
static Datum
journal_next(FunctionCallInfo fcinfo, FuncCallContext *funcctx)
{
	while (funcctx->call_cntr < funcctx->max_calls)
	{
		uint64	   *seqs = (uint64 *) funcctx->user_fctx;
		JournalRecord rec;
		JournalRow r;
		Datum		values[JOURNAL_NCOLS];
		bool		nulls[JOURNAL_NCOLS];

		/* a record that cannot be read is skipped, never served as an empty row */
		if (!ledger_journal_read(seqs[funcctx->call_cntr], &rec))
		{
			funcctx->call_cntr++;
			continue;
		}
		journal_row_from(&r, &rec);

		memset(nulls, 0, sizeof(nulls));
		values[0] = Int64GetDatum((int64) r.seq);
		values[1] = Int64GetDatum(r.timestamp);
		values[2] = UUIDPGetDatum(&r.id);
		values[3] = UUIDPGetDatum(&r.pending_id);
		values[4] = UUIDPGetDatum(&r.debit);
		values[5] = UUIDPGetDatum(&r.credit);
		values[6] = ledger_amount_to_numeric(r.amount);
		values[7] = ledger_amount_to_numeric(r.amount_actual);
		values[8] = Int32GetDatum(r.ledger);
		values[9] = Int32GetDatum(r.code);
		values[10] = Int32GetDatum(r.flags);
		values[11] = Int32GetDatum(r.state);
		values[12] = Int32GetDatum(r.period);
		values[13] = Int32GetDatum(r.kind);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
																	values, nulls)));
	}
	SRF_RETURN_DONE(funcctx);
}

/* the limit of a query: a negative limit means no limit */
static int
journal_limit(FunctionCallInfo fcinfo, int arg)
{
	int32		lim;

	if (PG_NARGS() <= arg || PG_ARGISNULL(arg))
		return -1;
	lim = PG_GETARG_INT32(arg);
	return lim < 0 ? -1 : lim;
}

/* ledger_journal_lookup(id uuid) -> the transfer's own record and the record
 * of the post or void that resolved it */
static void
gather_lookup(FunctionCallInfo fcinfo, void *arg, uint64 **seqs, int *n)
{
	pg_uuid_t  *id = (pg_uuid_t *) arg;
	uint64	   *list = (uint64 *) palloc(2 * sizeof(uint64));
	int64		idx;
	uint16		tenant;

	*n = 0;
	tenant = ledger_tenant_current();
	LWLockAcquire(ledger_lock, LW_SHARED);
	idx = ledger_find_transfer(ledger_state, id->data);
	if (idx >= 0 && ledger_state->transfers[idx].tenant == tenant)
	{
		list[(*n)++] = ledger_state->transfers[idx].seq;
		if (ledger_state->transfers[idx].post_seq != 0)
			list[(*n)++] = ledger_state->transfers[idx].post_seq;
	}
	LWLockRelease(ledger_lock);
	*seqs = list;
}

PG_FUNCTION_INFO_V1(ledger_journal_lookup);
Datum
ledger_journal_lookup(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	pg_uuid_t		id;

	memcpy(id.data, PG_GETARG_UUID_P(0)->data, 16);

	require_ready();

	if (SRF_IS_FIRSTCALL())
		funcctx = journal_firstcall(fcinfo, gather_lookup, &id);
	else
		funcctx = SRF_PERCALL_SETUP();
	PG_RETURN_DATUM(journal_next(fcinfo, funcctx));
}

/* ledger_journal_account(id uuid, lim int) -> the records that named it */
static void
gather_account(FunctionCallInfo fcinfo, void *arg, uint64 **seqs, int *n)
{
	pg_uuid_t  *id = (pg_uuid_t *) arg;
	int			lim = journal_limit(fcinfo, 1);
	uint64	   *list = NULL;
	int			count = 0;
	uint16		tenant;

	tenant = ledger_tenant_current();
	LWLockAcquire(ledger_lock, LW_SHARED);
	ledger_journal_by_account(ledger_state, id->data, tenant, &list, &count);
	LWLockRelease(ledger_lock);

	if (lim >= 0 && count > lim)
		count = lim;
	*seqs = list;
	*n = count;
}

PG_FUNCTION_INFO_V1(ledger_journal_account);
Datum
ledger_journal_account(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	pg_uuid_t		id;

	memcpy(id.data, PG_GETARG_UUID_P(0)->data, 16);

	require_ready();

	if (SRF_IS_FIRSTCALL())
		funcctx = journal_firstcall(fcinfo, gather_account, &id);
	else
		funcctx = SRF_PERCALL_SETUP();
	PG_RETURN_DATUM(journal_next(fcinfo, funcctx));
}

/* ledger_journal_ledger(ledger int, lim int) -> the records of a ledger */
static void
gather_ledger(FunctionCallInfo fcinfo, void *arg, uint64 **seqs, int *n)
{
	uint32		led = *(uint32 *) arg;
	int			lim = journal_limit(fcinfo, 1);
	uint64	   *list = NULL;
	int			count = 0;
	uint16		tenant;

	tenant = ledger_tenant_current();
	LWLockAcquire(ledger_lock, LW_SHARED);
	ledger_journal_by_ledger(ledger_state, led, tenant, &list, &count);
	LWLockRelease(ledger_lock);

	if (lim >= 0 && count > lim)
		count = lim;
	*seqs = list;
	*n = count;
}

PG_FUNCTION_INFO_V1(ledger_journal_ledger);
Datum
ledger_journal_ledger(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	uint32		led = (uint32) PG_GETARG_INT32(0);

	require_ready();

	if (SRF_IS_FIRSTCALL())
		funcctx = journal_firstcall(fcinfo, gather_ledger, &led);
	else
		funcctx = SRF_PERCALL_SETUP();
	PG_RETURN_DATUM(journal_next(fcinfo, funcctx));
}

/* ledger_journal_range(from_ts bigint, to_ts bigint, lim int) */
static void
gather_range(FunctionCallInfo fcinfo, void *arg, uint64 **seqs, int *n)
{
	int64	   *range = (int64 *) arg;
	int64		from_ts = range[0],
				to_ts = range[1];
	int			lim = journal_limit(fcinfo, 2);
	uint64		first,
				last;
	uint64	   *list;
	int			count = 0;
	int			lim_eff;
	uint64		seq;
	uint16		tenant;

	if (to_ts < from_ts)
	{
		*seqs = NULL;
		*n = 0;
		return;
	}

	tenant = ledger_tenant_current();
	lim_eff = (lim >= 0) ? lim : (int) (ledger_journal_nrecords() + 1);

	first = ledger_journal_seek((uint64) from_ts);
	last = ledger_journal_nrecords();

	/* one pass: fill the list as it goes, up to the limit */
	{
		int			cap = (lim >= 0 && lim < 1024) ? lim + 1 : 1024;

		list = (uint64 *) palloc(Max(cap, 1) * sizeof(uint64));
		count = 0;
		for (seq = first; seq <= last && count < lim_eff; seq++)
		{
			JournalRecord rec;

			if (!ledger_journal_read(seq, &rec))
				break;
			if (rec.timestamp > (uint64) to_ts)
				break;
			if (!ledger_record_visible(&rec, tenant))
				continue;	/* it belongs to another book */
			if (count == cap)
			{
				cap *= 2;
				list = (uint64 *) repalloc(list, Max(cap, 1) * sizeof(uint64));
			}
			list[count++] = seq;
		}
	}
	*seqs = list;
	*n = count;
}

PG_FUNCTION_INFO_V1(ledger_journal_range);
Datum
ledger_journal_range(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	int64		range[2];

	range[0] = PG_GETARG_INT64(0);
	range[1] = PG_GETARG_INT64(1);

	require_ready();

	if (SRF_IS_FIRSTCALL())
		funcctx = journal_firstcall(fcinfo, gather_range, range);
	else
		funcctx = SRF_PERCALL_SETUP();
	PG_RETURN_DATUM(journal_next(fcinfo, funcctx));
}

/*
 * ledger_journal_records(from_seq bigint, lim int) -> bytea
 *
 * The journal's records after a position, as they are on disk.  This is
 * what a replica fetches: the records are the ledger's truth, and the
 * replica applies them with the same code the primary does.
 */
PG_FUNCTION_INFO_V1(sql_ledger_journal_records);
Datum
sql_ledger_journal_records(PG_FUNCTION_ARGS)
{
	int64		from_seq = PG_GETARG_INT64(0);
	int32		lim = PG_GETARG_INT32(1);
	uint8	   *buf;
	int			bytes;
	bytea	   *out;

	require_ready();

	if (from_seq < 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("ledgerext: the position cannot be negative")));
	if (lim <= 0)
	{
		out = (bytea *) palloc(VARHDRSZ);
		SET_VARSIZE(out, VARHDRSZ);
		PG_RETURN_BYTEA_P(out);
	}
	if (lim > 4096)
		lim = 4096;			/* the most a replica may ask for at a time */

	buf = (uint8 *) palloc((size_t) lim * sizeof(JournalRecord));
	bytes = ledger_journal_records_bytes(ledger_state, (uint64) from_seq, lim, buf);
	if (bytes <= 0)
	{
		out = (bytea *) palloc(VARHDRSZ);
		SET_VARSIZE(out, VARHDRSZ);
		PG_RETURN_BYTEA_P(out);
	}

	out = (bytea *) palloc(VARHDRSZ + bytes);
	SET_VARSIZE(out, VARHDRSZ + bytes);
	memcpy(VARDATA(out), buf, bytes);

	PG_RETURN_BYTEA_P(out);
}

/*
 * ledger_replica_ack(name text, seq bigint) -> void
 *
 * A replica's acknowledgement: the position it has applied and made
 * durable.  The first acknowledgement registers the replica, so the
 * primary waits for it from then on.
 */
PG_FUNCTION_INFO_V1(sql_ledger_replica_ack);
Datum
sql_ledger_replica_ack(PG_FUNCTION_ARGS)
{
	text	   *name = PG_GETARG_TEXT_PP(0);
	int64		seq = PG_GETARG_INT64(1);
	char	   *cname;

	require_ready();

	if (seq < 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("ledgerext: the position cannot be negative")));

	cname = text_to_cstring(name);
	ledger_replica_ack(cname, (uint64) seq);

	PG_RETURN_VOID();
}

/* ledger_journal_count() -> bigint */
PG_FUNCTION_INFO_V1(sql_ledger_journal_count);
Datum
sql_ledger_journal_count(PG_FUNCTION_ARGS)
{
	require_ready();
	PG_RETURN_INT64((int64) ledger_journal_nrecords());
}

/* ledger_journal_balances() -> the balances the journal implies */
PG_FUNCTION_INFO_V1(ledger_journal_balances);
Datum
ledger_journal_balances(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	require_ready();

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		int			nids = 0;
		BalanceRow *rows;
		uint16		tenant;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		(void) get_call_result_type(fcinfo, NULL, &tupdesc);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		tenant = ledger_tenant_current();
		rows = ledger_journal_replay(ledger_state, 0, tenant, &nids);
		funcctx->user_fctx = rows;
		funcctx->max_calls = nids;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	if (funcctx->call_cntr < funcctx->max_calls)
	{
		BalanceRow *r = &((BalanceRow *) funcctx->user_fctx)[funcctx->call_cntr];
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};

		values[0] = UUIDPGetDatum(&r->id);
		values[1] = ledger_amount_to_numeric(r->dp);
		values[2] = ledger_amount_to_numeric(r->cp);
		values[3] = ledger_amount_to_numeric(r->d);
		values[4] = ledger_amount_to_numeric(r->c);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
																	values, nulls)));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ledger_close_period() -> int: the number of the period it closes */
PG_FUNCTION_INFO_V1(ledger_close_period);
Datum
ledger_close_period(PG_FUNCTION_ARGS)
{
	require_ready();

	/*
	 * The close is queued and applied when the transaction commits, so the
	 * number returned is the period that is open now, which is the one the
	 * close will end.
	 */
	{
		int32		period;

		/*
		 * Closing a period writes to the ledger, so a role that owns no
		 * book may not do it.  The close is the cluster's own business:
		 * its record carries no book, so every book sees it, and the
		 * tenant is resolved only to know that the role owns one.
		 */
		ledger_tenant_require();
		LWLockAcquire(ledger_lock, LW_SHARED);
		period = (int32) ledger_state->period + ledger_queued_closes();
		LWLockRelease(ledger_lock);
		ledger_queue_close();
		PG_RETURN_INT32(period);
	}
}

/* ledger_journal_periods() -> (period int, close_seq bigint, records bigint) */
PG_FUNCTION_INFO_V1(ledger_journal_periods);
Datum
ledger_journal_periods(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	require_ready();

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		PeriodRow  *rows;
		uint64		nrecords = ledger_journal_nrecords();
		uint64		seq,
					close = 0;
		int			n = 0,
					cap = 16;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		(void) get_call_result_type(fcinfo, NULL, &tupdesc);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		rows = (PeriodRow *) palloc0(cap * sizeof(PeriodRow));

		/* the periods are the runs of records between two closes */
		for (seq = 1; seq <= nrecords; seq++)
		{
			JournalRecord rec;

			if (!ledger_journal_read(seq, &rec))
				break;
			if (rec.kind != JOURNAL_CLOSE)
				continue;
			if (n == cap)
			{
				cap *= 2;
				rows = (PeriodRow *) repalloc(rows, cap * sizeof(PeriodRow));
				memset(rows + n, 0, (size_t) (cap - n) * sizeof(PeriodRow));
			}
			rows[n].period = (int32) rec.period;
			rows[n].close_seq = (int64) rec.seq;
			rows[n].records = (int64) (rec.seq - close);
			close = rec.seq;
			n++;
		}

		/* the period that is still open */
		if (nrecords > close)
		{
			if (n == cap)
			{
				cap *= 2;
				rows = (PeriodRow *) repalloc(rows, cap * sizeof(PeriodRow));
				memset(rows + n, 0, (size_t) (cap - n) * sizeof(PeriodRow));
			}
			rows[n].period = (int32) ledger_state->period;
			rows[n].close_seq = 0;
			rows[n].records = (int64) (nrecords - close);
			n++;
		}

		funcctx->user_fctx = rows;
		funcctx->max_calls = n;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	if (funcctx->call_cntr < funcctx->max_calls)
	{
		PeriodRow *r = &((PeriodRow *) funcctx->user_fctx)[funcctx->call_cntr];
		Datum		values[3];
		bool		nulls[3] = {false, false, false};

		values[0] = Int32GetDatum(r->period);
		values[1] = Int64GetDatum(r->close_seq);
		values[2] = Int64GetDatum(r->records);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
																	values, nulls)));
	}
	SRF_RETURN_DONE(funcctx);
}

/* ledger_period_balances(period int) -> the balances at the close of a period */
PG_FUNCTION_INFO_V1(ledger_period_balances);
Datum
ledger_period_balances(PG_FUNCTION_ARGS)
{
	int32		period = PG_GETARG_INT32(0);
	FuncCallContext *funcctx;

	require_ready();

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc	tupdesc;
		int			nids = 0;
		BalanceRow *rows;
		uint64		nrecords = ledger_journal_nrecords();
		uint64		seq,
					close = 0,
					upto = 0;
		uint16		tenant;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		(void) get_call_result_type(fcinfo, NULL, &tupdesc);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		tenant = ledger_tenant_current();

		/* the close of the period, or the end of the journal */
		for (seq = 1; seq <= nrecords; seq++)
		{
			JournalRecord rec;

			if (!ledger_journal_read(seq, &rec))
				break;
			if (rec.kind == JOURNAL_CLOSE && rec.period == (uint32) period)
			{
				close = rec.seq;
				break;
			}
		}
		if (close == 0)
		{
			/* only the open period has no close; any other number is not a period */
			if (period < 1 || (uint64) period != ledger_state->period)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("ledger: period %d does not exist", period)));
			upto = nrecords;
		}
		else
			upto = close;

		rows = ledger_journal_replay(ledger_state, upto, tenant, &nids);
		funcctx->user_fctx = rows;
		funcctx->max_calls = nids;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	if (funcctx->call_cntr < funcctx->max_calls)
	{
		BalanceRow *r = &((BalanceRow *) funcctx->user_fctx)[funcctx->call_cntr];
		Datum		values[5];
		bool		nulls[5] = {false, false, false, false, false};

		values[0] = UUIDPGetDatum(&r->id);
		values[1] = ledger_amount_to_numeric(r->dp);
		values[2] = ledger_amount_to_numeric(r->cp);
		values[3] = ledger_amount_to_numeric(r->d);
		values[4] = ledger_amount_to_numeric(r->c);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(heap_form_tuple(funcctx->tuple_desc,
																	values, nulls)));
	}
	SRF_RETURN_DONE(funcctx);
}
