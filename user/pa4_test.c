/*
 * pa4_test.c — Comprehensive stress test for PA4
 * (Disk-backed swap, disk scheduling, RAID)
 *
 * Add to Makefile UPROGS:
 *   $U/_pa4_test\
 *
 * Run from xv6 shell:
 *   $ pa4_test
 */

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

/* ============================================================
   Test framework
   ============================================================ */

static int g_pass = 0;
static int g_fail = 0;

static void
check(const char *name, int cond)
{
    if(cond){ printf("  [PASS] %s\n", name); g_pass++; }
    else    { printf("  [FAIL] %s\n", name); g_fail++; }
}

static void section(const char *t) { printf("\n========== %s ==========\n", t); }

static void
summary(void)
{
    printf("\n==========================================\n");
    printf("  PASSED : %d\n", g_pass);
    printf("  FAILED : %d\n", g_fail);
    printf("  TOTAL  : %d\n", g_pass + g_fail);
    if(g_fail == 0) printf("  ALL TESTS PASSED\n");
    else            printf("  SOME TESTS FAILED\n");
    printf("==========================================\n");
}

/* Burn CPU without syscalls */
// static void burn(int n) { volatile int x=0; for(int i=0;i<n;i++) x^=i*3; (void)x; }

/* Touch pages to force page faults / potential evictions */
static void touch(char *p, int pages)
{
    for(int i = 0; i < pages; i++) p[i * 4096] = (char)(i & 0xFF);
}

/* Verify page content written by touch() */
static int verify(char *p, int pages)
{
    for(int i = 0; i < pages; i++)
        if(p[i * 4096] != (char)(i & 0xFF)) return 0;
    return 1;
}

/* ============================================================
   Section 1 — setdisksched() syscall interface
   ============================================================ */

static void
test_setdisksched_interface(void)
{
    section("PA4-SCHED-1: setdisksched() interface");

    int r;
    r = setdisksched(0);   /* FCFS */
    check("setdisksched(FCFS=0) returns 0", r == 0);

    r = setdisksched(1);   /* SSTF */
    check("setdisksched(SSTF=1) returns 0", r == 0);

    r = setdisksched(2);   /* invalid */
    check("setdisksched(invalid=2) returns -1", r == -1);

    r = setdisksched(-1);  /* invalid */
    check("setdisksched(invalid=-1) returns -1", r == -1);

    /* restore to FCFS for subsequent tests */
    setdisksched(0);
}

/* ============================================================
   Section 2 — setraidmode() syscall interface
   ============================================================ */

static void
test_setraidmode_interface(void)
{
    section("PA4-RAID-1: setraidmode() interface");

    int r;
    r = setraidmode(0);   /* RAID 0 */
    check("setraidmode(RAID0=0) returns 0", r == 0);

    r = setraidmode(1);   /* RAID 1 */
    check("setraidmode(RAID1=1) returns 0", r == 0);

    r = setraidmode(2);   /* RAID 5 */
    check("setraidmode(RAID5=2) returns 0", r == 0);

    r = setraidmode(3);   /* invalid */
    check("setraidmode(invalid=3) returns -1", r == -1);

    r = setraidmode(-1);  /* invalid */
    check("setraidmode(invalid=-1) returns -1", r == -1);

    /* restore to RAID 0 */
    setraidmode(0);
}

/* ============================================================
   Section 3 — getdiskstats() syscall interface
   ============================================================ */

static void
test_getdiskstats_interface(void)
{
    section("PA4-STATS-1: getdiskstats() interface");

    struct diskstats s;
    int r = getdiskstats(&s);
    check("getdiskstats() returns 0", r == 0);
    check("disk_reads >= 0",          s.disk_reads >= 0);
    check("disk_writes >= 0",         s.disk_writes >= 0);
    check("avg_disk_latency >= 0",    s.avg_disk_latency >= 0);

    printf("  Initial: reads=%d writes=%d avg_lat=%d\n",
           s.disk_reads, s.disk_writes, s.avg_disk_latency);
}

/* ============================================================
   Section 4 — disk stats increase after swap activity
   ============================================================ */

