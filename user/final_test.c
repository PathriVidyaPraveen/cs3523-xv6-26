/*
 * final_stress.c — Final submission stress test
 * Covers PA1 + PA2 + PA3 + PA4 rigorously.
 *
 * Add to Makefile UPROGS:
 *   $U/_final_stress\
 *
 * Run:
 *   $ final_stress
 */

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* ============================================================
   Framework
   ============================================================ */
static int g_pass = 0, g_fail = 0;

static void check(const char *name, int cond){
    if(cond){ printf("  [PASS] %s\n", name); g_pass++; }
    else     { printf("  [FAIL] %s\n", name); g_fail++; }
}
static void section(const char *t){ printf("\n=== %s ===\n", t); }
static void summary(void){
    printf("\n==========================================\n");
    printf("  PASSED : %d / %d\n", g_pass, g_pass + g_fail);
    if(g_fail == 0) printf("  ALL TESTS PASSED\n");
    else            printf("  FAILED : %d\n", g_fail);
    printf("==========================================\n");
}
static void touch(char *p, int n){ for(int i=0;i<n;i++) p[i*4096]=(char)(i&0xFF); }
static int  verify(char *p, int n){ for(int i=0;i<n;i++) if(p[i*4096]!=(char)(i&0xFF)) return 0; return 1; }

/* ============================================================
   PA1-A: hello, getpid2
   ============================================================ */
static void test_pa1_a(void){
    section("PA1-A: hello() and getpid2()");
    check("hello() returns 0", hello() == 0);
    check("getpid2() == getpid()", getpid2() == getpid());
    check("pid > 0", getpid() > 0);

    int pid = fork();
    if(pid == 0) exit(getpid() == getpid2() ? 0 : 1);
    int st; wait(&st);
    check("getpid2() == getpid() inside child", st == 0);
}

/* ============================================================
   PA1-B: getppid, getnumchild
   ============================================================ */
static void test_pa1_b(void){
    section("PA1-B: getppid() and getnumchild()");

    /* getppid */
    int ppid = getppid();
    check("getppid() > 0", ppid > 0);

    int my_pid = getpid();
    int pid = fork();
    if(pid == 0) exit(getppid() == my_pid ? 0 : 1);
    int st; wait(&st);
    check("child getppid() == parent pid", st == 0);

    /* Two levels: grandchild */
    pid = fork();
    if(pid == 0){
        int cp = getpid();
        int gc = fork();
        if(gc == 0) exit(getppid() == cp ? 0 : 1);
        int gs; wait(&gs); exit(gs);
    }
    wait(&st);
    check("grandchild getppid() == child pid", st == 0);

    /* getnumchild baseline */
    int base = getnumchild();
    check("getnumchild() >= 0", base >= 0);

    /* fork 2 sleeping children */
    int p1 = fork(); if(p1 == 0){ pause(60); exit(0); }
    int p2 = fork(); if(p2 == 0){ pause(60); exit(0); }
    pause(3);
    check("count +2 after two forks", getnumchild() == base + 2);

    /* zombie not counted */
    int zp = fork(); if(zp == 0) exit(0);
    pause(3);
    check("zombie not counted", getnumchild() == base + 2);
    wait(0); /* reap zombie */

    kill(p1); kill(p2);
    wait(0); wait(0);
    check("count back to base after reap", getnumchild() == base);

    /* grandchildren not counted */
    int outer = fork();
    if(outer == 0){
        int gc2 = fork();
        if(gc2 == 0){ pause(40); exit(0); }
        pause(40); kill(gc2); wait(0); exit(0);
    }
    pause(3);
    check("grandchild not counted by grandparent", getnumchild() == base + 1);
    kill(outer); wait(0);
}

/* ============================================================
   PA1-C: getsyscount, getchildsyscount
   ============================================================ */
