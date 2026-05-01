// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0 
// 10001000 -- virtio disk 
// 80000000 -- qemu's boot ROM loads the kernel here,
//             then jumps here.
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0 0x10000000L
#define UART0_IRQ 10

// virtio mmio interface
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

// qemu puts platform-level interrupt controller (PLIC) here.
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80000000L
#define PHYSTOP (KERNBASE + 6*1024*1024)

// ---- PA4: disk-backed swap layout ----
// The real disk layout is:
//   blocks [0 .. SWAP_FS_SIZE-1]       : xv6 filesystem  (untouched)
//   blocks [SWAP_FS_SIZE .. end]       : 4 simulated disk regions
//
// Simulated disk d, position p  ->  physical block = SWAP_FS_SIZE + d*SWAP_SIM_DISK_BLOCKS + p

#define SWAP_FS_SIZE          2000   // filesystem occupies blocks 0..1999
#define SWAP_NDISKS           4      // number of simulated disks
#define SWAP_MAX_SLOTS        4096   // maximum number of swap slots
#define SWAP_BLOCKS_PER_SLOT  4      // PGSIZE/BSIZE = 4096/1024
#define SWAP_SIM_DISK_BLOCKS  4096   // blocks allocated per simulated disk region
#define SWAP_ROTATIONAL_DELAY 10     // constant latency added to every request

// RAID modes
#define RAID_MODE_0  0   // striping  — slot s → disk s%4, pos (s/4)*SWAP_BLOCKS_PER_SLOT
#define RAID_MODE_1  1   // mirroring — write to disk 0 and disk 1
#define RAID_MODE_5  2   // distributed parity — 3 data + 1 parity per stripe of 4

// Disk scheduling policies
#define DISK_SCHED_FCFS  0
#define DISK_SCHED_SSTF  1

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