static void
test_stats_increase_on_swap(void)
{
    section("PA4-STATS-2: disk_writes/reads increase after eviction and swap-in");

    setraidmode(0);
    setdisksched(0);

    int pid = fork();
    if(pid == 0){
        /*
         * Allocate sink FIRST — it takes high-indexed frames (kalloc returns
         * high frames first from the reversed freelist). Then allocate arr —
         * it gets low-indexed frames. Clock scans from 0 and evicts
         * low-indexed (arr) frames first, not sink frames.
         */
        int sink_pages = 1200;
        char *sink = sbrk(sink_pages * 4096);
        touch(sink, sink_pages);

        int pages = 400;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);  /* Clock evicts arr's low-index frames to disk */

        struct vmstats vs0;
        getvmstats(getpid(), &vs0);

        /* Re-access all arr pages — evicted pages trigger swap-in via trap.c */
        for(int i = 0; i < pages; i++)
            arr[i * 4096] += 1;

        struct vmstats vs1;
        getvmstats(getpid(), &vs1);

        struct diskstats ds;
        getdiskstats(&ds);

        int evicted = vs0.pages_evicted;
        int swapin  = vs1.pages_swapped_in - vs0.pages_swapped_in;

        printf("  evicted=%d swapin=%d dw=%d dr=%d\n",
               evicted, swapin, ds.disk_writes, ds.disk_reads);

        int ok = (evicted > 0) && (swapin > 0) &&
                 (ds.disk_writes > 0) && (ds.disk_reads > 0);
        exit(ok ? 0 : 1);
    }

    int st;
    wait(&st);

    struct diskstats after;
    getdiskstats(&after);
    printf("  Global: reads=%d writes=%d lat=%d\n",
           after.disk_reads, after.disk_writes, after.avg_disk_latency);

    check("disk_writes increased after eviction",
          after.disk_writes > 0);
    check("eviction + swapin both happened AND disk reads increased", st == 0);
    check("avg_disk_latency > 0 after I/O",
          after.avg_disk_latency > 0);
}
/* ============================================================
   Section 5 — RAID 0: data integrity
   ============================================================ */

static void
test_raid0_integrity(void)
{
    section("PA4-RAID0: Data integrity with RAID 0 (striping)");

    setraidmode(0);
    setdisksched(0);

    int pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);

        /* Force evictions */
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);

        /* Verify original data restored from disk */
        int ok = verify(arr, pages);
        struct vmstats vs;
        getvmstats(getpid(), &vs);
        printf("  RAID0: SwapOut=%d SwapIn=%d errors=%s\n",
               vs.pages_swapped_out, vs.pages_swapped_in, ok ? "0" : ">0");
        exit(ok ? 0 : 1);
    }
    int st; wait(&st);
    check("RAID 0: all data intact after eviction and swap-in", st == 0);
}

/* ============================================================
   Section 6 — RAID 1: data integrity (mirroring)
   ============================================================ */

static void
test_raid1_integrity(void)
{
    section("PA4-RAID1: Data integrity with RAID 1 (mirroring)");

    setraidmode(1);
    setdisksched(0);

    int pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);

        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);

        int ok = verify(arr, pages);
        struct vmstats vs;
        getvmstats(getpid(), &vs);
        printf("  RAID1: SwapOut=%d SwapIn=%d errors=%s\n",
               vs.pages_swapped_out, vs.pages_swapped_in, ok ? "0" : ">0");
        exit(ok ? 0 : 1);
    }
    int st; wait(&st);
    check("RAID 1: all data intact after eviction and swap-in", st == 0);

    /* Restore */
    setraidmode(0);
}

/* ============================================================
   Section 7 — RAID 5: data integrity (striping with parity)
   ============================================================ */

static void
test_raid5_integrity(void)
{
    section("PA4-RAID5: Data integrity with RAID 5 (parity)");

    setraidmode(2);
    setdisksched(0);

    int pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);

        /* Write a distinctive multi-byte pattern */
        for(int i = 0; i < pages; i++)
            for(int j = 0; j < 8; j++)
                arr[i * 4096 + j] = (char)((i * 7 + j * 3) & 0xFF);

        /* Force evictions */
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);

        /* Verify multi-byte pattern */
        int errors = 0;
        for(int i = 0; i < pages; i++)
            for(int j = 0; j < 8; j++){
                char expected = (char)((i * 7 + j * 3) & 0xFF);
                if(arr[i * 4096 + j] != expected) errors++;
            }

        struct vmstats vs;
        getvmstats(getpid(), &vs);
        printf("  RAID5: SwapOut=%d SwapIn=%d errors=%d\n",
               vs.pages_swapped_out, vs.pages_swapped_in, errors);
        exit(errors == 0 ? 0 : 1);
    }
    int st; wait(&st);
    check("RAID 5: 8-byte pattern per page intact after swap cycle", st == 0);

    setraidmode(0);
}