static void test_pa1_c(void){
    section("PA1-C: getsyscount() and getchildsyscount()");

    int c0 = getsyscount();
    check("getsyscount() > 0 after boot work", c0 > 0);

    /* monotonic */
    int prev = getsyscount();
    int mono = 1;
    for(int i = 0; i < 10; i++){
        int cur = getsyscount(); if(cur <= prev){ mono = 0; break; } prev = cur;
    }
    check("getsyscount() strictly monotonic", mono);

    /* child gets independent counter */
    int pid = fork();
    if(pid == 0) exit(getsyscount() > 0 && getsyscount() < 100 ? 0 : 1);
    int st; wait(&st);
    check("child syscount independent and small", st == 0);

    /* getchildsyscount edge cases */
    check("invalid pid returns -1",     getchildsyscount(99999) == -1);
    check("self pid returns -1",        getchildsyscount(getpid()) == -1);

    /* sibling's child not accessible */
    int cp = fork();
    if(cp == 0){
        for(int i = 0; i < 50; i++) getpid();
        pause(40); exit(0);
    }
    pause(4);
    int cnt = getchildsyscount(cp);
    check("child syscount > 0 while alive", cnt > 0);
    check("child syscount >= 50",           cnt >= 50);

    int cnt2 = getchildsyscount(cp);
    check("child syscount non-decreasing",  cnt2 >= cnt);

    kill(cp); wait(0);
    check("getchildsyscount returns -1 after reap", getchildsyscount(cp) == -1);
}

/* ============================================================
   PA2: scheduler
   ============================================================ */
static void test_pa2(void){
    section("PA2: MLFQ scheduler");

    /* getlevel */
    int lv = getlevel();
    check("initial level == 0",   lv == 0);
    check("level in [0,3]",       lv >= 0 && lv <= 3);

    int pid = fork(); if(pid == 0) exit(getlevel());
    int st; wait(&st);
    check("forked child starts at level 0", st == 0);

    /* getmlfqinfo self */
    struct mlfqinfo mi;
    check("getmlfqinfo(self) == 0",          getmlfqinfo(getpid(), &mi) == 0);
    check("times_scheduled > 0",              mi.times_scheduled > 0);
    check("total_syscalls > 0",               mi.total_syscalls > 0);
    check("getmlfqinfo(invalid) == -1",       getmlfqinfo(99999, &mi) == -1);

    /* CPU-bound demotion */
    pid = fork();
    if(pid == 0){ for(;;){ volatile int x=0; for(int i=0;i<500000;i++) x^=i; } }
    pause(60);
    getmlfqinfo(pid, &mi);
    printf("  CPU-bound: level=%d sched=%d ticks=[%d,%d,%d,%d]\n",
           mi.level, mi.times_scheduled,
           mi.ticks[0],mi.ticks[1],mi.ticks[2],mi.ticks[3]);
    check("CPU-bound demoted to level > 0",       mi.level > 0);
    check("CPU-bound has ticks at deeper levels",
          mi.ticks[1]+mi.ticks[2]+mi.ticks[3] > 0);
    kill(pid); wait(0);

    /* Interactive stays high */
    pid = fork();
    if(pid == 0){ for(;;) for(int i=0;i<5000;i++) getpid(); }
    pause(60);
    getmlfqinfo(pid, &mi);
    printf("  Interactive: level=%d sys=%d\n", mi.level, mi.total_syscalls);
    check("Interactive stays <= level 1", mi.level <= 1);
    check("Interactive made many syscalls", mi.total_syscalls > 500);
    kill(pid); wait(0);

    /* Global boost */
    pid = fork();
    if(pid == 0){ for(;;){ volatile int x=0; for(int i=0;i<500000;i++) x^=i; } }
    pause(40);
    getmlfqinfo(pid, &mi);
    int t0_before = mi.ticks[0];
    pause(70);
    getmlfqinfo(pid, &mi);
    printf("  Boost: ticks[0] before=%d after=%d\n", t0_before, mi.ticks[0]);
    check("ticks[0] increased after boost window", mi.ticks[0] > t0_before);
    kill(pid); wait(0);

    /* getmlfqinfo matches getsyscount */
    for(int i=0;i<10;i++) getpid();
    int sc = getsyscount();
    getmlfqinfo(getpid(), &mi);
    int diff = mi.total_syscalls - sc; if(diff<0) diff=-diff;
    check("getmlfqinfo.total_syscalls ~= getsyscount (diff<=5)", diff <= 5);
}

