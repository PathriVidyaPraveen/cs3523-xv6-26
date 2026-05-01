#include "kernel/types.h"
#include "user/user.h"

// struct mlfqinfo{
//   int level;           // Current queue level
//   int ticks[4];        // Total ticks consumed at each level
//   int times_scheduled; // Number of times the process has been scheduled
//   int total_syscalls;  // Total system calls made
// };

void print_stats(int pid) {
    struct mlfqinfo info;
    if (getmlfqinfo(pid, &info) == 0) {
        printf("PID %d | Level: %d | Ticks: [L0:%d, L1:%d, L2:%d, L3:%d] | Scheduled: %d | Syscalls: %d\n", 
               pid, info.level, info.ticks[0], info.ticks[1], 
               info.ticks[2], info.ticks[3], info.times_scheduled, info.total_syscalls);
    }
}

int main() {
    int throw;
    printf("=== MLFQ Comprehensive SC-MLFQ Stress Test with Boost Verification ===\n\n");

    // TEST 1: CPU-Bound Process - Demotion and Boost Verification
    printf("\n--- TEST 1: CPU-Bound Process (Demotion & Boost) ---\n");
    printf("Expected: L0 -> L1 -> L2 -> L3 (demotion), then back to L0 (boost)\n\n");
    
    if (fork() == 0) {
        int pid = getpid();
        printf("[Child 1] CPU-Bound process started (PID: %d)\n", pid);
        
        // Phase 1: Initial CPU burst - should demote from L0 to L1
        printf("[Child 1] Phase 1: Heavy CPU workload\n");
        for (volatile int i = 0; i < 500000000; i++);
        print_stats(pid);
        
        // Phase 2: Continue CPU - should demote to L2
        printf("[Child 1] Phase 2: More CPU workload\n");
        for (volatile int i = 0; i < 500000000; i++);
        print_stats(pid);
        
        // Phase 3: More CPU - should reach L3
        printf("[Child 1] Phase 3: Even more CPU workload\n");
        for (volatile int i = 0; i < 500000000; i++);
        print_stats(pid);
        
        // Phase 4: More CPU burst
        printf("[Child 1] Phase 4: Final CPU burst (boost should happen)\n");
        for (volatile int i = 0; i < 500000000; i++);
        print_stats(pid);

        // Phase 5: Final CPU burst [boost should happen here]
        printf("[Child 1] Phase 5: Final CPU burst (boost should happen)\n");
        for (volatile int i = 0; i < 500000000; i++);
        print_stats(pid);
        
        printf("[Child 1] Finished. Final Level: %d\n", getlevel());
        exit(0);
    }
    wait(&throw);

    // TEST 2: Syscall-Heavy (Interactive) Process - Should Stay at High Priority
    printf("\n--- TEST 2: Interactive Process (High Syscall Rate) ---\n");
    printf("Expected: Should remain at L0/L1 due to frequent syscalls\n\n");
    
    if (fork() == 0) {
        int pid = getpid();
        printf("[Child 2] Interactive process started (PID: %d)\n", pid);
        
        for (int i = 0; i < 5000; i++) {
            getpid(); // High frequency syscalls
            if (i % 1000 == 0 && i > 0) {
                printf("[Child 2] After %d syscalls:\n", i);
                print_stats(pid);
            }
        }
        printf("[Child 2] Finished. Final Level: %d\n", getlevel());
        print_stats(pid);
        exit(0);
    }
    wait(&throw);

    // TEST 3: Mixed Workload - CPU & Syscalls
    printf("\n--- TEST 3: Mixed Workload (CPU + Syscalls) ---\n");
    printf("Expected: Should stay at lower levels due to frequent syscalls during CPU phases\n\n");
    
    if (fork() == 0) {
        int pid = getpid();
        printf("[Child 3] Mixed workload process started (PID: %d)\n", pid);
        
        for (int phase = 0; phase < 5; phase++) {
            printf("[Child 3] Phase %d - CPU burst:\n", phase + 1);
            
            // CPU Phase
            for (volatile int i = 0; i < 300000000; i++);
            
            // Syscall Phase
            for (int i = 0; i < 100; i++) {
                getpid();
            }
            
            print_stats(pid);
        }
        printf("[Child 3] Finished. Final Level: %d\n", getlevel());
        exit(0);
    }
    wait(&throw);

    // TEST 4: Multi-Process Boost Test - Shows boost benefits
    printf("\n--- TEST 4: Multi-Process Boost Verification ---\n");
    printf("Expected: CPU hog demotes, then boost brings it back; interactive stays high priority\n\n");
    
    // CPU-bound hog
    if (fork() == 0) {
        int pid = getpid();
        printf("[Hog] CPU hog started (PID: %d)\n", pid);
        for (volatile int i = 0; i < 500000000; i++);
        printf("[Hog] After first burst:\n");
        print_stats(pid);
        for (volatile int i = 0; i < 500000000; i++);
        printf("[Hog] After second burst (boost should occur):\n");
        print_stats(pid);
        exit(0);
    }

    // Interactive process running alongside hog
    if (fork() == 0) {
        int pid = getpid();
        printf("[Interactive] Started alongside hog (PID: %d)\n", pid);
        pause(1);  // Wait for hog to demote
        
        for (int i = 0; i < 500; i++) {
            getpid();  // Quick syscall
            if (i % 100 == 0 && i > 0) {
                printf("[Interactive] After %d syscalls:\n", i);
                print_stats(pid);
            }
        }
        printf("[Interactive] Finished - should still be at high priority (L0/L1)\n");
        print_stats(pid);
        exit(0);
    }

    wait(&throw);
    wait(&throw);
}