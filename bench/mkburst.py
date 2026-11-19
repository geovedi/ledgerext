#!/usr/bin/env python3
"""Build a hex batch of N transfers, for the burst and hot-path checks.

usage: mkburst.py <n> <debit> <credit> <ledger> <timeout> <id-prefix>

A timeout of 0 makes the transfers single-phase; a timeout of 1 makes them
pending transfers that expire in one second.  The id prefix is a hex
integer the ids count up from, so two batches never share an id.

The wire format belongs to mkbatch.py; this only says how many of which.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkbatch

n = int(sys.argv[1])
debit = sys.argv[2]
credit = sys.argv[3]
ledger = int(sys.argv[4])
timeout = int(sys.argv[5])
base = int(sys.argv[6], 16)
flags = 2 if timeout else 0

specs = ["%032x,%s,%s,7,%d,1,%d,00000000000000000000000000000000,%d"
         % (base + i, debit, credit, flags, ledger, timeout)
         for i in range(n)]
print(mkbatch.transfers(specs))
