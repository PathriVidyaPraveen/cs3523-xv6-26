// kernel/swap_disk.c
//
// Disk-backed swap replacing the in-memory swap from PA3.
//
// Physical disk layout (all on ROOTDEV = 1):
//   [0 .. 1999]         : xv6 filesystem (never touched here)
//   [2000 .. 18383]     : 4 simulated disk regions, each 4096 blocks
//
// Public API (called from kalloc.c, vm.c, trap.c):
//   swap_disk_init()          — called once from kinit()
//   disk_swap_out(pa)         — evict page at pa to disk; returns slot or -1
//   disk_swap_in(pa, slot)    — restore page from disk; frees slot
//   disk_swap_read(pa, slot)  — restore page from disk; does NOT free slot (for fork)
//   disk_swap_free(slot)      — free a slot without reading (for uvmunmap)
//   setdisksched_impl(policy) — switch FCFS/SSTF at runtime
//   setraidmode_impl(mode)    — switch RAID mode at runtime
//   get_disk_reads/writes/latency() — for stats syscall

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"
#include "fs.h"
#include "buf.h"

// ----------------------------------------------------------------
// Global state
// ----------------------------------------------------------------

static int swap_used[SWAP_MAX_SLOTS];
static struct spinlock swap_bitmap_lock;

static int disk_sched_policy = DISK_SCHED_FCFS;
static int current_head      = 0;
static int total_disk_reads  = 0;
static int total_disk_writes = 0;
static int total_disk_latency = 0;
static struct spinlock disk_sched_lock;

static int raid_mode = RAID_MODE_0;

// RAID 5 needs temporary page-sized buffers for parity computation.
// Protected by a sleeplock so we can call bread() while holding it.
static struct sleeplock raid5_lock;
static char raid5_old_data  [PGSIZE];
static char raid5_new_parity[PGSIZE];

// int  get_disk_reads(void)   { __sync_synchronize(); return total_disk_reads;   }
// int  get_disk_writes(void)  { __sync_synchronize(); return total_disk_writes;  }
// int  get_disk_latency(void) { __sync_synchronize(); return total_disk_latency; }

// ----------------------------------------------------------------
// Initialisation — call from kinit()
// ----------------------------------------------------------------

void
swap_disk_init(void)
{
    initlock(&swap_bitmap_lock, "swap_bitmap");
    initlock(&disk_sched_lock,  "disk_sched");
    initsleeplock(&raid5_lock,  "raid5");

    for(int i = 0; i < SWAP_MAX_SLOTS; i++)
        swap_used[i] = 0;
}

// ----------------------------------------------------------------
// Physical block address helpers
// ----------------------------------------------------------------

// Block on real disk corresponding to simulated disk d, position pos.
static uint
phys_block(int disk, int pos)
{
    return SWAP_FS_SIZE + disk * SWAP_SIM_DISK_BLOCKS + pos;
}

static int
my_abs(int x) { return x < 0 ? -x : x; }

// ----------------------------------------------------------------
// Swap slot allocation
// ----------------------------------------------------------------

static int
alloc_slot(void)
{
    acquire(&swap_bitmap_lock);
    for(int i = 0; i < SWAP_MAX_SLOTS; i++){
        if(!swap_used[i]){
            swap_used[i] = 1;
            release(&swap_bitmap_lock);
            return i;
        }
    }
    release(&swap_bitmap_lock);
    return -1;
}

void
disk_swap_free(int slot)
{
    if(slot < 0 || slot >= SWAP_MAX_SLOTS) return;
    acquire(&swap_bitmap_lock);
    swap_used[slot] = 0;
    release(&swap_bitmap_lock);
}

// ----------------------------------------------------------------
// Disk scheduling — block-level request batch
// ----------------------------------------------------------------

// One block-level request (BSIZE = 1024 bytes).
struct block_req {
    uint blockno;
    char *data;    // pointer into caller's page buffer
    int  write;
    int  level;    // requesting process's MLFQ level (lower = higher priority)
};