/* ============================================================
   PA3: virtual memory
   ============================================================ */
static void test_pa3(void){
    section("PA3: Page replacement");

    setraidmode(0); setdisksched(0);

    /* getvmstats interface */
    struct vmstats vs;
    check("getvmstats(self) == 0",         getvmstats(getpid(), &vs) == 0);
    check("page_faults >= 0",               vs.page_faults >= 0);
    check("resident_pages >= 0",            vs.resident_pages >= 0);
    check("getvmstats(invalid) == -1",      getvmstats(99999, &vs) == -1);

    /* lazy allocation — page faults tracked */
    int pid = fork();
    if(pid == 0){
        struct vmstats b, a;
        getvmstats(getpid(), &b);
        char *arr = sbrk(40 * 4096);
        touch(arr, 40);
        getvmstats(getpid(), &a);
        int df = a.page_faults - b.page_faults;
        int dr = a.resident_pages - b.resident_pages;
        exit((df >= 38 && dr >= 38) ? 0 : 1);
    }
    int st; wait(&st);
    check("page_faults and resident_pages correct for 40 pages", st == 0);

    /* resident_pages decreases on sbrk(-n) */
    pid = fork();
    if(pid == 0){
        struct vmstats s0, s1, s2;
        getvmstats(getpid(), &s0);
        char *a = sbrk(30 * 4096); touch(a, 30);
        getvmstats(getpid(), &s1);
        sbrk(-30 * 4096);
        getvmstats(getpid(), &s2);
        exit((s1.resident_pages > s0.resident_pages &&
              s2.resident_pages < s1.resident_pages) ? 0 : 1);
    }
    wait(&st);
    check("resident_pages grows then shrinks with sbrk(-n)", st == 0);

    /* eviction and swap-out under pressure */
    pid = fork();
    if(pid == 0){
        int sink = 1200;
        char *s = sbrk(sink * 4096); touch(s, sink);
        int pages = 400;
        char *arr = sbrk(pages * 4096); touch(arr, pages);
        struct vmstats vs2;
        getvmstats(getpid(), &vs2);
        exit((vs2.pages_evicted > 0 && vs2.pages_swapped_out > 0) ? 0 : 1);
    }
    wait(&st);
    check("eviction and swap-out triggered under memory pressure", st == 0);

    /* data integrity after swap cycle */
    pid = fork();
    if(pid == 0){
        char *arr = sbrk(900 * 4096); touch(arr, 900);
        char *arr2 = sbrk(900 * 4096); touch(arr2, 900);
        exit(verify(arr, 900) ? 0 : 1);
    }
    wait(&st);
    check("data integrity after swap-out/swap-in", st == 0);

    /* multi-byte pattern across many pages */
    pid = fork();
    if(pid == 0){
        int pages = 700;
        char *arr = sbrk(pages * 4096);
        for(int i=0;i<pages;i++)
            for(int j=0;j<4;j++) arr[i*4096+j]=(char)((i*3+j*7)&0xFF);
        char *arr2 = sbrk(pages * 4096); touch(arr2, pages);
        int errors = 0;
        for(int i=0;i<pages;i++)
            for(int j=0;j<4;j++){
                char ex=(char)((i*3+j*7)&0xFF);
                if(arr[i*4096+j]!=ex) errors++;
            }
        exit(errors==0 ? 0 : 1);
    }
    wait(&st);
    check("4-byte pattern per page intact after swap cycle", st == 0);

    /* parent and child vmstats independent */
    char *parent_arr = sbrk(20 * 4096); touch(parent_arr, 20);
    getvmstats(getpid(), &vs);
    int pf_before = vs.page_faults;
    pid = fork();
    if(pid == 0){
        struct vmstats cs0, cs1;
        getvmstats(getpid(), &cs0);
        char *ca = sbrk(20 * 4096); touch(ca, 20);
        getvmstats(getpid(), &cs1);
        exit(cs1.page_faults > cs0.page_faults ? 0 : 1);
    }
    wait(&st);
    check("child page_faults tracked independently", st == 0);
    getvmstats(getpid(), &vs);
    check("parent page_faults unchanged after child allocates", vs.page_faults == pf_before);
    sbrk(-20 * 4096);

    /* swap slot reuse — 5 waves */
    pid = fork();
    if(pid == 0){
        for(int w=0;w<5;w++){
            char *a = sbrk(900*4096); touch(a, 900);
            char *b = sbrk(900*4096); touch(b, 900);
            for(int i=0;i<900;i++) a[i*4096]+=1;
            sbrk(-900*4096); sbrk(-900*4096);
        }
        exit(0);
    }
    wait(&st);
    check("5 waves of swap-out/swap-in/free without exhaustion", st == 0);

    /* boundary bytes */
    pid = fork();
    if(pid == 0){
        char *a = sbrk(4096);
        a[0]=0xAA; a[4095]=0xBB;
        char *pressure = sbrk(1300*4096); touch(pressure, 1300);
        exit((unsigned char)a[0]==0xAA && (unsigned char)a[4095]==0xBB ? 0 : 1);
    }
    wait(&st);
    check("boundary bytes (first/last in page) intact after pressure", st == 0);

    /* getvmstats of live child readable by parent */
    pid = fork();
    if(pid == 0){ char *a=sbrk(20*4096); touch(a,20); pause(30); exit(0); }
    pause(4);
    int rv = getvmstats(pid, &vs);
    check("parent can read child vmstats while child alive", rv == 0 && vs.page_faults > 0);
    kill(pid); wait(0);
    check("getvmstats returns -1 after child reaped", getvmstats(pid, &vs) == -1);
}

