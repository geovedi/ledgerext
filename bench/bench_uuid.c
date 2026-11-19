/* UUID-based apply, parameterised by account/transfer table, with optional
   hash index.  Reports server-tps; CPU measured by the wrapper. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <libpq-fe.h>
static long long now_us(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec*1000000LL+ts.tv_nsec/1000;}
static void put_i16(unsigned char*p,int16_t v){uint16_t u=(uint16_t)v;p[0]=(u>>8)&0xff;p[1]=u&0xff;}
static void put_i32(unsigned char*p,int32_t v){uint32_t u=(uint32_t)v;for(int i=0;i<4;i++)p[i]=(u>>(8*(3-i)))&0xff;}
static void put_i64(unsigned char*p,int64_t v){uint64_t u=(uint64_t)v;for(int i=0;i<8;i++)p[i]=(u>>(8*(7-i)))&0xff;}
int main(int argc,char**argv){
  const char*acct=argc>1?argv[1]:"tb_accounts";
  const char*xfer=argc>2?argv[2]:"tb_transfers";
  int batch=argc>3?atoi(argv[3]):8189;
  int iters=argc>4?atoi(argv[4]):40;
  int nacc =argc>5?atoi(argv[5]):200000;
  const char*lockmode=getenv("LOCK")?getenv("LOCK"):"both"; /* both|update|none */
  const char*dsn=getenv("PGDSN")?getenv("PGDSN"):"host=127.0.0.1 port=55432 dbname=ledger user=postgres";
  PGconn*cn=PQconnectdb(dsn);
  if(PQstatus(cn)!=CONNECTION_OK){fprintf(stderr,"conn: %s\n",PQerrorMessage(cn));return 1;}
  PGresult*r; char q[1024];
  r=PQexec(cn,"create temp table tb_stage (id uuid, debit_account_id uuid, credit_account_id uuid, amount bigint)");
  if(PQresultStatus(r)!=PGRES_COMMAND_OK){fprintf(stderr,"temp: %s\n",PQerrorMessage(cn));return 1;} PQclear(r);
  snprintf(q,sizeof q,"select id::text from %s limit %d",acct,nacc);
  r=PQexec(cn,q);
  if(PQresultStatus(r)!=PGRES_TUPLES_OK){fprintf(stderr,"acct: %s\n",PQerrorMessage(cn));return 1;}
  int na=PQntuples(r);
  unsigned char(*aids)[16]=malloc((size_t)na*16);
  for(int i=0;i<na;i++){int L=PQgetlength(r,i,0);const char*sv=PQgetvalue(r,i,0);
    unsigned char out[16];int o=0;
    for(int k=0;k<L&&o<16;k++){char ch=sv[k];if(ch=='-')continue;
      if(ch>='0'&&ch<='9')ch-=48;else if(ch>='a'&&ch<='f')ch-=87;else if(ch>='A'&&ch<='F')ch-=55;out[o++]=(unsigned char)ch;}
    if(o!=16){fprintf(stderr,"bad uuid\n");return 1;} memcpy(aids[i],out,16);}
  PQclear(r);
  size_t rowlen=2+(4+16)*3+(4+8), hdrlen=19;
  size_t plen=hdrlen+(size_t)batch*rowlen+2;
  unsigned char*buf=malloc(plen);
  memcpy(buf,"PGCOPY\n\xff\r\n\0",11); put_i32(buf+11,0); put_i32(buf+15,0);
  unsigned char*p=buf+hdrlen;
  for(int i=0;i<batch;i++){put_i16(p,4);p+=2;
    put_i32(p,16);memcpy(p+4,aids[0],16);p+=20;
    put_i32(p,16);memcpy(p+4,aids[0],16);p+=20;
    put_i32(p,16);memcpy(p+4,aids[0],16);p+=20;
    put_i32(p,8);put_i64(p+4,100);p+=12;}
  put_i16(p,-1);
  char sqlcopy[128]; snprintf(sqlcopy,sizeof sqlcopy,"copy tb_stage from stdin (format binary)");
  char sql[4096]; sql[0]=0;
  /* none = transport only; insert = +transfer log; update/both = +balances */
  if(strcmp(lockmode,"none")!=0){
    snprintf(sql,sizeof sql,
      "insert into %s(id,debit_account_id,credit_account_id,amount) select id,debit_account_id,credit_account_id,amount from tb_stage;",xfer);
  }
  if(strcmp(lockmode,"none")!=0 && strcmp(lockmode,"update")!=0 && strcmp(lockmode,"insert")!=0){
    char tmp[4096]; snprintf(tmp,sizeof tmp,
      "select 1 from %s a where a.id in (select debit_account_id from tb_stage union select credit_account_id from tb_stage) order by a.id for update;",acct);
    strcat(sql,tmp);
  }
  if(strcmp(lockmode,"none")!=0 && strcmp(lockmode,"insert")!=0){
    char tmp[4096]; snprintf(tmp,sizeof tmp,
      "update %s a set debits_posted=a.debits_posted+coalesce(d.deb,0), credits_posted=a.credits_posted+coalesce(d.cred,0)"
      " from (select acct, coalesce(sum(amt) filter (where side='d'),0) deb, coalesce(sum(amt) filter (where side='c'),0) cred"
      "   from (select debit_account_id as acct,amount as amt,'d' as side from tb_stage"
      "         union all select credit_account_id,amount,'c' from tb_stage) z group by acct order by acct) d"
      " where a.id=d.acct;",acct);
    strcat(sql,tmp);
  }
  struct timespec ts0; clock_gettime(CLOCK_REALTIME,&ts0);
  uint64_t base=((uint64_t)getpid()<<44)|((uint64_t)ts0.tv_nsec<<22)|((uint64_t)ts0.tv_sec<<2);
  long long t0=now_us(); long long total=0;
  for(int it=0;it<iters;it++){
    for(int i=0;i<batch;i++){
      unsigned char*row=buf+hdrlen+(size_t)i*rowlen;
      memset(row+6,0,16); put_i64(row+6,base); put_i64(row+14,(uint64_t)it*batch+i+1);
      uint64_t h=(uint64_t)(it*batch+i)*2654435761u+12345;
      memcpy(row+26,aids[h%(uint64_t)na],16); memcpy(row+46,aids[(h>>17)%(uint64_t)na],16);
    }
    r=PQexec(cn,"BEGIN"); PQclear(r);
    r=PQexec(cn,"truncate tb_stage"); PQclear(r);
    r=PQexec(cn,sqlcopy);
    if(PQresultStatus(r)!=PGRES_COPY_IN){fprintf(stderr,"copy: %s\n",PQerrorMessage(cn));return 1;} PQclear(r);
    PQputCopyData(cn,buf,(int)plen); PQputCopyEnd(cn,NULL);
    r=PQgetResult(cn); while(r&&PQresultStatus(r)==PGRES_COPY_IN) r=PQgetResult(cn);
    if(!r||PQresultStatus(r)!=PGRES_COMMAND_OK){fprintf(stderr,"copyend: %s\n",PQerrorMessage(cn));return 1;} PQclear(r);
    if(sql[0]){
      r=PQexec(cn,sql);
      ExecStatusType st=PQresultStatus(r);
      if(st!=PGRES_TUPLES_OK&&st!=PGRES_COMMAND_OK){fprintf(stderr,"sql: %s\n",PQerrorMessage(cn));return 1;} PQclear(r);
    }
    total+=batch;
    r=PQexec(cn,"COMMIT");
    if(PQresultStatus(r)!=PGRES_COMMAND_OK){fprintf(stderr,"commit: %s\n",PQerrorMessage(cn));return 1;} PQclear(r);
  }
  long long t1=now_us();
  printf("%-12s/%-12s lock=%-7s n=%lld wall=%.3fs tps=%.0f\n",acct,xfer,lockmode,total,(t1-t0)/1e6,total/((t1-t0)/1e6));
  PQfinish(cn); return 0;
}