// Execute a batch of block requests according to the current scheduling policy.
// Must NOT hold any spinlock on entry (bread/bwrite use sleeplocks internally).
static void
execute_block_reqs(struct block_req *reqs, int n)
{
    int order[16];
    for(int i = 0; i < n; i++) order[i] = i;

    acquire(&disk_sched_lock);

    if(disk_sched_policy == DISK_SCHED_SSTF){
        for(int i = 0; i < n - 1; i++){
            int best = i;
            int best_dist  = my_abs((int)reqs[order[i]].blockno - current_head);
            int best_level = reqs[order[i]].level;
            for(int j = i + 1; j < n; j++){
                int dist  = my_abs((int)reqs[order[j]].blockno - current_head);
                int level = reqs[order[j]].level;
                if(dist < best_dist || (dist == best_dist && level < best_level)){
                    best = j; best_dist = dist; best_level = level;
                }
            }
            if(best != i){ int tmp = order[i]; order[i] = order[best]; order[best] = tmp; }
        }
    }

    for(int i = 0; i < n; i++){
        int bi = order[i];
        int lat = my_abs((int)reqs[bi].blockno - current_head) + SWAP_ROTATIONAL_DELAY;
        current_head = (int)reqs[bi].blockno;
        total_disk_latency += lat;
        if(reqs[bi].write) total_disk_writes++;
        else               total_disk_reads++;
    }

    __sync_synchronize();   // ensure counter writes are visible to all harts before release

    release(&disk_sched_lock);

    for(int i = 0; i < n; i++){
        int bi = order[i];
        struct buf *b = bread(ROOTDEV, reqs[bi].blockno);
        if(reqs[bi].write){
            memmove(b->data, reqs[bi].data, BSIZE);
            bwrite(b);
        } else {
            memmove(reqs[bi].data, b->data, BSIZE);
        }
        brelse(b);
    }
}

// ----------------------------------------------------------------
// RAID 0 — striping across NDISKS disks
// slot s → disk = s % NDISKS, position = (s / NDISKS) * SWAP_BLOCKS_PER_SLOT
// ----------------------------------------------------------------

static void
raid0_write(int slot, char *page)
{
    int disk = slot % SWAP_NDISKS;
    int pos  = (slot / SWAP_NDISKS) * SWAP_BLOCKS_PER_SLOT;
    int lvl  = myproc() ? myproc()->level : 3;

    struct block_req reqs[SWAP_BLOCKS_PER_SLOT];
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        reqs[b].blockno = phys_block(disk, pos + b);
        reqs[b].data    = page + b * BSIZE;
        reqs[b].write   = 1;
        reqs[b].level   = lvl;
    }
    execute_block_reqs(reqs, SWAP_BLOCKS_PER_SLOT);
}

static void
raid0_read(int slot, char *page)
{
    int disk = slot % SWAP_NDISKS;
    int pos  = (slot / SWAP_NDISKS) * SWAP_BLOCKS_PER_SLOT;
    int lvl  = myproc() ? myproc()->level : 3;

    struct block_req reqs[SWAP_BLOCKS_PER_SLOT];
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        reqs[b].blockno = phys_block(disk, pos + b);
        reqs[b].data    = page + b * BSIZE;
        reqs[b].write   = 0;
        reqs[b].level   = lvl;
    }
    execute_block_reqs(reqs, SWAP_BLOCKS_PER_SLOT);
}

// ----------------------------------------------------------------
// RAID 1 — mirroring onto disk 0 and disk 1
// slot s → position = s * SWAP_BLOCKS_PER_SLOT on each mirror disk
// ----------------------------------------------------------------

static void
raid1_write(int slot, char *page)
{
    int pos = slot * SWAP_BLOCKS_PER_SLOT;
    int lvl = myproc() ? myproc()->level : 3;

    // 2 mirrors × SWAP_BLOCKS_PER_SLOT blocks = 8 requests
    struct block_req reqs[2 * SWAP_BLOCKS_PER_SLOT];
    int n = 0;
    for(int d = 0; d < 2; d++){
        for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
            reqs[n].blockno = phys_block(d, pos + b);
            reqs[n].data    = page + b * BSIZE;
            reqs[n].write   = 1;
            reqs[n].level   = lvl;
            n++;
        }
    }
    execute_block_reqs(reqs, n);
}

