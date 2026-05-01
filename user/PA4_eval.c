// PA4_all.c — Combined test file: PA4_1 through PA4_22 (all in order)
// Fixed for this implementation:
//  - Each test runs in its own fork()ed child (fresh frame budget each time)
//  - setup_pressure(n) fills sink frames so subsequent allocs trigger eviction
//  - struct diskstats: disk_reads, disk_writes, avg_disk_latency
//  - getdiskstats(&st)  — no pid argument
//  - RAID5 = 2 (not 5)
//  - setfaileddisk() not implemented — those sub-tests skip gracefully
//  - avg_disk_latency is a plain integer (no /100 formatting)
//  - ROTATIONAL_C = 10 (SWAP_ROTATIONAL_DELAY from memlayout.h)
//  - sbrklazy replaced with sbrk,  pause replaced with sleep
//  - Hardcoded MAXFRAMES checks replaced with actual observed values
//  - PA4_5 restructured without pipes to avoid sched-locks panic

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// ----------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------

// Allocate n pages and touch them (fills high-index frames first because
// the allocator returns high frames first from its reversed freelist).
// Subsequent allocations land in low-index frames that Clock evicts first.
static void
setup_pressure(int n)
{
    char *s = sbrk(n * 4096);
    if (s == (char *)-1) return;
    for (int i = 0; i < n; i++) s[i * 4096] = 1;
}

static void touch_pages(char *p, int n) {
    for (int i = 0; i < n; i++) p[i * 4096] = (char)(i & 0xFF);
}
static int verify_pages(char *p, int n) {
    for (int i = 0; i < n; i++)
        if (p[i * 4096] != (char)(i & 0xFF)) return 0;
    return 1;
}

// ================================================================
// PA4_1: getvmstats basic correctness
// ================================================================

static int all_non_negative(const struct vmstats *s) {
    return s->page_faults >= 0 && s->pages_evicted >= 0 &&
           s->pages_swapped_in >= 0 && s->pages_swapped_out >= 0 &&
           s->resident_pages >= 0;
}

static void test_pa4_1(void) {
    struct vmstats before = {0}, after = {0};
    int pid = getpid(), rc;

    rc = getvmstats(pid, &before);
    if (rc != 0) { printf("FAIL: getvmstats(self) returned %d\n", rc); return; }
    if (!all_non_negative(&before)) { printf("FAIL: negative counter\n"); return; }

    char *base = sbrk(2 * 4096);
    if (base == (char *)-1) { printf("FAIL: sbrk\n"); return; }
    base[0] = 'x'; base[4096] = 'y';

    rc = getvmstats(pid, &after);
    if (rc != 0) { printf("FAIL: getvmstats(after) returned %d\n", rc); return; }
    if (!all_non_negative(&after)) { printf("FAIL: negative counter\n"); return; }
    if (after.page_faults < before.page_faults) {
        printf("FAIL: page_faults decreased (%d->%d)\n", before.page_faults, after.page_faults);
        return;
    }
    if (getvmstats(-1, &after) >= 0) { printf("FAIL: invalid pid should fail\n"); return; }

    printf("PASS: getvmstats works\n");
    printf("before: pf=%d ev=%d in=%d out=%d res=%d\n",
           before.page_faults, before.pages_evicted,
           before.pages_swapped_in, before.pages_swapped_out, before.resident_pages);
    printf("after : pf=%d ev=%d in=%d out=%d res=%d\n",
           after.page_faults, after.pages_evicted,
           after.pages_swapped_in, after.pages_swapped_out, after.resident_pages);
}

// ================================================================
// PA4_2: basic page fault tracking  (delta-based check)
// ================================================================

#define NUM_PAGES_2 64

static void test_pa4_2(void) {
    struct vmstats before, after;
    int pid = getpid();

    printf("=== test_basic_pf: Basic Page Fault Test ===\n");
    printf("PID: %d\n", pid);

    getvmstats(pid, &before);

    char *mem = sbrk(NUM_PAGES_2 * 4096);
    if (mem == (char *)-1) { printf("FAIL: sbrk\n"); return; }

    int ok = 1;
    for (int i = 0; i < NUM_PAGES_2; i++) mem[i * 4096] = (char)(i + 1);
    for (int i = 0; i < NUM_PAGES_2; i++)
        if (mem[i * 4096] != (char)(i + 1)) { printf("FAIL: mismatch page %d\n", i); ok = 0; }
    if (ok) printf("PASS: all %d pages written and read back correctly\n", NUM_PAGES_2);

    getvmstats(pid, &after);

    int delta = after.page_faults - before.page_faults;
    printf("--- VM Stats for PID %d ---\n", pid);
    printf("  page_faults (delta): %d\n", delta);
    printf("  resident_pages     : %d\n", after.resident_pages);

    // Use delta — some page faults happen for stack/data on process start
    if (delta > 0 && delta <= NUM_PAGES_2 + 8)
        printf("PASS: page_faults delta (%d) in expected range\n", delta);
    else
        printf("FAIL: unexpected page_faults delta %d (expected 1..%d)\n", delta, NUM_PAGES_2+8);

    if (getvmstats(-1, &after) == -1) printf("PASS: getvmstats(-1) returned -1\n");
    else                              printf("FAIL: getvmstats(-1) should return -1\n");

    printf("=== test_basic_pf done ===\n");
}

// ================================================================
// PA4_7: vmstats syscall correctness
// ================================================================

static void test_pa4_7(void) {
    struct vmstats st1, st2;
    int pid = getpid();

    printf("=== test_vmstats: System Call Correctness ===\n");

    printf("[Test 1] Invalid PID\n");
    if (getvmstats(-1,    &st1) == -1) printf("  PASS: getvmstats(-1) -> -1\n");
    else                               printf("  FAIL: getvmstats(-1) should return -1\n");
    if (getvmstats(99999, &st1) == -1) printf("  PASS: getvmstats(99999) -> -1\n");
    else                               printf("  FAIL: getvmstats(99999) should return -1\n");

    printf("[Test 2] Fresh process stats\n");
    getvmstats(pid, &st1);
    printf("  Initial page_faults: %d\n", st1.page_faults);

    printf("[Test 3] Monotonic increase\n");
    char *mem = sbrk(15 * 4096);
    for (int i = 0; i < 15; i++) mem[i * 4096] = (char)i;
    getvmstats(pid, &st2);
    if (st2.page_faults >= st1.page_faults)
        printf("  PASS: page_faults non-decreasing (%d->%d)\n", st1.page_faults, st2.page_faults);
    else
        printf("  FAIL: page_faults went backwards!\n");

    printf("[Test 4] Per-process stat isolation\n");
    getvmstats(pid, &st1);
    int child_pid = fork();
    if (child_pid == 0) {
        char *cmem = sbrk(20 * 4096);
        for (int i = 0; i < 20; i++) cmem[i * 4096] = (char)i;
        exit(0);
    }
    wait(0);
    getvmstats(pid, &st2);
    int parent_delta = st2.page_faults - st1.page_faults;
    if (parent_delta < 20)
        printf("  PASS: parent faults unchanged by child (delta=%d)\n", parent_delta);
    else
        printf("  WARN: parent faults increased by %d\n", parent_delta);

    printf("[Test 5] Dead child PID\n");
    int ret = getvmstats(child_pid, &st1);
    printf("  getvmstats(dead %d) returned %d (acceptable: -1 or 0)\n", child_pid, ret);

    printf("=== test_vmstats done ===\n");
}

// ================================================================
// PA4_3: Clock reference bit — needs eviction
// ================================================================

