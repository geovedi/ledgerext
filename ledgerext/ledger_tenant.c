/*
 * ledger_tenant.c - the tenant: the book a role owns.
 *
 * The binding between a role and a tenant is a row in ledger_tenants,
 * which the administrator writes.  It is a table and not a setting
 * because a role may alter its own role's settings - ALTER ROLE <itself>
 * SET is allowed - and a setting the role can change is a setting it can
 * walk out of, into another book's money.
 *
 * The binding is read once per transaction, by the extension itself, and
 * held until the transaction ends:
 *
 *   - the commit path must not read a table under the ledger's exclusive
 *     lock, and the per-transfer path must not read one at all, so the
 *     commit path uses the tenant the transaction resolved;
 *   - a tenant that changed under a running commit could move money
 *     between books, so the transaction's tenant is fixed when it first
 *     touches the ledger.
 *
 * The table is the extension's own, and the extension reads it itself: the
 * role is told only its own tenant, and never another role's.
 */
#include "postgres.h"

#include <string.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/extension.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "ledgerext.h"

/* the tenant of this transaction; 0 means it is not resolved yet, for the
 * table's own CHECK admits no tenant of 0 */
static uint16 tenant_cached = 0;

static uint16 resolve_tenant(void);

/*
 * The session's role's tenant, resolved once per transaction.
 *
 * A role with no row in the table has no tenant and therefore no book:
 * it may not move money and it may not read the ledger, and the error
 * says which row is missing.
 */
uint16
ledger_tenant_current(void)
{
	if (tenant_cached == 0)
		tenant_cached = resolve_tenant();
	return tenant_cached;
}

/*
 * The role must own a book: the resolution raises if it does not.  This is
 * what the queueing and the closing paths want - they want the refusal, not
 * the number.
 */
void
ledger_tenant_require(void)
{
	(void) ledger_tenant_current();
}

/*
 * Forget the tenant: the transaction that resolved it is over.
 */
void
ledger_tenant_reset(void)
{
	tenant_cached = 0;
}

/*
 * Look the session's role up in the binding table.
 *
 * The lookup is an index scan on the table's primary key: the table holds
 * one row per role that owns a book, and a table of thousands of books
 * must not be walked to find one of them.  The scan is under the
 * transaction's own snapshot, so a binding made in this transaction is
 * seen, and one made by another session is not, until it commits.
 */
static uint16
resolve_tenant(void)
{
	Oid			ext,
				nsp,
				relid,
				att_role,
				att_tenant,
				idxoid;
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	NameData	rolename_data;
	ScanKeyData	key;
	char	   *rolename;
	uint16		tenant = 0;
	bool		found = false;

	ext = get_extension_oid("ledgerext", true);
	if (!OidIsValid(ext))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("ledgerext: the extension is not installed")));

	nsp = get_extension_schema(ext);
	relid = RangeVarGetRelid(makeRangeVar(get_namespace_name(nsp),
										  "ledger_tenants", -1),
							  AccessShareLock, true);
	if (!OidIsValid(relid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("ledgerext: the tenants table is missing"),
				 errhint("The extension's own table ledger_tenants was dropped. "
						 "Create the extension again to get it back.")));

	att_role = get_attnum(relid, "role_name");
	att_tenant = get_attnum(relid, "tenant");
	if (att_role == InvalidOid || att_tenant == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("ledgerext: the tenants table is not the one this "
						"version expects"),
				 errhint("The extension's own table ledger_tenants must have "
						 "a role_name and a tenant column.")));

	rel = table_open(relid, AccessShareLock);

	idxoid = RelationGetPrimaryKeyIndex(rel, true);
	if (!OidIsValid(idxoid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("ledgerext: the tenants table has no primary key"),
				 errhint("The extension's own table ledger_tenants must have a "
						 "primary key on role_name.")));

	/*
	 * The role is the session's own, not the effective one.  Inside a
	 * SECURITY DEFINER function the effective user is the function's
	 * owner, and a tenant that called such a function would be given the
	 * owner's book: it could read the owner's money and move it.  The
	 * session's role does not change inside the function, and a SET ROLE
	 * does change it - which is right, because the role that is then
	 * acting is the one that owns the book.
	 */
	rolename = GetUserNameFromId(GetOuterUserId(), false);
	memset(&rolename_data, 0, sizeof(NameData));
	strlcpy(NameStr(rolename_data), rolename, NAMEDATALEN);

	ScanKeyInit(&key, att_role, BTEqualStrategyNumber, F_NAMEEQ,
				PointerGetDatum(&rolename_data));

	scan = systable_beginscan(rel, idxoid, true, GetActiveSnapshot(), 1, &key);

	tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup))
	{
		Datum		datum;
		bool		isnull;
		int32		book;

		datum = heap_getattr(tup, att_tenant, RelationGetDescr(rel), &isnull);
		if (!isnull)
		{
			book = DatumGetInt32(datum);
			/*
			 * The table's own CHECK admits 1..65535, so this cannot fire
			 * unless the table was changed under the extension.  It is
			 * checked anyway: the cast to uint16 would fold a number over
			 * 65535 into another book's number, and 0 is the cluster's own.
			 */
			if (book < 1 || book > LEDGER_TENANT_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("ledgerext: the tenant %d is out of range",
								book),
						 errhint("A tenant is a number from 1 to %d.",
								 LEDGER_TENANT_MAX)));
			tenant = (uint16) book;
			found = true;
		}
	}
	systable_endscan(scan);
	table_close(rel, NoLock);

	if (!found)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("ledgerext: role \"%s\" has no tenant", rolename),
				 errhint("The administrator binds a role to a book with "
						 "INSERT INTO ledger_tenants VALUES ('%s', 1).",
						 rolename)));

	return tenant;
}
