#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

extern struct spinlock wait_lock;
extern struct proc proc[NPROC];

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  int n;
  struct proc *p = myproc();

  argint(0, &n);

  uint64 addr = p->sz;

  if(n < 0){
    if(growproc(n) < 0)
      return -1;
  } else {
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;

    p->sz += n;   // LAZY
  }

  return addr;
}
// sys_sbrk(void)
// {
//   uint64 addr;
//   int t;
//   int n;

//   argint(0, &n);
//   argint(1, &t);
//   addr = myproc()->sz;

//   if(t == SBRK_EAGER || n < 0) {
//     if(growproc(n) < 0) {
//       return -1;
//     }
//   } else {
//     // Lazily allocate memory for this process: increase its memory
//     // size but don't allocate memory. If the processes uses the
//     // memory, vmfault() will allocate it.
//     if(addr + n < addr)
//       return -1;
//     if(addr + n > TRAPFRAME)
//       return -1;
//     myproc()->sz += n;
//   }
//   return addr;
// }

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_hello(void){
  printf("Hello from the kernel!\n");
  return 0;
}

uint64 sys_getpid2(void){
  return myproc()->pid;
}

uint64 sys_getppid(void){
  struct proc *p = myproc();
  int ppid = -1;
  acquire(&wait_lock);
  if(p->parent){
    ppid = p->parent->pid;
  }
  release(&wait_lock);

  return ppid;
}

uint64 sys_getnumchild(void){
  struct proc *p = myproc();
  int count = 0;

  acquire(&wait_lock);

  for(struct proc *q = proc; q< &proc[NPROC];q++ ){
    if(q->parent == p && q->state != ZOMBIE){
      count++;
    }
  }

  release(&wait_lock);

  return count;
}

uint64 sys_getsyscount(void){
  return myproc()->syscount;
}

uint64 sys_getchildsyscount(void){
  int pid;
  argint(0,&pid);
  struct proc *p = myproc();
  acquire(&wait_lock);
  for(struct proc *q = proc;q<&proc[NPROC];q++){
    if(q->pid == pid && q->parent == p){
      int count = q->syscount;
      release(&wait_lock);
      return count;
    }
  }

  release(&wait_lock);
  return -1;
}

uint64
sys_getlevel(void)
{
  struct proc *p = myproc();
  int level;
  acquire(&p->lock);
  level = p->level;
  release(&p->lock);
  return level;
}

uint64
sys_getmlfqinfo(void)
{
  int pid;
  uint64 addr;
  argint(0, &pid);
  argaddr(1, &addr);
  struct proc *p;
  for(p = proc;p < &proc[NPROC];p++) {
    acquire(&p->lock);
    if(p->pid == pid && p->state != UNUSED){
      struct mlfqinfo info;
      info.level = p->level;
      for(int i = 0; i < NQUEUE; i++)
        info.ticks[i] = p->ticks_total[i];
      info.times_scheduled = p->times_scheduled;
      info.total_syscalls = p->syscount;
      release(&p->lock);
      if(copyout(myproc()->pagetable,addr, (char *)&info,sizeof(info)) < 0)
        return -1;

      return 0;
    }
    release(&p->lock);
  }

  return -1;
}


uint64
sys_getvmstats(void)
{
  int pid;
  uint64 addr;

  argint(0, &pid);
  argaddr(1, &addr);

  return getvmstats_helper(pid, addr);
}

uint64
sys_setdisksched(void)
{
  int policy;
  argint(0, &policy);
  return setdisksched_impl(policy);
}

uint64
sys_setraidmode(void)
{
  int mode;
  argint(0, &mode);
  return setraidmode_impl(mode);
}

uint64
sys_getdiskstats(void)
{
  uint64 addr;
  argaddr(0, &addr);
  return getdiskstats_helper(addr);
}