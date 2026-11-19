#!/usr/bin/env bash
# Correctness checks for the ledgerext extension (v0.7).
# Every check must PASS.  Requires a fresh ledgerpg container (./scripts/setup.sh).
#
# The work is applied when the transaction commits, so the statuses of the
# events are read with ledger_last_statuses() in a later transaction.
#
# There is no reset: the journal is append-only and the interface offers no
# way to clear the ledger.  So each group uses its own ledger number and its
# own accounts, and the totals are read per ledger.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
export TMPDIR="$REPO/tmp"
export PGDSN="host=127.0.0.1 port=55432 dbname=ledger user=postgres password=postgres"
fails=0

# psql on a node, as a role: one statement, or two in ONE session (the
# statuses live in the backend that applied them)
ps1(){ docker exec "$1" psql -U "$2" -d ledger -Atc "$3"; }
ps2(){ docker exec "$1" psql -U "$2" -d ledger -At -c "$3" -c "$4"; }
q(){ ps1 ledgerpg postgres "$1"; }
q2(){ ps2 ledgerpg postgres "$1" "$2"; }
# the second node, which setup.sh creates beside the first
qr(){ ps1 ledgerpg2 postgres "$1"; }
qr2(){ ps2 ledgerpg2 postgres "$1" "$2"; }
# the second node follows the first one, and then decides for itself
follow(){ qr "alter system set ledgerext.replica_of = 'host=ledgerpg port=5432 dbname=ledger user=postgres password=postgres'" >/dev/null
  qr "alter system set ledgerext.replica_poll_ms = 50" >/dev/null
  qr "select pg_reload_conf()" >/dev/null; }
promote(){ qr "alter system set ledgerext.replica_of = ''" >/dev/null
  qr "select pg_reload_conf()" >/dev/null; }