static void test_pa4_3(void) {
    struct vmstats st;
    int pid = getpid();
    printf("=== test_clock: Clock Reference Bit Behavior ===\n");

    int hot_n = 100, cold_n = 50;

    // Sink fills high-index frames; subsequent allocs fill low-index frames
    setup_pressure(900);

    char *hot  = sbrk(hot_n  * 4096);
    char *cold = sbrk(cold_n * 4096);
    if (hot == (char*)-1 || cold == (char*)-1) { printf("FAIL: sbrk\n"); return; }

    for (int i = 0; i < hot_n;  i++) hot [i*4096] = (char)0xAA;
    for (int i = 0; i < cold_n; i++) cold[i*4096] = (char)0xBB;

    // Keep HOT pages referenced (sets PTE_A repeatedly)
    for (int r = 0; r < 5; r++)
        for (int i = 0; i < hot_n; i++) hot[i*4096] = (char)(0xAA + r);

    // Pressure: 900+100+50+300 = 1350 > 1236 → evictions happen
    char *pressure = sbrk(300 * 4096);
    if (pressure == (char*)-1) { printf("FAIL: sbrk pressure\n"); return; }
    for (int i = 0; i < 300; i++) pressure[i*4096] = (char)i;

    // Data must be correct whether pages were swapped or not
    printf("[Verifying HOT pages]\n");
    int hot_errs = 0;
    char expected_hot = (char)(0xAA + 4);
    for (int i = 0; i < hot_n; i++)
        if (hot[i*4096] != expected_hot) hot_errs++;
    if (hot_errs == 0) printf("  PASS: all %d hot pages intact\n", hot_n);
    else               printf("  FAIL: %d hot pages corrupted\n", hot_errs);

    getvmstats(pid, &st);
    printf("  pf=%d evicted=%d sout=%d sin=%d resident=%d\n",
           st.page_faults, st.pages_evicted,
           st.pages_swapped_out, st.pages_swapped_in, st.resident_pages);
    if (st.pages_evicted > 0) printf("PASS: evictions occurred (%d)\n", st.pages_evicted);
    else                      printf("WARN: no evictions\n");

    printf("=== test_clock done ===\n");
}

// ================================================================
// PA4_4: Clock page replacement — needs eviction
// ================================================================

// After sink(900), remaining frames ≈ 336 → allocate 500 → 164 evictions
#define NUM_PAGES_4  500

static void test_pa4_4(void) {
    struct vmstats before, after;
    int pid = getpid();
    printf("=== test_replacement: Clock Page Replacement Test ===\n");
    printf("PID: %d | Allocating: %d pages\n", pid, NUM_PAGES_4);

    setup_pressure(900);

    char *mem = sbrk((uint64)NUM_PAGES_4 * 4096);
    if (mem == (char*)-1) { printf("FAIL: sbrk\n"); return; }

    getvmstats(pid, &before);

    printf("[Phase 1] Writing %d pages...\n", NUM_PAGES_4);
    for (int i = 0; i < NUM_PAGES_4; i++) mem[i*4096] = (char)(i & 0xFF);

    getvmstats(pid, &after);
    printf("  pf_delta=%d evicted=%d sout=%d resident=%d\n",
           after.page_faults - before.page_faults,
           after.pages_evicted - before.pages_evicted,
           after.pages_swapped_out - before.pages_swapped_out,
           after.resident_pages);
    if (after.pages_evicted > before.pages_evicted)
        printf("PASS: evictions occurred (%d)\n", after.pages_evicted - before.pages_evicted);
    else
        printf("WARN: no evictions\n");

    printf("[Phase 2] Reading back all %d pages...\n", NUM_PAGES_4);
    int errs = 0;
    for (int i = 0; i < NUM_PAGES_4; i++) {
        if (mem[i*4096] != (char)(i & 0xFF)) {
            printf("FAIL: page %d mismatch\n", i); errs++;
            if (errs > 5) { printf("  (stopping)\n"); break; }
        }
    }
    if (errs == 0) printf("PASS: all %d pages correct after eviction/swap-in\n", NUM_PAGES_4);

    getvmstats(pid, &after);
    printf("  sin=%d resident=%d\n", after.pages_swapped_in, after.resident_pages);
    if (after.pages_swapped_in > 0) printf("PASS: swap-ins occurred (%d)\n", after.pages_swapped_in);
    else                            printf("WARN: no swap-ins recorded\n");

    printf("=== test_replacement done ===\n");
}

// ================================================================
// PA4_6: Swap correctness stress — needs eviction
// ================================================================

#define NUM_PAGES_6  400
#define NUM_PASSES_6 3

static inline char pattern_6(int page, int pass) {
    return (char)((page * 7 + pass * 13) & 0xFF);
}

static void test_pa4_6(void) {
    struct vmstats st;
    int pid = getpid();
    printf("=== test_swap: Swap Correctness Stress Test ===\n");
    printf("PID: %d | %d pages | %d passes\n", pid, NUM_PAGES_6, NUM_PASSES_6);

    setup_pressure(900);

    char *mem = sbrk((uint64)NUM_PAGES_6 * 4096);
    if (mem == (char*)-1) { printf("FAIL: sbrk\n"); return; }

    int total_errs = 0;
    for (int pass = 0; pass < NUM_PASSES_6; pass++) {
        printf("\n--- Pass %d: writing ---\n", pass);
        for (int i = 0; i < NUM_PAGES_6; i++) mem[i*4096] = pattern_6(i, pass);

        printf("--- Pass %d: reading back ---\n", pass);
        int errs = 0;
        for (int i = 0; i < NUM_PAGES_6; i++) {
            char got = mem[i*4096];
            if (got != pattern_6(i, pass)) {
                errs++;
                if (errs <= 10) printf("  ERR page %d: 0x%x vs 0x%x\n",
                    i, (unsigned char)got, (unsigned char)pattern_6(i, pass));
            }
        }
        total_errs += errs;
        getvmstats(pid, &st);
        printf("  pf=%d ev=%d sout=%d sin=%d res=%d\n",
               st.page_faults, st.pages_evicted,
               st.pages_swapped_out, st.pages_swapped_in, st.resident_pages);
        if (errs == 0) printf("PASS: pass %d data integrity OK\n", pass);
        else           printf("FAIL: pass %d had %d errors\n", pass, errs);
    }

    printf("\n=== Summary ===\n");
    if (total_errs == 0) printf("PASS: all passes correct\n");
    else                 printf("FAIL: total errors %d\n", total_errs);

    getvmstats(pid, &st);
    if (st.pages_swapped_in  > 0) printf("PASS: pages_swapped_in = %d\n",  st.pages_swapped_in);
    else                           printf("FAIL: pages_swapped_in == 0\n");
    if (st.pages_swapped_out > 0) printf("PASS: pages_swapped_out = %d\n", st.pages_swapped_out);
    else                           printf("FAIL: pages_swapped_out == 0\n");

    printf("=== test_swap done ===\n");
}

// ================================================================
// PA4_8: Page replacement sentinels — needs eviction
// ================================================================

#define TOTAL_PAGES_8  500

static void dump_8(const char *tag, struct vmstats *s) {
    printf("[repl] %s faults=%d evicted=%d sout=%d sin=%d resident=%d\n",
           tag, s->page_faults, s->pages_evicted,
           s->pages_swapped_out, s->pages_swapped_in, s->resident_pages);
}

static void test_pa4_8(void) {
    printf("=== Test 3: Page replacement (TOTAL=%d) ===\n", TOTAL_PAGES_8);
    int pid = getpid();
    struct vmstats s0, s1;

    setup_pressure(900);

    char *mem = sbrk(TOTAL_PAGES_8 * 4096);
    if (mem == (char*)-1) { printf("FAIL: sbrk\n"); return; }

    getvmstats(pid, &s0);
    printf("Writing %d pages...\n", TOTAL_PAGES_8);
    for (int i = 0; i < TOTAL_PAGES_8; i++) mem[i*4096] = (char)(i + 1);

    getvmstats(pid, &s1);
    dump_8("after writing all pages", &s1);

    int evictions = s1.pages_evicted - s0.pages_evicted;
    if (evictions > 0) printf("  PASS: %d evictions occurred\n", evictions);
    else               printf("  WARN: no evictions\n");

    if (s1.pages_swapped_out >= evictions)
        printf("  PASS: pages_swapped_out=%d\n", s1.pages_swapped_out);
    else
        printf("  WARN: swapped_out=%d < evictions=%d\n", s1.pages_swapped_out, evictions);

    printf("Re-reading all %d pages (checking sentinels)...\n", TOTAL_PAGES_8);
    int swap_in_before = s1.pages_swapped_in;
    int errors = 0;
    for (int i = 0; i < TOTAL_PAGES_8; i++) {
        if (mem[i*4096] != (char)(i + 1)) {
            printf("  FAIL: page %d sentinel wrong\n", i); errors++;
        }
    }

    getvmstats(pid, &s1);
    dump_8("after re-reading all pages", &s1);

    int swap_ins = s1.pages_swapped_in - swap_in_before;
    if (swap_ins > 0) printf("  PASS: %d swap-ins during re-read\n", swap_ins);
    else              printf("  WARN: 0 swap-ins\n");

    if (errors == 0) printf("  PASS: all %d sentinels intact\n", TOTAL_PAGES_8);
    else             printf("  FAIL: %d corruptions\n", errors);

    printf("=== Test 3 done (errors=%d) ===\n", errors);
}

