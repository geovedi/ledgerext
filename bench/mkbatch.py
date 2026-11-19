#!/usr/bin/env python3
"""Build a packed ledger batch (the bytea ledger_apply() takes) as hex.

Wire format, matching ledgerext/ledgerext.h:

  batch header (16 bytes): uint32 count | uint32 kind | uint32 pad | uint32 pad
  transfer (144 bytes):    id[16] | debit[16] | credit[16] | pending_id[16]
                           | user_data[16] | uint128 amount | uint128 amount_actual
                           | uint64 timestamp | uint32 flags | uint32 code
                           | uint32 ledger | uint32 timeout | uint64 pad
  account (56 bytes):      id[16] | user_data[16] | uint64 timestamp
                           | uint32 flags | uint32 code | uint32 ledger | uint32 pad

kind: 0 = transfers, 1 = accounts.

Usage:
  mkbatch.py TRANSFER [TRANSFER ...]      -> a transfer batch
  mkbatch.py --accounts ACCOUNT ...        -> an account batch
where TRANSFER = id,debit,credit,amount[,flags[,code[,ledger[,pending_id[,timeout]]]]]
      ACCOUNT = id[,flags[,code[,ledger]]]

the timeout is in seconds and belongs to a pending transfer; it is 0 when
the transfer has none.

the amount is a decimal of any width, from 0 to 2^128-1; a negative amount
is refused here, not packed as a wrapped number.  It is carried as 16 bytes,
the low 64 bits first.

ids are 32 hex chars (16 bytes); "0"*32 means "none".

Transfer flags: 1=LINKED 2=PENDING 4=POST 8=VOID 16=BALANCING_DEBIT
                32=BALANCING_CREDIT 64=CLOSING_DEBIT 128=CLOSING_CREDIT
Account flags:  1=LINKED 2=DEBITS_MUST_NOT_EXCEED_CREDITS
                4=CREDITS_MUST_NOT_EXCEED_DEBITS
"""
import struct
import sys

LINKED, PENDING, POST, VOID = 1, 2, 4, 8
BAL_DEBIT, BAL_CREDIT, CLOSE_DEBIT, CLOSE_CREDIT = 16, 32, 64, 128
ZERO = "0" * 32


def h(x: str) -> bytes:
    x = x.strip().replace("-", "")
    if len(x) != 32:
        raise SystemExit("id must be 32 hex chars: %r" % x)
    return bytes.fromhex(x)


def transfers(specs):
    out = bytearray(struct.pack("<IIII", len(specs), 0, 0, 0))
    for s in specs:
        f = s.split(",")
        if len(f) < 4:
            raise SystemExit("need at least id,debit,credit,amount: %r" % s)
        tid, debit, credit, amount = f[0], f[1], f[2], int(f[3])
        if amount < 0:
            raise SystemExit("the amount is unsigned: %r" % f[3])
        if amount > (1 << 128) - 1:
            raise SystemExit("the amount is above 2^128-1: %r" % f[3])
        flags = int(f[4]) if len(f) > 4 and f[4] != "" else 0
        code = int(f[5]) if len(f) > 5 and f[5] != "" else 1
        ledger = int(f[6]) if len(f) > 6 and f[6] != "" else 1
        pending = f[7] if len(f) > 7 and f[7] != "" else ZERO
        timeout = int(f[8]) if len(f) > 8 and f[8] != "" else 0
        out += struct.pack(
            "<16s16s16s16s16sQQQQQIIIIQ",
            h(tid), h(debit), h(credit), h(pending), bytes(16),
            amount & 0xFFFFFFFFFFFFFFFF, amount >> 64,
            0, 0,
            0, flags, code, ledger, timeout,
            0,
        )
    return out.hex()


def accounts(specs):
    out = bytearray(struct.pack("<IIII", len(specs), 1, 0, 0))
    for s in specs:
        f = s.split(",")
        aid = f[0]
        flags = int(f[1]) if len(f) > 1 and f[1] != "" else 0
        code = int(f[2]) if len(f) > 2 and f[2] != "" else 1
        ledger = int(f[3]) if len(f) > 3 and f[3] != "" else 1
        out += struct.pack("<16s16sQIIII", h(aid), bytes(16), 0, flags, code, ledger, 0)
    return out.hex()


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0] == "--accounts":
        print(accounts(args[1:]))
    else:
        print(transfers(args))
