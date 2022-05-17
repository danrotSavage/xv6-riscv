#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];
uint64 cpu_counters[NCPU];
// we added zombie, sleeping and unused list therefore NCPU + 3
int cpus_first_proc[NCPU + 3];
struct spinlock list_lock[NCPU + 3];

struct proc proc[NPROC];
int sleeping_index[NPROC];
#define SLEEPINGLIST NCPU
#define ZOMBIELIST (NCPU + 1)
#define UNUSEDLIST (NCPU + 2)

struct proc *initproc;

int nextpid = 1;
int first_process = 0;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);
extern uint64 cas(volatile void *addr, int expected, int newval);
struct proc *remove_proc_from_unused();
void add_node_runnable(int process_index, int cpu_index);
int remove_proc_from_runnable(int process_index, int cpu_index);
int min_cpu_count(void);
int remove_proc_from_sleep(int process_index);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  int j = 0;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    initlock(&p->lock2, "list_lock");
    p->next_index_in_list = -1;
    p->kstack = KSTACK((int)(p - proc));
    j++;
  }

  for (int i = 0; i < NCPU + 3; i++)
  {
    struct spinlock k;
    initlock(&k, "head_list_lock");
    cpus_first_proc[i] = -1;
    list_lock[i] = k;
    if (i < NCPU)
      cpu_counters[i] = 0;
  }

  for (int i = 0; i < NPROC; i++)
  {
    proc[i].index_in_proc = i;
    sleeping_index[i] = 0;
  }

  for (int i = 0; i < NPROC; i++)
  {
    add_node_runnable(i, UNUSEDLIST);
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  // spinlock implementation
  // acquire(&pid_lock);
  // pid = nextpid;
  // nextpid = nextpid + 1;
  // release(&pid_lock);

  // cas implementation
  do
  {
    pid = nextpid;
  } while (!cas(&nextpid, pid, pid + 1));
  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  p = remove_proc_from_unused();

  acquire(&p->lock);
  //     for (p = proc; p < &proc[NPROC]; p++)
  // {
  //   acquire(&p->lock);
  //   if (p->state == UNUSED)
  //   {
  //     goto found;
  //   }
  //   else
  //   {
  //     release(&p->lock);
  //   }
  // }
  // return 0;

  p->pid = allocpid();
  p->state = USED;
  if (first_process == 0)
  {
    p->cpu_number = 0;
    first_process++;
  }

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
  remove_proc_from_runnable(p->index_in_proc, ZOMBIELIST);
  add_node_runnable(p->index_in_proc, UNUSEDLIST);
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  add_node_runnable(p->index_in_proc, 0);

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);

  np->state = RUNNABLE;

#if ON
  np->cpu_number = min_cpu_count();
#else
  np->cpu_number = p->cpu_number;