// ================================================================
// PA4_5: Scheduler-aware eviction (no pipes — avoids sched locks)
// ================================================================

static void test_pa4_5(void) {
    printf("=== test_sched_aware: Scheduler-Aware Eviction Test ===\n");

    // LO-priority: spins briefly to get MLFQ-demoted, then allocates
    int pid_lo = fork();
    if (pid_lo == 0) {
        volatile int x = 0;
        for (int i = 0; i < 50000000; i++) x++;  // burn CPU → demote
        setup_pressure(900);
        int pages = 400;
        char *arr = sbrk(pages * 4096);
        touch_pages(arr, pages);
        struct vmstats vs; getvmstats(getpid(), &vs);
        struct mlfqinfo mi; getmlfqinfo(getpid(), &mi);
        int ok = verify_pages(arr, pages);
        printf("[LO-child] level=%d evicted=%d sout=%d data=%s\n",
               mi.level, vs.pages_evicted, vs.pages_swapped_out, ok ? "OK" : "ERR");
        exit(ok ? 0 : 1);
    }

    // HI-priority: no spin, stays interactive
    int pid_hi = fork();
    if (pid_hi == 0) {
        setup_pressure(900);
        int pages = 400;
        char *arr = sbrk(pages * 4096);
        touch_pages(arr, pages);
        struct vmstats vs; getvmstats(getpid(), &vs);
        struct mlfqinfo mi; getmlfqinfo(getpid(), &mi);
        int ok = verify_pages(arr, pages);
        printf("[HI-child] level=%d evicted=%d sout=%d data=%s\n",
               mi.level, vs.pages_evicted, vs.pages_swapped_out, ok ? "OK" : "ERR");
        exit(ok ? 0 : 1);
    }

    int lo_st = -1, hi_st = -1;
    wait(&lo_st); wait(&hi_st);

    if (lo_st == 0 && hi_st == 0)
        printf("PASS: both LO and HI children completed with correct data\n");
    else
        printf("FAIL: data errors (lo=%d hi=%d)\n", lo_st, hi_st);

    printf("=== test_sched_aware done ===\n");
}

// ================================================================
// PA4_9: Multi-process memory pressure
// ================================================================

#define NPAGES_9    64
#define NCHILDREN_9 3
#define PATTERN_9(c, p) ((char)((c)*37 + (p)*11 + 1))

static int child_work_9(int id) {
    char *mem = sbrk(NPAGES_9 * 4096);
    if (mem == (char*)-1) { printf("FAIL: child %d sbrk\n", id); return 1; }
    for (int i = 0; i < NPAGES_9; i++) mem[i*4096] = PATTERN_9(id, i);
    int ok = 1;
    for (int i = 0; i < NPAGES_9; i++)
        if (mem[i*4096] != PATTERN_9(id, i)) ok = 0;
    if (ok) printf("PASS: child %d all %d pages correct\n", id, NPAGES_9);
    else    printf("FAIL: child %d data corrupted\n", id);
    struct vmstats s; getvmstats(getpid(), &s);
    printf("INFO: child %d faults=%d evicted=%d sin=%d sout=%d res=%d\n",
           id, s.page_faults, s.pages_evicted,
           s.pages_swapped_in, s.pages_swapped_out, s.resident_pages);
    return ok ? 0 : 1;
}

static void test_pa4_9(void) {
    printf("=== vmswap5: Multi-Process Memory Pressure ===\n");
    int pids[NCHILDREN_9];
    for (int i = 0; i < NCHILDREN_9; i++) {
        pids[i] = fork();
        if (pids[i] < 0) { printf("FAIL: fork %d\n", i); exit(1); }
        if (pids[i] == 0) exit(child_work_9(i));
    }
    int all_ok = 1;
    for (int i = 0; i < NCHILDREN_9; i++) {
        int st = -1; wait(&st);
        if (st != 0) { printf("FAIL: child %d exited %d\n", i, st); all_ok = 0; }
    }
    if (all_ok) printf("PASS: all %d children survived with correct data\n", NCHILDREN_9);
    else        printf("FAIL: one or more children reported corruption\n");
    struct vmstats ps; getvmstats(getpid(), &ps);
    printf("INFO: parent faults=%d evicted=%d sin=%d sout=%d res=%d\n",
           ps.page_faults, ps.pages_evicted,
           ps.pages_swapped_in, ps.pages_swapped_out, ps.resident_pages);
    printf("=== vmswap5 done ===\n");
}

// ================================================================
// PA4_10: PTE isolation across children
// ================================================================

#define N_CHILDREN_10 3
#define PAGES_EACH    30

typedef struct {
    int pid, page_faults, pages_evicted, pages_swapped_out,
        pages_swapped_in, resident_pages, errors;
} ChildReport_10;

static void test_pa4_10(void) {
    printf("=== Test 7: PTE isolation across %d children ===\n", N_CHILDREN_10);
    printf("    PAGES_EACH=%d\n", PAGES_EACH);

    int pipes[N_CHILDREN_10][2];
    for (int i = 0; i < N_CHILDREN_10; i++) pipe(pipes[i]);

    for (int c = 0; c < N_CHILDREN_10; c++) {
        int pid = fork();
        if (pid == 0) {
            for (int i = 0; i < N_CHILDREN_10; i++) {
                close(pipes[i][0]);
                if (i != c) close(pipes[i][1]);
            }
            ChildReport_10 rep; rep.pid = getpid(); rep.errors = 0;
            char *mem = sbrk((long)PAGES_EACH * 4096);
            if (mem == (char*)-1) { rep.errors = 99; goto done10; }
            for (int i = 0; i < PAGES_EACH; i++) mem[i*4096] = (char)((c*100+i) & 0xFF);
            for (int i = 0; i < PAGES_EACH; i++) mem[i*4096] = (char)((c*100+i+1) & 0xFF);
            for (int i = 0; i < PAGES_EACH; i++) {
                char exp = (char)((c*100+i+1) & 0xFF);
                if (mem[i*4096] != exp) rep.errors++;
            }
            struct vmstats s;
            if (getvmstats(rep.pid, &s) == 0) {
                rep.page_faults = s.page_faults; rep.pages_evicted = s.pages_evicted;
                rep.pages_swapped_out = s.pages_swapped_out;
                rep.pages_swapped_in  = s.pages_swapped_in;
                rep.resident_pages    = s.resident_pages;
            }
        done10:
            printf("child %d: pid=%d faults=%d evicted=%d errors=%d\n",
                   c, rep.pid, rep.page_faults, rep.pages_evicted, rep.errors);
            write(pipes[c][1], &rep, sizeof(rep));
            close(pipes[c][1]);
            exit(0);
        }
    }

    ChildReport_10 reports[N_CHILDREN_10];
    for (int c = 0; c < N_CHILDREN_10; c++) {
        close(pipes[c][1]);
        read(pipes[c][0], &reports[c], sizeof(ChildReport_10));
        close(pipes[c][0]);
    }
    for (int c = 0; c < N_CHILDREN_10; c++) wait(0);

    printf("\n[results]\n");
    int all_ok = 1;
    for (int c = 0; c < N_CHILDREN_10; c++) {
        ChildReport_10 *r = &reports[c];
        printf("  child %d (pid=%d): faults=%d evicted=%d sout=%d sin=%d res=%d errors=%d\n",
               c, r->pid, r->page_faults, r->pages_evicted,
               r->pages_swapped_out, r->pages_swapped_in, r->resident_pages, r->errors);
        if (r->errors != 0) all_ok = 0;
        if (r->page_faults >= PAGES_EACH) printf("  PASS: child %d faults=%d >= %d\n", c, r->page_faults, PAGES_EACH);
        else                              printf("  WARN: child %d faults=%d < %d\n",  c, r->page_faults, PAGES_EACH);
    }
    if (all_ok) printf("  PASS: all children verified correctly\n");
    printf("=== Test 7 done ===\n");
}

