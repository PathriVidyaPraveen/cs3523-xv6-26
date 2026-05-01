/*
 * pa_stress.c — Comprehensive stress test for PA1 + PA2 + PA3
 *
 * Add to Makefile UPROGS:
 *   $U/_pa_stress\
 *
 * Run from xv6 shell:
 *   $ pa_stress
 */

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* =================================================================
   Test framework
   ================================================================= */

static int g_pass = 0;
static int g_fail = 0;

static void
check(const char *name, int cond)
{
    if(cond){
        printf("  [PASS] %s\n", name);
        g_pass++;
    } else {
        printf("  [FAIL] %s\n", name);
        g_fail++;
    }
}

static void
section(const char *title)
{
    printf("\n========== %s ==========\n", title);
}

static void
print_summary(void)
{
    printf("\n");
    printf("==========================================\n");
    printf("  PASSED : %d\n", g_pass);
    printf("  FAILED : %d\n", g_fail);
    printf("  TOTAL  : %d\n", g_pass + g_fail);
    printf("==========================================\n");
    if(g_fail == 0)
        printf("  ALL TESTS PASSED\n");
    else
        printf("  SOME TESTS FAILED — see [FAIL] lines above\n");
    printf("==========================================\n");
}

/* Consume CPU without making syscalls */
static void
cpu_burn(int iters)
{
    volatile int x = 0;
    for(int i = 0; i < iters; i++) x ^= i * 3;
    (void)x;
}

/* =================================================================
   PA1 — System call tests
   ================================================================= */

static void
test_hello(void)
{
    section("PA1-A1: hello()");
    printf("  (kernel should print 'Hello from the kernel!' below)\n");
    int r = hello();
    check("hello() returns 0", r == 0);
}

static void
test_getpid2(void)
{
    section("PA1-A2: getpid2()");

    int a = getpid();
    int b = getpid2();
    check("getpid2() == getpid() in parent", a == b);
    check("PID is positive",                 b > 0);

    /* child must also report its own pid correctly */
    int pid = fork();
    if(pid == 0){
        int cp  = getpid();
        int cp2 = getpid2();
        exit(cp == cp2 ? 0 : 1);
    }
    int st;
    wait(&st);
    check("getpid2() == getpid() in child too", st == 0);
}

static void
test_getppid(void)
{
    section("PA1-B1: getppid()");

    int ppid = getppid();
    check("parent's getppid() > 0", ppid > 0);

    int parent_pid = getpid();
    int pid = fork();
    if(pid == 0){
        int cp = getppid();
        exit(cp == parent_pid ? 0 : 1);
    }
    int st;
    wait(&st);
    check("child's getppid() == parent's pid", st == 0);

    /* Two levels deep: grandchild's getppid must equal child's pid */
    pid = fork();
    if(pid == 0){
        /* child */
        int child_pid = getpid();
        int gc_pid = fork();
        if(gc_pid == 0){
            /* grandchild */
            exit(getppid() == child_pid ? 0 : 1);
        }
        int gs;
        wait(&gs);
        exit(gs);
    }
    wait(&st);
    check("grandchild's getppid() == child's pid", st == 0);
}

static void
test_getnumchild(void)
{
    section("PA1-B2: getnumchild()");

    int base = getnumchild();
    check("getnumchild() >= 0 at start", base >= 0);

    /* fork 3 sleeping children */
    int pids[3];
    for(int i = 0; i < 3; i++){
        pids[i] = fork();
        if(pids[i] == 0){ pause(60); exit(0); }
    }
    pause(4);
    int with3 = getnumchild();
    check("count increases by 3 after 3 forks", with3 == base + 3);

    /* kill and reap all three */
    for(int i = 0; i < 3; i++) kill(pids[i]);
    for(int i = 0; i < 3; i++) wait(0);
    int after_reap = getnumchild();
    check("count back to baseline after wait()", after_reap == base);

    /* zombie must NOT be counted */
    int zpid = fork();
    if(zpid == 0) exit(0);   /* becomes zombie immediately */
    pause(4);                 /* let child exit and become zombie */
    int with_zombie = getnumchild();
    check("zombie child not counted", with_zombie == base);
    wait(0);                  /* reap zombie */

    /* one sleeping child */
    int cpid = fork();
    if(cpid == 0){ pause(40); exit(0); }
    pause(3);
    int with_one = getnumchild();
    check("exactly 1 alive child counted", with_one == base + 1);
    kill(cpid);
    wait(0);

    /* children of children are NOT counted */
    int outer = fork();
    if(outer == 0){
        /* this child forks a grandchild */
        int gc = fork();
        if(gc == 0){ pause(40); exit(0); }
        pause(40);
        kill(gc);
        wait(0);
        exit(0);
    }
    pause(3);
    int with_grandchild = getnumchild();
    check("grandchild not counted by grandparent's getnumchild()",
          with_grandchild == base + 1);
    kill(outer);
    wait(0);
}