#endif

  add_node_runnable(np->index_in_proc, np->cpu_number);

  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  add_node_runnable(p->index_in_proc, ZOMBIELIST);

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {

        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {

          // Found one.
          pid = np->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {

    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    int id = cpuid();
    int process = cpus_first_proc[id];

#if ON

    if (process == -1)
    {
      for (int i = 0; i < NCPU; i++)
      {
        if (cpu_counters[i] > 0 && id != i)
        {
          id = i;
          process = cpus_first_proc[i];

          if (process == 0 || process == 1)
            process = proc[process].next_index_in_list;
          if ((process != -1) && (process == 0 || process == 1))
            process = proc[process].next_index_in_list;

          if (process != -1)
          {
            // printf("i stoel process %d, im cpu %d\n",process,id);
            break;
          }
        }
      }
    }
#endif
    if (process != -1)
    {
      p = &proc[process];

      acquire(&p->lock);

      // printf("ind scheduler\n");
      int removed = remove_proc_from_runnable(p->index_in_proc, id);

      if (removed != 0)
      {
        p->state = RUNNING;
        c->proc = p;
        p->cpu_number = cpuid();
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;

        // todo check what to do if goes to sleep/zombie
      }
      release(&p->lock);
    }

    //   for (p = proc; p < &proc[NPROC]; p++)
    //   {
    //     acquire(&p->lock);
    //     if (p->state == RUNNABLE)
    //     {
    //       // Switch to chosen process.  It is the process's job
    //       // to release its lock and then reacquire it
    //       // before jumping back to us.
    //       p->state = RUNNING;
    //       c->proc = p;
    //       swtch(&c->context, &p->context);

    //       // Process is done running for now.
    //       // It should have changed its p->state before coming back.
    //       c->proc = 0;
    //     }
    //     release(&p->lock);
    //   }
    // }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
  {
    printf("noff value is %d", mycpu()->noff);
    panic("sched locks");
  }
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  p->state = RUNNABLE;
  add_node_runnable(p->index_in_proc, p->cpu_number);
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  add_node_runnable(p->index_in_proc, SLEEPINGLIST);

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

int wakeup_help(struct proc *p)
{

  int removed = remove_proc_from_runnable(p->index_in_proc, SLEEPINGLIST);
// they dont want the loop on all processes check todo
#if ON
  p->cpu_number = min_cpu_count();
#endif

  add_node_runnable(p->index_in_proc, p->cpu_number);
  p->state = RUNNABLE;

  return removed;
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
// todo go over only sleeping list

void wakeup(void *chan)
{

  int cpu_index = SLEEPINGLIST;
  // struct proc *p;
  struct proc *curr;

  curr = 0;

  // printf("getting sleeping lock");
  // printf("acquiring sleeping list lock\n");

  // printf("acquiring list lock in add node\n");

  acquire(&list_lock[cpu_index]);

  int first_proc = cpus_first_proc[cpu_index];

  if (first_proc == -1)
  {
    release(&list_lock[cpu_index]);
    return;
  }
  else
  {
    curr = &proc[first_proc];
    acquire(&curr->lock2);
    release(&list_lock[cpu_index]);
    while (curr->next_index_in_list != -1)
    {
      if (curr->state == SLEEPING && curr->chan == chan)
      {
        sleeping_index[curr->index_in_proc] = 1;
      }

      int next = curr->next_index_in_list;
      // printf("add - my index is %d, next index is %d\n" , curr->index_in_proc ,next);

      struct proc *next_node = &proc[next];
      acquire(&next_node->lock2);
      release(&curr->lock2);
      curr = next_node;
    }

    if (curr->state == SLEEPING && curr->chan == chan)
    {
      sleeping_index[curr->index_in_proc] = 1;
    }
    // printf("add 3 \n");

    release(&curr->lock2);

    int removed = 0;
    for (int i = 0; i < NPROC; i++)
    {
      if (sleeping_index[i] == 1)
      {
        removed = wakeup_help(&proc[i]);
        if (removed == 1)
          sleeping_index[i] = 0;
      }
    }
    // printf("add 4 \n");
  }

  // printf("leaving wakeup\n");

  // for (p = proc; p < &proc[NPROC]; p++)
  //    {

  // if (need to wake up)
  // sleeparray [ i] =1 ;

  //     if (p != myproc())
  //     {

  //       acquire(&p->lock);
  //       if (p->state == SLEEPING && p->chan == chan)
  //       {
  //
  //   for (p = proc; p < &proc[NPROC]; p++)
  //   {
  //     if (p != myproc())
  //     {

  //       acquire(&p->lock);
  //       if (p->state == SLEEPING && p->chan == chan)
  //       {

  // // they dont want the loop on all processes check todo
  // #if ON
  //         p->cpu_number = min_cpu_count();
  // #endif

  //         add_node_runnable(p->index_in_proc, p->cpu_number);

  //         p->state = RUNNABLE;
  //       }
  //       release(&p->lock);
  //    }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {

    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

int set_cpu(int cpu_num)
{
  struct proc *p = myproc();
  yield(); // todo: check how to yield correctly
  int cpu_old_number = p->cpu_number;
  if (!cas(&p->cpu_number, cpu_old_number, cpu_num))
  {
    return cpu_num;
  }
  else
    return -1;
}

int remove_proc_from_runnable(int process_index, int cpu_index)
{

  int ret = 0;
  struct proc *pred, *curr;

  curr = 0;

  // printf("acquiring list lock in remove from runnable\n");
  acquire(&list_lock[cpu_index]);

  int first_proc = cpus_first_proc[cpu_index];

  if (first_proc == -1)
  {
    release(&list_lock[cpu_index]);
    return 0;
  }

  else
  {

    // no matter what lock first,release whole list
    pred = &proc[first_proc];

    acquire(&pred->lock2);

    // if first proccess is the one we want
    if (process_index == first_proc)
    {
      if (cpu_index < NCPU)
      {
        uint64 old_counter;
        do
        {
          old_counter = cpu_counters[cpu_index];
        } while (!cas(&cpu_counters[cpu_index], old_counter, old_counter - 1));
      }

      cpus_first_proc[cpu_index] = pred->next_index_in_list;
      pred->next_index_in_list = -1;
      release(&list_lock[cpu_index]);
      release(&pred->lock2);

      return 1;
    }

    release(&list_lock[cpu_index]);

    // catch second
    if (pred->next_index_in_list != -1)
    {

      curr = &proc[pred->next_index_in_list];

      acquire(&curr->lock2);

      if (process_index == curr->index_in_proc)
      {

        if (cpu_index < NCPU)
        {
          uint64 old_counter;
          do
          {
            old_counter = cpu_counters[cpu_index];
          } while (!cas(&cpu_counters[cpu_index], old_counter, old_counter - 1));
        }

        pred->next_index_in_list = curr->next_index_in_list;
        curr->next_index_in_list = -1;
        release(&curr->lock2);
        release(&pred->lock2);

        return 1;
      }

      else
      {

        while (curr->next_index_in_list != -1)
        {

          release(&pred->lock2);
          pred = curr;
          curr = &proc[pred->next_index_in_list];
          acquire(&curr->lock2);

          if (process_index == curr->index_in_proc)
          {

            if (cpu_index < NCPU)
            {
              uint64 old_counter;
              do
              {
                old_counter = cpu_counters[cpu_index];
              } while (!cas(&cpu_counters[cpu_index], old_counter, old_counter - 1));
            }

            pred->next_index_in_list = curr->next_index_in_list;
            curr->next_index_in_list = -1;
            release(&curr->lock2);
            release(&pred->lock2);
            return 1;
          }
        }

        release(&curr->lock2);
        release(&pred->lock2);
      }
    }
    else
    {
      release(&pred->lock2);
    }
    return ret;
  }
}

struct proc *remove_proc_from_unused()
{
  struct proc *curr;

  curr = 0;

  acquire(&list_lock[UNUSEDLIST]);

  int first_proc = cpus_first_proc[UNUSEDLIST];

  if (first_proc == -1)
  {
    release(&list_lock[UNUSEDLIST]);
    return curr;
  }
  else
  {

    curr = &proc[first_proc];

    acquire(&curr->lock2);
    cpus_first_proc[UNUSEDLIST] = curr->next_index_in_list;
    curr->next_index_in_list = -1;

    release(&list_lock[UNUSEDLIST]);
    release(&curr->lock2);
    return curr;
  }
}

void add_node_runnable(int process_index, int cpu_index)
{

  struct proc *curr;

  curr = 0;

  // printf("acquiring list lock in add node\n");

  acquire(&list_lock[cpu_index]);
  if (cpu_index < NCPU)
  {
    uint64 old_counter;
    do
    {
      old_counter = cpu_counters[cpu_index];
    } while (!cas(&cpu_counters[cpu_index], old_counter, old_counter + 1));
  }

  int first_proc = cpus_first_proc[cpu_index];

  if (first_proc == -1)
  {
    cpus_first_proc[cpu_index] = process_index;
    release(&list_lock[cpu_index]);

    return;
  }
  else
  {
    curr = &proc[first_proc];
    acquire(&curr->lock2);
    release(&list_lock[cpu_index]);
    while (curr->next_index_in_list != -1)
    {

      int next = curr->next_index_in_list;
      // printf("add - my index is %d, next index is %d\n" , curr->index_in_proc ,next);

      struct proc *next_node = &proc[next];
      acquire(&next_node->lock2);
      release(&curr->lock2);
      curr = next_node;
    }

    // printf("add 3 \n");

    curr->next_index_in_list = process_index;
    release(&curr->lock2);
    // printf("add 4 \n");
  }
}

int get_cpu()
{

  // struct proc *p = &proc[0];
  // printf("my name is: %s , my cpu is: %d and my spot in proc is %d\n", p->name, p->cpu_number, p->index_in_proc);

  // struct proc *k = &proc[1];
  // printf("my name is: %s , my cpu is: %d and my spot in proc is %d\n", k->name, k->cpu_number, k->index_in_proc);

  // add_node_runnable(0, p->cpu_number);
  // printf("after adding 0\n");
  // add_node_runnable(1, p->cpu_number);
  //   printf("after adding 1\n");

  // add_node_runnable(2, p->cpu_number);
  //   printf("after adding 2\n");

  // add_node_runnable(3, p->cpu_number);

  // printf("add successfully , should not be -1 %d\n", cpus_first_proc[0]);

  // remove_proc_from_runnable(1, p->cpu_number);
  // remove_proc_from_runnable(2, p->cpu_number);

  // printf("removed successfully , should be 3 %d\n", proc[cpus_first_proc[0]].next_index_in_list);

  struct proc *p = myproc(); // todo: check how this can fail
  return p->cpu_number;
  // return 1;
}

uint64 cpu_process_count(int cpu_index)
{
  return cpu_counters[cpu_index];
}

int min_cpu_count()
{
  uint64 min = cpu_counters[0];
  int cpu_index = 0;
  for (int i = 1; i < NCPU; i++)
  {
    if (cpu_counters[i] < min)
    {
      cpu_index = i;
    }
  }
  return cpu_index;
}
// int remove_proc_from_sleep(int process_index)
// {

//   int cpu_index = SLEEPINGLIST;
//   int ret = 0;
//   struct proc *pred, *curr;

//   curr = 0;

//   int first_proc = cpus_first_proc[cpu_index];

//   if (first_proc == -1)
//   {
//     return 0;
//   }

//   else
//   {

//     // no matter what lock first,release whole list
//     pred = &proc[first_proc];

//     // if first proccess is the one we want
//     if (process_index == first_proc)
//     {
//       cpus_first_proc[cpu_index] = pred->next_index_in_list;
//       pred->next_index_in_list = -1;
//       return 1;
//     }

//     // catch second
//     if (pred->next_index_in_list != -1)
//     {

//       curr = &proc[pred->next_index_in_list];

//       if (process_index == curr->index_in_proc)
//       {

//         pred->next_index_in_list = curr->next_index_in_list;
//         curr->next_index_in_list = -1;

//         return 1;
//       }

//       else
//       {

//         while (curr->next_index_in_list != -1)
//         {

//           pred = curr;
//           curr = &proc[pred->next_index_in_list];

//           if (process_index == curr->index_in_proc)
//           {

//             pred->next_index_in_list = curr->next_index_in_list;
//             curr->next_index_in_list = -1;

//             return 1;
//           }
//         }

//       }
//     }
//     return ret;
//   }