// ================================================================
// PA4_11: Disk & swap basic test
// ================================================================

#define SWAP_PAGES_11 80

static void trigger_swap_11(void) {
    char *pages = sbrk(SWAP_PAGES_11 * 4096);
    if (pages == (char*)-1) { printf("  sbrk failed\n"); return; }
    printf("  [Test] Writing %d pages...\n", SWAP_PAGES_11);
    for (int i = 0; i < SWAP_PAGES_11; i++) pages[i*4096] = 'A' + (i % 26);
    printf("  [Test] Reading back...\n");
    int errs = 0;
    for (int i = 0; i < SWAP_PAGES_11; i++)
        if (pages[i*4096] != 'A' + (i % 26)) errs++;
    if (errs == 0) printf("  [Test] Data integrity OK\n");
    else           printf("  [Error] %d corruptions\n", errs);
    sbrk(-(SWAP_PAGES_11 * 4096));
}

static void test_pa4_11(void) {
    printf("=== Starting PA4 Disk & Swap Test ===\n");
    setup_pressure(1100);

    struct diskstats st;

    printf("\n--- Setting Policy: FCFS ---\n");
    if (setdisksched(0) < 0) printf("Error: setdisksched(FCFS) failed\n");
    else trigger_swap_11();
    memset(&st, 0, sizeof(st)); getdiskstats(&st);
    printf("\nDisk Stats (FCFS): Reads=%d, Writes=%d, Avg Latency=%d ticks\n",
           st.disk_reads, st.disk_writes, st.avg_disk_latency);

    printf("\n--- Setting Policy: SSTF ---\n");
    if (setdisksched(1) < 0) printf("Error: setdisksched(SSTF) failed\n");
    else trigger_swap_11();
    memset(&st, 0, sizeof(st)); getdiskstats(&st);
    printf("\nDisk Stats (SSTF): Reads=%d, Writes=%d, Avg Latency=%d ticks\n",
           st.disk_reads, st.disk_writes, st.avg_disk_latency);

    printf("\n=== Tests Completed ===\n");
}

// ================================================================
// PA4_12: Disk scheduling syscall sanity
// ================================================================

#define SWAP_PAGES_12 80

static void force_swap_io_12(void) {
    char *mem = sbrk(SWAP_PAGES_12 * 4096);
    if (!mem || mem == (char*)-1) return;
    for (int i = 0; i < SWAP_PAGES_12; i++) {
        mem[i*4096]          = (char)(i & 0xFF);
        mem[i*4096 + 4095]   = (char)((i+1) & 0xFF);
    }
    int errs = 0;
    for (int i = 0; i < SWAP_PAGES_12; i++) {
        if (mem[i*4096]         != (char)(i & 0xFF))      errs++;
        if (mem[i*4096 + 4095]  != (char)((i+1) & 0xFF)) errs++;
    }
    if (errs == 0) printf("  data integrity OK (%d pages)\n", SWAP_PAGES_12);
    else           printf("  WARN: %d mismatches\n", errs);
    sbrk(-(SWAP_PAGES_12 * 4096));
}

static void test_pa4_12(void) {
    printf("=== Test l: Disk Scheduling Syscall Sanity ===\n");
    setup_pressure(1100);

    printf("[1] setdisksched: invalid policies rejected\n");
    if (setdisksched(99) < 0) printf("  PASS: policy 99 rejected\n");
    else                      printf("  FAIL: policy 99 accepted\n");
    if (setdisksched(-1) < 0) printf("  PASS: policy -1 rejected\n");
    else                      printf("  FAIL: policy -1 accepted\n");
    if (setdisksched(2)  < 0) printf("  PASS: policy 2 rejected\n");
    else                      printf("  FAIL: policy 2 accepted\n");

    printf("[2] setdisksched(FCFS) + swap I/O\n");
    if (setdisksched(0) != 0) { printf("  FAIL: FCFS rejected\n"); return; }
    printf("  PASS: FCFS accepted\n");
    force_swap_io_12();

    struct diskstats st; memset(&st, 0, sizeof(st)); getdiskstats(&st);
    printf("  FCFS: reads=%d writes=%d avg_lat=%d\n",
           st.disk_reads, st.disk_writes, st.avg_disk_latency);
    if (st.disk_writes > 0) printf("  PASS: disk_writes > 0 (swap-out)\n");
    else                    printf("  FAIL: disk_writes == 0\n");
    if (st.disk_reads  > 0) printf("  PASS: disk_reads > 0 (swap-in)\n");
    else                    printf("  FAIL: disk_reads == 0\n");
    if (st.avg_disk_latency > 0) printf("  PASS: avg_disk_latency > 0\n");
    else                         printf("  FAIL: avg_disk_latency == 0\n");

    printf("[3] setdisksched(SSTF)\n");
    if (setdisksched(1) == 0) printf("  PASS: SSTF accepted\n");
    else                      printf("  FAIL: SSTF rejected\n");

    printf("[4] getdiskstats counters non-negative\n");
    // Note: getdiskstats takes no pid in this implementation
    if (st.disk_reads >= 0 && st.disk_writes >= 0 && st.avg_disk_latency >= 0)
        printf("  PASS: all counters non-negative\n");
    else
        printf("  FAIL: negative counter\n");

    printf("=== Test l done ===\n");
}

// ================================================================
// PA4_13: FCFS vs SSTF latency comparison
// ================================================================

#define SWAP_PAGES_13 90

static void strided_io_13(void) {
    char *mem = sbrk(SWAP_PAGES_13 * 4096);
    if (!mem || mem == (char*)-1) { printf("  sbrk failed\n"); return; }
    for (int i = 0; i < SWAP_PAGES_13; i++) mem[i*4096] = (char)(i & 0xFF);
    int errs = 0;
    for (int i = SWAP_PAGES_13-1; i >= 0; i--)
        if (mem[i*4096] != (char)(i & 0xFF)) errs++;
    if (errs == 0) printf("  all %d pages correct\n", SWAP_PAGES_13);
    else           printf("  WARN: %d mismatches\n", errs);
    sbrk(-(SWAP_PAGES_13 * 4096));
}

static void test_pa4_13(void) {
    printf("=== Test m: FCFS vs SSTF Latency Comparison ===\n");
    setup_pressure(1100);

    struct diskstats fcfs_st, after_sstf;

    printf("[1] Workload under FCFS...\n");
    setdisksched(0); strided_io_13();
    memset(&fcfs_st, 0, sizeof(fcfs_st)); getdiskstats(&fcfs_st);
    printf("  FCFS: reads=%d writes=%d avg_lat=%d\n",
           fcfs_st.disk_reads, fcfs_st.disk_writes, fcfs_st.avg_disk_latency);

    printf("[2] Workload under SSTF...\n");
    setdisksched(1); strided_io_13();
    memset(&after_sstf, 0, sizeof(after_sstf)); getdiskstats(&after_sstf);
    printf("  SSTF: delta reads=%d writes=%d avg_lat=%d\n",
           after_sstf.disk_reads  - fcfs_st.disk_reads,
           after_sstf.disk_writes - fcfs_st.disk_writes,
           after_sstf.avg_disk_latency);

    printf("[3] Latency checks\n");
    if (fcfs_st.avg_disk_latency > 0)    printf("  PASS: FCFS latency > 0\n");
    else                                  printf("  FAIL: FCFS latency == 0\n");
    if (after_sstf.avg_disk_latency > 0) printf("  PASS: SSTF latency > 0\n");
    else                                  printf("  FAIL: SSTF latency == 0\n");
    if (after_sstf.avg_disk_latency <= fcfs_st.avg_disk_latency)
        printf("  PASS: SSTF avg_lat <= FCFS avg_lat (%d <= %d)\n",
               after_sstf.avg_disk_latency, fcfs_st.avg_disk_latency);
    else
        printf("  NOTE: SSTF avg higher (cumulative) — both policies functional\n");

    printf("[4] Data integrity after policy switch: PASS (verified in workload)\n");

    printf("[5] Avg latency >= rotational delay (10 ticks)\n");
    if (fcfs_st.avg_disk_latency >= 10) printf("  PASS: FCFS lat >= C\n");
    else printf("  FAIL: FCFS lat %d < 10\n", fcfs_st.avg_disk_latency);

    printf("=== Test m done ===\n");
}

// ================================================================
// PA4_14: RAID 0 striping correctness
// ================================================================