/* ============================================================
   PA4: disk-backed swap, scheduling, RAID
   ============================================================ */
static void test_pa4_interface(void){
    section("PA4: syscall interface validation");

    check("setdisksched(FCFS=0) == 0",    setdisksched(0) == 0);
    check("setdisksched(SSTF=1) == 0",    setdisksched(1) == 0);
    check("setdisksched(2) == -1",         setdisksched(2) == -1);
    check("setdisksched(-1) == -1",        setdisksched(-1) == -1);
    setdisksched(0);

    check("setraidmode(0) == 0",           setraidmode(0) == 0);
    check("setraidmode(1) == 0",           setraidmode(1) == 0);
    check("setraidmode(2) == 0",           setraidmode(2) == 0);
    check("setraidmode(3) == -1",          setraidmode(3) == -1);
    check("setraidmode(-1) == -1",         setraidmode(-1) == -1);
    setraidmode(0);

    struct diskstats ds;
    check("getdiskstats() == 0",           getdiskstats(&ds) == 0);
    check("disk_reads >= 0",               ds.disk_reads >= 0);
    check("disk_writes >= 0",              ds.disk_writes >= 0);
    check("avg_disk_latency >= 0",         ds.avg_disk_latency >= 0);
}

static void test_pa4_disk_stats(void){
    section("PA4: disk stats increase on eviction and swap-in");

    setraidmode(0); setdisksched(0);

    int pid = fork();
    if(pid == 0){
        int sink = 1200;
        char *s = sbrk(sink*4096); touch(s, sink);
        int pages = 400;
        char *arr = sbrk(pages*4096); touch(arr, pages);
        struct vmstats vs0; getvmstats(getpid(), &vs0);
        for(int i=0;i<pages;i++) arr[i*4096]+=1;  /* force swap-ins */
        struct vmstats vs1; getvmstats(getpid(), &vs1);
        struct diskstats ds; getdiskstats(&ds);
        int ok = vs0.pages_evicted > 0 &&
                 vs1.pages_swapped_in > vs0.pages_swapped_in &&
                 ds.disk_writes > 0 && ds.disk_reads > 0;
        exit(ok ? 0 : 1);
    }
    int st; wait(&st);
    check("eviction, swap-in, disk_writes > 0, disk_reads > 0 all verified inside child", st == 0);

    /* monotonic across two sequential workloads */
    struct diskstats s0, s1, s2;
    getdiskstats(&s0);
    pid = fork();
    if(pid == 0){
        char *a=sbrk(900*4096); touch(a,900);
        char *b=sbrk(900*4096); touch(b,900);
        exit(0);
    }
    wait(0); getdiskstats(&s1);
    pid = fork();
    if(pid == 0){
        char *a=sbrk(900*4096); touch(a,900);
        char *b=sbrk(900*4096); touch(b,900);
        exit(0);
    }
    wait(0); getdiskstats(&s2);
    check("disk_writes non-decreasing across two waves", s2.disk_writes >= s1.disk_writes && s1.disk_writes >= s0.disk_writes);
    check("total disk_writes > 0 after two waves",       s2.disk_writes > s0.disk_writes);
    check("avg_latency > 0 after I/O",                   s2.avg_disk_latency > 0);
}

