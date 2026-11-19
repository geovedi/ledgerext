#!/usr/bin/env bash
# Sustained benchmark of the ledgerext extension, with the client pinned to
# 2 CPUs (the reference harness gives the client --cpus=2 as well).
#
# usage: ext_bench.sh <batch> <clients> <iters>
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$REPO/tmp"
export PGDSN="host=127.0.0.1 port=55432 dbname=ledger user=postgres password=postgres"
# The run must fit the grid: a transfer that does not fit is refused, so a
# run past the capacity would do less work and report a fast-fast number.
B="${1:-8189}"; C="${2:-2}"; IT="${3:-400}"
BENCH_LEDGER="${BENCH_LEDGER:-9}"
CID=$(docker inspect -f '{{.Id}}' ledgerpg)
CG=/sys/fs/cgroup/system.slice/docker-$CID.scope
q(){ docker exec ledgerpg psql -U postgres -d ledger -Atc "$1"; }

CAP=$(q "show ledgerext.max_transfers")
TX=$((B * C * IT))
if [ "$TX" -gt "$CAP" ]; then
	echo "the run's $TX transfers do not fit the grid's $CAP: the transfers that"
	echo "do not fit are refused, so the number would be a fast-fast one"
	exit 1
fi

w0=$(q "select wal_bytes from pg_stat_wal")
c0=$(awk '/usage_usec/{print $2}' $CG/cpu.stat)
t0=$(date +%s.%N)
LEDGER=$BENCH_LEDGER taskset -c 0,1 "$REPO/bench/bench_ext" "$B" "$IT" 200000 "$C" >/dev/null
t1=$(date +%s.%N)
c1=$(awk '/usage_usec/{print $2}' $CG/cpu.stat)
w1=$(q "select wal_bytes from pg_stat_wal")

python3 -c "
tx=$B*$C*$IT; el=$t1-$t0; cpu=($c1-$c0)/1e6; wal=$w1-$w0
print('ledgerext batch=$B clients=$C  transfers=%d  tps=%.0f  cpu=%.2fs (%.2f cores)  cpu/transfer=%.3fus  wal/transfer=%.1fB'
      % (tx, tx/el, cpu, cpu/el, cpu/tx*1e6, wal/tx))"