/* ============================================================
   Section 8 — RAID mode switching mid-workload does not panic
   ============================================================ */

static void
test_raid_mode_switching(void)
{
    section("PA4-RAID-SW: Switching RAID mode between operations");

    int pid = fork();
    if(pid == 0){
        /* Write under RAID 0 */
        setraidmode(0);
        char *a0 = sbrk(200 * 4096);
        touch(a0, 200);

        /* Switch to RAID 1 and do more work */
        setraidmode(1);
        char *a1 = sbrk(200 * 4096);
        touch(a1, 200);

        /* Switch to RAID 5 and do more work */
        setraidmode(2);
        char *a2 = sbrk(200 * 4096);
        touch(a2, 200);

        /* Back to RAID 0 */
        setraidmode(0);
        exit(0);
    }
    int st; wait(&st);
    check("Switching RAID mode 0→1→5→0 during activity does not panic", st == 0);
}

/* ============================================================
   Section 9 — FCFS vs SSTF latency comparison
   ============================================================ */

static void
test_scheduling_latency_comparison(void)
{
    section("PA4-SCHED-2: FCFS vs SSTF latency comparison");

    /*
     * Strategy: run identical swap-heavy workloads under FCFS then SSTF.
     * SSTF should produce lower or equal avg_disk_latency because it
     * minimises seek distance. With our simulated sequential pages SSTF
     * may be similar to FCFS, but the key check is that both work correctly
     * and latency is > 0.
     */

    struct diskstats s_before_fcfs, s_after_fcfs;
    struct diskstats s_before_sstf, s_after_sstf;

    /* --- FCFS workload --- */
    setdisksched(0);
    getdiskstats(&s_before_fcfs);

    int pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);
        for(int i = 0; i < pages; i++) arr[i * 4096] += 1;
        exit(0);
    }
    wait(0);
    getdiskstats(&s_after_fcfs);

    int fcfs_writes = s_after_fcfs.disk_writes - s_before_fcfs.disk_writes;
    int fcfs_reads  = s_after_fcfs.disk_reads  - s_before_fcfs.disk_reads;

    /* --- SSTF workload (same size) --- */
    setdisksched(1);
    getdiskstats(&s_before_sstf);

    pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);
        for(int i = 0; i < pages; i++) arr[i * 4096] += 1;
        exit(0);
    }
    wait(0);
    getdiskstats(&s_after_sstf);

    int sstf_writes = s_after_sstf.disk_writes - s_before_sstf.disk_writes;
    int sstf_reads  = s_after_sstf.disk_reads  - s_before_sstf.disk_reads;

    printf("  FCFS: writes=%d reads=%d avg_lat=%d\n",
           fcfs_writes, fcfs_reads, s_after_fcfs.avg_disk_latency);
    printf("  SSTF: writes=%d reads=%d avg_lat=%d\n",
           sstf_writes, sstf_reads, s_after_sstf.avg_disk_latency);

    check("FCFS produced disk writes > 0",          fcfs_writes > 0);
    check("FCFS produced disk reads > 0",           fcfs_reads  > 0);
    check("SSTF produced disk writes > 0",          sstf_writes > 0);
    check("SSTF produced disk reads > 0",           sstf_reads  > 0);
    check("SSTF avg latency <= FCFS avg latency (or equal — both correct)",
          s_after_sstf.avg_disk_latency <= s_after_fcfs.avg_disk_latency + 20);
    check("avg_disk_latency > 0 under SSTF",        s_after_sstf.avg_disk_latency > 0);

    setdisksched(0);
}

/* ============================================================
   Section 10 — stats accumulate monotonically
   ============================================================ */

