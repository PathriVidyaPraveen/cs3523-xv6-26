#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "frame.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from, and returns to, trampoline.S
// return value is user satp for trampoline.S to switch to.
//
uint64
usertrap(void)
{
  int which_dev = 0;

  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);  //DOC: kernelvec

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();
  
  if(r_scause() == 8){
    // system call

    if(killed(p))
      kexit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sepc, scause, and sstatus,
    // so enable only now that we're done with those registers.
    intr_on();

    syscall();
  } 
  else if((which_dev = devintr()) != 0){
    // ok
  } 
  else if(r_scause() == 12 || r_scause() == 13 || r_scause() == 15) { // THIS WAS MISSING!
    uint64 va = PGROUNDDOWN(r_stval());

    if(va >= p->sz){
      setkilled(p);
    } else {
      pte_t *pte = walk(p->pagetable, va, 0);

      // 1. Page is Mapped and Valid 
      if(pte && (*pte & PTE_V)){
        if(r_scause() == 15 && ((*pte & PTE_W) == 0)){
          setkilled(p); // True permission fault
        } else {
          // It trapped because the Clock algorithm cleared PTE_A.
          // Restore the hardware Accessed/Dirty bits so it doesn't fault again.
          *pte |= (1L << 6); // Set PTE_A
          if(r_scause() == 15) *pte |= (1L << 7); // Set PTE_D

          // Update reference bit in the frame table
          acquire(&frame_table_lock);
          int fidx = (PTE2PA(*pte) - KERNBASE) / PGSIZE;
          ft[fidx].ref_bit = 1;
          release(&frame_table_lock);
        }
      } 
      // 2. Page is Swapped Out
      else if(pte && (*pte & PTE_S)){
        int idx = GET_SWAP_IDX(*pte);
        char *mem = kalloc();
        if(mem == 0){
          setkilled(p);
        } else {
          disk_swap_in((uint64)mem, idx);
          
          // Restore PTE
          *pte = ((*pte & 0x3FF) & ~PTE_S) | PTE_V | PA2PTE(mem);
          
          // Set A/D bits immediately to prevent instant re-fault
          *pte |= (1L << 6); // Set PTE_A
          if(r_scause() == 15) *pte |= (1L << 7); // Set PTE_D
          
          sfence_vma();

          acquire(&frame_table_lock);
          int fidx = ((uint64)mem - KERNBASE) / PGSIZE;
          ft[fidx].in_use = 1;
          ft[fidx].owner = p;
          ft[fidx].va = va;
          ft[fidx].ref_bit = 1;
          release(&frame_table_lock);

          p->page_faults++;
          p->pages_swapped_in++;
          p->resident_pages++;
        }
      } 
      // 3. Lazy Allocation
      else {
        char *mem = kalloc();
        if(mem == 0){
          setkilled(p);
        } else {
          memset(mem, 0, PGSIZE);
          if(mappages(p->pagetable, va, PGSIZE, (uint64)mem, PTE_R | PTE_W | PTE_U) != 0){
            kfree(mem);
            setkilled(p);
          } else {
            // Set A/D bits immediately to prevent instant re-fault
            pte_t *new_pte = walk(p->pagetable, va, 0);
            if(new_pte){
                *new_pte |= (1L << 6); // Set PTE_A
                if(r_scause() == 15) *new_pte |= (1L << 7); // Set PTE_D
            }

            acquire(&frame_table_lock);
            int fidx = ((uint64)mem - KERNBASE) / PGSIZE;
            ft[fidx].in_use = 1;
            ft[fidx].owner = p;
            ft[fidx].va = va;
            ft[fidx].ref_bit = 1;
            release(&frame_table_lock);

            p->page_faults++;
            p->resident_pages++;
          }
        }
      }
    }
  } 
  else {
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2) {
    // p is already defined at the top of usertrap() — do NOT redeclare it
    if(p != 0){
      acquire(&p->lock);
      p->ticks_in_level++;
      p->ticks_total[p->level]++;
      release(&p->lock);
    }

    global_ticks++;
    if(global_ticks % BOOST_INTERVAL == 0)
      mlfq_boost();

    if(p != 0){
      acquire(&p->lock);
      if(p->ticks_in_level >= quantum[p->level]){
        int deltaT = p->ticks_in_level;
        int deltaS = p->syscount - p->slice_start_syscount;
        if(deltaS < deltaT && p->level < NQUEUE - 1)
          p->level++;
        p->ticks_in_level = 0;
        release(&p->lock);
        yield();
      } else {
        release(&p->lock);
      }
    }
  }
  prepare_return();

  // the user page table to switch to, for trampoline.S
  uint64 satp = MAKE_SATP(p->pagetable);

  // return to trampoline.S; satp value in a0.
  return satp;
}

//
// set up trapframe and control registers for a return to user space
//
void
prepare_return(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(). because a trap from kernel
  // code to usertrap would be a disaster, turn off interrupts.
  intr_off();

  // send syscalls, interrupts, and exceptions to uservec in trampoline.S
  uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
  w_stvec(trampoline_uservec);

  // set up trapframe values that uservec will need when
  // the process next traps into the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  
  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  if((which_dev = devintr()) == 0){
    // interrupt or trap from an unknown source
    printf("scause=0x%lx sepc=0x%lx stval=0x%lx\n", scause, r_sepc(), r_stval());
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2 && myproc() != 0){
    struct proc *p = myproc();
    acquire(&p->lock);
    p->ticks_in_level++;
    p->ticks_total[p->level]++;
    release(&p->lock);
    global_ticks++;
    if(global_ticks % BOOST_INTERVAL == 0){
      mlfq_boost();
    }
    acquire(&p->lock);
    if(p->ticks_in_level >= quantum[p->level]){
      int deltaT = p->ticks_in_level;
      int deltaS = p->syscount - p->slice_start_syscount;

      if(deltaS < deltaT && p->level < NQUEUE - 1) {
        p->level++;
      }

      p->ticks_in_level = 0;

      release(&p->lock);
      yield();
    }else{
      release(&p->lock);
    }
}

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  if(cpuid() == 0){
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // ask for the next timer interrupt. this also clears
  // the interrupt request. 1000000 is about a tenth
  // of a second.
  w_stimecmp(r_time() + 1000000);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if(scause == 0x8000000000000009L){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000005L){
    // timer interrupt.
    clockintr();
    return 2;
  } else {
    return 0;
  }
}

