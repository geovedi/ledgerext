#!/usr/bin/env bash
# Create the --cpus=2 PG container with the ledgerext extension preloaded,
# build the extension and the benchmark clients, and install everything.
#
# usage: ./scripts/setup.sh
set -eu
REPO="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$REPO/tmp"
mkdir -p "$TMPDIR"

echo "== building the extension =="
cd "$REPO/ledgerext"
PG_CONFIG=/usr/bin/pg_config make
# PGXS does not track the header: if it is newer than the shared object, the
# objects were built against the old struct layouts and LTO would mix the
# two.  Rebuild from scratch in that case.
if [ ledgerext.h -nt ledgerext.so ]; then
	echo "   (the header changed: rebuilding everything)"
	PG_CONFIG=/usr/bin/pg_config make clean
	PG_CONFIG=/usr/bin/pg_config make
fi
ls -la ledgerext.so

echo "== building the benchmark clients =="
cd "$REPO/bench"
gcc -O2 -I/usr/include/postgresql -o bench_ext bench_ext.c -lpq -lpthread
gcc -O2 -I/usr/include/postgresql -o bench_uuid bench_uuid.c -lpq
ls -la bench_ext bench_uuid

# the image's entrypoint runs a temporary server while it initialises the
# data directory; connecting to that one is a race, so wait until the
# postmaster itself is the container's first process, and then until it
# answers
waitready(){
  for i in $(seq 1 120); do
    if docker exec "$1" sh -c 'tr "\0" " " < /proc/1/cmdline | grep -q "^postgres"' 2>/dev/null; then
      if docker exec "$1" psql -U postgres -d ledger -Atc "select 1" >/dev/null 2>&1; then
        return 0
      fi
    fi
    sleep 1
  done
  echo "the container $1 did not come up" >&2
  return 1
}

echo "== creating the container =="
docker rm -f -v ledgerpg >/dev/null 2>&1 || true
docker rm -f -v ledgerpg2 >/dev/null 2>&1 || true
# the two nodes talk over the user-defined network, where the container
# names resolve; the default bridge has no name service
docker network inspect ledgernet >/dev/null 2>&1 || docker network create ledgernet >/dev/null
# the port is bound to the loopback: the container holds a throwaway ledger,
# and a published PostgreSQL is a target
docker create --name ledgerpg --cpus=2 --memory=2g --network ledgernet \
  -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=ledger -p 127.0.0.1:55432:5432 postgres:18 \
  -c shared_preload_libraries=ledgerext \
  -c wal_compression=off \
  -c max_connections=200 \
  -c fsync=on -c synchronous_commit=on \
  -c shared_buffers=128MB -c wal_buffers=16MB \
  -c checkpoint_timeout=30min -c max_wal_size=8GB -c max_replication_slots=4 -c wal_level=replica >/dev/null

# the .so must exist before the postmaster starts
docker cp "$REPO/ledgerext/ledgerext.so" ledgerpg:/usr/lib/postgresql/18/lib/ledgerext.so
docker cp "$REPO/ledgerext/ledgerext.control" ledgerpg:/usr/share/postgresql/18/extension/ledgerext.control
docker cp "$REPO/ledgerext/ledgerext--0.1.sql" ledgerpg:/usr/share/postgresql/18/extension/ledgerext--0.1.sql

docker start ledgerpg >/dev/null
waitready ledgerpg
# the extension says at startup whether the grid got its huge pages
docker logs ledgerpg 2>&1 | grep -i "custom resource manager\|huge page" || true

docker exec ledgerpg psql -U postgres -d ledger -Atc "create extension ledgerext"
# the benchmark's client connects as postgres, and a role with no binding
# has no book: the administrator binds it
docker exec ledgerpg psql -U postgres -d ledger -Atc "insert into ledger_tenants values ('postgres', 1)"
docker exec -i ledgerpg psql -U postgres -d ledger -q -v ON_ERROR_STOP=1 < "$REPO/bench/vanilla.sql"

# the second node: it can follow the first one.  It starts as a primary of
# its own; the check suite turns it into a replica when it needs one, so
# that the benchmark measures a node with no standby attached.
echo "== creating the second node =="
docker create --name ledgerpg2 --cpus=2 --memory=2g --network ledgernet \
  -e POSTGRES_PASSWORD=postgres -e POSTGRES_DB=ledger -p 127.0.0.1:55433:5432 postgres:18 \
  -c shared_preload_libraries=ledgerext \
  -c wal_compression=off \
  -c max_connections=200 \
  -c fsync=on -c synchronous_commit=on \
  -c shared_buffers=128MB -c wal_buffers=16MB \
  -c checkpoint_timeout=30min -c max_wal_size=8GB -c max_replication_slots=4 -c wal_level=replica >/dev/null
docker cp "$REPO/ledgerext/ledgerext.so" ledgerpg2:/usr/lib/postgresql/18/lib/ledgerext.so
docker cp "$REPO/ledgerext/ledgerext.control" ledgerpg2:/usr/share/postgresql/18/extension/ledgerext.control
docker cp "$REPO/ledgerext/ledgerext--0.1.sql" ledgerpg2:/usr/share/postgresql/18/extension/ledgerext--0.1.sql
docker start ledgerpg2 >/dev/null
waitready ledgerpg2
docker exec ledgerpg2 psql -U postgres -d ledger -Atc "create extension ledgerext"
docker exec ledgerpg2 psql -U postgres -d ledger -Atc "insert into ledger_tenants values ('postgres', 1)"

echo "== ready.  next:  ./scripts/test_ext.sh  then  bench/final_matrix.sh =="
echo "== the check suite needs a fresh cluster, which setup.sh recreates:"
echo "== run ./scripts/setup.sh before every ./scripts/test_ext.sh =="