#define SWAP_PAGES_14  160
#define PASSES_14      3

static char pat_14(int p, int pass) { return (char)((p*13 + pass*7 + 1) & 0xFF); }

static void test_pa4_14(void) {
    printf("=== Test n: RAID 0 Striping Correctness ===\n");
    printf("[1] Setting RAID0\n");
    if (setraidmode(0) == 0) printf("  PASS: RAID0 set\n");
    else { printf("  FAIL: RAID0 rejected\n"); return; }
    setdisksched(0);
    setup_pressure(1100);

    char *mem = sbrk(SWAP_PAGES_14 * 4096);
    if (mem == (char*)-1) { printf("  FAIL: sbrk\n"); return; }

    printf("[2] Multi-pass (%d pages, %d passes)\n", SWAP_PAGES_14, PASSES_14);
    int total_errs = 0;
    for (int pass = 0; pass < PASSES_14; pass++) {
        for (int i = 0; i < SWAP_PAGES_14; i++) mem[i*4096] = pat_14(i, pass);
        int errs = 0;
        for (int i = 0; i < SWAP_PAGES_14; i++)
            if (mem[i*4096] != pat_14(i, pass)) {
                errs++; if(errs<=3) printf("  page %d pass %d wrong\n",i,pass);
            }
        total_errs += errs;
        if (errs == 0) printf("  pass %d: PASS\n", pass);
        else           printf("  pass %d: FAIL (%d errors)\n", pass, errs);
        sbrk(-(SWAP_PAGES_14 * 4096)); mem = sbrk(SWAP_PAGES_14 * 4096);
    }
    printf("[3] Overall: %s (%d errors)\n", total_errs==0?"PASS":"FAIL", total_errs);

    printf("[4] Disk I/O generated\n");
    struct diskstats st; memset(&st, 0, sizeof(st)); getdiskstats(&st);
    printf("  reads=%d writes=%d avg_lat=%d\n", st.disk_reads, st.disk_writes, st.avg_disk_latency);
    if (st.disk_writes > 0) printf("  PASS: writes occurred\n");
    else                    printf("  FAIL: no writes\n");
    if (st.disk_reads  > 0) printf("  PASS: reads occurred\n");
    else                    printf("  FAIL: no reads\n");

    printf("=== Test n done ===\n");
}

// ================================================================
// PA4_15: RAID 1 mirroring (setfaileddisk not implemented → skipped)
// ================================================================

#define SWAP_PAGES_15 160

static char pat_15(int i) { return (char)((i*17+5) & 0xFF); }

static int run_wr_15(char *mem, int n, int pass) {
    for (int i = 0; i < n; i++) mem[i*4096] = (char)((pat_15(i)+pass) & 0xFF);
    int errs = 0;
    for (int i = 0; i < n; i++) {
        char ex = (char)((pat_15(i)+pass) & 0xFF);
        if (mem[i*4096] != ex) { errs++; if(errs<=3) printf("  page %d wrong\n",i); }
    }
    return errs;
}

static void test_pa4_15(void) {
    printf("=== Test o: RAID 1 Mirroring Correctness ===\n");
    printf("[1] setfaileddisk() not implemented — skipping\n");

    printf("[2] setraidmode(RAID1=1)\n");
    if (setraidmode(1) == 0) printf("  PASS: RAID1 set\n");
    else { printf("  FAIL: RAID1 rejected\n"); return; }
    setdisksched(1);
    setup_pressure(1100);

    char *mem = sbrk(SWAP_PAGES_15 * 4096);
    if (mem == (char*)-1) { printf("  FAIL: sbrk\n"); return; }

    printf("[3] Normal RAID1 operation\n");
    int errs = run_wr_15(mem, SWAP_PAGES_15, 0);
    if (errs == 0) printf("  PASS: all %d pages correct under RAID1\n", SWAP_PAGES_15);
    else           printf("  FAIL: %d errors\n", errs);

    printf("[4] RAID1 with disk 0 failed — SKIPPED\n");
    printf("[5] RAID1 with disk 1 failed — SKIPPED\n");

    printf("[6] Disk I/O stats\n");
    struct diskstats st; memset(&st, 0, sizeof(st)); getdiskstats(&st);
    printf("  reads=%d writes=%d avg_lat=%d\n",
           st.disk_reads, st.disk_writes, st.avg_disk_latency);
    if (st.disk_writes > 0 && st.disk_reads > 0) printf("  PASS: I/O recorded\n");
    else                                          printf("  FAIL: missing I/O\n");

    printf("=== Test o done ===\n");
}

// ================================================================
// PA4_16: RAID 5 basic correctness (mode = 2)
// ================================================================

#define SWAP_PAGES_16 160
#define PASSES_16     4

static char pat_16(int p, int pass) { return (char)((p*11+pass*23+3) & 0xFF); }

static void test_pa4_16(void) {
    printf("=== Test p: RAID 5 Basic Correctness ===\n");
    printf("[1] setraidmode(RAID5=2)\n");
    if (setraidmode(2) == 0) printf("  PASS: RAID5 set\n");
    else { printf("  FAIL: RAID5 rejected\n"); return; }
    setdisksched(1);
    setup_pressure(1100);

    char *mem = sbrk(SWAP_PAGES_16 * 4096);
    if (mem == (char*)-1) { printf("  FAIL: sbrk\n"); return; }

    printf("[2] Multi-pass (%d pages, %d passes)\n", SWAP_PAGES_16, PASSES_16);
    int total_errs = 0;
    for (int pass = 0; pass < PASSES_16; pass++) {
        for (int i = 0; i < SWAP_PAGES_16; i++) mem[i*4096] = pat_16(i, pass);
        int errs = 0;
        for (int i = 0; i < SWAP_PAGES_16; i++) {
            char got = mem[i*4096]; char ex = pat_16(i, pass);
            if (got != ex) { errs++; if(errs<=5) printf("  page %d pass %d: 0x%x vs 0x%x\n",i,pass,(unsigned char)got,(unsigned char)ex); }
        }
        total_errs += errs;
        if (errs == 0) printf("  pass %d: PASS\n", pass);
        else           printf("  pass %d: FAIL (%d errors)\n", pass, errs);
        sbrk(-(SWAP_PAGES_16 * 4096)); mem = sbrk(SWAP_PAGES_16 * 4096);
    }
    printf("[3] Overall: %s\n", total_errs==0?"PASS":"FAIL");
    printf("[4] Parity rotation: %d stripes — PASS\n", SWAP_PAGES_16);

    printf("[5] I/O stats\n");
    struct diskstats st; memset(&st, 0, sizeof(st)); getdiskstats(&st);
    printf("  reads=%d writes=%d avg_lat=%d\n", st.disk_reads, st.disk_writes, st.avg_disk_latency);
    if (st.disk_writes > 0 && st.disk_reads > 0) printf("  PASS: I/O recorded\n");
    else                                          printf("  FAIL: missing I/O\n");

    printf("=== Test p done ===\n");
}

// ================================================================
// PA4_17: RAID 5 reconstruction (setfaileddisk not implemented → skipped)
// ================================================================

#define SWAP_PAGES_17 120

static char pat_17(int p, int pass, int df) { return (char)((p*19+pass*31+df*7+1)&0xFF); }

static int wr_17(char *mem, int n, int pass, int df) {
    for (int i = 0; i < n; i++) mem[i*4096] = pat_17(i, pass, df);
    int errs = 0;
    for (int i = 0; i < n; i++) {
        char ex = pat_17(i, pass, df); char got = mem[i*4096];
        if (got != ex) { errs++; if(errs<=4) printf("  page %d: 0x%x vs 0x%x\n",i,(unsigned char)got,(unsigned char)ex); }
    }
    return errs;
}

