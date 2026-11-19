MODULE_big = ledgerext
OBJS = ledgerext.o ledger_state.o ledger_wal.o ledger_journal.o ledger_sql.o ledger_expiry.o ledger_replica.o ledger_tenant.o
DATA = ledgerext--0.1.sql
# the replica's worker connects to the primary with libpq, which the
# backend's own process already has; the headers are the client's.
PG_CPPFLAGS = -I$(includedir)
SHLIB_LINK = -lpq
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