static void
test_getsyscount(void)
{
    section("PA1-C1/C2: getsyscount()");

    int c0 = getsyscount();
    check("getsyscount() > 0 (already made syscalls)", c0 > 0);

    /* make exactly 20 getpid calls */
    for(int i = 0; i < 20; i++) getpid();
    int c1 = getsyscount();

    check("count increased after 20 getpid calls", c1 > c0);
    /* delta = 20 getpid + 1 getsyscount = 21; allow tiny slack */
    int delta = c1 - c0;
    check("delta is approximately 21 (20 getpid + 1 getsyscount)",
          delta >= 20 && delta <= 25);

    /* strictly monotonic: each getsyscount() call must return more than previous */
    int mono_ok = 1;
    int prev = getsyscount();
    for(int i = 0; i < 10; i++){
        int cur = getsyscount();
        if(cur <= prev){ mono_ok = 0; break; }
        prev = cur;
    }
    check("getsyscount() strictly monotonically increasing", mono_ok);

    /* child starts its own independent counter from 0 */
    int pid = fork();
    if(pid == 0){
        int cc = getsyscount();
        /* child has made very few syscalls; counter should be small */
        exit(cc > 0 && cc < 50 ? 0 : 1);
    }
    int st;
    wait(&st);
    check("child starts with its own small syscount", st == 0);
}

static void
test_getchildsyscount(void)
{
    section("PA1-C3: getchildsyscount()");

    /* invalid pid */
    check("invalid pid returns -1",
          getchildsyscount(99999) == -1);

    /* self is not own child */
    check("self pid returns -1",
          getchildsyscount(getpid()) == -1);

    /* fork a child that makes 50 calls then sleeps */
    int pid = fork();
    if(pid == 0){
        for(int i = 0; i < 50; i++) getpid();
        pause(60);
        exit(0);
    }
    pause(5);

    int cnt = getchildsyscount(pid);
    check("child syscount > 0 while alive", cnt > 0);
    check("child syscount >= 50", cnt >= 50);

    /* count only increases as child keeps running */
    int cnt2 = getchildsyscount(pid);
    check("child syscount does not decrease", cnt2 >= cnt);

    kill(pid);
    wait(0);

    /* after reap, child is gone */
    check("after wait(), getchildsyscount returns -1",
          getchildsyscount(pid) == -1);

    /* sibling's pid is not a child of our process */
    /* (sibling pids are not easy to observe, tested implicitly via -1 return above) */

    /* parent A cannot read child of parent B */
    int other_child = fork();
    if(other_child == 0){
        /* this child forks its own grandchild */
        int gc = fork();
        if(gc == 0){ pause(40); exit(0); }
        /* tell grandpa the grandchild's pid via exit code (low byte) */
        pause(40);
        kill(gc);
        wait(0);
        exit(0);
    }
    pause(3);
    /* we (grandparent) should not be able to query child of other_child */
    /* (we can't easily get gc's pid here, so we just verify other_child is countable) */
    int oc_cnt = getchildsyscount(other_child);
    check("can read syscount of direct child", oc_cnt >= 0);
    kill(other_child);
    wait(0);
}

/* =================================================================
   PA2 — SC-MLFQ scheduler tests
   ================================================================= */

static void
test_getlevel(void)
{
    section("PA2: getlevel()");

    int lv = getlevel();
    check("initial level == 0", lv == 0);
    check("level in [0,3]", lv >= 0 && lv <= 3);

    /* child also starts at level 0 */
    int pid = fork();
    if(pid == 0) exit(getlevel());
    int st;
    wait(&st);
    check("newly forked child starts at level 0", st == 0);

    /* level query is stable when process is idle */
    int lv2 = getlevel();
    check("level stays 0 for mostly-idle process", lv2 == 0);
}

