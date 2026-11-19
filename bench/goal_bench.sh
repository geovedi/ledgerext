#!/usr/bin/env bash
# The goal's shape: the server pinned to one CPU in its own container, the
# client pinned to one CPU in a second container, both on the private
# network.  This is the one-core measurement shape.
#
# The cluster must be fresh: ./scripts/setup.sh before every run.  A run on a
# cluster that has been used before reports 0 bytes of journal and
# fast-fast numbers, because the ledger's grid is full and the transfers
# are refused.
#
# usage: bench/goal_bench.sh [batch] [iters] [clients] [accounts] [memory] [profile]
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$REPO/tmp"
mkdir -p "$TMPDIR"
B="${1:-8189}"; IT="${2:-200}"; C="${3:-4}"; ACC="${4:-1000}"; MEM="${5:-8g}"
PROF="${6:-0}"

# the server gets one CPU; the client gets its own container and its own
docker update --cpus=1 --memory="$MEM" --memory-swap="$MEM" ledgerpg >/dev/null
docker rm -f -v ledgercli >/dev/null 2>&1 || true
docker create --name ledgercli --cpus=1 --network ledgernet postgres:18 sleep infinity >/dev/null
docker cp "$REPO/bench/bench_ext" ledgercli:/bench_ext
docker start ledgercli >/dev/null

SCID=$(docker inspect -f '{{.Id}}' ledgerpg); SCG=/sys/fs/cgroup/system.slice/docker-$SCID.scope
CCID=$(docker inspect -f '{{.Id}}' ledgercli); CCG=/sys/fs/cgroup/system.slice/docker-$CCID.scope
q(){ docker exec ledgerpg psql -U postgres -d ledger -Atc "$1"; }
# The run must fit the grid: a transfer that does not fit is refused, so a
# run past the capacity would do less work and report a fast-fast number.
CAP=$(q "show ledgerext.max_transfers")
TX=$((B * C * IT))
if [ "$TX" -gt "$CAP" ]; then
	echo "the run's $TX transfers do not fit the grid's $CAP: the transfers that"
	echo "do not fit are refused, so the number would be a fast-fast one"
	exit 1
fi
# the container's bytes written, across all its devices
wbytes(){ awk '{for(i=1;i<=NF;i++) if($i ~ /^wbytes=/){split($i,a,"=");t+=a[2]}} END{print t+0}' "$1/io.stat"; }
w0=$(q "select wal_bytes from pg_stat_wal")
io0=$(wbytes $SCG)
sc0=$(awk '/usage_usec/{print $2}' $SCG/cpu.stat); cc0=$(awk '/usage_usec/{print $2}' $CCG/cpu.stat)
t0=$(date +%s.%N)
if [ "$PROF" = "1" ]; then
  sudo perf record -a -g -e cycles:u -o "$TMPDIR/perf8.data" -- sleep 25 >/dev/null 2>&1 &
  PERFPID=$!
  sleep 2
fi
docker exec -e PGDSN="host=ledgerpg port=5432 dbname=ledger user=postgres password=postgres" \
  ledgercli /bench_ext "$B" "$IT" "$ACC" "$C" > "$TMPDIR/gb.out"
t1=$(date +%s.%N)
if [ "$PROF" = "1" ]; then wait $PERFPID; fi
sc1=$(awk '/usage_usec/{print $2}' $SCG/cpu.stat); cc1=$(awk '/usage_usec/{print $2}' $CCG/cpu.stat)
io1=$(wbytes $SCG)
w1=$(q "select wal_bytes from pg_stat_wal")
cat "$TMPDIR/gb.out"
python3 -c "
tx=$B*$C*$IT; el=$t1-$t0; sc=($sc1-$sc0)/1e6; cc=($cc1-$cc0)/1e6; wal=$w1-$w0; dio=($io1-$io0)
print('  1 server CPU + 1 client CPU  mem=$MEM accs=$ACC transfers=%d tps=%.0f  server_cpu=%.3f cores (%.3f us/transfer)  client_cpu=%.3f cores  wal/transfer=%.1fB  disk/transfer=%.1fB (%.0f MB/s)'
      % (tx, tx/el, sc, sc/tx*1e6, cc, wal/tx, dio/tx, dio/el/1e6))"
docker rm -f -v ledgercli >/dev/null 2>&1 || true