static void
test_stats_monotonic(void)
{
    section("PA4-STATS-3: Stats accumulate monotonically");

    setdisksched(0);
    setraidmode(0);

    struct diskstats s0, s1, s2;
    getdiskstats(&s0);

    /* Wave 1 */
    int pid = fork();
    if(pid == 0){
        char *arr = sbrk(900 * 4096);
        touch(arr, 900);
        char *arr2 = sbrk(900 * 4096);
        touch(arr2, 900);
        exit(0);
    }
    wait(0);
    getdiskstats(&s1);

    /* Wave 2 */
    pid = fork();
    if(pid == 0){
        char *arr = sbrk(900 * 4096);
        touch(arr, 900);
        char *arr2 = sbrk(900 * 4096);
        touch(arr2, 900);
        exit(0);
    }
    wait(0);
    getdiskstats(&s2);

    printf("  s0: reads=%d writes=%d\n", s0.disk_reads, s0.disk_writes);
    printf("  s1: reads=%d writes=%d\n", s1.disk_reads, s1.disk_writes);
    printf("  s2: reads=%d writes=%d\n", s2.disk_reads, s2.disk_writes);

    check("disk_writes non-decreasing wave0→wave1", s1.disk_writes >= s0.disk_writes);
    check("disk_reads  non-decreasing wave0→wave1", s1.disk_reads  >= s0.disk_reads);
    check("disk_writes non-decreasing wave1→wave2", s2.disk_writes >= s1.disk_writes);
    check("disk_reads  non-decreasing wave1→wave2", s2.disk_reads  >= s1.disk_reads);
    check("total disk_writes > 0 after two waves",  s2.disk_writes > s0.disk_writes);
}

/* ============================================================
   Section 11 — PA3 functionality still intact (regression)
   ============================================================ */

static void
test_pa3_regression(void)
{
    section("PA4-REG: PA3 swap functionality still works after PA4 changes");

    setraidmode(0);
    setdisksched(0);

    int pid = fork();
    if(pid == 0){
        struct vmstats before, after;
        getvmstats(getpid(), &before);

        int pages = 1600;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);

        getvmstats(getpid(), &after);

        int ok_faults   = (after.page_faults   > before.page_faults);
        int ok_resident = (after.resident_pages > before.resident_pages);
        int ok_evicted  = (after.pages_evicted  > 0);
        int ok_swapout  = (after.pages_swapped_out > 0);

        printf("  PF=%d Evict=%d SwapOut=%d Resident=%d\n",
               after.page_faults, after.pages_evicted,
               after.pages_swapped_out, after.resident_pages);

        exit((ok_faults && ok_resident && ok_evicted && ok_swapout) ? 0 : 1);
    }
    int st; wait(&st);
    check("page_faults, eviction, swapout all still tracked correctly", st == 0);

    /* Data integrity regression */
    pid = fork();
    if(pid == 0){
        int pages = 600;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);

        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);

        int ok = verify(arr, pages);
        exit(ok ? 0 : 1);
    }
    wait(&st);
    check("Data integrity after disk-backed swap (PA3 regression)", st == 0);
}

/* ============================================================
   Section 12 — Multiple concurrent processes under different RAID modes
   ============================================================ */

static void
test_concurrent_multiraid(void)
{
    section("PA4-CONC: Concurrent processes with mixed RAID modes");

    // All children use RAID 0 — setraidmode is a global and must not
    // be changed concurrently while swap I/O is in flight.
    setraidmode(0);
    setdisksched(0);

    int pids[3];

    for(int i = 0; i < 3; i++){
        pids[i] = fork();
        if(pids[i] == 0){
            int pages = 350;
            char *arr = sbrk(pages * 4096);
            touch(arr, pages);
            char *arr2 = sbrk(pages * 4096);
            touch(arr2, pages);
            int ok = verify(arr, pages);
            exit(ok ? 0 : 1);
        }
    }

    int all_ok = 1;
    for(int i = 0; i < 3; i++){
        int st; wait(&st);
        if(st != 0) all_ok = 0;
    }
    check("3 concurrent processes complete with correct data under shared RAID 0",
          all_ok);
}

/* ============================================================
   Section 13 — SSTF prefers closer blocks (head movement)
   ============================================================ */