static void
test_getmlfqinfo(void)
{
    section("PA2: getmlfqinfo()");

    struct mlfqinfo info;
    int pid = getpid();

    int r = getmlfqinfo(pid, &info);
    check("returns 0 for own pid",                    r == 0);
    check("level in [0,3]",     info.level >= 0 && info.level <= 3);
    check("times_scheduled > 0",                      info.times_scheduled > 0);
    check("total_syscalls > 0",                       info.total_syscalls > 0);

    int sum = 0;
    for(int i = 0; i < 4; i++){
        check("ticks[i] >= 0", info.ticks[i] >= 0);
        sum += info.ticks[i];
    }
    check("sum of ticks > 0", sum > 0);

    /* invalid pid */
    r = getmlfqinfo(99999, &info);
    check("returns -1 for invalid pid", r == -1);

    /* total_syscalls in getmlfqinfo must match getsyscount closely */
    for(int i = 0; i < 10; i++) getpid();
    int sc  = getsyscount();
    getmlfqinfo(pid, &info);
    int diff = info.total_syscalls - sc;
    if(diff < 0) diff = -diff;
    check("getmlfqinfo.total_syscalls ~= getsyscount() (diff <= 5)", diff <= 5);
}

static void
test_cpu_demotion(void)
{
    section("PA2: CPU-bound process demotion");

    int pid = fork();
    if(pid == 0){
        /* pure CPU burn — no syscalls */
        for(;;) cpu_burn(500000);
        exit(0);
    }

    pause(60);   /* give child time to run and get demoted */

    struct mlfqinfo info;
    getmlfqinfo(pid, &info);

    printf("  CPU-bound: level=%d sched=%d ticks=[%d,%d,%d,%d] sys=%d\n",
           info.level, info.times_scheduled,
           info.ticks[0], info.ticks[1], info.ticks[2], info.ticks[3],
           info.total_syscalls);

    check("CPU-bound demoted to level > 0",           info.level > 0);
    check("CPU-bound scheduled many times",            info.times_scheduled > 2);
    check("CPU-bound has ticks at levels > 0",
          (info.ticks[1] + info.ticks[2] + info.ticks[3]) > 0);

    kill(pid);
    wait(0);
}

static void
test_interactive_stays_high(void)
{
    section("PA2: Syscall-heavy (interactive) process stays at high priority");

    int pid = fork();
    if(pid == 0){
        /* syscall storm — forces deltaS >= deltaT every slice */
        for(;;)
            for(int i = 0; i < 5000; i++) getpid();
        exit(0);
    }

    pause(60);

    struct mlfqinfo info;
    getmlfqinfo(pid, &info);

    printf("  Interactive: level=%d sched=%d ticks=[%d,%d,%d,%d] sys=%d\n",
           info.level, info.times_scheduled,
           info.ticks[0], info.ticks[1], info.ticks[2], info.ticks[3],
           info.total_syscalls);

    check("Interactive stays at level 0 or 1",   info.level <= 1);
    check("Interactive made many syscalls",       info.total_syscalls > 1000);
    check("Interactive has ticks at level 0",     info.ticks[0] > 0);
    check("Interactive NOT at level 3",           info.level < 3);

    kill(pid);
    wait(0);
}

static void
test_mlfq_comparison(void)
{
    section("PA2: CPU-bound vs Interactive priority comparison");

    int cpu_pid = fork();
    if(cpu_pid == 0){
        for(;;) cpu_burn(500000);
        exit(0);
    }

    int ia_pid = fork();
    if(ia_pid == 0){
        for(;;) for(int i = 0; i < 5000; i++) getpid();
        exit(0);
    }

    pause(80);

    struct mlfqinfo ci, ii;
    getmlfqinfo(cpu_pid, &ci);
    getmlfqinfo(ia_pid,  &ii);

    printf("  CPU-bound level=%d  |  Interactive level=%d\n",
           ci.level, ii.level);

    check("CPU-bound at same or lower priority (higher level number)",
          ci.level >= ii.level);

    kill(cpu_pid); kill(ia_pid);
    wait(0); wait(0);
}

static void
test_global_boost(void)
{
    section("PA2: Global priority boost every 128 global ticks");

    /*
     * Strategy: spawn a CPU-burn child, let it get demoted, record ticks[0],
     * wait > 128 global_ticks (≈ 43 pause-ticks for 3 CPUs),
     * verify ticks[0] increased (child was moved back to level 0 by boost
     * and accumulated new ticks there before being demoted again).
     */
    int pid = fork();
    if(pid == 0){
        for(;;) cpu_burn(500000);
        exit(0);
    }

    /* let it get demoted */
    pause(40);

    struct mlfqinfo before;
    getmlfqinfo(pid, &before);
    printf("  Before boost window: level=%d ticks0=%d\n",
           before.level, before.ticks[0]);

    /* wait for at least one full boost cycle */
    /* 128 global_ticks / 3 CPUs ≈ 43 pause-ticks; use 70 for safety */
    pause(70);

    struct mlfqinfo after;
    getmlfqinfo(pid, &after);
    printf("  After  boost window: level=%d ticks0=%d\n",
           after.level, after.ticks[0]);

    /* If boost fired, process was moved to level 0 and accumulated ticks there */
    check("ticks[0] increased after boost window (process was boosted)",
          after.ticks[0] > before.ticks[0]);

    kill(pid);
    wait(0);
}