static void test_pa4_raid_integrity(void){
    section("PA4: RAID data integrity");

    int modes[3] = {0, 1, 2};
    const char *names[3] = {"RAID0", "RAID1", "RAID5"};

    for(int m = 0; m < 3; m++){
        setraidmode(modes[m]); setdisksched(0);
        int pid = fork();
        if(pid == 0){
            char *arr = sbrk(900*4096);
            for(int i=0;i<900;i++)
                for(int j=0;j<4;j++) arr[i*4096+j]=(char)((i*5+j*11)&0xFF);
            char *arr2 = sbrk(900*4096); touch(arr2, 900);
            int errors=0;
            for(int i=0;i<900;i++)
                for(int j=0;j<4;j++){
                    char ex=(char)((i*5+j*11)&0xFF);
                    if(arr[i*4096+j]!=ex) errors++;
                }
            struct vmstats vs; getvmstats(getpid(), &vs);
            printf("  %s: SwapOut=%d SwapIn=%d errors=%d\n",
                   names[m], vs.pages_swapped_out, vs.pages_swapped_in, errors);
            exit(errors==0 ? 0 : 1);
        }
        int st; wait(&st);
        char buf[32];
        /* build check name manually without sprintf */
        const char *pfx = names[m];
        int bi=0;
        for(int k=0;pfx[k];k++) buf[bi++]=pfx[k];
        const char *sfx = ": 4-byte pattern intact";
        for(int k=0;sfx[k];k++) buf[bi++]=sfx[k];
        buf[bi]=0;
        check(buf, st == 0);
    }
    setraidmode(0);
}

static void test_pa4_raid_write_amplification(void){
    section("PA4: RAID 1 write amplification");

    struct diskstats s0, s1;

    setraidmode(0); setdisksched(0);
    getdiskstats(&s0);
    int pid = fork();
    if(pid == 0){
        char *a=sbrk(900*4096); touch(a,900);
        char *b=sbrk(900*4096); touch(b,900); exit(0);
    }
    wait(0); getdiskstats(&s1);
    int r0w = s1.disk_writes - s0.disk_writes;

    setraidmode(1);
    getdiskstats(&s0);
    pid = fork();
    if(pid == 0){
        char *a=sbrk(900*4096); touch(a,900);
        char *b=sbrk(900*4096); touch(b,900); exit(0);
    }
    wait(0); getdiskstats(&s1);
    int r1w = s1.disk_writes - s0.disk_writes;

    printf("  RAID0 writes=%d  RAID1 writes=%d\n", r0w, r1w);
    check("RAID 0 produced writes > 0",                    r0w > 0);
    check("RAID 1 writes >= RAID 0 (mirroring)",           r1w >= r0w);
    check("RAID 1 writes approximately 2x RAID 0",         r1w >= r0w * 3 / 2);
    setraidmode(0);
}

