-- Vanilla-Postgres baseline schema: ledger-shaped.
-- Accounts carry the four denormalized balance counters; transfers is the
-- only other table.  Logged (not UNLOGGED) so the WAL comparison is fair.
drop table if exists tb_transfers, tb_accounts;

create table tb_accounts (
  id uuid primary key,
  debits_posted bigint not null default 0,
  credits_posted bigint not null default 0,
  debits_pending bigint not null default 0,
  credits_pending bigint not null default 0
) with (fillfactor=60);

create table tb_transfers (
  id uuid primary key,
  debit_account_id uuid not null,
  credit_account_id uuid not null,
  amount bigint not null
);

insert into tb_accounts(id) select gen_random_uuid() from generate_series(1,200000);