static void
test_mixed_workload(void)
{
    section("PA2: Mixed workload (syscalls + CPU bursts) stays low level");

    int pid = fork();
    if(pid == 0){
        /* Interleave syscall bursts with short CPU bursts.
           Each slice: many syscalls first → deltaS >= deltaT → not demoted. */
        for(;;){
            for(int i = 0; i < 500; i++) getpid();
            cpu_burn(10000);
        }
        exit(0);
    }

    pause(80);

    struct mlfqinfo info;
    getmlfqinfo(pid, &info);
    printf("  Mixed: level=%d sys=%d\n", info.level, info.total_syscalls);

    check("Mixed workload does not reach level 3", info.level < 3);
    check("Mixed workload made many syscalls",     info.total_syscalls > 500);

    kill(pid);
    wait(0);
}

static void
test_quantum_lengths(void)
{
    section("PA2: Quantum lengths — lower levels get more ticks per slice");

    /*
     * A CPU-bound process is expected to accumulate ticks roughly in
     * proportion to the quantum at each level (2:4:8:16).
     * After full demotion to level 3, ticks_total[3] >> ticks_total[0].
     */
    int pid = fork();
    if(pid == 0){
        for(;;) cpu_burn(500000);
        exit(0);
    }

    pause(100);

    struct mlfqinfo info;
    getmlfqinfo(pid, &info);

    printf("  Ticks per level: [%d,%d,%d,%d]\n",
           info.ticks[0], info.ticks[1], info.ticks[2], info.ticks[3]);

    /* Level 0 quantum=2, level 3 quantum=16.
       After demotion, more ticks should pile up at deeper levels. */
    check("More ticks at level 3 than level 0 for CPU-bound process",
          info.ticks[3] >= info.ticks[0]);

    kill(pid);
    wait(0);
}

/* =================================================================
   PA3 — Page replacement tests
   ================================================================= */

// static void print_vm(const char *label, int pid)
// {
//     struct vmstats s;
//     if(getvmstats(pid, &s) < 0){
//         printf("  [%s] getvmstats FAILED\n", label);
//         return;
//     }
//     printf("  [%s] PF=%d Evict=%d SwapIn=%d SwapOut=%d Resident=%d\n",
//            label, s.page_faults, s.pages_evicted,
//            s.pages_swapped_in, s.pages_swapped_out,
//            s.resident_pages);
// }

static void
test_getvmstats_basic(void)
{
    section("PA3: getvmstats() validation");

    struct vmstats s;
    int r = getvmstats(getpid(), &s);
    check("getvmstats(self) returns 0",   r == 0);
    check("page_faults >= 0",             s.page_faults >= 0);
    check("resident_pages >= 0",          s.resident_pages >= 0);
    check("pages_evicted >= 0",           s.pages_evicted >= 0);
    check("pages_swapped_in >= 0",        s.pages_swapped_in >= 0);
    check("pages_swapped_out >= 0",       s.pages_swapped_out >= 0);

    r = getvmstats(99999, &s);
    check("getvmstats(invalid pid) returns -1", r == -1);

    /* parent can read a live child's stats */
    int pid = fork();
    if(pid == 0){
        char *arr = sbrk(5 * 4096);
        for(int i = 0; i < 5; i++) arr[i * 4096] = 1;
        pause(20);
        exit(0);
    }
    pause(4);
    r = getvmstats(pid, &s);
    check("parent can read child's vmstats while child is alive", r == 0);
    kill(pid);
    wait(0);

    /* after child exits and is reaped, stats unavailable */
    r = getvmstats(pid, &s);
    check("getvmstats returns -1 after child is reaped", r == -1);
}

