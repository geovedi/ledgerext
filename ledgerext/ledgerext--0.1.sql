-- ledgerext 0.1: a double-entry ledger inside PostgreSQL,
-- with a journal, pending transfers that expire, 128-bit amounts, and a
-- tenant per user.
--
-- The ledger is a set of books, one per tenant, and a role's tenant says
-- which book is its own.  The administrator binds a role with a row:
--
--   INSERT INTO ledger_tenants VALUES ('acme', 1);
--
-- The tenant is 16 bits, and it is the administrator's: it is in the
-- journal, and a number that changed would move an account from one book
-- to another.  A role with no row has no book at all: it may not move
-- money and it may not read the ledger.
--
-- The client never sends a tenant, and there is nowhere in the wire
-- format to put one: the server stamps the tenant on the account it
-- creates and the transfer it applies, and a write that names another
-- tenant's account is refused with the status 102,
-- account_in_another_tenant.
--
-- A batch is a PackedBatch header (count, kind) followed by that many
-- events, where kind 0 is transfers and kind 1 is accounts.
--
-- ledger_apply() and ledger_create_accounts() only queue the work; it is
-- validated, WAL-logged, journaled and applied when the transaction
-- commits.  Every event then has a status, read with ledger_last_statuses().
--
-- The journal holds every committed transfer and account creation, in
-- commit order, and nothing ever removes or alters one of its records.
-- ledger_reset() is gone: the books are closed, not wiped.
--
-- A pending transfer may carry a timeout in seconds.  When the timeout
-- passes, the server releases the reservation itself and journals the
-- release, so nobody has to void the transfer and the balances stay
-- reconstructible from the journal alone.
--
-- The ledger's records live in the journal alone, and the journal is made
-- durable before the transaction is acknowledged.  A second node can
-- follow this one: ledger_journal_records() gives a replica the records
-- after a position, and ledger_replica_ack() tells this node how far the
-- replica has got.  A node with ledgerext.replica_of set is a replica: it
-- follows another node, it decides nothing, and it applies no transfers of
-- its own until it is promoted.
--
-- Every amount is 128-bit unsigned, from 0 to 2^128-1, as
-- the ledger's own amounts are.  PostgreSQL has no 128-bit integer, so the amounts the
-- surface reports are numeric, which holds every value of the range
-- exactly.  ledger_amount(numeric) gives a client the 16 bytes of a decimal
-- amount, so it never has to write bytes by hand.

CREATE FUNCTION ledger_apply(bytea) RETURNS bigint
  AS 'MODULE_PATHNAME', 'ledger_apply' STRICT LANGUAGE C VOLATILE;
CREATE FUNCTION ledger_create_accounts(bytea) RETURNS bigint
  AS 'MODULE_PATHNAME', 'ledger_create_accounts' STRICT LANGUAGE C VOLATILE;
CREATE FUNCTION ledger_create_account(uuid, int DEFAULT 0, int DEFAULT 0, int DEFAULT 1) RETURNS void
  AS 'MODULE_PATHNAME', 'ledger_create_account' STRICT LANGUAGE C VOLATILE;
CREATE FUNCTION ledger_last_statuses() RETURNS bigint[]
  AS 'MODULE_PATHNAME', 'ledger_last_statuses' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_status_name(bigint) RETURNS text
  AS 'MODULE_PATHNAME', 'ledger_status_name' STRICT LANGUAGE C IMMUTABLE;
CREATE FUNCTION ledger_account_status_name(bigint) RETURNS text
  AS 'MODULE_PATHNAME', 'ledger_account_status_name' STRICT LANGUAGE C IMMUTABLE;
-- The 16 bytes of a decimal amount, low half first, so a client can build
-- a batch without writing bytes by hand.  A negative amount, or one above
-- 2^128-1, is an error.
CREATE FUNCTION ledger_amount(numeric) RETURNS bytea
  AS 'MODULE_PATHNAME', 'sql_ledger_amount' STRICT LANGUAGE C IMMUTABLE;
CREATE FUNCTION ledger_account_count() RETURNS bigint
  AS 'MODULE_PATHNAME', 'ledger_account_count' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_transfer_count() RETURNS bigint
  AS 'MODULE_PATHNAME', 'ledger_transfer_count' STRICT LANGUAGE C STABLE;
-- How close the ledger is to the size this node's memory holds: the
-- accounts and the transfers used, and the limits the grid was created
-- with.  The grid's size is fixed when the server starts, so this is
-- where an operator looks before the limit refuses a transfer.
CREATE FUNCTION ledger_capacity() RETURNS bigint[]
  AS 'MODULE_PATHNAME', 'ledger_capacity' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_totals() RETURNS numeric[]
  AS 'MODULE_PATHNAME', 'ledger_totals' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_ledger_totals(int) RETURNS numeric[]
  AS 'MODULE_PATHNAME', 'ledger_ledger_totals' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_balance(uuid) RETURNS numeric[]
  AS 'MODULE_PATHNAME', 'ledger_balance' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_lookup_transfer(id uuid, OUT debit uuid, OUT credit uuid, OUT amount numeric,
                                       OUT flags int, OUT code int, OUT ledger int, OUT state int)
  AS 'MODULE_PATHNAME', 'ledger_lookup_transfer' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_pending_transfers() RETURNS TABLE(id uuid, debit uuid, credit uuid,
                                                          amount numeric, code int, ledger int,
                                                          timeout int)
  AS 'MODULE_PATHNAME', 'ledger_pending_transfers' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_expiry_due() RETURNS bigint
  AS 'MODULE_PATHNAME', 'ledger_expiry_due' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_snapshot() RETURNS pg_lsn
  AS 'MODULE_PATHNAME', 'ledger_snapshot' STRICT LANGUAGE C VOLATILE;