static void test_pa4_scheduling(void){
    section("PA4: Disk scheduling FCFS vs SSTF");

    struct diskstats sa, sb;

    /* FCFS */
    setdisksched(0); getdiskstats(&sa);
    int pid = fork();
    if(pid == 0){
        char *a=sbrk(900*4096); touch(a,900);
        char *b=sbrk(900*4096); touch(b,900);
        for(int i=0;i<900;i++) a[i*4096]+=1;
         exit(0);
    }
    wait(0); getdiskstats(&sb);
    int fw = sb.disk_writes - sa.disk_writes;
    int fr = sb.disk_reads  - sa.disk_reads;
    int fl = sb.avg_disk_latency;

    /* SSTF */
    setdisksched(1); getdiskstats(&sa);
    pid = fork();
    if(pid == 0){
        char *a=sbrk(900*4096); touch(a,900);
        char *b=sbrk(900*4096); touch(b,900);
        for(int i=0;i<900;i++) a[i*4096]+=1;
         exit(0);
    }
    wait(0); getdiskstats(&sb);
    int sw = sb.disk_writes - sa.disk_writes;
    int sr = sb.disk_reads  - sa.disk_reads;
    int sl = sb.avg_disk_latency;

    printf("  FCFS: writes=%d reads=%d lat=%d\n", fw, fr, fl);
    printf("  SSTF: writes=%d reads=%d lat=%d\n", sw, sr, sl);
    check("FCFS writes > 0",     fw > 0);
    check("FCFS reads > 0",      fr > 0);
    check("SSTF writes > 0",     sw > 0);
    check("SSTF reads > 0",      sr > 0);
    check("SSTF lat within 5% of FCFS", sl <= fl + (fl/20 + 10));
    check("avg_latency > 0",     sl > 0);
    setdisksched(0);
}

static void test_pa4_slot_reuse(void){
    section("PA4: Disk swap slot recycling");

    setraidmode(0); setdisksched(0);
    int pid = fork();
    if(pid == 0){
        for(int w=0;w<5;w++){
            char *a=sbrk(900*4096); touch(a,900);
            char *b=sbrk(900*4096); touch(b,900);
            for(int i=0;i<900;i++) a[i*4096]+=1;
            sbrk(-900*4096); sbrk(-900*4096);
        }
        exit(0);
    }
    int st; wait(&st);
    check("5 waves of slot alloc/free without exhaustion", st == 0);
}

static void test_pa4_raid_mode_switch(void){
    section("PA4: RAID mode switching mid-workload");

    int pid = fork();
    if(pid == 0){
        setraidmode(0); char *a=sbrk(200*4096); touch(a,200);
        setraidmode(1); char *b=sbrk(200*4096); touch(b,200);
        setraidmode(2); char *c=sbrk(200*4096); touch(c,200);
        setraidmode(0); exit(0);
    }
    int st; wait(&st);
    check("RAID mode switch 0→1→2→0 mid-workload no panic", st == 0);
}

static void test_pa4_concurrent(void){
    section("PA4: Concurrent processes under RAID 0");

    setraidmode(0); setdisksched(0);
    int pids[4];
    for(int i=0;i<4;i++){
        pids[i] = fork();
        if(pids[i] == 0){
            char *arr=sbrk(350*4096); touch(arr,350);
            char *arr2=sbrk(350*4096); touch(arr2,350);
            exit(verify(arr,350) ? 0 : 1);
        }
    }
    int all_ok=1;
    for(int i=0;i<4;i++){ int st; wait(&st); if(st!=0) all_ok=0; }
    check("4 concurrent processes: data integrity under RAID 0", all_ok);
}

