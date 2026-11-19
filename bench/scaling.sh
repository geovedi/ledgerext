#!/usr/bin/env bash
# Does per-transfer CPU depend on the number of distinct accounts touched?
#
# In vanilla PG it must: every account is a row reached by a btree descent,
# and the transfer log is an indexed heap insert.
# In ledgerext it should not: accounts are slots in a shared-memory array
# reached by a hash probe.
#
# usage: bench/scaling.sh [batches]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_LEDGER="${BENCH_LEDGER:-9}"
export TMPDIR="$REPO/tmp"
export PGDSN="host=127.0.0.1 port=55432 dbname=ledger user=postgres password=postgres"
BATCHES="${1:-200}"
CID=$(docker inspect -f '{{.Id}}' ledgerpg)
CG=/sys/fs/cgroup/system.slice/docker-$CID.scope
q(){ docker exec ledgerpg psql -U postgres -d ledger -Atc "$1"; }

row(){ # label transfers cpu_us wal_bytes
  python3 -c "
print('  %-34s cpu/transfer=%7.3fus  wal/transfer=%6.1fB' % ('$1', $2/$3*1e6, $4/$3))"
}

ext(){ # accounts
  w0=$(q "select wal_bytes from pg_stat_wal"); c0=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); t0=$(date +%s.%N)
  LEDGER=$BENCH_LEDGER taskset -c 0,1 "$REPO/bench/bench_ext" 8189 $BATCHES $1 1 >/dev/null 2>&1
  t1=$(date +%s.%N); c1=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); w1=$(q "select wal_bytes from pg_stat_wal")
  python3 -c "
cpu=($c1-$c0)/1e6; tx=$BATCHES*8189
print('  %-34s cpu/transfer=%7.3fus  wal/transfer=%6.1fB' % ('ledgerext, $1 accounts', cpu/tx*1e6, ($w1-$w0)/tx))"
}

van(){ # accounts
  q "truncate tb_transfers; update tb_accounts set debits_posted=0,credits_posted=0;" >/dev/null
  w0=$(q "select wal_bytes from pg_stat_wal"); c0=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); t0=$(date +%s.%N)
  LOCK=update taskset -c 0,1 "$REPO/bench/bench_uuid" tb_accounts tb_transfers 8189 $BATCHES $1 >/dev/null 2>&1
  t1=$(date +%s.%N); c1=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); w1=$(q "select wal_bytes from pg_stat_wal")
  python3 -c "
cpu=($c1-$c0)/1e6; tx=$BATCHES*8189
print('  %-34s cpu/transfer=%7.3fus  wal/transfer=%6.1fB' % ('vanilla, $1 accounts', cpu/tx*1e6, ($w1-$w0)/tx))"
}

echo "===== per-transfer CPU vs distinct accounts touched (batch=8189) ====="
for a in 500 5000 50000 200000; do
  van $a
  ext $a
done