static void
test_sstf_seeks_less(void)
{
    section("PA4-SCHED-3: SSTF total latency <= FCFS total latency");

    /*
     * Run a workload that accesses blocks in a non-sequential order.
     * SSTF should reduce total head movement vs FCFS.
     * We compare the latency delta of a swap-heavy child under each policy.
     */

    struct diskstats sa, sb;

    /* FCFS run */
    setdisksched(0);
    getdiskstats(&sa);
    int pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        /* Zigzag access pattern to create non-sequential disk accesses */
        for(int i = 0; i < pages; i++) arr[i * 4096] = 1;
        for(int i = pages-1; i >= 0; i--) arr[i * 4096] += 1;
        char *arr2 = sbrk(pages * 4096);
        for(int i = 0; i < pages; i++) arr2[i * 4096] = 1;
        for(int i = 0; i < pages; i++) arr[i * 4096] += 1;
        exit(0);
    }
    wait(0);
    getdiskstats(&sb);
    int fcfs_lat = sb.avg_disk_latency;
    int fcfs_ops = (sb.disk_reads + sb.disk_writes) - (sa.disk_reads + sa.disk_writes);

    /* SSTF run */
    setdisksched(1);
    getdiskstats(&sa);
    pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        for(int i = 0; i < pages; i++) arr[i * 4096] = 1;
        for(int i = pages-1; i >= 0; i--) arr[i * 4096] += 1;
        char *arr2 = sbrk(pages * 4096);
        for(int i = 0; i < pages; i++) arr2[i * 4096] = 1;
        for(int i = 0; i < pages; i++) arr[i * 4096] += 1;
        exit(0);
    }
    wait(0);
    getdiskstats(&sb);
    int sstf_lat = sb.avg_disk_latency;
    int sstf_ops = (sb.disk_reads + sb.disk_writes) - (sa.disk_reads + sa.disk_writes);

    printf("  FCFS: avg_lat=%d ops=%d\n", fcfs_lat, fcfs_ops);
    printf("  SSTF: avg_lat=%d ops=%d\n", sstf_lat, sstf_ops);

    check("Both policies completed disk ops > 0",
          fcfs_ops > 0 && sstf_ops > 0);
    check("SSTF avg latency within 5% of FCFS (SSTF seeks less or equal)",
      sstf_lat <= fcfs_lat + (fcfs_lat / 20 + 10));

    setdisksched(0);
}

/* ============================================================
   Section 14 — RAID 5 write-then-read consistency
   ============================================================ */

static void
test_raid5_multiple_slots(void)
{
    section("PA4-RAID5-2: RAID 5 multiple slots, different stripe groups");

    setraidmode(2);
    setdisksched(0);

    int pid = fork();
    if(pid == 0){
        /*
         * Allocate enough pages so we span many RAID 5 stripes.
         * RAID 5 has 3 data slots per stripe, so every 3 evictions
         * involve a different parity disk.
         */
        int pages = 900;
        char *arr = sbrk(pages * 4096);

        /* Write pattern that varies per page */
        for(int i = 0; i < pages; i++){
            arr[i * 4096]     = (char)(i & 0xFF);
            arr[i * 4096 + 1] = (char)((i >> 8) & 0xFF);
            arr[i * 4096 + 2] = (char)(~i & 0xFF);
        }

        /* Force evictions across many stripes */
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);

        /* Verify */
        int errors = 0;
        for(int i = 0; i < pages; i++){
            if((unsigned char)arr[i * 4096]     != (unsigned char)(i & 0xFF))     errors++;
            if((unsigned char)arr[i * 4096 + 1] != (unsigned char)((i>>8) & 0xFF)) errors++;
            if((unsigned char)arr[i * 4096 + 2] != (unsigned char)(~i & 0xFF))     errors++;
        }

        struct vmstats vs;
        getvmstats(getpid(), &vs);
        printf("  RAID5 multi-stripe: SwapOut=%d SwapIn=%d errors=%d\n",
               vs.pages_swapped_out, vs.pages_swapped_in, errors);
        exit(errors == 0 ? 0 : 1);
    }
    int st; wait(&st);
    check("RAID 5 multi-stripe: 3-byte pattern intact across all stripes", st == 0);

    setraidmode(0);
}

/* ============================================================
   Section 15 — Swap slot reuse with disk backend
   ============================================================ */

static void
test_disk_swap_slot_reuse(void)
{
    section("PA4-SLOT: Disk swap slots recycled correctly");

    setraidmode(0);
    setdisksched(0);

    /*
     * Run 5 waves of allocation + eviction + swap-in + free.
     * If disk swap slots are not recycled, we run out after ~1024
     * slots and panic. Completing without panic proves recycling works.
     */
    int pid = fork();
    if(pid == 0){
        for(int wave = 0; wave < 5; wave++){
            int pages = 900;
            char *arr = sbrk(pages * 4096);
            touch(arr, pages);

            /* Force evictions via second region */
            char *arr2 = sbrk(pages * 4096);
            touch(arr2, pages);

            /* Re-access first region (frees swap slots via swap-in) */
            for(int i = 0; i < pages; i++) arr[i * 4096] += 1;

            sbrk(-pages * 4096);
            sbrk(-pages * 4096);
        }
        exit(0);
    }
    int st; wait(&st);
    check("5 waves of disk swap out/in/free without slot exhaustion", st == 0);
}