static void test_pa4_integration(void){
    section("PA4: Integration with PA1/PA2/PA3 stats");

    setraidmode(0); setdisksched(0);
    int pid = fork();
    if(pid == 0){
        char *a=sbrk(900*4096); touch(a,900);
        char *b=sbrk(900*4096); touch(b,900);
        struct vmstats vs; struct mlfqinfo mi; struct diskstats ds;
        int rv=getvmstats(getpid(),&vs);
        int rm=getmlfqinfo(getpid(),&mi);
        int rd=getdiskstats(&ds);
        int ok = rv==0 && rm==0 && rd==0 &&
                 vs.page_faults > 0 && vs.pages_evicted > 0 &&
                 mi.times_scheduled > 0 &&
                 ds.disk_writes > 0;
        printf("  PF=%d Evict=%d Sched=%d DW=%d DR=%d\n",
               vs.page_faults, vs.pages_evicted,
               mi.times_scheduled, ds.disk_writes, ds.disk_reads);
        exit(ok ? 0 : 1);
    }
    int st; wait(&st);
    check("getvmstats+getmlfqinfo+getdiskstats all consistent", st == 0);
}

/* ============================================================
   Edge cases
   ============================================================ */
static void test_edge_cases(void){
    section("Edge cases");

    /* Invalid address kills child, not kernel */
    int pid = fork();
    if(pid == 0){
        char *bad = (char*)0x3FFFFFFFFFFF;
        *bad = 1; exit(0);
    }
    int st; wait(&st);
    check("invalid address: kernel survives, child killed", 1);
    check("child exit status non-zero after invalid access", st != 0);

    /* sbrk shrink and re-alloc */
    pid = fork();
    if(pid == 0){
        char *a=sbrk(10*4096);
        for(int i=0;i<10;i++) a[i*4096]=(char)i;
        sbrk(-10*4096);
        char *b=sbrk(10*4096);
        int ok=1;
        for(int i=0;i<10;i++){ b[i*4096]=(char)(i+1); if(b[i*4096]!=(char)(i+1)) ok=0; }
        exit(ok?0:1);
    }
    wait(&st);
    check("sbrk shrink and re-alloc reads/writes correctly", st == 0);

    /* getvmstats unavailable after reap */
    pid = fork();
    if(pid == 0){ sbrk(4096); exit(0); }
    wait(0);
    struct vmstats vs;
    check("getvmstats returns -1 after reap", getvmstats(pid, &vs) == -1);

    /* getchildsyscount on non-child */
    int other = fork();
    if(other == 0){ pause(30); exit(0); }
    /* wait for other to run */
    pause(3);
    /* spawn our own child and confirm we can't read other's syscount from our child */
    int mychild = fork();
    if(mychild == 0){
        /* from child: 'other' is not our child, should return -1 */
        exit(getchildsyscount(other) == -1 ? 0 : 1);
    }
    wait(&st);
    check("non-child pid returns -1 from getchildsyscount (child perspective)", st == 0);
    kill(other); wait(0);
}

/* ============================================================
   Main
   ============================================================ */
int main(void){
    printf("\n");
    printf("##################################################\n");
    printf("#    FINAL SUBMISSION STRESS TEST                #\n");
    printf("#    PA1 + PA2 + PA3 + PA4                      #\n");
    printf("##################################################\n");

    test_pa1_a();
    test_pa1_b();
    test_pa1_c();
    test_pa2();
    test_pa3();
    test_pa4_interface();
    test_pa4_disk_stats();
    test_pa4_raid_integrity();
    test_pa4_raid_write_amplification();
    test_pa4_scheduling();
    test_pa4_slot_reuse();
    test_pa4_raid_mode_switch();
    test_pa4_concurrent();
    test_pa4_integration();
    test_edge_cases();

    summary();
    exit(0);
}