static void
test_lazy_alloc_page_faults(void)
{
    section("PA3: Lazy allocation — page_faults and resident_pages");

    int pid = fork();
    if(pid == 0){
        struct vmstats before, after;
        getvmstats(getpid(), &before);

        int pages = 40;
        char *arr = sbrk(pages * 4096);

        /* Touch each page exactly once */
        for(int i = 0; i < pages; i++)
            arr[i * 4096] = (char)(i & 0xFF);

        getvmstats(getpid(), &after);

        int df = after.page_faults   - before.page_faults;
        int dr = after.resident_pages - before.resident_pages;

        printf("  faults_delta=%d resident_delta=%d (expected ~%d each)\n",
               df, dr, pages);

        /* Allow a small tolerance for overheads */
        int ok = (df >= pages - 2 && df <= pages + 5 &&
                  dr >= pages - 2);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("page_faults ~= pages touched, resident_pages increased", st == 0);
}

static void
test_resident_pages_tracking(void)
{
    section("PA3: resident_pages increases on alloc, decreases on sbrk(-n)");

    int pid = fork();
    if(pid == 0){
        struct vmstats s0, s1, s2;
        getvmstats(getpid(), &s0);

        char *a = sbrk(30 * 4096);
        for(int i = 0; i < 30; i++) a[i * 4096] = 1;
        getvmstats(getpid(), &s1);

        sbrk(-30 * 4096);
        getvmstats(getpid(), &s2);

        printf("  resident: base=%d after_alloc=%d after_free=%d\n",
               s0.resident_pages, s1.resident_pages, s2.resident_pages);

        int grew = (s1.resident_pages > s0.resident_pages);
        int shrank = (s2.resident_pages < s1.resident_pages);
        exit((grew && shrank) ? 0 : 1);
    }
    int st;
    wait(&st);
    check("resident_pages grows on touch and shrinks on sbrk(-n)", st == 0);
}

static void
test_eviction_triggered(void)
{
    section("PA3: Eviction triggered under memory pressure");

    int pid = fork();
    if(pid == 0){
        /* 900 pages should exceed available physical frames and force evictions */
        int pages = 1350;
        char *arr = sbrk(pages * 4096);

        for(int i = 0; i < pages; i++)
            arr[i * 4096] = (char)(i & 0xFF);

        struct vmstats s;
        getvmstats(getpid(), &s);
        printf("  After touching %d pages: PF=%d Evict=%d SwapOut=%d Resident=%d\n",
               pages, s.page_faults, s.pages_evicted,
               s.pages_swapped_out, s.resident_pages);

        int ok = (s.pages_evicted > 0 && s.pages_swapped_out > 0);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("pages_evicted > 0 and pages_swapped_out > 0 under pressure", st == 0);
}

static void
test_swap_data_integrity(void)
{
    section("PA3: Data integrity across eviction and swap-in");

    int pid = fork();
    if(pid == 0){
        int pages = 700;
        char *arr = sbrk(pages * 4096);

        /* Write a unique byte to first location of each page */
        for(int i = 0; i < pages; i++)
            arr[i * 4096] = (char)(i & 0xFF);

        /* Force evictions by allocating another large region */
        char *arr2 = sbrk(pages * 4096);
        for(int i = 0; i < pages; i++)
            arr2[i * 4096] = (char)((~i) & 0xFF);

        /* Re-read first region — swap-ins should restore correct data */
        int errors = 0;
        for(int i = 0; i < pages; i++){
            char expected = (char)(i & 0xFF);
            if(arr[i * 4096] != expected)
                errors++;
        }

        struct vmstats s;
        getvmstats(getpid(), &s);
        printf("  SwapIn=%d errors=%d/%d\n", s.pages_swapped_in, errors, pages);

        int ok = (errors == 0 && s.pages_swapped_in > 0);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("Data integrity preserved after swap-out/swap-in (0 byte errors)", st == 0);
}

static void
test_swap_in_counted(void)
{
    section("PA3: pages_swapped_in counted correctly");

    int pid = fork();
    if(pid == 0){
        int pages = 1000;
        char *arr = sbrk(pages * 4096);

        /* Pass 1: touch all pages (many will be evicted) */
        for(int i = 0; i < pages; i++)
            arr[i * 4096] = (char)(i & 0xFF);

        struct vmstats s1;
        getvmstats(getpid(), &s1);

        /* Pass 2: force more pressure */
        char *arr2 = sbrk(1500 * 4096);
        for(int i = 0; i < pages; i++)
            arr2[i * 4096] = 0;

        /* Pass 3: re-access first region (forces swap-ins) */
        for(int i = 0; i < pages; i++)
            arr[i * 4096] += 1;

        struct vmstats s2;
        getvmstats(getpid(), &s2);

        printf("  SwapIn: before_pass3=%d after_pass3=%d\n",
               s1.pages_swapped_in, s2.pages_swapped_in);

        int ok = (s2.pages_swapped_in > s1.pages_swapped_in);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("pages_swapped_in increases after re-accessing evicted pages", st == 0);
}

static void
test_multi_byte_integrity(void)
{
    section("PA3: Multi-byte data integrity after swap (16 bytes per page)");

    int pid = fork();
    if(pid == 0){
        int pages = 300;
        char *arr = sbrk(pages * 4096);

        /* Write 16-byte pattern to each page */
        for(int i = 0; i < pages; i++)
            for(int j = 0; j < 16; j++)
                arr[i * 4096 + j] = (char)((i + j) & 0xFF);

        /* Force evictions */
        char *arr2 = sbrk(pages * 4096);
        for(int i = 0; i < pages; i++)
            arr2[i * 4096] = 0xFF;

        /* Verify full 16-byte pattern */
        int errors = 0;
        for(int i = 0; i < pages; i++)
            for(int j = 0; j < 16; j++){
                char expected = (char)((i + j) & 0xFF);
                if(arr[i * 4096 + j] != expected)
                    errors++;
            }

        printf("  errors=%d / %d byte-checks\n", errors, pages * 16);
        exit(errors == 0 ? 0 : 1);
    }
    int st;
    wait(&st);
    check("16-byte pattern per page intact after swap cycle (0 errors)", st == 0);
}

static void
test_fork_vm_independence(void)
{
    section("PA3: Parent and child VM stats are independent");

    char *parent_arr = sbrk(30 * 4096);
    for(int i = 0; i < 30; i++) parent_arr[i * 4096] = 1;

    struct vmstats p_before;
    getvmstats(getpid(), &p_before);

    int pid = fork();
    if(pid == 0){
        struct vmstats cs0;
        getvmstats(getpid(), &cs0);

        char *arr = sbrk(30 * 4096);
        for(int i = 0; i < 30; i++) arr[i * 4096] = 2;

        struct vmstats cs1;
        getvmstats(getpid(), &cs1);

        printf("  Child: faults before=%d after=%d\n",
               cs0.page_faults, cs1.page_faults);

        int ok = (cs1.page_faults > cs0.page_faults);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("Child's page_faults tracked independently", st == 0);

    struct vmstats p_after;
    getvmstats(getpid(), &p_after);
    check("Parent's page_faults unchanged after child allocated memory",
          p_after.page_faults == p_before.page_faults);

    sbrk(-30 * 4096);
}

static void
test_thrash(void)
{
    section("PA3: Thrashing — repeated eviction and swap-in");

    int pid = fork();
    if(pid == 0){
        int pages = 1350;
        char *arr = sbrk(pages * 4096);

        /* 3 full passes: each pass evicts and then re-swaps the prior pass */
        for(int pass = 0; pass < 3; pass++)
            for(int i = 0; i < pages; i++)
                arr[i * 4096] = (char)((pass * 7 + i) & 0xFF);

        struct vmstats s;
        getvmstats(getpid(), &s);
        printf("  Thrash: PF=%d Evict=%d SwapIn=%d SwapOut=%d\n",
               s.page_faults, s.pages_evicted,
               s.pages_swapped_in, s.pages_swapped_out);

        int ok = (s.pages_evicted > 0 && s.pages_swapped_in > 0 &&
                  s.pages_swapped_out > 0);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("Thrash workload: evicted > 0 AND swapped_in > 0 AND swapped_out > 0",
          st == 0);
}

static void
test_multiproc_memory(void)
{
    section("PA3: 4 concurrent processes each allocating under pressure");

    int pids[4];
    for(int i = 0; i < 4; i++){
        pids[i] = fork();
        if(pids[i] == 0){
            int pages = 300;
            char *arr = sbrk(pages * 4096);
            for(int j = 0; j < pages; j++) arr[j * 4096] = (char)j;
            /* Re-touch to force swap-ins of evicted pages */
            for(int j = 0; j < pages; j++) arr[j * 4096] += 1;

            struct vmstats s;
            getvmstats(getpid(), &s);
            printf("  Child PID=%d: PF=%d Evict=%d Resident=%d\n",
                   getpid(), s.page_faults, s.pages_evicted, s.resident_pages);
            exit(0);
        }
    }

    int all_ok = 1;
    for(int i = 0; i < 4; i++){
        int st;
        wait(&st);
        if(st != 0) all_ok = 0;
    }
    check("4 concurrent memory-heavy processes completed without panic", all_ok);
}

static void
test_boundary_access(void)
{
    section("PA3: Boundary byte access in a page");

    int pid = fork();
    if(pid == 0){
        char *arr = sbrk(4096);
        arr[0]    = (char)0xAA;
        arr[4095] = (char)0xBB;
        /* Touch more pages to potentially evict this one */
        char *arr2 = sbrk(200 * 4096);
        for(int i = 0; i < 200; i++) arr2[i * 4096] = 1;
        /* Re-check boundary bytes after possible eviction */
        int ok = (arr[0] == (char)0xAA && arr[4095] == (char)0xBB);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("Boundary bytes (first and last in page) correct after eviction pressure",
          st == 0);
}

static void
test_sbrk_shrink_reuse(void)
{
    section("PA3: sbrk shrink and re-allocate");

    int pid = fork();
    if(pid == 0){
        char *a = sbrk(20 * 4096);
        for(int i = 0; i < 20; i++) a[i * 4096] = (char)i;

        sbrk(-20 * 4096);

        /* Re-allocate same amount; pages should be fresh (no stale data) */
        char *b = sbrk(20 * 4096);
        int ok = 1;
        for(int i = 0; i < 20; i++){
            b[i * 4096] = (char)(i + 1);
            if(b[i * 4096] != (char)(i + 1)) ok = 0;
        }
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("Memory re-allocated after sbrk(-n) reads/writes correctly", st == 0);
}

static void
test_swap_slot_reuse(void)
{
    section("PA3: Swap slots are reused after pages are freed");

    /*
     * Allocate and evict many pages in waves.
     * If swap slots were not freed after swap-in, we'd run out of MAX_SWAP_PAGES.
     * Completing without a panic proves slots are being recycled.
     */
    int pid = fork();
    if(pid == 0){
        for(int wave = 0; wave < 5; wave++){
            int pages = 300;
            char *arr = sbrk(pages * 4096);
            for(int i = 0; i < pages; i++) arr[i * 4096] = (char)(wave + i);
            /* Re-access to swap them back in (frees the swap slots) */
            for(int i = 0; i < pages; i++) arr[i * 4096] += 1;
            sbrk(-pages * 4096);
        }
        exit(0);
    }
    int st;
    wait(&st);
    check("5 waves of 300-page alloc/evict/swapin/free without swap exhaustion",
          st == 0);
}

static void
test_scheduler_aware_eviction(void)
{
    section("PA3: Scheduler-aware eviction — low-priority loses pages first");

    /*
     * Low-priority process: burn CPU to get demoted first, then allocate.
     * High-priority process: stay interactive while allocating.
     * Under memory pressure the Clock eviction should prefer the low-priority
     * process's pages (higher p->level value).
     */
    int low_pid = fork();
    if(low_pid == 0){
        /* get demoted */
        for(int r = 0; r < 8; r++) cpu_burn(1000000);
        char *arr = sbrk(300 * 4096);
        for(int i = 0; i < 300; i++) arr[i * 4096] = 1;
        pause(100);
        struct vmstats s;
        getvmstats(getpid(), &s);
        printf("  Low-prio  level=%d evicted=%d swapped_out=%d\n",
               getlevel(), s.pages_evicted, s.pages_swapped_out);
        exit(0);
    }

    pause(20);  /* let low-priority get demoted before high-priority starts */

    int high_pid = fork();
    if(high_pid == 0){
        /* stay interactive */
        char *arr = sbrk(300 * 4096);
        for(int i = 0; i < 300; i++){
            arr[i * 4096] = 1;
            getpid();   /* keep making syscalls to preserve priority */
        }
        pause(100);
        struct vmstats s;
        getvmstats(getpid(), &s);
        printf("  High-prio level=%d evicted=%d swapped_out=%d\n",
               getlevel(), s.pages_evicted, s.pages_swapped_out);
        exit(0);
    }

    wait(0);
    wait(0);
    check("Scheduler-aware eviction test completed without panic", 1);
    printf("  (manual check: low-prio should have more evictions than high-prio)\n");
}

static void
test_invalid_access_child_killed(void)
{
    section("PA3: Invalid virtual address kills process (not kernel panic)");

    int pid = fork();
    if(pid == 0){
        /* Access far beyond p->sz — should kill child, not kernel */
        char *bad = (char*)0x3FFFFFFFFFFF;
        *bad = 1;
        exit(0);  /* should not reach here */
    }
    int st;
    wait(&st);
    /*
     * The child should have been killed (non-zero exit or killed flag).
     * xv6 wait() returns the exit status. A killed process exits with -1
     * which becomes 255 in a uint8 exit code field.
     * Just verify the kernel is still alive (we got here).
     */
    check("Kernel survived invalid address access in child", 1);
    check("Child was killed (exit status != 0)", st != 0);
}

/* =================================================================
   Integration tests
   ================================================================= */

static void
test_syscount_stable_during_vm_stress(void)
{
    section("Integration: getsyscount() correct during VM stress");

    int pid = fork();
    if(pid == 0){
        int c0 = getsyscount();
        char *arr = sbrk(200 * 4096);
        for(int i = 0; i < 200; i++) arr[i * 4096] = (char)i;
        for(int i = 0; i < 50; i++) getpid();
        int c1 = getsyscount();
        exit((c1 > c0) ? 0 : 1);
    }
    int st;
    wait(&st);
    check("getsyscount() correct and increasing during heavy memory usage", st == 0);
}

static void
test_getmlfqinfo_after_vm_stress(void)
{
    section("Integration: getmlfqinfo() consistent after VM stress");

    int pid = fork();
    if(pid == 0){
        /* Do heavy VM work then query own mlfq info */
        char *arr = sbrk(400 * 4096);
        for(int i = 0; i < 400; i++) arr[i * 4096] = 1;

        struct mlfqinfo info;
        int r = getmlfqinfo(getpid(), &info);
        int sc = getsyscount();

        int diff = info.total_syscalls - sc;
        if(diff < 0) diff = -diff;

        printf("  mlfqinfo.total_syscalls=%d getsyscount=%d diff=%d\n",
               info.total_syscalls, sc, diff);

        int ok = (r == 0 && diff <= 10 &&
                  info.level >= 0 && info.level <= 3 &&
                  info.times_scheduled > 0);
        exit(ok ? 0 : 1);
    }
    int st;
    wait(&st);
    check("getmlfqinfo() consistent with getsyscount() after VM stress", st == 0);
}

static void
test_parent_reads_child_vm_and_sched_stats(void)
{
    section("Integration: Parent reads both vmstats and mlfqinfo of child");

    int pid = fork();
    if(pid == 0){
        /* child: allocate, spin, sleep */
        char *arr = sbrk(100 * 4096);
        for(int i = 0; i < 100; i++) arr[i * 4096] = 1;
        for(int r = 0; r < 3; r++) cpu_burn(500000);
        pause(80);
        exit(0);
    }

    pause(20);

    struct vmstats vs;
    struct mlfqinfo mi;
    int rv = getvmstats(pid, &vs);
    int rm = getmlfqinfo(pid, &mi);

    printf("  Child vmstats: PF=%d Evict=%d Resident=%d\n",
           vs.page_faults, vs.pages_evicted, vs.resident_pages);
    printf("  Child mlfqinfo: level=%d sched=%d sys=%d\n",
           mi.level, mi.times_scheduled, mi.total_syscalls);

    check("getvmstats()  of child succeeds",            rv == 0);
    check("getmlfqinfo() of child succeeds",            rm == 0);
    check("Child page_faults > 0",                      vs.page_faults > 0);
    check("Child times_scheduled > 0",                  mi.times_scheduled > 0);
    check("Child total_syscalls > 0",                   mi.total_syscalls > 0);

    kill(pid);
    wait(0);
}

/* =================================================================
   Main
   ================================================================= */

int
main(void)
{
    printf("\n");
    printf("##################################################\n");
    printf("#  PA1 + PA2 + PA3 COMPREHENSIVE STRESS TEST     #\n");
    printf("##################################################\n");

    /* ---- PA1 ---- */
    test_hello();
    test_getpid2();
    test_getppid();
    test_getnumchild();
    test_getsyscount();
    test_getchildsyscount();

    /* ---- PA2 ---- */
    test_getlevel();
    test_getmlfqinfo();
    test_cpu_demotion();
    test_interactive_stays_high();
    test_mlfq_comparison();
    test_global_boost();
    test_mixed_workload();
    test_quantum_lengths();

    /* ---- PA3 ---- */
    test_getvmstats_basic();
    test_lazy_alloc_page_faults();
    test_resident_pages_tracking();
    test_eviction_triggered();
    test_swap_data_integrity();
    test_swap_in_counted();
    test_multi_byte_integrity();
    test_fork_vm_independence();
    test_thrash();
    test_multiproc_memory();
    test_boundary_access();
    test_sbrk_shrink_reuse();
    test_swap_slot_reuse();
    test_scheduler_aware_eviction();
    test_invalid_access_child_killed();

    /* ---- Integration ---- */
    test_syscount_stable_during_vm_stress();
    test_getmlfqinfo_after_vm_stress();
    test_parent_reads_child_vm_and_sched_stats();

    print_summary();
    exit(0);
}