chk(){ if [ "$2" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1: got '$3' want '$2'"; fails=$((fails+1)); fi; }
waitpg(){ for i in $(seq 1 90); do q "select 1" >/dev/null 2>&1 && break; sleep 1; done; }
hardkill(){ docker kill ledgerpg >/dev/null; sleep 2; docker restart ledgerpg >/dev/null; waitpg; }
mk(){ python3 "$REPO/bench/mkbatch.py" "$@"; }

# the ledger's status codes
CREATED=4294967295
EXISTS=46
EXISTS_DIFF_AMOUNT=39
LINKED_FAILED=1
LINKED_OPEN=2
RESERVED_FLAG=4
FLAGS_EXCLUSIVE=7
ACCOUNTS_DIFFERENT=12
LEDGER_ZERO=19
CODE_ZERO=20
DEBIT_NOT_FOUND=21
CREDIT_NOT_FOUND=22
SAME_LEDGER=23
TRANSFER_LEDGER=24
PENDING_NOT_FOUND=25
PENDING_NOT_PENDING=26
EXCEEDS_PENDING=31
PENDING_DIFF_AMOUNT=32
ALREADY_POSTED=33
ALREADY_VOIDED=34
EXPIRED=35
OVERFLOWS_DEBITS_PENDING=47
OVERFLOWS_CREDITS_PENDING=48
OVERFLOWS_DEBITS_POSTED=49
OVERFLOWS_CREDITS_POSTED=50
OVERFLOWS_DEBITS=51
OVERFLOWS_CREDITS=52
EXCEEDS_CREDITS=54
EXCEEDS_DEBITS=55
CLOSING_MUST_PEND=64
DEBIT_CLOSED=65
CREDIT_CLOSED=66
AST_EXISTS=21
AST_NO_SLOTS=100

# queue a transfer batch, then read the statuses of its events
st(){ q2 "select ledger_apply(decode('$(mk "$@")','hex'))" "select ledger_last_statuses()" | tail -1; }
# queue an account batch, then read the statuses
sa(){ q2 "select ledger_create_accounts(decode('$(mk --accounts "$@")','hex'))" "select ledger_last_statuses()" | tail -1; }
# create one account, then read the status
s1(){ q2 "select ledger_create_account('$1',$2,$3,$4)" "select ledger_last_statuses()" | tail -1; }
# the totals of one ledger: {naccounts, dp, cp, d, c}
tot(){ q "select ledger_ledger_totals($1)"; }
bal(){ q "select ledger_balance('$1')"; }
# the four counters of an account, without the flags and the ledger
cnt(){ q "select (ledger_balance('$1'::uuid))[1]||','||(ledger_balance('$1'::uuid))[2]||','||(ledger_balance('$1'::uuid))[3]||','||(ledger_balance('$1'::uuid))[4]"; }
# how many accounts the journal's replay disagrees with the engine about
recon(){ local ids=""; for id in "$@"; do ids="$ids${ids:+,}'$id'::uuid"; done; q "select count(*) from ledger_journal_balances() j where j.id in ($ids) and (j.debits_pending <> (ledger_balance(j.id))[1] or j.credits_pending <> (ledger_balance(j.id))[2] or j.debits_posted <> (ledger_balance(j.id))[3] or j.credits_posted <> (ledger_balance(j.id))[4])"; }

A=00000000-0000-0000-0000-00000000000a
B=00000000-0000-0000-0000-00000000000b
C=00000000-0000-0000-0000-00000000000c
D=00000000-0000-0000-0000-00000000000d
L=00000000-0000-0000-0000-00000000000e
X=00000000-0000-0000-0000-0000000000ff
T1=00000000-0000-0000-0000-000000000001
T2=00000000-0000-0000-0000-000000000002
T3=00000000-0000-0000-0000-000000000003
T4=00000000-0000-0000-0000-000000000004
T5=00000000-0000-0000-0000-000000000005
T6=00000000-0000-0000-0000-000000000006
T7=00000000-0000-0000-0000-000000000007
T8=00000000-0000-0000-0000-000000000008
T9=00000000-0000-0000-0000-000000000009

# account and transfer ids are global, so each group takes its own namespace:
# ids N gives A, B, C, D, L and T1..T9 fresh ids for the group.  ids 0 gives
# the ids the first group uses.
ids(){ local p n; p=$(printf '%08x' "$1")
  n(){ printf '%s-0000-0000-0000-%012x' "$p" $(( $1 * 256 + $2 )); }
  A=$(n 0 10); B=$(n 0 11); C=$(n 0 12); D=$(n 0 13); L=$(n 0 14)
  T1=$(n 1 1); T2=$(n 1 2); T3=$(n 1 3); T4=$(n 1 4); T5=$(n 1 5)
  T6=$(n 1 6); T7=$(n 1 7); T8=$(n 1 8); T9=$(n 1 9); }

echo "--- 1. a transfer moves money and every event gets a status ---"
chk "account A created" "{$CREATED}" "$(s1 $A 0 10 1)"
chk "account B created" "{$CREATED}" "$(s1 $B 0 20 1)"
chk "the same account again" "{$AST_EXISTS}" "$(s1 $A 0 10 1)"
chk "T1 created" "{$CREATED}" "$(st $T1,$A,$B,100)"
chk "balance A" "{0,0,100,0,0,10,1}" "$(bal $A)"
chk "balance B" "{0,0,0,100,0,20,1}" "$(bal $B)"
chk "totals" "{2,0,0,100,100}" "$(tot 1)"
chk "lookup" "$A|$B|100|0|1|1|0" "$(q "select debit||'|'||credit||'|'||amount||'|'||flags||'|'||code||'|'||ledger||'|'||state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the status name" "created" "$(q "select ledger_status_name($CREATED)")"

echo "--- 2. idempotency: the same id may be submitted again ---"
chk "the same event again" "{$EXISTS}" "$(st $T1,$A,$B,100)"
chk "the same id, another amount" "{$EXISTS_DIFF_AMOUNT}" "$(st $T1,$A,$B,7)"
chk "nothing moved twice" "{2,0,0,100,100}" "$(tot 1)"

echo "--- 3. validation: a failing event does not fail the batch ---"
chk "unknown debit account" "{$DEBIT_NOT_FOUND}" "$(st $T2,$X,$B,5)"
chk "unknown credit account" "{$CREDIT_NOT_FOUND}" "$(st $T2,$A,$X,5)"
chk "the same account twice" "{$ACCOUNTS_DIFFERENT}" "$(st $T2,$A,$A,5)"
chk "the client refuses a negative amount" "1" \
  "$(mk $T2,$A,$B,-5 >/dev/null 2>&1 && echo 0 || echo 1)"
chk "unknown flags" "{$RESERVED_FLAG}" "$(st $T2,$A,$B,10,512,1,1)"
chk "pending and post together" "{$FLAGS_EXCLUSIVE}" "$(st $T2,$A,$B,10,6,1,1)"
chk "ledger 0" "{$LEDGER_ZERO}" "$(st $T2,$A,$B,10,0,1,0)"
chk "code 0" "{$CODE_ZERO}" "$(st $T2,$A,$B,10,0,0,1)"
chk "a closing transfer must be pending" "{$CLOSING_MUST_PEND}" "$(st $T2,$A,$B,10,64,1,1)"
chk "nothing leaked from the failures" "{2,0,0,100,100}" "$(tot 1)"
chk "a good event in the same batch still applies" "{$CREATED}" "$(st $T3,$A,$B,10)"

echo "--- 4. rollback and savepoints ---"
q "begin; select ledger_apply(decode('$(mk "$T4,$A,$B,10")','hex')); rollback;" >/dev/null
chk "ROLLBACK discards" "{2,0,0,110,110}" "$(tot 1)"
chk "ROLLBACK TO SAVEPOINT discards only the savepoint" "{2,0,0,120,120}" \
  "$(docker exec ledgerpg psql -U postgres -d ledger -Atc "begin; select ledger_apply(decode('$(mk "$T4,$A,$B,10")','hex')); savepoint s; select ledger_apply(decode('$(mk "$T5,$A,$B,1000")','hex')); rollback to savepoint s; commit; select ledger_ledger_totals(1)" | tail -1)"

ids 5
echo "--- 5. the balance-limit flags (ledger 2) ---"
s1 $L 2 50 2 >/dev/null
s1 $A 0 10 2 >/dev/null
s1 $B 0 20 2 >/dev/null
chk "debits may not exceed credits" "{$EXCEEDS_CREDITS}" "$(st $T6,$L,$A,1,0,1,2)"
chk "credit L first" "{$CREATED}" "$(st $T7,$A,$L,50,0,1,2)"
chk "within the limit now" "{$CREATED}" "$(st $T8,$L,$A,50,0,1,2)"
chk "ledger 2 totals" "{3,0,0,100,100}" "$(tot 2)"

ids 6
echo "--- 6. pending, post and void (ledger 3) ---"
P3=00000000-0000-0000-0000-00000000001a
P4=00000000-0000-0000-0000-00000000001b
s1 $P3 0 10 3 >/dev/null; s1 $P4 0 20 3 >/dev/null
chk "pending created" "{$CREATED}" "$(st $T1,$P3,$P4,100,2,1,3)"
chk "pending moves no money" "{2,100,100,0,0}" "$(tot 3)"
chk "pending is listed" "1" "$(q "select count(*) from ledger_pending_transfers()")"
chk "pending transfer is stored" "1" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "post of an unknown pending" "{$PENDING_NOT_FOUND}" "$(st $T2,$P3,$P4,100,4,1,3,$T9)"
chk "post it" "{$CREATED}" "$(st $T2,$P3,$P4,100,4,1,3,$T1)"
chk "the post moves the money" "{2,0,0,100,100}" "$(tot 3)"
chk "the pending is resolved" "2" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "no pending left" "0" "$(q "select count(*) from ledger_pending_transfers()")"
chk "double post" "{$ALREADY_POSTED}" "$(st $T3,$P3,$P4,100,4,1,3,$T1)"

chk "another pending" "{$CREATED}" "$(st $T4,$P3,$P4,40,2,1,3)"
chk "void of a wrong amount" "{$PENDING_DIFF_AMOUNT}" "$(st $T5,$P3,$P4,39,8,1,3,$T4)"
chk "post of too much" "{$EXCEEDS_PENDING}" "$(st $T5,$P3,$P4,41,4,1,3,$T4)"
chk "void it" "{$CREATED}" "$(st $T5,$P3,$P4,0,8,1,3,$T4)"
chk "the void moves no money" "{2,0,0,100,100}" "$(tot 3)"
chk "the voided pending state" "3" "$(q "select state from ledger_lookup_transfer('$T4'::uuid)")"
chk "no pending left after the void" "0" "$(q "select count(*) from ledger_pending_transfers()")"
chk "post of a voided pending" "{$ALREADY_VOIDED}" "$(st $T6,$P3,$P4,40,4,1,3,$T4)"

ids 7
echo "--- 7. a partial post moves only part of the money (ledger 4) ---"
P5=00000000-0000-0000-0000-00000000001c
P6=00000000-0000-0000-0000-00000000001d
s1 $P5 0 10 4 >/dev/null; s1 $P6 0 20 4 >/dev/null
chk "pending 100" "{$CREATED}" "$(st $T7,$P5,$P6,100,2,1,4)"
chk "post 40 of it" "{$CREATED}" "$(st $T8,$P5,$P6,40,4,1,4,$T7)"
chk "only 40 moved" "{2,0,0,40,40}" "$(tot 4)"

ids 8
echo "--- 8. closing transfers close an account, voiding reopens it (ledger 5) ---"
P7=00000000-0000-0000-0000-00000000001e
P8=00000000-0000-0000-0000-00000000001f
s1 $P7 0 10 5 >/dev/null; s1 $P8 0 20 5 >/dev/null
chk "closing pending" "{$CREATED}" "$(st $T1,$P7,$P8,100,66,1,5)"
chk "P7 is closed" "{100,0,0,0,32,10,5}" "$(bal $P7)"
chk "money into a closed account" "{$CREDIT_CLOSED}" "$(st $T2,$P8,$P7,10,0,1,5)"
chk "money out of a closed account" "{$DEBIT_CLOSED}" "$(st $T2,$P7,$P8,10,0,1,5)"
chk "void the closing transfer" "{$CREATED}" "$(st $T3,$P7,$P8,0,8,1,5,$T1)"
chk "P7 is open again" "{0,0,0,0,0,10,5}" "$(bal $P7)"
chk "money flows again" "{$CREATED}" "$(st $T4,$P7,$P8,10,0,1,5)"

ids 9
echo "--- 9. balancing transfers move at most what is there (ledger 6) ---"
P9=00000000-0000-0000-0000-000000000020
PA=00000000-0000-0000-0000-000000000021
s1 $P9 0 10 6 >/dev/null; s1 $PA 0 20 6 >/dev/null
st $T1,$PA,$P9,100,0,1,6 >/dev/null                     # P9 now holds 100 credits
chk "balancing transfer" "{$CREATED}" "$(st $T2,$P9,$PA,1000,16,1,6)"
chk "it moved only the surplus" "100" "$(q "select amount from ledger_lookup_transfer('$T2'::uuid)")"
chk "and the books balance" "{2,0,0,200,200}" "$(tot 6)"

ids 0
echo "--- 10. ledgers partition the books ---"
s1 $C 0 30 2 >/dev/null
chk "ledger 1 totals" "{2,0,0,120,120}" "$(tot 1)"
chk "ledger 6 totals" "{2,0,0,200,200}" "$(tot 6)"
chk "a transfer across ledgers" "{$SAME_LEDGER}" "$(st $T3,$A,$C,5,0,1,1)"
chk "a transfer in the wrong ledger" "{$TRANSFER_LEDGER}" "$(st $T3,$A,$B,5,0,1,2)"

ids 11
echo "--- 11. linked events fail as a unit (ledger 7) ---"
PB=00000000-0000-0000-0000-000000000022
PC=00000000-0000-0000-0000-000000000023
s1 $PB 0 10 7 >/dev/null; s1 $PC 0 20 7 >/dev/null
chk "a broken chain" "{$LINKED_FAILED,$DEBIT_NOT_FOUND,$LINKED_FAILED}" \
  "$(st $T1,$PB,$PC,10,1,1,7 $T2,$X,$PC,10,1,1,7 $T3,$PB,$PC,10,0,1,7)"
chk "the chain left nothing behind" "{2,0,0,0,0}" "$(tot 7)"
chk "a chain left open at the end" "{$LINKED_FAILED,$LINKED_OPEN}" \
  "$(st $T4,$PB,$PC,10,1,1,7 $T5,$PB,$PC,10,1,1,7)"
chk "still nothing behind" "{2,0,0,0,0}" "$(tot 7)"
chk "a chain that works" "{$CREATED,$CREATED}" "$(st $T6,$PB,$PC,10,1,1,7 $T7,$PB,$PC,10,0,1,7)"
chk "both applied" "{2,0,0,20,20}" "$(tot 7)"

echo "--- 12. durability: kill -9 after a checkpoint ---"
pre=$(tot 9); pt=$(q "select ledger_journal_count()")
q "checkpoint" >/dev/null
hardkill
chk "totals after checkpoint+kill" "$pre" "$(tot 9)"
chk "journal count" "$pt" "$(q "select ledger_journal_count()")"

echo "--- 13. durability: work after a snapshot, then a checkpoint, then kill ---"
q "select ledger_snapshot()" >/dev/null
LEDGER=9 $REPO/bench/bench_ext 8189 3 50000 1 >/dev/null
q "checkpoint" >/dev/null
q "checkpoint" >/dev/null
pre=$(tot 9)
hardkill
chk "snapshot + catch-up" "$pre" "$(tot 9)"

ids 14
echo "--- 14. durability: pending and post survive a restart (ledger 10) ---"
PD=00000000-0000-0000-0000-000000000024
PE=00000000-0000-0000-0000-000000000025
s1 $PD 0 10 10 >/dev/null; s1 $PE 0 20 10 >/dev/null
st $T1,$PD,$PE,100,2,1,10 >/dev/null
st $T2,$PD,$PE,100,4,1,10,$T1 >/dev/null
q "select ledger_snapshot()" >/dev/null
pre=$(tot 10)
hardkill
chk "the posted balance survives" "$pre" "$(tot 10)"
chk "the posted state survives" "2" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"

echo "--- 15. durability: a clean restart ---"
pre=$(tot 10)
docker restart ledgerpg >/dev/null; waitpg
chk "a clean restart keeps the state" "$pre" "$(tot 10)"

echo "--- 16. the journal is not reset by anything ---"
before=$(q "select ledger_journal_count()")
chk "the reset is gone" "ERROR:  function ledger_reset() does not exist" \
  "$(q "select ledger_reset()" 2>&1 | head -1 | cut -c1-60)"
chk "the journal still holds its records" "$before" "$(q "select ledger_journal_count()")"

ids 1
echo "--- 17. the journal is readable four ways ---"
JA=00000000-0000-0000-0000-000000000030
JB=00000000-0000-0000-0000-000000000031
JT=00000000-0000-0000-0000-000000000032
s1 $JA 0 10 20 >/dev/null; s1 $JB 0 20 20 >/dev/null
st $JT,$JA,$JB,100,0,1,20 >/dev/null
chk "by id" "$JA|$JB|100|20|1|0" \
  "$(q "select debit||'|'||credit||'|'||amount_actual||'|'||ledger||'|'||code||'|'||state from ledger_journal_lookup('$JT'::uuid)")"
chk "by account" "1" "$(q "select count(*) from ledger_journal_account('$JA'::uuid)")"
chk "by ledger" "3" "$(q "select count(*) from ledger_journal_ledger(20)")"
jts=$(q "select ts from ledger_journal_lookup('$JT'::uuid)")
chk "by range" "1" "$(q "select count(*) from ledger_journal_range($jts, $jts) where id='$JT'")"
chk "an unknown id returns no rows" "0" "$(q "select count(*) from ledger_journal_lookup('$X'::uuid)")"
chk "a range that ends before it begins" "0" "$(q "select count(*) from ledger_journal_range(1000000, 0)")"
# the timestamps are nanoseconds since the Unix epoch, so a range in the
# future has to be a range of nanoseconds
chk "a range in the future" "0" "$(q "select count(*) from ledger_journal_range(2000000000000000000, 2000000000000000001)")"

echo "--- 17b. the journal's queries return every row, once ---"
# A set-returning function's body runs once per row.  If the work of
# finding the rows ran on every call, a query over many accounts would take
# a thousand times as long, and the row count would still be right.  So the
# counts are checked exactly, and a query over a large ledger is timed.
n=$(q "select count(*) from ledger_journal_ledger(20)")
chk "the by-ledger query returns every record" "$n" \
  "$(q "select count(*) from ledger_journal_ledger(20, -1)")"
chk "the by-account query returns every record" "1" \
  "$(q "select count(*) from ledger_journal_account('$JA'::uuid, -1)")"
chk "the range query returns every record in the range" \
  "$(q "select ledger_journal_count()")" \
  "$(q "select count(*) from ledger_journal_range(0, 9223372036854775807, -1)")"
# and the cost does not grow with the number of rows returned
t0=$(date +%s%N)
q "select count(*) from ledger_journal_ledger(20, -1)" >/dev/null
t1=$(date +%s%N)
chk "the query is not quadratic" "1" "$([ $(( (t1-t0)/1000000 )) -lt 2000 ] && echo 1 || echo 0)"

echo "--- 18. the journal's edge cases ---"
JC=00000000-0000-0000-0000-000000000033
JD=00000000-0000-0000-0000-000000000034
s1 $JC 0 10 21 >/dev/null; s1 $JD 0 20 21 >/dev/null
st 00000000-0000-0000-0000-000000000035,$JC,$JD,0,0,1,21 >/dev/null   # a zero amount
chk "a zero-amount transfer is in the journal" "1" \
  "$(q "select count(*) from ledger_journal_lookup('00000000-0000-0000-0000-000000000035'::uuid)")"
n0=$(q "select ledger_journal_count()")
st 00000000-0000-0000-0000-000000000036,$JC,$X,5,0,1,21 >/dev/null       # a rejected transfer
chk "a rejected transfer is not in the journal" "$n0" "$(q "select ledger_journal_count()")"
chk "a post of a nonexistent pending leaves no record" "$n0" "$(q "select ledger_journal_count()")"
st 00000000-0000-0000-0000-000000000037,$JC,$JD,10,0,1,21 00000000-0000-0000-0000-000000000038,$X,$JD,10,0,1,21 >/dev/null
chk "the earlier transfer of the batch is in the journal" "1" \
  "$(q "select count(*) from ledger_journal_lookup('00000000-0000-0000-0000-000000000037'::uuid)")"
chk "the failing one is not" "0" \
  "$(q "select count(*) from ledger_journal_lookup('00000000-0000-0000-0000-000000000038'::uuid)")"

echo "--- 19. a broken linked chain leaves no records ---"
JE=00000000-0000-0000-0000-000000000039
JF=00000000-0000-0000-0000-00000000003a
s1 $JE 0 10 22 >/dev/null; s1 $JF 0 20 22 >/dev/null
n0=$(q "select ledger_journal_count()")
st 00000000-0000-0000-0000-00000000003b,$JE,$JF,10,1,1,22 00000000-0000-0000-0000-00000000003c,$X,$JF,10,1,1,22 >/dev/null
chk "the chain left no record" "$n0" "$(q "select ledger_journal_count()")"
chk "no member of the chain is in the journal" "0" \
  "$(q "select count(*) from ledger_journal_lookup('00000000-0000-0000-0000-00000000003b'::uuid)")"

echo "--- 20. the journal's order is the order of application ---"
chk "the account creations come before the transfers" "1" \
  "$(q "select kind from ledger_journal_ledger(22) order by seq limit 1")"

ids 0
echo "--- 21. the balances are reconstructible from the journal ---"
eng=$(q "select (ledger_ledger_totals(1))[4]")
chk "the replay agrees with the engine" "$eng" \
  "$(q "select sum(debits_posted) from ledger_journal_balances() where id in ('$A','$B')")"

echo "--- 22. the books can be closed ---"
before=$(q "select ledger_journal_count()")
chk "the close returns the period" "1" "$(q "select ledger_close_period()")"
chk "the close is in the journal" "$((before+1))" "$(q "select ledger_journal_count()")"
chk "the period is closed" "1" "$(q "select count(*) from ledger_journal_periods() where close_seq > 0")"
chk "the balances at the close are the journal's balances" "$eng" \
  "$(q "select sum(debits_posted) from ledger_period_balances(1) where id in ('$A','$B')")"
st $T9,$A,$B,10,0,1,1 >/dev/null
chk "the transfer after the close is in the new period" "2" \
  "$(q "select period from ledger_journal_lookup('$T9'::uuid) limit 1")"
chk "the closed period's balances did not change" "$eng" \
  "$(q "select sum(debits_posted) from ledger_period_balances(1) where id in ('$A','$B')")"

echo "--- 23. the journal survives a crash ---"
pre=$(q "select ledger_journal_count()")
hardkill
chk "the journal is the same after the kill" "$pre" "$(q "select ledger_journal_count()")"

echo "--- 24. the journal is the log: a cut tail is not repaired ---"
q "select ledger_snapshot()" >/dev/null
st $T5,$A,$B,11,0,1,1 >/dev/null
pre=$(q "select ledger_journal_count()")
# cut the journal's tail.  The journal is the log now and it is made durable
# before the commit is acknowledged, so a crash cannot lose a record: what
# this cuts is damage, and a damaged journal has nothing to be repaired from
pd=$(q "show data_directory")
sz=$(docker exec ledgerpg stat -c %s "$pd/ledgerext.journal")
# one record is 176 bytes: the journal's record is the disk's cost of a
# transfer, and the test pins that cost
docker exec ledgerpg truncate -s $((sz - 176)) "$pd/ledgerext.journal"
chk "the tail was really cut" "$((sz - 176))" "$(docker exec ledgerpg stat -c %s "$pd/ledgerext.journal")"
hardkill
chk "the cut record is gone" "$((pre - 1))" "$(q "select ledger_journal_count()")"
chk "the ledger agrees with the journal" "0" "$(recon $A $B)"

echo "--- 23b. a close and a transfer in one transaction ---"
ids 23
s1 $A 0 10 23 >/dev/null; s1 $B 0 20 23 >/dev/null
# the close is applied last in its transaction, so the transfer belongs to
# the period the close ends
tid=${T1//-/}
q "begin; select ledger_close_period(); select ledger_apply(decode('$(mk $tid,$A,$B,7,0,10,23)','hex')); commit" >/dev/null 2>&1
p=$(q "select period from ledger_journal_lookup('$T1'::uuid) limit 1")
chk "the transfer is in the closing period" "1" \
  "$(q "select count(*) from ledger_journal_lookup('$T1'::uuid) where period = $p")"
chk "the close is in the same period" "1" \
  "$(q "select count(*) from ledger_journal_range(0, 9223372036854775807, -1) where kind = 2 and period = $p")"
chk "the close comes after the transfer" "1" \
  "$(q "select count(*) from ledger_journal_range(0, 9223372036854775807, -1) where kind = 2 and period = $p and seq > (select seq from ledger_journal_lookup('$T1'::uuid) limit 1)")"

echo "--- 24b. a serializable transaction is refused ---"
chk "serializable is refused" "1" "$(q "begin isolation level serializable; select ledger_create_account('$X',0,1,1); commit" 2>&1 | grep -c 'serializable transaction cannot queue')"

echo "--- 24c. hostile input does not hurt the server ---"
q "select ledger_balance(NULL), ledger_create_account(NULL,0,1,1), ledger_apply(NULL)" >/dev/null 2>&1
chk "the server survives NULL arguments" "1" "$(q "select 1")"
chk "a NULL argument yields NULL" "" "$(q "select ledger_balance(NULL)")"
chk "an unknown period is an error" "1" "$(q "select * from ledger_period_balances(99999)" 2>&1 | grep -c 'does not exist')"
chk "period zero is an error" "1" "$(q "select * from ledger_period_balances(0)" 2>&1 | grep -c 'does not exist')"
# two closes in one transaction end two different periods
c=$(q "begin; select ledger_close_period(); select ledger_close_period(); commit" | grep -E '^[0-9]+$' | paste -sd' ')
set -- $c
chk "two closes in one transaction end consecutive periods" "$(( $1 + 1 ))" "$2"
chk "the journal records the same two periods" "2" \
  "$(q "select count(*) from ledger_journal_range(0, 9223372036854775807, -1) where kind = 2 and period in ($1, $2)")"

echo "--- 26. a pending transfer with a timeout expires by itself ---"
ids 30
EX1=00000000-0000-0000-0000-000000000040
EX2=00000000-0000-0000-0000-000000000041
s1 $EX1 0 10 30 >/dev/null; s1 $EX2 0 20 30 >/dev/null
chk "a pending transfer with a one-second timeout is created" "{$CREATED}" \
  "$(st $T1,$EX1,$EX2,100,2,1,30,00000000000000000000000000000000,1)"
chk "the reservation is held" "100" "$(q "select (ledger_balance('$EX1'::uuid))[1]")"
chk "the posted counters are untouched" "0,0" \
  "$(q "select (ledger_balance('$EX1'::uuid))[3]||','||(ledger_balance('$EX1'::uuid))[4]")"
sleep 3
chk "the reservation is released" "0,0,0,0" \
  "$(cnt $EX1)"
chk "the transfer is expired" "4" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "it is no longer pending" "0" "$(q "select count(*) from ledger_pending_transfers() where id='$T1'::uuid")"
chk "nothing is due any more" "0" "$(q "select ledger_expiry_due()")"

echo "--- 26b. a pending transfer without a timeout never expires ---"
ids 31
EY1=00000000-0000-0000-0000-000000000043
EY2=00000000-0000-0000-0000-000000000044
s1 $EY1 0 10 31 >/dev/null; s1 $EY2 0 20 31 >/dev/null
chk "a pending transfer with no timeout is created" "{$CREATED}" \
  "$(st $T1,$EY1,$EY2,100,2,1,31)"
sleep 3
chk "the reservation is still held" "100" "$(q "select (ledger_balance('$EY1'::uuid))[1]")"
chk "it is still pending" "1" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the timeout is zero" "0" "$(q "select timeout from ledger_pending_transfers() where id='$T1'::uuid")"

echo "--- 26c. the transfers are released in the order of their expiry times ---"
ids 32
EZ1=00000000-0000-0000-0000-000000000045
EZ2=00000000-0000-0000-0000-000000000046
s1 $EZ1 0 10 32 >/dev/null; s1 $EZ2 0 20 32 >/dev/null
# the first expires in one second, the second in five
st $T1,$EZ1,$EZ2,100,2,1,32,00000000000000000000000000000000,1 >/dev/null
st $T2,$EZ1,$EZ2,200,2,1,32,00000000000000000000000000000000,5 >/dev/null
sleep 3
chk "the one that expires first is released" "4" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the one that expires later is still pending" "1" "$(q "select state from ledger_lookup_transfer('$T2'::uuid)")"
chk "its reservation is still held" "200" "$(q "select (ledger_balance('$EZ1'::uuid))[1]")"
sleep 4
chk "the later one is released too" "4" "$(q "select state from ledger_lookup_transfer('$T2'::uuid)")"

echo "--- 27. a post or a void of an expired transfer is refused ---"
ids 33
ER1=00000000-0000-0000-0000-000000000047
ER2=00000000-0000-0000-0000-000000000048
s1 $ER1 0 10 33 >/dev/null; s1 $ER2 0 20 33 >/dev/null
st $T1,$ER1,$ER2,100,2,1,33,00000000000000000000000000000000,1 >/dev/null
sleep 3
chk "the transfer has expired" "4" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "a post of it is refused" "{$EXPIRED}" "$(st $T2,$ER1,$ER2,100,4,1,33,$T1)"
chk "a void of it is refused" "{$EXPIRED}" "$(st $T3,$ER1,$ER2,0,8,1,33,$T1)"
chk "the refusal moved no money" "0,0,0,0" \
  "$(cnt $ER1)"
chk "the refused post left no record" "0" "$(q "select count(*) from ledger_journal_lookup('$T2'::uuid)")"
chk "the status has a name" "pending_transfer_expired" "$(q "select ledger_status_name($EXPIRED)")"

echo "--- 27b. the refusal comes from the clock, not from the release ---"
ids 34
EC1=00000000-0000-0000-0000-000000000049
EC2=00000000-0000-0000-0000-00000000004a
s1 $EC1 0 10 34 >/dev/null; s1 $EC2 0 20 34 >/dev/null
# the worker is held back, so the release cannot have run
q "alter system set ledgerext.expiry_interval_ms = 60000" >/dev/null
q "select pg_reload_conf()" >/dev/null
sleep 1
st $T1,$EC1,$EC2,100,2,1,34,00000000000000000000000000000000,1 >/dev/null
sleep 2
chk "the reservation is still held: the worker has not run" "100" \
  "$(q "select (ledger_balance('$EC1'::uuid))[1]")"
chk "the transfer is still pending" "1" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "a post is refused by the clock alone" "{$EXPIRED}" "$(st $T2,$EC1,$EC2,100,4,1,34,$T1)"
chk "the refusal moved no money" "100" "$(q "select (ledger_balance('$EC1'::uuid))[1]")"
q "alter system reset ledgerext.expiry_interval_ms" >/dev/null
q "select pg_reload_conf()" >/dev/null

echo "--- 27c. a transfer resolved before its expiry time is never expired ---"
ids 35
EP1=00000000-0000-0000-0000-00000000004b
EP2=00000000-0000-0000-0000-00000000004c
s1 $EP1 0 10 35 >/dev/null; s1 $EP2 0 20 35 >/dev/null
st $T1,$EP1,$EP2,100,2,1,35,00000000000000000000000000000000,2 >/dev/null
chk "it is posted before the timeout" "{$CREATED}" "$(st $T2,$EP1,$EP2,100,4,1,35,$T1)"
sleep 3
chk "it is still posted" "2" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the money did not move twice" "0,0,100,0" \
  "$(cnt $EP1)"
chk "the expiry left no record" "0" "$(q "select count(*) from ledger_journal_lookup('$T1'::uuid) where kind = 3")"

echo "--- 28. the journal holds the expiry as its own entry ---"
ids 36
EJ1=00000000-0000-0000-0000-00000000004d
EJ2=00000000-0000-0000-0000-00000000004e
s1 $EJ1 0 10 36 >/dev/null; s1 $EJ2 0 20 36 >/dev/null
st $T1,$EJ1,$EJ2,100,2,1,36,00000000000000000000000000000000,1 >/dev/null
sleep 3
chk "the lookup returns two rows" "2" "$(q "select count(*) from ledger_journal_lookup('$T1'::uuid)")"
chk "the second row is the expiry" "3" "$(q "select kind from ledger_journal_lookup('$T1'::uuid) order by seq desc limit 1")"
chk "the expiry has the expired state" "4" "$(q "select state from ledger_journal_lookup('$T1'::uuid) order by seq desc limit 1")"
chk "the expiry names the transfer" "$T1" "$(q "select id from ledger_journal_lookup('$T1'::uuid) order by seq desc limit 1")"
chk "the expiry moved nothing" "0" "$(q "select amount_actual from ledger_journal_lookup('$T1'::uuid) order by seq desc limit 1")"
chk "the expiry released the amount" "100" "$(q "select amount from ledger_journal_lookup('$T1'::uuid) order by seq desc limit 1")"
chk "the account's chain holds the expiry" "1" \
  "$(q "select count(*) from ledger_journal_account('$EJ1'::uuid, -1) where kind = 3")"
chk "the ledger's records hold the expiry" "1" \
  "$(q "select count(*) from ledger_journal_ledger(36, -1) where kind = 3")"
chk "the time range holds the expiry" "1" \
  "$(q "select count(*) from ledger_journal_range(0, 9223372036854775807, -1) where kind = 3 and id='$T1'::uuid")"
chk "the balances are reconstructible from the journal" "0" \
  "$(recon $EJ1)"
chk "and so are the credit's" "0" \
  "$(recon $EJ2)"

echo "--- 29. an expiry after a close belongs to the period it happened in ---"
ids 37
ED1=00000000-0000-0000-0000-00000000004f
ED2=00000000-0000-0000-0000-000000000050
s1 $ED1 0 10 37 >/dev/null; s1 $ED2 0 20 37 >/dev/null
st $T1,$ED1,$ED2,100,2,1,37,00000000000000000000000000000000,1 >/dev/null
closed=$(q "select ledger_close_period()")
before=$(q "select sum(debits_pending) from ledger_period_balances($closed)")
sleep 3
chk "the expiry is in the period it happened in" "$((closed+1))" \
  "$(q "select period from ledger_journal_lookup('$T1'::uuid) where kind = 3")"
chk "the close's balances did not change" "$before" \
  "$(q "select sum(debits_pending) from ledger_period_balances($closed)")"

echo "--- 30. a closing transfer that expires reopens the account ---"
ids 38
EV1=00000000-0000-0000-0000-000000000051
EV2=00000000-0000-0000-0000-000000000052
s1 $EV1 0 10 38 >/dev/null; s1 $EV2 0 20 38 >/dev/null
chk "the closing pending is created" "{$CREATED}" "$(st $T1,$EV1,$EV2,100,66,1,38,00000000000000000000000000000000,1)"
chk "the account is closed" "32" "$(q "select (ledger_balance('$EV1'::uuid))[5]")"
sleep 3
chk "the account is open again" "0" "$(q "select (ledger_balance('$EV1'::uuid))[5]")"
chk "the reservation is released" "0" "$(q "select (ledger_balance('$EV1'::uuid))[1]")"
chk "money flows again" "{$CREATED}" "$(st $T2,$EV1,$EV2,10,0,1,38)"

echo "--- 30b. a closing transfer that was voided is never expired ---"
ids 39
EW1=00000000-0000-0000-0000-000000000053
EW2=00000000-0000-0000-0000-000000000054
s1 $EW1 0 10 39 >/dev/null; s1 $EW2 0 20 39 >/dev/null
st $T1,$EW1,$EW2,100,66,1,39,00000000000000000000000000000000,2 >/dev/null
chk "it is voided before the timeout" "{$CREATED}" "$(st $T2,$EW1,$EW2,0,8,1,39,$T1)"
sleep 3
chk "the account stays open" "0" "$(q "select (ledger_balance('$EW1'::uuid))[5]")"
chk "the transfer is still voided" "3" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the expiry left no record" "0" "$(q "select count(*) from ledger_journal_lookup('$T1'::uuid) where kind = 3")"

echo "--- 31. an expiry survives a kill -9 ---"
ids 40
EK1=00000000-0000-0000-0000-000000000055
EK2=00000000-0000-0000-0000-000000000056
s1 $EK1 0 10 40 >/dev/null; s1 $EK2 0 20 40 >/dev/null
st $T1,$EK1,$EK2,100,2,1,40,00000000000000000000000000000000,1 >/dev/null
sleep 3
chk "it has expired" "4" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
exp=$(q "select count(*) from ledger_journal_lookup('$T1'::uuid) where kind = 3")
q "select ledger_snapshot()" >/dev/null
hardkill
chk "the expiry is still in effect" "4" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the reservation is still released" "0" "$(q "select (ledger_balance('$EK1'::uuid))[1]")"
chk "the journal holds exactly one expiry row" "$exp" \
  "$(q "select count(*) from ledger_journal_lookup('$T1'::uuid) where kind = 3")"
chk "the balances still come from the journal" "0" \
  "$(recon $EK1)"

echo "--- 31b. the pending set survives a kill -9 ---"
ids 41
EL1=00000000-0000-0000-0000-000000000057
EL2=00000000-0000-0000-0000-000000000058
s1 $EL1 0 10 41 >/dev/null; s1 $EL2 0 20 41 >/dev/null
# the timeout is long, so the transfer cannot expire during the restart
st $T1,$EL1,$EL2,100,2,1,41,00000000000000000000000000000000,300 >/dev/null
hardkill
chk "it is still pending after the restart" "1" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the reservation is still held" "100" "$(q "select (ledger_balance('$EL1'::uuid))[1]")"
chk "it is still in the pending set" "1" "$(q "select count(*) from ledger_pending_transfers() where id='$T1'::uuid")"
chk "its timeout survived the restart" "300" "$(q "select timeout from ledger_pending_transfers() where id='$T1'::uuid")"

echo "--- 31c. the worker releases what is due after the restart ---"
ids 42
EM1=00000000-0000-0000-0000-000000000059
EM2=00000000-0000-0000-0000-00000000005a
s1 $EM1 0 10 42 >/dev/null; s1 $EM2 0 20 42 >/dev/null
# the timeout is short, so it is due by the time the server is back
st $T1,$EM1,$EM2,100,2,1,42,00000000000000000000000000000000,1 >/dev/null
hardkill
sleep 2
chk "the worker expires it after the restart" "4" "$(q "select state from ledger_lookup_transfer('$T1'::uuid)")"
chk "the reservation is released" "0" "$(q "select (ledger_balance('$EM1'::uuid))[1]")"
chk "the journal holds exactly one expiry row" "1" \
  "$(q "select count(*) from ledger_journal_lookup('$T1'::uuid) where kind = 3")"
chk "the journal's replay agrees with the engine after the expiry" "0" \
  "$(recon $EM1 $EM2)"

# point a journal's version byte at an old version, restart, and check the
# refusal; a fresh cluster takes the refused node's place
refuse_old_journal(){
  docker exec ledgerpg sh -c "printf '\\x$1' | dd of=$pd/ledgerext.journal bs=1 seek=4 count=1 conv=notrunc 2>/dev/null"
  docker restart ledgerpg >/dev/null; sleep 3
  chk "the $2 journal is refused" "1" \
    "$(docker logs ledgerpg 2>&1 | tail -40 | grep -c 'was written by another version')"
  chk "the refusal names the file to remove" "1" \
    "$(docker logs ledgerpg 2>&1 | tail -40 | grep -q 'ledgerext.journal' && echo 1 || echo 0)"
  docker rm -f -v ledgerpg >/dev/null 2>&1
  $REPO/scripts/setup.sh >/dev/null 2>&1
  chk "a fresh ledger starts" "1" "$(q "select 1")"
}

echo "--- 32. a data directory written by another version is refused ---"
# the journal's header carries its version, and a journal of another version
# is refused with the message that names the file, rather than read
pd=$(q "show data_directory")
q "select ledger_snapshot()" >/dev/null
# this version writes version 1; 9 is any other version
refuse_old_journal 9 9


echo "--- 33. a burst of expiries is released in bounded rounds ---"
ids 43
BR1=00000000-0000-0000-0000-000000000060
BR2=00000000-0000-0000-0000-000000000061
s1 $BR1 0 10 43 >/dev/null; s1 $BR2 0 20 43 >/dev/null
# 5000 transfers that all expire in the same second
python3 "$REPO/bench/mkburst.py" 100000 "$BR1" "$BR2" 43 1 b000 > "$TMPDIR/burst.hex"
python3 -c "
h=open('$TMPDIR/burst.hex').read().strip()
print(\"select ledger_apply(decode('\" + h + \"','hex'));\")" > "$TMPDIR/burst.sql"
chk "the burst is applied" "100000" \
  "$(docker exec -i ledgerpg psql -U postgres -d ledger -Atf - < "$TMPDIR/burst.sql" | tail -1)"
chk "the burst is pending" "100000" "$(q "select count(*) from ledger_pending_transfers()")"
sleep 1.5
# a commit from another transaction must not wait for the whole burst
t0=$(date +%s%N)
q "select ledger_close_period()" >/dev/null
t1=$(date +%s%N)
chk "a commit is not held up by the burst" "1" \
  "$([ $(( (t1-t0)/1000000 )) -lt 2000 ] && echo 1 || echo 0)"
# and the burst is released without any client asking
for i in $(seq 1 240); do
  [ "$(q "select count(*) from ledger_journal_range(0, 9223372036854775807, -1) where kind = 3")" = "100000" ] && break
  sleep 0.25
done
chk "the burst is released" "100000" \
  "$(q "select count(*) from ledger_journal_range(0, 9223372036854775807, -1) where kind = 3")"
chk "nothing is left pending" "0" "$(q "select count(*) from ledger_pending_transfers()")"
chk "the balances are reconstructible" "0" \
  "$(recon $BR1)"

echo "--- 34. the hot path is unchanged ---"
ids 44
HP1=00000000-0000-0000-0000-000000000062
HP2=00000000-0000-0000-0000-000000000063
s1 $HP1 0 10 44 >/dev/null; s1 $HP2 0 20 44 >/dev/null
# a batch of single-phase transfers: every event is created, nothing else
python3 "$REPO/bench/mkburst.py" 200 "$HP1" "$HP2" 44 0 30000 > "$TMPDIR/hot.hex"
python3 -c "
h=open('$TMPDIR/hot.hex').read().strip()
print(\"select ledger_apply(decode('\" + h + \"','hex'));\")
print(\"select count(*) from unnest(ledger_last_statuses()) s where s = 4294967295;\")" > "$TMPDIR/hot.sql"
chk "the batch commits and every event is created" "200,200" \
  "$(docker exec -i ledgerpg psql -U postgres -d ledger -Atf - < "$TMPDIR/hot.sql" | paste -sd,)"
chk "the money moved exactly once" "1400,1400" \
  "$(q "select (ledger_ledger_totals(44))[4]||','||(ledger_ledger_totals(44))[5]")"

echo "--- 35. a mixed workload: the journal agrees with the engine ---"
ids 45
MX1=00000000-0000-0000-0000-000000000064
MX2=00000000-0000-0000-0000-000000000065
s1 $MX1 0 10 45 >/dev/null; s1 $MX2 0 20 45 >/dev/null
# four pending transfers, one for each outcome
st $T1,$MX1,$MX2,100,2,1,45,00000000000000000000000000000000,60 >/dev/null   # posted
st $T2,$MX1,$MX2,200,2,1,45,00000000000000000000000000000000,60 >/dev/null   # voided
st $T3,$MX1,$MX2,300,2,1,45,00000000000000000000000000000000,1 >/dev/null    # expired
st $T4,$MX1,$MX2,400,2,1,45 >/dev/null                                       # never expires
chk "the one that is posted" "{$CREATED}" "$(st $T5,$MX1,$MX2,100,4,1,45,$T1)"
chk "the one that is voided" "{$CREATED}" "$(st $T6,$MX1,$MX2,0,8,1,45,$T2)"
sleep 3
chk "the one that expires" "4" "$(q "select state from ledger_lookup_transfer('$T3'::uuid)")"
chk "the one that never expires" "1" "$(q "select state from ledger_lookup_transfer('$T4'::uuid)")"
chk "the journal's balances are the engine's, for every account" "0" \
  "$(recon $MX1 $MX2)"
# the posted one moved its money, the voided and the expired ones moved
# nothing, and the one with no timeout still holds its reservation
chk "the engine's own counters" "400,0,100,0" \
  "$(cnt $MX1)"
chk "the credit account's counters" "0,400,0,100" \
  "$(cnt $MX2)"
chk "and the other account agrees too" "0" \
  "$(recon $MX2)"

echo "--- 36. the ledger is not executable by the public ---"
# the ledger's functions move money and read a book, so nothing is
# executable by PUBLIC: the administrator grants a role the functions of
# the book it owns, and a role that owns no book gets nothing at all
q "create role ledger_reader login" >/dev/null
chk "a plain role cannot move money" "permission denied" \
  "$(docker exec ledgerpg psql -U ledger_reader -d ledger -Atc "select ledger_apply(''::bytea)" 2>&1 | grep -o 'permission denied' | head -1)"
chk "a plain role cannot read the book" "permission denied" \
  "$(docker exec ledgerpg psql -U ledger_reader -d ledger -Atc "select ledger_balance('00000000-0000-0000-0000-00000000000a'::uuid)" 2>&1 | grep -o 'permission denied' | head -1)"
q "grant execute on function ledger_expiry_due() to ledger_reader" >/dev/null
chk "the granted function works" "0" \
  "$(docker exec ledgerpg psql -U ledger_reader -d ledger -Atc "select ledger_expiry_due()")"
q "drop owned by ledger_reader" >/dev/null 2>&1
q "drop role ledger_reader" >/dev/null 2>&1

ids 46
echo "--- 37. a transfer past 64 bits moves the money (ledger 50) ---"
W1=00000000-0000-0000-0000-000000000070
W2=00000000-0000-0000-0000-000000000071
W3=00000000-0000-0000-0000-000000000072
W4=00000000-0000-0000-0000-000000000073
s1 $W1 0 10 50 >/dev/null; s1 $W2 0 20 50 >/dev/null
s1 $W3 0 10 50 >/dev/null; s1 $W4 0 20 50 >/dev/null
chk "a transfer of 2^64 is created" "{$CREATED}" \
  "$(st $T1,$W1,$W2,18446744073709551616,0,1,50)"
chk "the debit account holds it exactly" "18446744073709551616" \
  "$(q "select (ledger_balance('$W1'::uuid))[3]")"
chk "the credit account holds it exactly" "18446744073709551616" \
  "$(q "select (ledger_balance('$W2'::uuid))[4]")"
chk "the transfer's amount reads back" "18446744073709551616" \
  "$(q "select amount from ledger_lookup_transfer('$T1'::uuid)")"
chk "the largest amount is created" "{$CREATED}" \
  "$(st $T2,$W3,$W4,340282366920938463463374607431768211455,0,1,50)"
chk "it is held exactly" "340282366920938463463374607431768211455" \
  "$(q "select (ledger_balance('$W3'::uuid))[3]")"
chk "the books balance" "340282366920938463463374607431768211455" \
  "$(q "select (ledger_balance('$W4'::uuid))[4]")"

ids 47
echo "--- 38. a lifetime total past 64 bits survives a restart (ledger 51) ---"
L1=00000000-0000-0000-0000-000000000074
L2=00000000-0000-0000-0000-000000000075
s1 $L1 0 10 51 >/dev/null; s1 $L2 0 20 51 >/dev/null
st $T1,$L1,$L2,18446744073709551616,0,1,51 >/dev/null
st $T2,$L1,$L2,18446744073709551616,0,1,51 >/dev/null
chk "the lifetime total" "36893488147419103232" \
  "$(q "select (ledger_balance('$L1'::uuid))[3]")"
q "select ledger_snapshot()" >/dev/null
hardkill
chk "the same number after the restart" "36893488147419103232" \
  "$(q "select (ledger_balance('$L1'::uuid))[3]")"
chk "and the credit side too" "36893488147419103232" \
  "$(q "select (ledger_balance('$L2'::uuid))[4]")"
chk "the journal's replay agrees" "0" "$(recon $L1 $L2)"

ids 48
echo "--- 39. the amounts read back exactly (ledger 52) ---"
R1=00000000-0000-0000-0000-000000000076
R2=00000000-0000-0000-0000-000000000077
R3=00000000-0000-0000-0000-000000000078
R4=00000000-0000-0000-0000-000000000079
s1 $R1 0 10 52 >/dev/null; s1 $R2 0 20 52 >/dev/null
s1 $R3 0 10 52 >/dev/null; s1 $R4 0 20 52 >/dev/null
st $T1,$R1,$R2,18446744073709551617,0,1,52 >/dev/null
chk "the balance is the exact decimal" "18446744073709551617" \
  "$(q "select (ledger_balance('$R1'::uuid))[3]")"
chk "the per-ledger totals are exact" "18446744073709551617" \
  "$(q "select (ledger_ledger_totals(52))[4]")"
chk "the journal's row is exact" "18446744073709551617" \
  "$(q "select amount from ledger_journal_lookup('$T1'::uuid)")"
chk "the journal's amount_actual is exact" "18446744073709551617" \
  "$(q "select amount_actual from ledger_journal_lookup('$T1'::uuid)")"
chk "reading it twice gives the same digits" \
  "$(q "select amount from ledger_journal_lookup('$T1'::uuid)")" \
  "$(q "select amount from ledger_journal_lookup('$T1'::uuid)")"
# a pending transfer of a full-width amount shows in the pending set
st $T2,$R3,$R4,18446744073709551617,2,1,52 >/dev/null
chk "the pending set holds the exact amount" "18446744073709551617" \
  "$(q "select amount from ledger_pending_transfers() where id='$T2'::uuid")"
chk "a balance of zero reads as 0" "0" \
  "$(q "select (ledger_balance('$R3'::uuid))[4]")"
chk "the helper gives the 16 bytes" "18446744073709551617" \
  "$(q "select encode(ledger_amount(18446744073709551617), 'hex')" | python3 -c "
import sys
b=bytes.fromhex(sys.stdin.read().strip())
print(int.from_bytes(b,'little'))")"
chk "the helper refuses a negative amount" "1" \
  "$(q "select ledger_amount(-1)" 2>&1 | grep -c 'not an amount')"
chk "the helper refuses an amount above 2^128-1" "1" \
  "$(q "select ledger_amount(340282366920938463463374607431768211456)" 2>&1 | grep -c 'not an amount')"

ids 49
echo "--- 40. an amount that cannot fit is refused, not wrapped (ledger 53) ---"
# 49: the posted debits are at the limit
O1=00000000-0000-0000-0000-00000000007a
O2=00000000-0000-0000-0000-00000000007b
s1 $O1 0 10 53 >/dev/null; s1 $O2 0 20 53 >/dev/null
st $T1,$O1,$O2,340282366920938463463374607431768211455,0,1,53 >/dev/null
chk "the debit account is at the limit" "340282366920938463463374607431768211455" \
  "$(q "select (ledger_balance('$O1'::uuid))[3]")"
chk "one more unit is refused" "{$OVERFLOWS_DEBITS_POSTED}" \
  "$(st $T2,$O1,$O2,1,0,1,53)"
chk "nothing changed" "340282366920938463463374607431768211455" \
  "$(q "select (ledger_balance('$O1'::uuid))[3]")"

# 50: the posted credits are at the limit
P1=00000000-0000-0000-0000-00000000007c
P2=00000000-0000-0000-0000-00000000007d
P3=00000000-0000-0000-0000-00000000007e
s1 $P1 0 10 53 >/dev/null; s1 $P2 0 20 53 >/dev/null; s1 $P3 0 10 53 >/dev/null
st $T3,$P3,$P2,340282366920938463463374607431768211455,0,1,53 >/dev/null
chk "one more unit into it is refused" "{$OVERFLOWS_CREDITS_POSTED}" \
  "$(st $T4,$P1,$P2,1,0,1,53)"

# 47: the pending debits are at the limit
Q1=00000000-0000-0000-0000-00000000007f
Q2=00000000-0000-0000-0000-000000000080
Q3=00000000-0000-0000-0000-000000000081
s1 $Q1 0 10 53 >/dev/null; s1 $Q2 0 20 53 >/dev/null; s1 $Q3 0 10 53 >/dev/null
st $T5,$Q1,$Q2,340282366920938463463374607431768211455,2,1,53 >/dev/null
chk "one more pending unit is refused" "{$OVERFLOWS_DEBITS_PENDING}" \
  "$(st $T6,$Q1,$Q3,1,2,1,53)"

# 48: the pending credits are at the limit
S1=00000000-0000-0000-0000-000000000082
S2=00000000-0000-0000-0000-000000000083
S3=00000000-0000-0000-0000-000000000084
s1 $S1 0 10 53 >/dev/null; s1 $S2 0 20 53 >/dev/null; s1 $S3 0 10 53 >/dev/null
st $T7,$S3,$S2,340282366920938463463374607431768211455,2,1,53 >/dev/null
chk "one more pending unit into it is refused" "{$OVERFLOWS_CREDITS_PENDING}" \
  "$(st $T8,$S1,$S2,1,2,1,53)"

# 51 and 52: the sum of the pending and the posted counters overflows, while
# neither counter does.  The posted takes just under half the range, the
# pending reserves the other half, and one more unit breaks the sum alone.
# Fresh ids: the ones above were consumed by the refused batches.
ids 53
U1=00000000-0000-0000-0000-000000000085
U2=00000000-0000-0000-0000-000000000086
U3=00000000-0000-0000-0000-000000000087
U4=00000000-0000-0000-0000-000000000088
U5=00000000-0000-0000-0000-000000000089
U6=00000000-0000-0000-0000-00000000008a
s1 $U1 0 10 53 >/dev/null; s1 $U2 0 20 53 >/dev/null
s1 $U3 0 10 53 >/dev/null; s1 $U4 0 20 53 >/dev/null
s1 $U5 0 10 53 >/dev/null; s1 $U6 0 20 53 >/dev/null
chk "the posted takes just under half" "{$CREATED}" \
  "$(st $T1,$U1,$U2,170141183460469231731687303715884105727,0,1,53)"
chk "the pending takes the other half" "{$CREATED}" \
  "$(st $T2,$U1,$U3,170141183460469231731687303715884105728,2,1,53)"
chk "the sum overflows, not either counter" "{$OVERFLOWS_DEBITS}" \
  "$(st $T3,$U1,$U4,1,0,1,53)"
chk "the counter itself did not change" "170141183460469231731687303715884105727" \
  "$(q "select (ledger_balance('$U1'::uuid))[3]")"
# and the same on the credit side, with a clean debit account
chk "the credit side is set up" "{$CREATED}" \
  "$(st $T4,$U5,$U6,170141183460469231731687303715884105727,0,1,53)"
chk "its pending reserves the rest" "{$CREATED}" \
  "$(st $T5,$U5,$U6,170141183460469231731687303715884105728,2,1,53)"
chk "the credit sum overflows too" "{$OVERFLOWS_CREDITS}" \
  "$(st $T6,$U3,$U6,1,0,1,53)"

ids 50
echo "--- 41. the journal holds the whole amount (ledger 54) ---"
J1=00000000-0000-0000-0000-00000000008b
J2=00000000-0000-0000-0000-00000000008c
J3=00000000-0000-0000-0000-00000000008d
J4=00000000-0000-0000-0000-00000000008e
s1 $J1 0 10 54 >/dev/null; s1 $J2 0 20 54 >/dev/null
s1 $J3 0 10 54 >/dev/null; s1 $J4 0 20 54 >/dev/null
st $T1,$J1,$J2,18446744073709551616,0,1,54 >/dev/null
chk "the journal's row holds the whole amount" "18446744073709551616" \
  "$(q "select amount from ledger_journal_lookup('$T1'::uuid)")"
chk "the replay reproduces the counters" "0" "$(recon $J1 $J2)"
# a full-width pending transfer that expires releases the whole reservation
st $T2,$J3,$J4,18446744073709551616,2,1,54,00000000000000000000000000000000,1 >/dev/null
chk "the reservation is held" "18446744073709551616" \
  "$(q "select (ledger_balance('$J3'::uuid))[1]")"
sleep 3
chk "the reservation is released in full" "0" \
  "$(q "select (ledger_balance('$J3'::uuid))[1]")"
chk "the expiry's record holds the amount" "18446744073709551616" \
  "$(q "select amount from ledger_journal_lookup('$T2'::uuid) order by seq desc limit 1")"
chk "the journal still agrees with the engine" "0" "$(recon $J3 $J4)"

ids 51
echo "--- 42. the post path's sentinel is the largest amount (ledger 55) ---"
Z1=00000000-0000-0000-0000-00000000008f
Z2=00000000-0000-0000-0000-000000000090
V1=00000000-0000-0000-0000-000000000091
V2=00000000-0000-0000-0000-000000000092
s1 $Z1 0 10 55 >/dev/null; s1 $Z2 0 20 55 >/dev/null
# a void of a pending transfer past 64 bits releases the whole reservation
s1 $V1 0 10 55 >/dev/null; s1 $V2 0 20 55 >/dev/null
st $T5,$V1,$V2,18446744073709551616,2,1,55 >/dev/null
chk "a void of a full-width pending releases it all" "{$CREATED}" \
  "$(st $T6,$V1,$V2,0,8,1,55,$T5)"
chk "the full-width void moved no money" "0" \
  "$(q "select (ledger_balance('$V1'::uuid))[3]")"
chk "the full-width void's reservation is gone" "0" \
  "$(q "select (ledger_balance('$V1'::uuid))[1]")"
st $T1,$Z1,$Z2,100,2,1,55 >/dev/null
chk "a post of the largest amount posts the whole pending" "{$CREATED}" \
  "$(st $T2,$Z1,$Z2,340282366920938463463374607431768211455,4,1,55,$T1)"
chk "the whole pending moved" "100" \
  "$(q "select (ledger_balance('$Z1'::uuid))[3]")"
# a partial post still posts exactly what it asks for
st $T3,$Z1,$Z2,80,2,1,55 >/dev/null
chk "a partial post moves the part" "{$CREATED}" \
  "$(st $T4,$Z1,$Z2,30,4,1,55,$T3)"
chk "only the part moved" "130" \
  "$(q "select (ledger_balance('$Z1'::uuid))[3]")"
# A post releases the pending transfer's whole reservation,
# even a partial one, and keeps only the posted part on the posted side
chk "the whole reservation is released" "0" \
  "$(q "select (ledger_balance('$Z1'::uuid))[1]")"

ids 52
echo "--- 43. the grid-wide total does not wrap (ledger 56) ---"
# two accounts hold 2^127 each, so the sum of the ledger's posted debits is
# 2^128: one more than a single counter can hold
Q1=00000000-0000-0000-0000-0000000000a0
Q2=00000000-0000-0000-0000-0000000000a1
Q3=00000000-0000-0000-0000-0000000000a2
Q4=00000000-0000-0000-0000-0000000000a3
s1 $Q1 0 10 56 >/dev/null; s1 $Q2 0 20 56 >/dev/null
s1 $Q3 0 10 56 >/dev/null; s1 $Q4 0 20 56 >/dev/null
st $T1,$Q1,$Q2,170141183460469231731687303715884105728,0,1,56 >/dev/null
st $T2,$Q3,$Q4,170141183460469231731687303715884105728,0,1,56 >/dev/null
chk "one account holds 2^127" "170141183460469231731687303715884105728" \
  "$(q "select (ledger_balance('$Q1'::uuid))[3]")"
chk "the ledger's posted debits total 2^128" "340282366920938463463374607431768211456" \
  "$(q "select (ledger_ledger_totals(56))[4]")"
chk "and its posted credits too" "340282366920938463463374607431768211456" \
  "$(q "select (ledger_ledger_totals(56))[5]")"
chk "the journal still agrees with the engine" "0" "$(recon $Q1 $Q2 $Q3 $Q4)"

ids 54
echo "--- 44. a replica carries the ledger, and a failover loses nothing (ledger 60) ---"
# the second node follows the first one: it asks for the records after the
# position it has, applies them, and acknowledges what it has applied
reconr(){ local ids=""; for id in "$@"; do ids="$ids${ids:+,}'$id'::uuid"; done; qr "select count(*) from ledger_journal_balances() j where j.id in ($ids) and (j.debits_pending <> (ledger_balance(j.id))[1] or j.credits_pending <> (ledger_balance(j.id))[2] or j.debits_posted <> (ledger_balance(j.id))[3] or j.credits_posted <> (ledger_balance(j.id))[4])"; }
# an account or a transfer on the replica
sr1(){ ps2 ledgerpg2 postgres "select ledger_create_account('$1',$2,$3,$4)" "select ledger_last_statuses()" | tail -1; }
srt(){ ps2 ledgerpg2 postgres "select ledger_apply(decode('$(mk "$@")','hex'))" "select ledger_last_statuses()" | tail -1; }

follow

s1 $A 0 10 60 >/dev/null
s1 $B 0 20 60 >/dev/null
chk "the transfer is committed" "{$CREATED}" "$(st $T1,$A,$B,100,0,1,60)"
# the commit waits for the replica, so what the client is told about is
# already on the other node
chk "the replica has the money" "100" "$(qr "select (ledger_balance('$A'::uuid))[3]")"
chk "the replica's credit account has it too" "100" "$(qr "select (ledger_balance('$B'::uuid))[4]")"
chk "the replica's journal holds the transfer" "100" \
  "$(qr "select amount from ledger_journal_lookup('$T1'::uuid)")"
chk "the replica's journal is the primary's length" "$(q "select ledger_journal_count()")" \
  "$(qr "select ledger_journal_count()")"
chk "the replica's journal agrees with its engine" "0" "$(reconr $A $B)"
chk "the replica's ledger totals are the primary's" "$(tot 60)" \
  "$(qr "select ledger_ledger_totals(60)")"

# the primary dies: the ledger must be on the other node
docker kill ledgerpg >/dev/null
sleep 2
# the replica is promoted: it stops following and starts deciding
promote
sleep 1
chk "the failover lost nothing" "100" "$(qr "select (ledger_balance('$A'::uuid))[3]")"
chk "the credit survived it too" "100" "$(qr "select (ledger_balance('$B'::uuid))[4]")"
chk "the transfer is still in the journal" "100" \
  "$(qr "select amount from ledger_journal_lookup('$T1'::uuid)")"
chk "the promoted node's journal agrees with its engine" "0" "$(reconr $A $B)"

# the promoted node decides for itself, and its journal continues where the
# replicated records left off
P1=00000000-0000-0000-0000-0000000000b0
P2=00000000-0000-0000-0000-0000000000b1
P3=00000000-0000-0000-0000-0000000000b2
P4=00000000-0000-0000-0000-0000000000b3
sr1 $P1 0 10 61 >/dev/null
sr1 $P2 0 20 61 >/dev/null
before=$(qr "select ledger_journal_count()")
chk "the promoted node accepts a transfer" "{$CREATED}" "$(srt $P3,$P1,$P2,50,0,1,61)"
chk "the journal continued where the replication left off" "$((before + 1))" \
  "$(qr "select ledger_journal_count()")"
chk "the new transfer moved the money" "50" "$(qr "select (ledger_balance('$P1'::uuid))[3]")"

# the primary comes back, and the suite's last group needs it
docker start ledgerpg >/dev/null
waitpg

ids 55
echo "--- 45. the ledger refuses clearly at its capacity ---"
# the grid's size is fixed when the server starts, so the capacity is
# lowered with a setting and a restart.  The ledger already holds more
# transfers than a small table would, so the capacity is lowered to just
# above what is there, and the ledger is filled to the new limit.
CAPA=00000000-0000-0000-0000-0000000000d1
CAPB=00000000-0000-0000-0000-0000000000d2
FREE=100
have=$(q "select ledger_transfer_count()")
cap=$((have + FREE))
docker exec ledgerpg psql -U postgres -d ledger -Atc \
  "alter system set ledgerext.max_transfers = $cap" >/dev/null
docker restart ledgerpg >/dev/null; waitpg
chk "the grid is smaller now" "$cap" "$(q "show ledgerext.max_transfers")"
chk "the capacity is visible" "$cap" "$(q "select (ledger_capacity())[4]")"

s1 $CAPA 0 10 70 >/dev/null
s1 $CAPB 0 20 70 >/dev/null
# a pending transfer with a timeout: it has to expire even at the limit
python3 bench/mkburst.py 1 $CAPA $CAPB 70 3 300000 > "$TMPDIR/cap1.hex"
python3 -c "
h=open('$TMPDIR/cap1.hex').read().strip()
print(\"select ledger_apply(decode('\" + h + \"','hex'));\")" > "$TMPDIR/cap1.sql"
docker exec -i ledgerpg psql -U postgres -d ledger -Atf - < "$TMPDIR/cap1.sql" | tail -1
chk "the pending reservation is held" "7" "$(q "select (ledger_balance('$CAPA'::uuid))[1]")"
# now fill the table: the rest of the batch does not fit
python3 bench/mkburst.py 200 $CAPA $CAPB 70 0 400000 > "$TMPDIR/cap2.hex"
python3 -c "
h=open('$TMPDIR/cap2.hex').read().strip()
print(\"select ledger_apply(decode('\" + h + \"','hex'));\")
print(\"select count(*) filter (where s = 4294967295), count(*) filter (where s = 101) from unnest(ledger_last_statuses()) s;\")" > "$TMPDIR/cap2.sql"
IFS='|' read fit refused <<< "$(docker exec -i ledgerpg psql -U postgres -d ledger -Atf - < "$TMPDIR/cap2.sql" | tail -1)"
chk "the transfers that fitted were created" "$((FREE - 1))" "$fit"
chk "the ones that did not fit were refused" "$((200 - FREE + 1))" "$refused"
chk "the refusal has its own name" "no_transfer_slots" "$(q "select ledger_status_name(101)")"
chk "only the money that fitted moved" "$(( (FREE - 1) * 7 ))" "$(q "select (ledger_balance('$CAPA'::uuid))[3]")"
chk "the journal still agrees with the engine" "0" "$(recon $CAPA $CAPB)"
chk "the transfer count is the capacity" "$cap" "$(q "select ledger_transfer_count()")"

# the money comes back even at the limit: the expiry needs no new row
sleep 4
chk "the reservation is released at the limit" "0" "$(q "select (ledger_balance('$CAPA'::uuid))[1]")"
chk "the journal still agrees with the engine" "0" "$(recon $CAPA $CAPB)"

# the server restarts at the limit: the journal fits the grid
docker restart ledgerpg >/dev/null; waitpg
chk "the server restarts at the limit" "1" "$(q "select 1")"
chk "the balances survived the restart" "$(( (FREE - 1) * 7 ))" "$(q "select (ledger_balance('$CAPA'::uuid))[3]")"
chk "the journal still agrees with the engine" "0" "$(recon $CAPA $CAPB)"

# a ledger that does not fit a smaller grid says so, and says what to do.
# The accounts still fit, so it is the transfers that do not.
acc=$(q "select ledger_account_count()")
pd=$(q "show data_directory")
docker exec ledgerpg rm -f "$pd/ledgerext.snap"
docker exec ledgerpg psql -U postgres -d ledger -Atc \
  "alter system set ledgerext.max_transfers = $((acc + 16))" >/dev/null
docker restart ledgerpg >/dev/null 2>&1 || true
sleep 5
chk "the server refuses to start on a ledger that does not fit" "1" \
  "$(docker logs ledgerpg 2>&1 | tail -40 | grep -c 'transfer table is full')"
chk "the refusal names the file" "1" \
  "$(docker logs ledgerpg 2>&1 | tail -40 | grep -q 'ledgerext.journal' && echo 1 || echo 0)"
chk "the refusal names the setting" "1" \
  "$(docker logs ledgerpg 2>&1 | tail -40 | grep -q 'ledgerext.max_transfers' && echo 1 || echo 0)"
chk "the refusal says the data is not lost" "1" \
  "$(docker logs ledgerpg 2>&1 | tail -40 | grep -q 'not lost' && echo 1 || echo 0)"

# and the ledger is not damaged by the refusal: put the capacity back and
# the whole ledger is there, exactly
vol=$(docker inspect ledgerpg --format '{{range .Mounts}}{{if eq .Destination "/var/lib/postgresql"}}{{.Name}}{{end}}{{end}}')
docker run --rm -v "$vol":/d postgres:18 sh -c \
  'sed -i "/max_transfers/d" /d/18/docker/postgresql.auto.conf'
docker start ledgerpg >/dev/null 2>&1 || true
waitpg
chk "the ledger is whole again" "$(( (FREE - 1) * 7 ))" "$(q "select (ledger_balance('$CAPA'::uuid))[3]")"

# the account table refuses the same way
docker exec ledgerpg rm -f "$pd/ledgerext.snap"
docker exec ledgerpg psql -U postgres -d ledger -Atc \
  "alter system set ledgerext.max_accounts = 16" >/dev/null
docker restart ledgerpg >/dev/null 2>&1 || true
sleep 5
chk "the server refuses to start on a ledger that does not fit the accounts" "1" \
  "$(docker logs ledgerpg 2>&1 | tail -40 | grep -c 'account table is full')"
chk "the refusal names the accounts setting" "1" \
  "$(docker logs ledgerpg 2>&1 | tail -40 | grep -q 'ledgerext.max_accounts' && echo 1 || echo 0)"
docker run --rm -v "$vol":/d postgres:18 sh -c \
  'sed -i "/max_accounts/d" /d/18/docker/postgresql.auto.conf'
docker start ledgerpg >/dev/null 2>&1 || true
waitpg
chk "the whole ledger is back" "$(( (FREE - 1) * 7 ))" "$(q "select (ledger_balance('$CAPA'::uuid))[3]")"
chk "the journal still agrees with the engine" "0" "$(recon $CAPA $CAPB)"
# the transfer that was refused was never recorded, so its id was not burnt
REFUSED=$(printf '00000000-0000-0000-0000-%012x' $((0x400000 + FREE - 1)))
chk "the refused transfer is accepted now" "{$CREATED}" \
  "$(st $REFUSED,$CAPA,$CAPB,7,0,1,70)"
chk "and it moved the money" "$(( FREE * 7 ))" "$(q "select (ledger_balance('$CAPA'::uuid))[3]")"
chk "the journal still agrees with the engine" "0" "$(recon $CAPA $CAPB)"

ids 56
echo "--- 46. the chain is the chain ---"
# The chain of an account is the records that named it, in commit order: the
# audit trail from an account back through the transfers that touched it.
# The stamp builds it out of the rows the apply resolved, so the chain is
# worth checking: it is the thing that must not change.
s1 $A 0 10 71 >/dev/null
s1 $B 0 20 71 >/dev/null
s1 $C 0 30 71 >/dev/null
chk "T1 created" "{$CREATED}" "$(st $T1,$A,$B,100,0,1,71)"
chk "T2 created" "{$CREATED}" "$(st $T2,$B,$A,100,0,1,71)"
chk "T3 created" "{$CREATED}" "$(st $T3,$A,$C,100,0,1,71)"
chk "the chain of A is the records that named it" "$T1|$T2|$T3" \
  "$(q "select string_agg(id::text,'|' order by seq) from ledger_journal_account('$A'::uuid)")"
chk "the chain of B is its own" "$T1|$T2" \
  "$(q "select string_agg(id::text,'|' order by seq) from ledger_journal_account('$B'::uuid)")"
chk "the chain of C is its own" "$T3" \
  "$(q "select string_agg(id::text,'|' order by seq) from ledger_journal_account('$C'::uuid)")"
# a transfer that did not take effect is not a fact, so it is not in the chain
chk "the transfer that failed" "{$CREDIT_NOT_FOUND}" "$(st $T4,$A,$X,100,0,1,71)"
chk "the failed transfer is not in the chain" "$T1|$T2|$T3" \
  "$(q "select string_agg(id::text,'|' order by seq) from ledger_journal_account('$A'::uuid)")"
# a broken chain leaves nothing behind, in the chain either
chk "the broken chain" "{$LINKED_FAILED,$DEBIT_NOT_FOUND,$LINKED_FAILED}" \
  "$(st $T5,$A,$B,100,1,1,71 $T6,$X,$B,100,1,1,71 $T8,$A,$B,100,0,1,71)"
chk "the chain has no trace of the broken chain" "$T1|$T2|$T3" \
  "$(q "select string_agg(id::text,'|' order by seq) from ledger_journal_account('$A'::uuid)")"
# the release of a pending transfer is a record of the account too
chk "the pending transfer is created" "{$CREATED}" \
  "$(st $T7,$A,$B,100,2,1,71,00000000000000000000000000000000,1)"
sleep 4
chk "the chain holds the pending and its release" "$T1|$T2|$T3|$T7|$T7" \
  "$(q "select string_agg(id::text,'|' order by seq) from ledger_journal_account('$A'::uuid)")"
# the chain survives a restart: the head is in the grid and the links are in
# the journal
docker restart ledgerpg >/dev/null; waitpg
chk "the chain survives a restart" "$T1|$T2|$T3|$T7|$T7" \
  "$(q "select string_agg(id::text,'|' order by seq) from ledger_journal_account('$A'::uuid)")"
chk "the balances are the balances" "200,100" \
  "$(q "select (ledger_balance('$A'::uuid))[3]||','||(ledger_balance('$A'::uuid))[4]")"
chk "the journal still agrees with the engine" "0" "$(recon $A $B $C)"

ids 57
echo "--- 47. the journal's record: 176 bytes, and it says the same things ---"
SA=$A
SB=$B
s1 $SA 0 10 57 >/dev/null; s1 $SB 0 20 57 >/dev/null
chk "a pending transfer is created" "{$CREATED}" "$(st $T1,$SA,$SB,100,2,1,57)"
chk "the reservation is held" "100,100" \
  "$(q "select (ledger_balance('$SA'::uuid))[1]||','||(ledger_balance('$SB'::uuid))[2]")"
chk "the pending's own record says what it reserved" "100|100" \
  "$(q "select amount||'|'||amount_actual from ledger_journal_lookup('$T1'::uuid) where pending_id = '00000000-0000-0000-0000-000000000000'::uuid limit 1")"
chk "the post is created" "{$CREATED}" "$(st $T2,$SA,$SB,100,4,1,57,$T1)"
chk "the reservation is released and the money is posted" "0,0,100,0" \
  "$(q "select (ledger_balance('$SA'::uuid))[1]||','||(ledger_balance('$SA'::uuid))[2]||','||(ledger_balance('$SA'::uuid))[3]||','||(ledger_balance('$SA'::uuid))[4]")"
chk "and the credit account has it" "0,0,0,100" \
  "$(q "select (ledger_balance('$SB'::uuid))[1]||','||(ledger_balance('$SB'::uuid))[2]||','||(ledger_balance('$SB'::uuid))[3]||','||(ledger_balance('$SB'::uuid))[4]")"
chk "the post's own record says what it moved" "100|100" \
  "$(q "select amount||'|'||amount_actual from ledger_journal_lookup('$T2'::uuid) limit 1")"
chk "the journal's replay agrees with the engine" "0" "$(recon $SA $SB)"
chk "another pending transfer is created" "{$CREATED}" "$(st $T3,$SA,$SB,40,2,1,57)"
chk "it is voided" "{$CREATED}" "$(st $T4,$SA,$SB,0,8,1,57,$T3)"
chk "the void released the reservation" "0" \
  "$(q "select (ledger_balance('$SA'::uuid))[1]")"
chk "the journal's replay agrees with the engine after the void" "0" "$(recon $SA $SB)"
# the record's size is the disk's cost of a transfer, and the test pins it
pd=$(q "show data_directory")
sz=$(docker exec ledgerpg stat -c %s "$pd/ledgerext.journal")
n=$(q "select ledger_journal_count()")
chk "the record is 176 bytes" "176" "$(( (sz - 4096) / n ))"
chk "the journal's header is a page" "4096" "$(( sz - n * 176 ))"

echo "--- 48. the snapshot's cost is a fraction of the journal's ---"
# the body is deferred while the journal behind the last one is under the
# threshold, and a deferred snapshot writes nothing at all
q "select ledger_snapshot()" >/dev/null
snap0=$(docker exec ledgerpg stat -c '%s %Y' "$pd/ledgerext.snap")
sleep 8
snap1=$(docker exec ledgerpg stat -c '%s %Y' "$pd/ledgerext.snap")
chk "the automatic snapshot deferred the body" "$snap0" "$snap1"
chk "the threshold follows the body's size" "32" \
  "$(q "show ledgerext.snapshot_body_multiple")"

# the journal's record carries its book in the high half of its flags field.
# The flags are the record's second uint32, at byte 148 of the 176-byte
# record, little-endian, so the book is in bytes 150 and 151; the id is the
# record's first 16 bytes.
tq(){ ps1 ledgerpg "$1" "$2"; }
tq2(){ ps1 ledgerpg "$1" "$2" 2>&1; }
# apply a batch as a role, then read the statuses in the same session: the
# statuses live in the backend that applied them
tst(){ ps2 ledgerpg "$1" "select ledger_apply(decode('$(mk "${@:2}")','hex'))" \
  "select ledger_last_statuses()" | tail -1; }
# create one account as a role, then read the status
ts1(){ ps2 ledgerpg "$1" "select ledger_create_account('$2',$3,$4,$5)" \
  "select ledger_last_statuses()" | tail -1; }
# the ledger's functions, which the administrator grants to the roles that
# may use the ledger
BOOKFUNCS="ledger_apply(bytea), ledger_create_accounts(bytea), ledger_create_account(uuid,int,int,int), ledger_last_statuses(), ledger_status_name(bigint), ledger_account_status_name(bigint), ledger_amount(numeric), ledger_totals(), ledger_ledger_totals(int), ledger_balance(uuid), ledger_lookup_transfer(uuid), ledger_pending_transfers(), ledger_journal_lookup(uuid), ledger_journal_account(uuid,int), ledger_journal_ledger(int,int), ledger_journal_range(bigint,bigint,int), ledger_journal_balances(), ledger_period_balances(int)"
tenant_of_record(){ q "select get_byte(r, i*176+151)*256 + get_byte(r, i*176+150)
  from ledger_journal_records(0, 4096) r, generate_series(0, length(r)/176 - 1) i
  where substring(r from i*176+1 for 16) = uuid_send('$1'::uuid)"; }

# The books' groups read the journal's records byte for byte and follow them
# from a second node, and they are written for a ledger that holds only their
# own work: after a long run the journal is past the 4096 records the read
# takes and the replica's catch-up is a timing check.  Recreate the cluster
# the way the refusal groups did.
echo "--- 49. a fresh cluster for the books ---"
docker rm -f -v ledgerpg ledgerpg2 >/dev/null 2>&1
$REPO/scripts/setup.sh >/dev/null 2>&1
chk "a fresh cluster starts" "1" "$(q "select 1")"

ids 58
echo "--- 50. the ledger is a set of books, one per tenant ---"
# Two roles, two books, and the money of one is not the money of the other.
# The administrator binds a role with a row in the extension's own table:
#   insert into ledger_tenants values ('tenant_a', 2);
# A role with no row has no book at all, and the books' functions go to the
# role that owns the book - the cluster's own functions do not.
q "create role tenant_a login" >/dev/null
q "create role tenant_b login" >/dev/null
q "create role tenant_none login" >/dev/null
q "insert into ledger_tenants values ('tenant_a', 2), ('tenant_b', 3)" >/dev/null
q "grant execute on function $BOOKFUNCS to tenant_a, tenant_b" >/dev/null
# both books use the same ledger number on purpose: the ledger is the
# client's own partition and says nothing about the books
chk "tenant_a opens its book" "{$CREATED}" "$(ts1 tenant_a $A 0 10 61)"
chk "tenant_a's second account" "{$CREATED}" "$(ts1 tenant_a $B 0 20 61)"
chk "tenant_b opens its book" "{$CREATED}" "$(ts1 tenant_b $C 0 10 61)"
chk "tenant_b's second account" "{$CREATED}" "$(ts1 tenant_b $D 0 20 61)"
chk "tenant_a's transfer is created" "{$CREATED}" "$(tst tenant_a $T1,$A,$B,100,0,1,61)"
chk "tenant_b's transfer is created" "{$CREATED}" "$(tst tenant_b $T2,$C,$D,100,0,1,61)"
chk "tenant_a's own money" "{0,0,100,0,0,10,61}" "$(tq tenant_a "select ledger_balance('$A'::uuid)")"
chk "tenant_a's credit account" "{0,0,0,100,0,20,61}" "$(tq tenant_a "select ledger_balance('$B'::uuid)")"
chk "tenant_b's own money" "{0,0,100,0,0,10,61}" "$(tq tenant_b "select ledger_balance('$C'::uuid)")"
chk "tenant_a is not told tenant_b's balance" "" "$(tq tenant_a "select ledger_balance('$C'::uuid)")"
chk "tenant_b is not told tenant_a's balance" "" "$(tq tenant_b "select ledger_balance('$A'::uuid)")"
chk "tenant_a's totals hold only its book" "{2,0,0,100,100}" "$(tq tenant_a "select ledger_ledger_totals(61)")"
chk "tenant_b's totals hold only its book" "{2,0,0,100,100}" "$(tq tenant_b "select ledger_ledger_totals(61)")"
chk "tenant_a's grand total is its book" "{2,0,0,100,100}" "$(tq tenant_a "select ledger_totals()")"
chk "tenant_b's grand total is its book" "{2,0,0,100,100}" "$(tq tenant_b "select ledger_totals()")"
chk "tenant_a's pending transfer" "{$CREATED}" "$(tst tenant_a $T3,$A,$B,50,2,1,61)"
chk "tenant_a holds a reservation" "1" "$(tq tenant_a "select count(*) from ledger_pending_transfers()")"
chk "tenant_b sees no reservation" "0" "$(tq tenant_b "select count(*) from ledger_pending_transfers()")"
chk "tenant_b cannot look up tenant_a's transfer" "" "$(tq tenant_b "select id from ledger_journal_lookup('$T1'::uuid)")"
chk "tenant_a can look up its own transfer" "1" "$(tq tenant_a "select count(*) from ledger_journal_lookup('$T1'::uuid) where id = '$T1'::uuid")"
chk "tenant_a's journal holds its own record" "2" "$(tq tenant_a "select count(*) from ledger_journal_account('$A'::uuid)")"
chk "tenant_b's journal holds its own record" "1" "$(tq tenant_b "select count(*) from ledger_journal_account('$C'::uuid)")"
chk "tenant_a cannot read tenant_b's journal" "0" "$(tq tenant_a "select count(*) from ledger_journal_account('$C'::uuid)")"
chk "tenant_b's ledger journal holds no other book" "0" "$(tq tenant_b "select count(*) from ledger_journal_ledger(61) where debit in ('$A'::uuid,'$B'::uuid) or credit in ('$A'::uuid,'$B'::uuid)")"
chk "tenant_a's ledger journal holds no other book" "0" "$(tq tenant_a "select count(*) from ledger_journal_ledger(61) where debit in ('$C'::uuid,'$D'::uuid) or credit in ('$C'::uuid,'$D'::uuid)")"
chk "tenant_b's ledger journal holds its own transfer" "1" "$(tq tenant_b "select count(*) from ledger_journal_ledger(61) where id = '$T2'::uuid")"
chk "tenant_b cannot see tenant_a's transfer in the ledger journal" "0" "$(tq tenant_b "select count(*) from ledger_journal_ledger(61) where id = '$T1'::uuid")"
chk "tenant_a's range holds no other book" "0" "$(tq tenant_a "select count(*) from ledger_journal_range(0, 9223372036854775807, -1) where debit in ('$C'::uuid,'$D'::uuid) or credit in ('$C'::uuid,'$D'::uuid)")"
chk "tenant_a's journal balances hold its book" "2" "$(tq tenant_a "select count(*) from ledger_journal_balances()")"
chk "tenant_b's journal balances hold its book" "2" "$(tq tenant_b "select count(*) from ledger_journal_balances()")"

# a write that names another book's account is refused, and moves nothing
chk "tenant_a cannot move tenant_b's money" "{102}" "$(tst tenant_a $T4,$C,$D,10,0,1,61)"
chk "tenant_b's account is untouched" "{0,0,100,0,0,10,61}" "$(tq tenant_b "select ledger_balance('$C'::uuid)")"
chk "tenant_a cannot take tenant_b's account id" "{102}" "$(ts1 tenant_a $C 0 10 61)"
chk "tenant_b's account is still tenant_b's" "{0,0,100,0,0,10,61}" "$(tq tenant_b "select ledger_balance('$C'::uuid)")"
chk "the status of a transfer across books" "account_in_another_tenant" "$(q "select ledger_status_name(102)")"
chk "the status of an account in another book" "account_in_another_tenant" "$(q "select ledger_account_status_name(102)")"

# a transfer that carries another book's transfer id is refused with the
# extension's own status, and the fields are never compared, so nothing of
# the other book's transfer leaks through the status: not its amount, its
# code, its ledger, its pending id, its timeout or its accounts
chk "tenant_b cannot take tenant_a's transfer id" "{103}" "$(tst tenant_b $T1,$C,$D,100,0,1,61)"
chk "tenant_a's transfer is untouched" "{50,0,100,0,0,10,61}" "$(tq tenant_a "select ledger_balance('$A'::uuid)")"
chk "tenant_b's own transfer is untouched" "{0,0,100,0,0,10,61}" "$(tq tenant_b "select ledger_balance('$C'::uuid)")"
chk "the status of an id in another book" "id_in_another_tenant" "$(q "select ledger_status_name(103)")"
chk "the account vocabulary has no such status" "" "$(q "select ledger_account_status_name(103)")"

# a post or a void of another book's pending transfer is refused, and the
# refusal says only that: the pending's own fields are never compared, so
# nothing of another book's transfer leaks through the status
chk "tenant_b cannot post tenant_a's pending transfer" "{102}" "$(tst tenant_b $T6,$A,$B,50,4,1,61,$T3)"
chk "tenant_b cannot void tenant_a's pending transfer" "{102}" "$(tst tenant_b $T7,$A,$B,50,8,1,61,$T3)"
chk "the refused post moved no money" "{50,0,100,0,0,10,61}" "$(tq tenant_a "select ledger_balance('$A'::uuid)")"
chk "the pending transfer is still pending" "1" "$(tq tenant_a "select count(*) from ledger_pending_transfers()")"
chk "tenant_b holds no reservation" "0" "$(tq tenant_b "select count(*) from ledger_pending_transfers()")"

# the binding table is the administrator's: a bound role may read it, and
# only the owner writes it
chk "a bound role can read its own binding" "2" \
  "$(tq tenant_a "select tenant from ledger_tenants where role_name = current_user")"
chk "a bound role cannot write the binding table" "permission denied for table ledger_tenants" \
  "$(tq2 tenant_a "insert into ledger_tenants values ('tenant_c', 4)" | grep -o 'permission denied for table ledger_tenants' | head -1)"
chk "the binding table still holds its three rows" "3" "$(q "select count(*) from ledger_tenants")"

# a role with no binding has no book: it is refused on a write and on a read,
# and the refusal names the role and the table
q "grant execute on function ledger_apply(bytea), ledger_balance(uuid), ledger_totals() to tenant_none" >/dev/null
refusal=$(tq2 tenant_none "select ledger_balance('$A'::uuid)")
chk "a role with no binding cannot move money" "has no tenant" \
  "$(tq2 tenant_none "select ledger_apply(decode('$(mk $T5,$A,$B,1,0,1,61)','hex'))" | grep -o 'has no tenant' | head -1)"
chk "a role with no binding cannot read its balance" "has no tenant" \
  "$(echo "$refusal" | grep -o 'has no tenant' | head -1)"
chk "the refusal names the role" "tenant_none" \
  "$(echo "$refusal" | grep -o 'tenant_none' | head -1)"
chk "the refusal names the table" "ledger_tenants" \
  "$(echo "$refusal" | grep -o 'ledger_tenants' | head -1)"

# the record carries the book, so a restart rebuilds each account in its own
chk "the record of tenant_a's transfer carries tenant 2" "2" "$(tenant_of_record $T1)"
chk "the record of tenant_b's transfer carries tenant 3" "3" "$(tenant_of_record $T2)"
chk "the record of tenant_a's account carries tenant 2" "2" "$(tenant_of_record $A)"
chk "the record of tenant_b's account carries tenant 3" "3" "$(tenant_of_record $C)"

hardkill
chk "tenant_a's book survives the restart" "{50,0,100,0,0,10,61}" "$(tq tenant_a "select ledger_balance('$A'::uuid)")"
chk "tenant_b's book survives the restart" "{0,0,100,0,0,10,61}" "$(tq tenant_b "select ledger_balance('$C'::uuid)")"
chk "tenant_a is still not told tenant_b's balance" "" "$(tq tenant_a "select ledger_balance('$C'::uuid)")"
chk "the books are still apart in the journal" "2" "$(tq tenant_a "select count(*) from ledger_journal_balances()")"
chk "tenant_b's book is still apart in the journal" "2" "$(tq tenant_b "select count(*) from ledger_journal_balances()")"

# A SECURITY DEFINER function must not lend the caller its owner's book.
# The tenant is the session's own role, so the definer's function acts on the
# caller's book, and the caller never sees the owner's.
q "create function svc_read() returns numeric[] language plpgsql security definer as \$\$ begin return ledger_totals(); end \$\$" >/dev/null
q "alter function svc_read() owner to tenant_b" >/dev/null
q "grant execute on function svc_read() to tenant_a, tenant_b" >/dev/null
before=$(tq tenant_a "select ledger_totals()")
owner=$(tq tenant_b "select ledger_totals()")
chk "the definer's function reads the caller's book" "$before" "$(tq tenant_a "select svc_read()")"
chk "the definer's function reads the other caller's own book" "$owner" "$(tq tenant_b "select svc_read()")"
chk "the caller's read after the definer's call is its own book" "" \
  "$(tq2 tenant_a "begin; select svc_read(); select ledger_balance('$C'::uuid); commit;" | grep -vE '^(BEGIN|COMMIT|ROLLBACK)$' | tail -1)"
chk "the caller's write after the definer's call is refused" "{102}" \
  "$(ps2 ledgerpg tenant_a "begin; select svc_read(); select ledger_apply(decode('$(mk $T9,$C,$D,10,0,1,61)','hex')); commit;" "select ledger_last_statuses()" | tail -1)"
chk "the owner's money is untouched" "{0,0,100,0,0,10,61}" \
  "$(tq tenant_b "select ledger_balance('$C'::uuid)")"
q "drop function svc_read()" >/dev/null

# The tenant is the role that is acting, and a SET ROLE is a change of role:
# the transaction keeps the book it had when it first touched the ledger, and
# the next transaction picks up the role that is then acting.
q "create role tenant_m login" >/dev/null
q "grant tenant_a, tenant_b to tenant_m" >/dev/null
q "insert into ledger_tenants values ('tenant_m', 4)" >/dev/null
q "grant execute on function ledger_totals(), ledger_balance(uuid) to tenant_m" >/dev/null
chk "a SET ROLE before the ledger acts on the new role's book" \
  "{50,0,100,0,0,10,61}" \
  "$(ps1 ledgerpg tenant_m "begin; set role tenant_a; select ledger_balance('$A'::uuid); commit;" | grep -vE '^(BEGIN|COMMIT|ROLLBACK|SET)$' | tail -1)"
chk "the transaction keeps its own book across a SET ROLE" "" \
  "$(ps1 ledgerpg tenant_m "begin; select ledger_totals(); set role tenant_a; select ledger_balance('$A'::uuid); commit;" | grep -vE '^(BEGIN|COMMIT|ROLLBACK|SET)$' | tail -1)"
chk "the next transaction picks up the role that is acting" "{0,0,100,0,0,10,61}" \
  "$(ps1 ledgerpg tenant_m "begin; set role tenant_b; select ledger_balance('$C'::uuid); commit;" | grep -vE '^(BEGIN|COMMIT|ROLLBACK|SET)$' | tail -1)"
q "drop owned by tenant_m" >/dev/null 2>&1
q "delete from ledger_tenants where role_name = 'tenant_m'" >/dev/null 2>&1
q "revoke tenant_a, tenant_b from tenant_m" >/dev/null 2>&1
q "drop role tenant_m" >/dev/null 2>&1

q "drop owned by tenant_a" >/dev/null 2>&1
q "drop owned by tenant_b" >/dev/null 2>&1
q "drop owned by tenant_none" >/dev/null 2>&1
q "delete from ledger_tenants where role_name in ('tenant_a','tenant_b')" >/dev/null 2>&1
q "drop role tenant_a" >/dev/null 2>&1
q "drop role tenant_b" >/dev/null 2>&1
q "drop role tenant_none" >/dev/null 2>&1

echo "--- 51. the failover carries the books, and keeps them apart ---"
# The cluster was recreated after group 44, so the second node is a primary
# of its own again: it is configured as a replica once more.
#
# A standby carries the books in the records it applies, but its bindings
# are its own: the administrator binds a role on every node, and the number
# must be the one the primary stamped in the records.
nq(){ ps1 ledgerpg2 "$1" "$2"; }
# apply a batch as a role on the second node, then read the statuses there
nst(){ ps2 ledgerpg2 "$1" "select ledger_apply(decode('$(mk "${@:2}")','hex'))" \
  "select ledger_last_statuses()" | tail -1; }
q "create role tenant_a login" >/dev/null
q "create role tenant_b login" >/dev/null
q "insert into ledger_tenants values ('tenant_a', 2), ('tenant_b', 3)" >/dev/null
q "grant execute on function $BOOKFUNCS to tenant_a, tenant_b" >/dev/null
qr "create role tenant_a login" >/dev/null
qr "create role tenant_b login" >/dev/null
qr "insert into ledger_tenants values ('tenant_a', 2), ('tenant_b', 3)" >/dev/null
qr "grant execute on function $BOOKFUNCS to tenant_a, tenant_b" >/dev/null
chk "the books are still the books of the roles" "{50,0,100,0,0,10,61}" \
  "$(tq tenant_a "select ledger_balance('$A'::uuid)")"
chk "the other book is still the other one" "{0,0,100,0,0,10,61}" \
  "$(tq tenant_b "select ledger_balance('$C'::uuid)")"

follow
for i in $(seq 1 60); do
  [ "$(qr "select ledger_journal_count()")" = "$(q "select ledger_journal_count()")" ] && break
  sleep 1
done
chk "the second node has the primary's records" "$(q "select ledger_journal_count()")" \
  "$(qr "select ledger_journal_count()")"
chk "the second node carries tenant_a's book" "{50,0,100,0,0,10,61}" \
  "$(nq tenant_a "select ledger_balance('$A'::uuid)")"
chk "the second node carries tenant_b's book" "{0,0,100,0,0,10,61}" \
  "$(nq tenant_b "select ledger_balance('$C'::uuid)")"
chk "the second node keeps the books apart" "" \
  "$(nq tenant_a "select ledger_balance('$C'::uuid)")"
chk "the second node carries the reservation" "1" \
  "$(nq tenant_a "select count(*) from ledger_pending_transfers()")"

# the primary dies and the standby is promoted: it starts deciding
docker kill ledgerpg >/dev/null
sleep 2
promote
sleep 1
chk "the promoted node keeps tenant_a's book" "{50,0,100,0,0,10,61}" \
  "$(nq tenant_a "select ledger_balance('$A'::uuid)")"
chk "the promoted node keeps tenant_b's book" "{0,0,100,0,0,10,61}" \
  "$(nq tenant_b "select ledger_balance('$C'::uuid)")"
chk "the promoted node keeps the books apart" "" \
  "$(nq tenant_a "select ledger_balance('$C'::uuid)")"
chk "the promoted node accepts the book's own write" "{$CREATED}" \
  "$(nst tenant_a $T8,$A,$B,10,0,1,61)"
chk "the promoted node's book holds the new money" "{50,0,110,0,0,10,61}" \
  "$(nq tenant_a "select ledger_balance('$A'::uuid)")"
chk "the promoted node refuses a write across books" "{102}" \
  "$(nst tenant_b $T9,$A,$B,10,0,1,61)"
chk "the refused write moved no money" "{50,0,110,0,0,10,61}" \
  "$(nq tenant_a "select ledger_balance('$A'::uuid)")"
chk "the other book's money is untouched" "{0,0,100,0,0,10,61}" \
  "$(nq tenant_b "select ledger_balance('$C'::uuid)")"

# the primary comes back, and the suite's last group needs it
docker start ledgerpg >/dev/null
waitpg
q "drop owned by tenant_a" >/dev/null 2>&1
q "drop owned by tenant_b" >/dev/null 2>&1
q "delete from ledger_tenants where role_name in ('tenant_a','tenant_b')" >/dev/null 2>&1
q "drop role tenant_a" >/dev/null 2>&1
q "drop role tenant_b" >/dev/null 2>&1
qr "drop owned by tenant_a" >/dev/null 2>&1
qr "drop owned by tenant_b" >/dev/null 2>&1
qr "delete from ledger_tenants where role_name in ('tenant_a','tenant_b')" >/dev/null 2>&1
qr "drop role tenant_a" >/dev/null 2>&1
qr "drop role tenant_b" >/dev/null 2>&1

echo "--- 25. the account table is bounded (this group must be last) ---"
cat > "$TMPDIR/cap.sql" <<'SQL'
select ledger_create_accounts(decode(
  (select string_agg(substr(lpad(to_hex(:n),8,'0'), 2*g-1, 2), '' order by g desc)
     from generate_series(1,4) g)||'01000000'||'0000000000000000'||
  (select string_agg(encode(uuid_send(gen_random_uuid()),'hex')
       || repeat('00',16) || '0000000000000000'
       || '00000000' || '00000001' || '00000001' || '00000000', '')
   from generate_series(1,:n)), 'hex'));
select count(*) from unnest(ledger_last_statuses()) s where s = 100;
SQL
have=$(q "select ledger_account_count()")
refused=0
for i in 1 2 3 4 5 6 7; do
  c=$(docker exec -i ledgerpg psql -U postgres -d ledger -At -v n=200000 -f - < "$TMPDIR/cap.sql" | tail -1)
  refused=$((refused + c))
done
chk "the events that did not fit are refused" "$((1400000 - (1048576 - have)))" "$refused"
chk "the table is full" "1048576" "$(q "select ledger_account_count()")"

echo
if [ $fails -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$fails CHECK(S) FAILED"; fi
exit $fails