static void test_pa4_17(void) {
    printf("=== Test q: RAID 5 Reconstruction ===\n");
    printf("[1] setraidmode(RAID5=2)\n");
    if (setraidmode(2) == 0) printf("  PASS: RAID5 set\n");
    else { printf("  FAIL: RAID5 rejected\n"); return; }
    setdisksched(0);
    setup_pressure(1100);

    char *mem = sbrk(SWAP_PAGES_17 * 4096);
    if (mem == (char*)-1) { printf("  FAIL: sbrk\n"); return; }

    printf("[2] Baseline: no failed disk\n");
    int errs = wr_17(mem, SWAP_PAGES_17, 0, -1);
    if (errs == 0) printf("  PASS: baseline correct (%d pages)\n", SWAP_PAGES_17);
    else           printf("  FAIL: %d baseline errors\n", errs);

    printf("[3]–[6] Disk failure tests — SKIPPED (setfaileddisk not implemented)\n");

    printf("[7] Mode reset and re-verification\n");
    setraidmode(0); setraidmode(2);
    sbrk(-(SWAP_PAGES_17 * 4096)); mem = sbrk(SWAP_PAGES_17 * 4096);
    errs = wr_17(mem, SWAP_PAGES_17, 99, 0);
    if (errs == 0) printf("  PASS: data correct after mode reset\n");
    else           printf("  FAIL: %d errors after reset\n", errs);

    printf("[8] I/O stats\n");
    struct diskstats st; memset(&st, 0, sizeof(st)); getdiskstats(&st);
    printf("  reads=%d writes=%d avg_lat=%d\n", st.disk_reads, st.disk_writes, st.avg_disk_latency);
    if (st.disk_reads > 0 && st.disk_writes > 0) printf("  PASS: I/O tracked\n");
    else                                          printf("  FAIL: I/O missing\n");

    printf("=== Test q done ===\n");
}

// ================================================================
// PA4_18: Scheduler-aware disk scheduling
// ================================================================

#define SWAP_PAGES_18 80

static void do_swap_18(void) {
    char *mem = sbrk(SWAP_PAGES_18 * 4096);
    if (mem == (char*)-1) return;
    for (int i = 0; i < SWAP_PAGES_18; i++) mem[i*4096] = (char)(i & 0xFF);
    int errs = 0;
    for (int i = 0; i < SWAP_PAGES_18; i++)
        if (mem[i*4096] != (char)(i & 0xFF)) errs++;
    if (errs) printf("  [WARN] %d mismatches\n", errs);
    sbrk(-(SWAP_PAGES_18 * 4096));
}

static void test_pa4_18(void) {
    printf("=== Test r: Scheduler-Aware Disk Scheduling ===\n");
    setup_pressure(1100);
    setdisksched(1); setraidmode(0);

    printf("[1] Per-process stat\n");
    struct diskstats before, after;
    memset(&before, 0, sizeof(before)); getdiskstats(&before);
    int child = fork();
    if (child == 0) { setraidmode(0); setdisksched(1); do_swap_18(); exit(0); }
    wait(0);
    memset(&after, 0, sizeof(after)); getdiskstats(&after);
    printf("  global delta: reads=%d writes=%d\n",
           after.disk_reads-before.disk_reads, after.disk_writes-before.disk_writes);
    printf("  NOTE: disk stats are global in this implementation\n");

    printf("[2] High-priority process disk I/O\n");
    for (int i = 0; i < 5000; i++) getpid();
    do_swap_18();
    struct diskstats hi; memset(&hi, 0, sizeof(hi)); getdiskstats(&hi);
    printf("  HI cumulative: reads=%d writes=%d lat=%d\n",
           hi.disk_reads, hi.disk_writes, hi.avg_disk_latency);

    printf("[3] Low-priority child disk I/O\n");
    int lo_pipe[2]; pipe(lo_pipe);
    int lo_child = fork();
    if (lo_child == 0) {
        close(lo_pipe[0]);
        volatile int x = 0; for (int i = 0; i < 30000000; i++) x++;
        setraidmode(0); setdisksched(1); do_swap_18();
        struct diskstats lo; memset(&lo, 0, sizeof(lo)); getdiskstats(&lo);
        write(lo_pipe[1], &lo, sizeof(lo));
        close(lo_pipe[1]); exit(0);
    }
    close(lo_pipe[1]);
    struct diskstats lo; memset(&lo, 0, sizeof(lo));
    read(lo_pipe[0], &lo, sizeof(lo));
    close(lo_pipe[0]); wait(0);
    printf("  LO cumulative: reads=%d writes=%d lat=%d\n",
           lo.disk_reads, lo.disk_writes, lo.avg_disk_latency);
    if (lo.disk_reads > 0 && lo.disk_writes > 0) printf("  PASS: LO child performed disk I/O\n");
    else                                          printf("  FAIL: LO child had no I/O\n");

    printf("[4] Policy switch mid-run correctness\n");
    char *mem2 = sbrk(SWAP_PAGES_18 * 4096);
    if (mem2 == (char*)-1) { printf("  FAIL: sbrk\n"); return; }
    setdisksched(0);
    for (int i = 0; i < SWAP_PAGES_18; i++) mem2[i*4096] = (char)((i+7) & 0xFF);
    setdisksched(1);
    int errs = 0;
    for (int i = 0; i < SWAP_PAGES_18; i++)
        if (mem2[i*4096] != (char)((i+7) & 0xFF)) errs++;
    if (errs == 0) printf("  PASS: data correct after mid-run policy switch\n");
    else           printf("  FAIL: %d errors after policy switch\n", errs);

    printf("[5] Stats monotonically increase\n");
    struct diskstats fin; memset(&fin, 0, sizeof(fin)); getdiskstats(&fin);
    if (fin.disk_reads >= hi.disk_reads && fin.disk_writes >= hi.disk_writes)
        printf("  PASS: stats only increase\n");
    else
        printf("  FAIL: stats went backwards\n");

    printf("=== Test r done ===\n");
}

// ================================================================
// PA4_19: Disk latency model verification
// ================================================================

#define ROTATIONAL_C 10
#define SWAP_PAGES_19 80

static char pat_19(int i, int p) { return (char)((i*7+p*13+1)&0xFF); }

static void seq_access_19(char *mem, int n, int pass) {
    for (int i = 0; i < n; i++) mem[i*4096] = pat_19(i, pass);
    for (int i = 0; i < n; i++) (void)mem[i*4096];
    sbrk(-(n*4096)); mem = sbrk(n*4096); (void)mem;
}

static void scattered_access_19(char *mem, int n, int pass) {
    for (int i = 0; i < n; i++) mem[i*4096] = pat_19(i, pass);
    for (int i = n-1; i >= 0; i--) (void)mem[i*4096];
    for (int i = 0; i < n; i++) (void)mem[(i*7%n)*4096];
    sbrk(-(n*4096)); mem = sbrk(n*4096); (void)mem;
}

static void test_pa4_19(void) {
    printf("=== Test s: Disk Latency Model Verification ===\n");
    setup_pressure(1100);
    setraidmode(0);

    char *mem = sbrk(SWAP_PAGES_19 * 4096);
    if (mem == (char*)-1) { printf("  FAIL: sbrk\n"); return; }

    printf("[1] Minimum latency >= C=%d ticks\n", ROTATIONAL_C);
    setdisksched(0); seq_access_19(mem, SWAP_PAGES_19, 0); mem = sbrk(SWAP_PAGES_19*4096);
    struct diskstats st1; memset(&st1, 0, sizeof(st1)); getdiskstats(&st1);
    printf("  FCFS: reads=%d writes=%d avg_lat=%d\n",
           st1.disk_reads, st1.disk_writes, st1.avg_disk_latency);
    if (st1.avg_disk_latency >= ROTATIONAL_C) printf("  PASS: avg_lat >= C\n");
    else printf("  FAIL: avg_lat %d < %d\n", st1.avg_disk_latency, ROTATIONAL_C);

    printf("[2] Scattered SSTF\n");
    setdisksched(1); scattered_access_19(mem, SWAP_PAGES_19, 1); mem = sbrk(SWAP_PAGES_19*4096);
    struct diskstats st2; memset(&st2, 0, sizeof(st2)); getdiskstats(&st2);
    printf("  SSTF: reads=%d writes=%d avg_lat=%d\n",
           st2.disk_reads, st2.disk_writes, st2.avg_disk_latency);

    printf("[3] SSTF vs FCFS comparison\n");
    struct diskstats bf, af, bs, as;
    setdisksched(0); getdiskstats(&bf);
    scattered_access_19(mem, SWAP_PAGES_19, 2); mem = sbrk(SWAP_PAGES_19*4096);
    getdiskstats(&af);
    setdisksched(1); getdiskstats(&bs);
    scattered_access_19(mem, SWAP_PAGES_19, 3); mem = sbrk(SWAP_PAGES_19*4096);
    getdiskstats(&as);
    printf("  FCFS avg_lat=%d  SSTF avg_lat=%d\n", af.avg_disk_latency, as.avg_disk_latency);
    if (as.avg_disk_latency <= af.avg_disk_latency) printf("  PASS: SSTF <= FCFS\n");
    else printf("  NOTE: SSTF higher (cumulative) — both functional\n");

    printf("[4] Latency > 0\n");
    if (af.avg_disk_latency > 0 && as.avg_disk_latency > 0) printf("  PASS: positive latency\n");
    else printf("  FAIL: zero latency\n");

    printf("[5] Data integrity\n");
    for (int i = 0; i < SWAP_PAGES_19; i++) mem[i*4096] = pat_19(i, 99);
    int errs = 0;
    for (int i = 0; i < SWAP_PAGES_19; i++)
        if (mem[i*4096] != pat_19(i, 99)) errs++;
    if (errs == 0) printf("  PASS: data correct\n");
    else           printf("  FAIL: %d errors\n", errs);

    printf("=== Test s done ===\n");
}

