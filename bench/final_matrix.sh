#!/usr/bin/env bash
# Head-to-head: vanilla PG (ledger-shaped tables) vs the ledgerext
# extension, in the same --cpus=2 container.
#
# Prereqs: ledgerpg container running (see README), bench/ binaries built:
#   gcc -O2 -I/usr/include/postgresql -o bench/bench_ext  bench/bench_ext.c  -lpq -lpthread
#   gcc -O2 -I/usr/include/postgresql -o bench/bench_uuid bench/bench_uuid.c -lpq
# and the vanilla schema loaded:
#   docker exec -i ledgerpg psql -U postgres -d ledger < bench/vanilla.sql
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BENCH_LEDGER="${BENCH_LEDGER:-9}"
export TMPDIR="$REPO/tmp"
export PGDSN="host=127.0.0.1 port=55432 dbname=ledger user=postgres password=postgres"
CID=$(docker inspect -f '{{.Id}}' ledgerpg)
CG=/sys/fs/cgroup/system.slice/docker-$CID.scope
q(){ docker exec ledgerpg psql -U postgres -d ledger -Atc "$1"; }

measure(){ # label transfers t0 t1 cpu0 cpu1 wal0 wal1
  python3 -c "
tx=$2; el=$4-$3; cpu=($6-$5)/1e6; wal=$8-$7
print('%-26s transfers=%9d tps=%9.0f cpu=%5.2fs (%4.2f cores) cpu/transfer=%7.3fus wal/transfer=%5.1fB'
      % ('$1',tx,tx/el,cpu,cpu/el,cpu/tx*1e6,wal/tx))"
}

run_ext(){ # batch clients iters_per_client
  w0=$(q "select wal_bytes from pg_stat_wal"); c0=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); t0=$(date +%s.%N)
  LEDGER=$BENCH_LEDGER taskset -c 0,1 "$REPO/bench/bench_ext" "$1" "$3" 200000 "$2" >/dev/null 2>&1
  t1=$(date +%s.%N); c1=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); w1=$(q "select wal_bytes from pg_stat_wal")
  measure "ledgerext batch=$1 c=$2" $(python3 -c "print($1*$2*$3)") $t0 $t1 $c0 $c1 $w0 $w1
}

run_vanilla(){ # batch clients rounds
  export LOCK=update
  q "truncate tb_transfers; update tb_accounts set debits_posted=0,credits_posted=0;" >/dev/null
  w0=$(q "select wal_bytes from pg_stat_wal"); c0=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); t0=$(date +%s.%N)
  for i in $(seq 1 $3); do
    # one process per client, $3 iterations each: a fresh backend per batch
    # would charge backend-fork CPU to the server and inflate the result
    p=""; for c in $(seq 1 $2); do taskset -c 0,1 "$REPO/bench/bench_uuid" tb_accounts tb_transfers $1 $3 200000 >/dev/null 2>&1 & p="$p $!"; done; wait $p
  done
  t1=$(date +%s.%N); c1=$(awk '/usage_usec/{print $2}' $CG/cpu.stat); w1=$(q "select wal_bytes from pg_stat_wal")
  measure "vanilla    batch=$1 c=$2" $(python3 -c "print($1*$2*$3)") $t0 $t1 $c0 $c1 $w0 $w1
}

echo "===== --cpus=2 container, PG 18.7, fsync=on, synchronous_commit=on, logged tables ====="
for c in 1 2 4; do
  run_vanilla 8189 $c $((200/c))
  run_ext     8189 $c $((200/c))
done