-- the journal

CREATE FUNCTION ledger_journal_lookup(id uuid)
  RETURNS TABLE(seq bigint, ts bigint, id uuid, pending_id uuid,
                debit uuid, credit uuid, amount numeric, amount_actual numeric,
                ledger int, code int, flags int, state int, period int, kind int)
  AS 'MODULE_PATHNAME', 'ledger_journal_lookup' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_journal_account(id uuid, lim int DEFAULT 1000)
  RETURNS TABLE(seq bigint, ts bigint, id uuid, pending_id uuid,
                debit uuid, credit uuid, amount numeric, amount_actual numeric,
                ledger int, code int, flags int, state int, period int, kind int)
  AS 'MODULE_PATHNAME', 'ledger_journal_account' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_journal_ledger(ledger int, lim int DEFAULT 1000)
  RETURNS TABLE(seq bigint, ts bigint, id uuid, pending_id uuid,
                debit uuid, credit uuid, amount numeric, amount_actual numeric,
                ledger int, code int, flags int, state int, period int, kind int)
  AS 'MODULE_PATHNAME', 'ledger_journal_ledger' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_journal_range(from_ts bigint, to_ts bigint, lim int DEFAULT 1000)
  RETURNS TABLE(seq bigint, ts bigint, id uuid, pending_id uuid,
                debit uuid, credit uuid, amount numeric, amount_actual numeric,
                ledger int, code int, flags int, state int, period int, kind int)
  AS 'MODULE_PATHNAME', 'ledger_journal_range' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_journal_count() RETURNS bigint
  AS 'MODULE_PATHNAME', 'sql_ledger_journal_count' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_journal_records(from_seq bigint, lim int) RETURNS bytea
  AS 'MODULE_PATHNAME', 'sql_ledger_journal_records' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_replica_ack(name text, seq bigint) RETURNS void
  AS 'MODULE_PATHNAME', 'sql_ledger_replica_ack' STRICT LANGUAGE C VOLATILE;
CREATE FUNCTION ledger_journal_balances()
  RETURNS TABLE(id uuid, debits_pending numeric, credits_pending numeric,
                debits_posted numeric, credits_posted numeric)
  AS 'MODULE_PATHNAME', 'ledger_journal_balances' STRICT LANGUAGE C STABLE;
CREATE FUNCTION ledger_journal_periods()
  RETURNS TABLE(period int, close_seq bigint, records bigint)
  AS 'MODULE_PATHNAME', 'ledger_journal_periods' STRICT LANGUAGE C STABLE;

-- closing the books

CREATE FUNCTION ledger_close_period() RETURNS int
  AS 'MODULE_PATHNAME', 'ledger_close_period' STRICT LANGUAGE C VOLATILE;
CREATE FUNCTION ledger_period_balances(period int)
  RETURNS TABLE(id uuid, debits_pending numeric, credits_pending numeric,
                debits_posted numeric, credits_posted numeric)
  AS 'MODULE_PATHNAME', 'ledger_period_balances' STRICT LANGUAGE C STABLE;

-- Who may use the ledger
--
-- The ledger is a set of books, one per tenant, and a role's tenant says
-- which book is its own: ledger_apply() moves money in that book and the
-- reads show that book, not only the caller's.  PostgreSQL grants EXECUTE
-- on a new function to PUBLIC, which would hand the whole book to every
-- role that can connect to the database.  So nothing is executable by
-- PUBLIC here: the administrator grants EXECUTE, function by function or for
-- the whole set, to the roles that may use the ledger.

-- The tenants: which book a role owns.
--
-- One row per role, and the row is the authority: a role with a row may
-- move money in that book and read it, and a role without a row may do
-- neither - it has no book at all.  Only the owner of this table writes
-- it, which is the administrator: a session setting would not do,
-- because a role may alter its own role's settings, and a setting the
-- role can change is a setting it can walk out of.
--
-- The table is the extension's own, and the extension reads it itself: a
-- role is told only its own tenant, and never another role's.  Every role
-- may read the table, so a role can look up its own binding; only the
-- owner writes it.  The rows hold role names - which pg_roles shows
-- anyway - and tenant numbers, which mean nothing to a role that is not
-- bound to them.
CREATE TABLE ledger_tenants (
  role_name name PRIMARY KEY,
  tenant    int  NOT NULL CHECK (tenant BETWEEN 1 AND 65535)
);
GRANT SELECT ON ledger_tenants TO PUBLIC;

DO $$
DECLARE
  f record;
BEGIN
  FOR f IN
    SELECT p.oid::regprocedure AS sig
      FROM pg_proc p
      JOIN pg_depend d ON d.objid = p.oid
     WHERE d.refclassid = 'pg_extension'::regclass
       AND d.refobjid = (SELECT oid FROM pg_extension WHERE extname = 'ledgerext')
  LOOP
    EXECUTE format('REVOKE EXECUTE ON FUNCTION %s FROM PUBLIC', f.sig);
  END LOOP;
END
$$;