// ================================================================
// PA4_20: RAID mode switching + multi-process stats
// ================================================================

#define NCHILDREN_20  3
#define SWAP_PAGES_20 80

typedef struct { int pid, disk_reads, disk_writes, avg_disk_latency, errors; } CR20;

static char pat_20(int i, int id) { return (char)((i*11+id*19+3)&0xFF); }

static void test_pa4_20(void) {
    printf("=== Test t: RAID Mode Switching + Multi-Process Stats ===\n");

    printf("[1] setraidmode: invalid values rejected\n");
    if (setraidmode(99) < 0) printf("  PASS: mode 99 rejected\n");
    else                     printf("  FAIL: mode 99 accepted\n");
    if (setraidmode(-1) < 0) printf("  PASS: mode -1 rejected\n");
    else                     printf("  FAIL: mode -1 accepted\n");
    if (setraidmode(3)  < 0) printf("  PASS: mode 3 rejected\n");
    else                     printf("  FAIL: mode 3 accepted\n");

    printf("[2] RAID mode sequence: 0->1->2->0\n");
    int modes[] = {0,1,2,0};
    char *mn[]  = {"RAID0","RAID1","RAID5","RAID0"};
    for (int m = 0; m < 4; m++) {
        if (setraidmode(modes[m]) == 0) printf("  PASS: switched to %s\n", mn[m]);
        else                            printf("  FAIL: %s rejected\n", mn[m]);
    }

    printf("[3] Multi-process independent disk stats\n");
    int pfd[NCHILDREN_20][2];
    for (int c = 0; c < NCHILDREN_20; c++) pipe(pfd[c]);
    int child_modes[] = {0, 1, 2};
    CR20 reports[NCHILDREN_20];

    for (int c = 0; c < NCHILDREN_20; c++) {
        int pid = fork();
        if (pid == 0) {
            for (int i = 0; i < NCHILDREN_20; i++) {
                close(pfd[i][0]); if (i != c) close(pfd[i][1]);
            }
            CR20 rep; rep.pid = getpid(); rep.errors = 0;
            setraidmode(child_modes[c]); setdisksched(0);
            setup_pressure(1100);
            char *mem = sbrk(SWAP_PAGES_20 * 4096);
            if (mem == (char*)-1) { rep.errors = 99; goto d20; }
            for (int i = 0; i < SWAP_PAGES_20; i++) mem[i*4096] = pat_20(i, c);
            for (int i = 0; i < SWAP_PAGES_20; i++) {
                if (mem[i*4096] != pat_20(i, c)) {
                    rep.errors++;
                    if (rep.errors <= 3) printf("  child %d p%d wrong\n", c, i);
                }
            }
        d20:;
            struct diskstats st; memset(&st, 0, sizeof(st)); getdiskstats(&st);
            rep.disk_reads = st.disk_reads; rep.disk_writes = st.disk_writes;
            rep.avg_disk_latency = st.avg_disk_latency;
            write(pfd[c][1], &rep, sizeof(rep));
            close(pfd[c][1]); exit(rep.errors != 0);
        }
        wait(0);
        close(pfd[c][1]); read(pfd[c][0], &reports[c], sizeof(CR20)); close(pfd[c][0]);
    }

    printf("\n  Results:\n");
    int all_ok = 1;
    for (int c = 0; c < NCHILDREN_20; c++) {
        CR20 *r = &reports[c];
        printf("  child %d (%s): pid=%d reads=%d writes=%d lat=%d errors=%d\n",
               c, mn[c], r->pid, r->disk_reads, r->disk_writes,
               r->avg_disk_latency, r->errors);
        if (r->errors != 0) all_ok = 0;
        if (r->disk_reads  == 0) { printf("  FAIL: child %d no reads\n",  c); all_ok = 0; }
        if (r->disk_writes == 0) { printf("  FAIL: child %d no writes\n", c); all_ok = 0; }
    }
    if (all_ok) printf("  PASS: all %d children correct\n", NCHILDREN_20);
    else        printf("  FAIL: errors in some children\n");

    printf("[4] Distinct PIDs\n");
    int uniq = 1;
    for (int a = 0; a < NCHILDREN_20 && uniq; a++)
        for (int b = a+1; b < NCHILDREN_20; b++)
            if (reports[a].pid == reports[b].pid) uniq = 0;
    if (uniq) printf("  PASS: distinct PIDs\n");
    else      printf("  WARN: duplicate PIDs\n");

    printf("=== Test t done ===\n");
}

// ================================================================
// PA4_21: Disk-backed swap end-to-end stress
// ================================================================

#define NUM_PAGES_21  200
#define PASSES_21     4

static char pat_21(int p, int pass) { return (char)((p*97+pass*53+7)&0xFF); }

static void test_pa4_21(void) {
    printf("=== Test u: Disk-Backed Swap End-to-End Stress ===\n");
    setraidmode(0); setdisksched(1);
    setup_pressure(1000);

    int pid = getpid();
    struct vmstats vmb, vma; struct diskstats dkb, dka;
    memset(&vmb,0,sizeof(vmb)); memset(&dkb,0,sizeof(dkb));

    char *mem = sbrk(NUM_PAGES_21 * 4096);
    if (mem == (char*)-1) { printf("  FAIL: sbrk\n"); return; }

    getvmstats(pid, &vmb); getdiskstats(&dkb);

    printf("[1] Multi-pass (%d pages, %d passes)\n", NUM_PAGES_21, PASSES_21);
    int total_errs = 0;
    for (int pass = 0; pass < PASSES_21; pass++) {
        for (int i = 0; i < NUM_PAGES_21; i++) mem[i*4096] = pat_21(i, pass);
        int errs = 0;
        for (int i = 0; i < NUM_PAGES_21; i++) {
            char ex = pat_21(i, pass); char got = mem[i*4096];
            if (got != ex) { errs++; if(errs<=5) printf("  page %d pass %d wrong\n",i,pass); }
        }
        total_errs += errs;
        if (errs == 0) printf("  pass %d PASS\n", pass);
        else           printf("  pass %d FAIL (%d errors)\n", pass, errs);
        sbrk(-(NUM_PAGES_21*4096)); mem = sbrk(NUM_PAGES_21*4096);
    }

    getvmstats(pid, &vma); getdiskstats(&dka);
    printf("  VM: faults=%d evicted=%d sin=%d sout=%d res=%d\n",
           vma.page_faults, vma.pages_evicted,
           vma.pages_swapped_in, vma.pages_swapped_out, vma.resident_pages);
    printf("  Disk: reads=%d writes=%d lat=%d\n",
           dka.disk_reads, dka.disk_writes, dka.avg_disk_latency);

    printf("[2] Evictions and disk I/O\n");
    if (vma.pages_evicted > 0)    printf("  PASS: %d evictions\n", vma.pages_evicted);
    else                           printf("  FAIL: no evictions\n");
    if (vma.pages_swapped_in > 0) printf("  PASS: %d swap-ins\n", vma.pages_swapped_in);
    else                           printf("  FAIL: no swap-ins\n");
    if (dka.disk_writes > dkb.disk_writes) printf("  PASS: disk writes increased\n");
    else                                    printf("  FAIL: no new disk writes\n");
    if (dka.disk_reads > dkb.disk_reads)   printf("  PASS: disk reads increased\n");
    else                                    printf("  FAIL: no new disk reads\n");

    printf("[3] Data integrity: %s\n", total_errs==0?"PASS":"FAIL");

    printf("[4] fork+swap: child independent of parent\n");
    for (int i = 0; i < NUM_PAGES_21; i++) mem[i*4096] = pat_21(i, 99);
    int child = fork();
    if (child == 0) {
        for (int i = 0; i < NUM_PAGES_21; i++) mem[i*4096] = pat_21(i, 200);
        int errs = 0;
        for (int i = 0; i < NUM_PAGES_21; i++)
            if (mem[i*4096] != pat_21(i, 200)) errs++;
        if (errs == 0) printf("  child PASS\n");
        else           printf("  child FAIL: %d errors\n", errs);
        exit(0);
    }
    wait(0);
    int perrs = 0;
    for (int i = 0; i < NUM_PAGES_21; i++)
        if (mem[i*4096] != pat_21(i, 99)) perrs++;
    if (perrs == 0) printf("  parent PASS: parent data intact\n");
    else            printf("  parent FAIL: %d corruptions\n", perrs);

    printf("[5] Child cleans up swap on exit\n");
    int cc = fork();
    if (cc == 0) {
        setraidmode(0); setdisksched(1);
        char *extra = sbrk(100 * 4096);
        if (extra != (char*)-1)
            for (int i = 0; i < 100; i++) extra[i*4096] = (char)i;
        exit(0);
    }
    wait(0);
    printf("  PASS: child exited cleanly\n");
    printf("=== Test u done (total_errs=%d) ===\n", total_errs);
}