static void
raid1_read(int slot, char *page)
{
    // Read from primary mirror (disk 0).
    int pos = slot * SWAP_BLOCKS_PER_SLOT;
    int lvl = myproc() ? myproc()->level : 3;

    struct block_req reqs[SWAP_BLOCKS_PER_SLOT];
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        reqs[b].blockno = phys_block(0, pos + b);
        reqs[b].data    = page + b * BSIZE;
        reqs[b].write   = 0;
        reqs[b].level   = lvl;
    }
    execute_block_reqs(reqs, SWAP_BLOCKS_PER_SLOT);
}

// ----------------------------------------------------------------
// RAID 5 — distributed parity (3 data + 1 parity per stripe of 4)
//
// stripe  = slot / (SWAP_NDISKS - 1)   i.e. slot / 3
// data_pos = slot % (SWAP_NDISKS - 1)  i.e. slot % 3  (0, 1, or 2)
// parity_disk = stripe % SWAP_NDISKS
// data_disk   = data_pos-th disk skipping parity_disk
//
// Physical position on any disk for this stripe: stripe * SWAP_BLOCKS_PER_SLOT
// ----------------------------------------------------------------

static void
raid5_map(int slot, int *data_disk_out, int *parity_disk_out, int *stripe_out)
{
    int stripe      = slot / (SWAP_NDISKS - 1);
    int data_pos    = slot % (SWAP_NDISKS - 1);
    int parity_disk = stripe % SWAP_NDISKS;

    int count = 0, data_disk = 0;
    for(int d = 0; d < SWAP_NDISKS; d++){
        if(d == parity_disk) continue;
        if(count == data_pos){ data_disk = d; break; }
        count++;
    }

    *data_disk_out   = data_disk;
    *parity_disk_out = parity_disk;
    *stripe_out      = stripe;
}

static void
raid5_write(int slot, char *new_data)
{
    int data_disk, parity_disk, stripe;
    raid5_map(slot, &data_disk, &parity_disk, &stripe);
    int pos = stripe * SWAP_BLOCKS_PER_SLOT;

    // Exclusive access to global temp buffers.
    // sleeplock allows bread() calls inside.
    acquiresleep(&raid5_lock);

    // Read old data for differential parity update.
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        struct buf *buf = bread(ROOTDEV, phys_block(data_disk, pos + b));
        memmove(raid5_old_data + b * BSIZE, buf->data, BSIZE);
        brelse(buf);
    }

    // Read old parity into raid5_new_parity.
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        struct buf *buf = bread(ROOTDEV, phys_block(parity_disk, pos + b));
        memmove(raid5_new_parity + b * BSIZE, buf->data, BSIZE);
        brelse(buf);
    }

    // new_parity = old_parity XOR old_data XOR new_data
    for(int i = 0; i < PGSIZE; i++)
        raid5_new_parity[i] ^= raid5_old_data[i] ^ new_data[i];

    // Write new_data and new_parity through the scheduler.
    struct block_req reqs[2 * SWAP_BLOCKS_PER_SLOT];
    int lvl = myproc() ? myproc()->level : 3;
    int n = 0;
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        reqs[n].blockno = phys_block(data_disk, pos + b);
        reqs[n].data    = new_data + b * BSIZE;
        reqs[n].write   = 1;
        reqs[n].level   = lvl;
        n++;
    }
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        reqs[n].blockno = phys_block(parity_disk, pos + b);
        reqs[n].data    = raid5_new_parity + b * BSIZE;
        reqs[n].write   = 1;
        reqs[n].level   = lvl;
        n++;
    }
    execute_block_reqs(reqs, n);

    releasesleep(&raid5_lock);
}