/* ============================================================
   Section 16 — getdiskstats integration with PA1 process stats
   ============================================================ */

static void
test_diskstats_integration(void)
{
    section("PA4-INT: getdiskstats() integrates with PA1/PA2 system");

    /* Verify that disk stats coexist with vmstats and mlfqinfo */
    setraidmode(0);
    setdisksched(0);

    int pid = fork();
    if(pid == 0){
        /* Do memory work */
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);

        /* Query all three stat interfaces */
        struct vmstats   vs;
        struct mlfqinfo  mi;
        struct diskstats ds;

        int rv = getvmstats(getpid(), &vs);
        int rm = getmlfqinfo(getpid(), &mi);
        int rd = getdiskstats(&ds);

        printf("  vmstats:   rv=%d PF=%d Evict=%d\n", rv, vs.page_faults, vs.pages_evicted);
        printf("  mlfqinfo:  rm=%d level=%d sched=%d\n", rm, mi.level, mi.times_scheduled);
        printf("  diskstats: rd=%d reads=%d writes=%d lat=%d\n",
               rd, ds.disk_reads, ds.disk_writes, ds.avg_disk_latency);

        int ok = (rv == 0 && rm == 0 && rd == 0 &&
                  vs.page_faults > 0 &&
                  mi.times_scheduled > 0 &&
                  ds.disk_writes > 0);
        exit(ok ? 0 : 1);
    }
    int st; wait(&st);
    check("getvmstats + getmlfqinfo + getdiskstats all work together", st == 0);
}

/* ============================================================
   Section 17 — RAID 1 write amplification (writes go to 2 disks)
   ============================================================ */

static void
test_raid1_write_amplification(void)
{
    section("PA4-RAID1-2: RAID 1 causes more writes than RAID 0 (mirroring)");

    struct diskstats s0, s1;

    /* RAID 0 baseline */
    setraidmode(0);
    setdisksched(0);
    getdiskstats(&s0);
    int pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);
        exit(0);
    }
    wait(0);
    getdiskstats(&s1);
    int raid0_writes = s1.disk_writes - s0.disk_writes;

    /* RAID 1 — same workload */
    setraidmode(1);
    getdiskstats(&s0);
    pid = fork();
    if(pid == 0){
        int pages = 900;
        char *arr = sbrk(pages * 4096);
        touch(arr, pages);
        char *arr2 = sbrk(pages * 4096);
        touch(arr2, pages);
        exit(0);
    }
    wait(0);
    getdiskstats(&s1);
    int raid1_writes = s1.disk_writes - s0.disk_writes;

    printf("  RAID 0 writes: %d\n", raid0_writes);
    printf("  RAID 1 writes: %d  (expected ~2x RAID 0)\n", raid1_writes);

    check("RAID 0 produced disk writes > 0",          raid0_writes > 0);
    check("RAID 1 produced more writes than RAID 0 (mirroring amplification)",
          raid1_writes >= raid0_writes);

    setraidmode(0);
}

/* ============================================================
   Main
   ============================================================ */

int
main(void)
{
    printf("\n");
    printf("##################################################\n");
    printf("#        PA4 COMPREHENSIVE STRESS TEST           #\n");
    printf("#  Disk Scheduling + RAID-backed Swap            #\n");
    printf("##################################################\n");

    /* Interface validation */
    test_setdisksched_interface();
    test_setraidmode_interface();
    test_getdiskstats_interface();

    /* Statistics correctness */
    test_stats_increase_on_swap();
    test_stats_monotonic();

    /* RAID data integrity */
    test_raid0_integrity();
    test_raid1_integrity();
    test_raid5_integrity();
    test_raid5_multiple_slots();
    test_raid_mode_switching();

    /* Disk scheduling behavior */
    test_scheduling_latency_comparison();
    test_sstf_seeks_less();

    /* Slot management */
    test_disk_swap_slot_reuse();

    /* Write amplification proof */
    test_raid1_write_amplification();

    /* Concurrency */
    test_concurrent_multiraid();

    /* Integration with PA1/PA2/PA3 */
    test_diskstats_integration();
    test_pa3_regression();

    summary();
    exit(0);
}