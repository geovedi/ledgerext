#!/usr/bin/env bash
# Decompose where the per-transfer CPU goes: vanilla PG vs the extension.
# Each row is the SAME 8189-transfer batch; rows add one layer at a time.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_LEDGER="${BENCH_LEDGER:-9}"
export TMPDIR="$REPO/tmp"
export PGDSN="host=127.0.0.1 port=55432 dbname=ledger user=postgres password=postgres"
ROUNDS="${1:-10}"
CID=$(docker inspect -f '{{.Id}}' ledgerpg)
CG=/sys/fs/cgroup/system.slice/docker-$CID.scope
q(){ docker exec ledgerpg psql -U postgres -d ledger -Atc "$1"; }

report(){ # label t0 t1 c0 c1 w0 w1
  python3 -c "
tx=$ROUNDS*8189
el=$3-$2; cpu=($5-$4)/1e6; wal=$7-$6
print('%-40s cpu/transfer=%7.3fus  wal/transfer=%6.1fB  tps=%9.0f'
      % ('$1', cpu/tx*1e6, wal/tx, tx/el))"
}

vanilla(){ # label lockmode
  q "truncate tb_transfers; update tb_accounts set debits_posted=0,credits_posted=0;" >/dev/null
  w0=$(q "select wal_bytes from pg_stat_wal"); c0=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); t0=$(date +%s.%N)
  # one process, $ROUNDS iterations: a new backend per iteration would charge
  # backend-fork CPU to the server, which is not part of the per-transfer cost
  LOCK=$2 taskset -c 0,1 "$REPO/bench/bench_uuid" tb_accounts tb_transfers 8189 $ROUNDS 200000 >/dev/null 2>&1
  t1=$(date +%s.%N); c1=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); w1=$(q "select wal_bytes from pg_stat_wal")
  report "$1" $t0 $t1 $c0 $c1 $w0 $w1
}

extension(){ # label
  w0=$(q "select wal_bytes from pg_stat_wal"); c0=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); t0=$(date +%s.%N)
  LEDGER=$BENCH_LEDGER taskset -c 0,1 "$REPO/bench/bench_ext" 8189 $ROUNDS 200000 1 >/dev/null 2>&1
  t1=$(date +%s.%N); c1=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); w1=$(q "select wal_bytes from pg_stat_wal")
  report "$1" $t0 $t1 $c0 $c1 $w0 $w1
}

echo "===== cost decomposition, batch=8189, 2-CPU container ====="
vanilla "vanilla 1: transport only (COPY)" none
vanilla "vanilla 2: + transfer log (heap+btree)" insert
vanilla "vanilla 3: + balance update (full)" update
extension "ledgerext: packed batch + shmem grid"