static void
raid5_read(int slot, char *page)
{
    int data_disk, parity_disk, stripe;
    raid5_map(slot, &data_disk, &parity_disk, &stripe);
    (void)parity_disk;
    int pos = stripe * SWAP_BLOCKS_PER_SLOT;
    int lvl = myproc() ? myproc()->level : 3;

    struct block_req reqs[SWAP_BLOCKS_PER_SLOT];
    for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
        reqs[b].blockno = phys_block(data_disk, pos + b);
        reqs[b].data    = page + b * BSIZE;
        reqs[b].write   = 0;
        reqs[b].level   = lvl;
    }
    execute_block_reqs(reqs, SWAP_BLOCKS_PER_SLOT);
}

// Reconstruct a slot's data when its data disk is unavailable:
// XOR all other disks (the 2 remaining data disks + parity disk) together.
void
raid5_reconstruct(int slot, char *page)
{
    int data_disk, parity_disk, stripe;
    raid5_map(slot, &data_disk, &parity_disk, &stripe);
    int pos = stripe * SWAP_BLOCKS_PER_SLOT;

    acquiresleep(&raid5_lock);
    memset(page, 0, PGSIZE);

    for(int d = 0; d < SWAP_NDISKS; d++){
        if(d == data_disk) continue;   // skip the "failed" disk
        for(int b = 0; b < SWAP_BLOCKS_PER_SLOT; b++){
            struct buf *buf = bread(ROOTDEV, phys_block(d, pos + b));
            for(int i = 0; i < BSIZE; i++)
                page[b * BSIZE + i] ^= buf->data[i];
            brelse(buf);
        }
    }

    releasesleep(&raid5_lock);
}

// ----------------------------------------------------------------
// Public swap interface
// ----------------------------------------------------------------

// Evict page at physical address pa to disk.
// Returns swap slot index (stored in PTE), or -1 if swap is full.
int
disk_swap_out(uint64 pa)
{
    int slot = alloc_slot();
    if(slot < 0) return -1;

    char *page = (char *)pa;
    switch(raid_mode){
    case RAID_MODE_0: raid0_write(slot, page); break;
    case RAID_MODE_1: raid1_write(slot, page); break;
    case RAID_MODE_5: raid5_write(slot, page); break;
    default:          raid0_write(slot, page); break;
    }

    struct proc *p = myproc();
    if(p) p->disk_writes++;

    return slot;
}

// Read slot back into memory at pa; frees the swap slot.
void
disk_swap_in(uint64 pa, int slot)
{
    char *page = (char *)pa;
    switch(raid_mode){
    case RAID_MODE_0: raid0_read(slot, page); break;
    case RAID_MODE_1: raid1_read(slot, page); break;
    case RAID_MODE_5: raid5_read(slot, page); break;
    default:          raid0_read(slot, page); break;
    }
    disk_swap_free(slot);

    struct proc *p = myproc();
    if(p) p->disk_reads++;
}

// Read slot into page WITHOUT freeing the slot.
// Used by fork (uvmcopy) so the parent keeps its swap slot.
void
disk_swap_read(uint64 pa, int slot)
{
    char *page = (char *)pa;
    switch(raid_mode){
    case RAID_MODE_0: raid0_read(slot, page); break;
    case RAID_MODE_1: raid1_read(slot, page); break;
    case RAID_MODE_5: raid5_read(slot, page); break;
    default:          raid0_read(slot, page); break;
    }

    struct proc *p = myproc();
    if(p) p->disk_reads++;
}

// ----------------------------------------------------------------
// Control syscall helpers
// ----------------------------------------------------------------

int
setdisksched_impl(int policy)
{
    if(policy != DISK_SCHED_FCFS && policy != DISK_SCHED_SSTF) return -1;
    acquire(&disk_sched_lock);
    disk_sched_policy = policy;
    release(&disk_sched_lock);
    return 0;
}

int
setraidmode_impl(int mode)
{
    if(mode != RAID_MODE_0 && mode != RAID_MODE_1 && mode != RAID_MODE_5) return -1;
    raid_mode = mode;
    return 0;
}

// ----------------------------------------------------------------
// Statistics accessors
// ----------------------------------------------------------------

int  get_disk_reads(void)   { __sync_synchronize(); return total_disk_reads;   }
int  get_disk_writes(void)  { __sync_synchronize(); return total_disk_writes;  }
int  get_disk_latency(void) { __sync_synchronize(); return total_disk_latency; }