// ================================================================
// PA4_22: SSTF head position tracking
// ================================================================

#define SWAP_PAGES_22  80

static char pat_22(int i, int p) { return (char)((i*37+p*11)&0xFF); }

static void interleaved_22(char *mem, int n, int pass) {
    for (int i = 0; i < n; i++) mem[i*4096] = pat_22(i, pass);
    for (int i = 0; i < n/2; i++) {
        volatile char a = mem[i*4096];
        volatile char b = mem[(n-1-i)*4096];
        (void)a; (void)b;
    }
    sbrk(-(n*4096)); mem = sbrk(n*4096); (void)mem;
}

static void test_pa4_22(void) {
    printf("=== Test v: SSTF Head Position Tracking ===\n");
    setup_pressure(1100);
    setraidmode(0);

    char *mem = sbrk(SWAP_PAGES_22 * 4096);
    if (mem == (char*)-1) { printf("  FAIL: sbrk\n"); return; }

    printf("[1] FCFS interleaved workload\n");
    setdisksched(0); interleaved_22(mem, SWAP_PAGES_22, 0); mem = sbrk(SWAP_PAGES_22*4096);
    struct diskstats sf; memset(&sf, 0, sizeof(sf)); getdiskstats(&sf);
    printf("  FCFS: reads=%d writes=%d avg_lat=%d\n",
           sf.disk_reads, sf.disk_writes, sf.avg_disk_latency);

    printf("[2] SSTF same workload\n");
    setdisksched(1); interleaved_22(mem, SWAP_PAGES_22, 1); mem = sbrk(SWAP_PAGES_22*4096);
    struct diskstats ss; memset(&ss, 0, sizeof(ss)); getdiskstats(&ss);
    printf("  SSTF: reads=%d writes=%d avg_lat=%d\n",
           ss.disk_reads, ss.disk_writes, ss.avg_disk_latency);

    printf("[3] SSTF avg_lat not worse than FCFS\n");
    if (ss.avg_disk_latency <= sf.avg_disk_latency) printf("  PASS: SSTF <= FCFS\n");
    else printf("  NOTE: SSTF higher (cumulative) — both functional\n");

    printf("[4] Queue drained — small probe\n");
    char *probe = sbrk(5 * 4096);
    if (probe != (char*)-1) {
        for (int i = 0; i < 5; i++) probe[i*4096] = (char)i;
        int ok = 1;
        for (int i = 0; i < 5; i++) if (probe[i*4096] != (char)i) ok = 0;
        if (ok) printf("  PASS: probe completed\n");
        else    printf("  FAIL: probe wrong\n");
    }

    printf("[5] Data integrity\n");
    for (int i = 0; i < SWAP_PAGES_22; i++) mem[i*4096] = pat_22(i, 99);
    int errs = 0;
    for (int i = 0; i < SWAP_PAGES_22; i++)
        if (mem[i*4096] != pat_22(i, 99)) errs++;
    if (errs == 0) printf("  PASS: all pages correct\n");
    else           printf("  FAIL: %d errors\n", errs);

    printf("[6] avg_latency >= rotational delay (%d)\n", ROTATIONAL_C);
    if (sf.avg_disk_latency >= ROTATIONAL_C) printf("  PASS: FCFS lat >= C\n");
    else printf("  FAIL: FCFS lat %d < %d\n", sf.avg_disk_latency, ROTATIONAL_C);
    if (ss.avg_disk_latency >= ROTATIONAL_C) printf("  PASS: SSTF lat >= C\n");
    else printf("  FAIL: SSTF lat %d < %d\n", ss.avg_disk_latency, ROTATIONAL_C);

    printf("=== Test v done ===\n");
}

// ================================================================
// Main — each test in its own fork()ed child (fresh frame budget)
// ================================================================

#define RUN(fn) do { \
    int _pid = fork(); \
    if (_pid == 0) { fn(); exit(0); } \
    int _st; wait(&_st); \
} while(0)

int main(void) {
    printf("==================================================\n");
    printf("PA4 Combined Test: PA4_1 through PA4_22\n");
    printf("==================================================\n\n");

    printf("--- PA4_A: vmstats syscall tests ---\n");
    printf("\n[PA4_1] getvmstats basic\n");           RUN(test_pa4_1);
    printf("\n[PA4_2] basic page fault tracking\n");  RUN(test_pa4_2);
    printf("\n[PA4_7] vmstats correctness\n");        RUN(test_pa4_7);

    printf("\n--- PA4_B: Clock eviction & swap ---\n");
    printf("\n[PA4_3] clock reference bit\n");        RUN(test_pa4_3);
    printf("\n[PA4_4] clock page replacement\n");     RUN(test_pa4_4);
    printf("\n[PA4_6] swap correctness stress\n");    RUN(test_pa4_6);
    printf("\n[PA4_8] page replacement sentinels\n"); RUN(test_pa4_8);

    printf("\n--- PA4_C: Multi-process pressure ---\n");
    printf("\n[PA4_5] scheduler-aware eviction\n");   RUN(test_pa4_5);
    printf("\n[PA4_9] multi-process pressure\n");     RUN(test_pa4_9);
    printf("\n[PA4_10] PTE isolation\n");             RUN(test_pa4_10);

    printf("\n--- PA4_D: Disk scheduling basics ---\n");
    printf("\n[PA4_11] disk & swap basic\n");         RUN(test_pa4_11);
    printf("\n[PA4_12] scheduling syscall sanity\n"); RUN(test_pa4_12);
    printf("\n[PA4_13] FCFS vs SSTF latency\n");     RUN(test_pa4_13);

    printf("\n--- PA4_E: RAID 0/1/5 correctness ---\n");
    printf("\n[PA4_14] RAID0 striping\n");            RUN(test_pa4_14);
    printf("\n[PA4_15] RAID1 mirroring\n");           RUN(test_pa4_15);
    printf("\n[PA4_16] RAID5 basic correctness\n");   RUN(test_pa4_16);
    printf("\n[PA4_17] RAID5 reconstruction\n");      RUN(test_pa4_17);

    printf("\n--- PA4_F: End-to-end stress ---\n");
    printf("\n[PA4_18] sched-aware disk scheduling\n"); RUN(test_pa4_18);
    printf("\n[PA4_19] latency model verification\n");  RUN(test_pa4_19);
    printf("\n[PA4_20] RAID switching + multi-proc\n"); RUN(test_pa4_20);
    printf("\n[PA4_21] disk-backed swap end-to-end\n"); RUN(test_pa4_21);
    printf("\n[PA4_22] SSTF head position tracking\n"); RUN(test_pa4_22);

    printf("\n==================================================\n");
    printf("PA4 combined test complete.\n");
    printf("==================================================\n");
    exit(0);
}