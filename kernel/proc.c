#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"


extern struct queue mlfq[MLFQ_CNT];
int max_age = 5; // to implement aging, the queue time after 


struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.

void add_to_queue(struct queue *q, struct proc *p)
{
  if (p->mlfq_q == q && p->in_mlfq == 1)
  {
    printf("already present!!!\n");
    return;
  }
  struct proc *it = q->begin;
  if (it == 0)
  { // queue was empty
    q->begin = p;
    p->mlfq_q = q;
    p->queue_time = 0;
    p->in_mlfq = 1;
    p->mlfq_nxt = 0;
    p->queue_wait_time = 0;
    return;
  }
  while (it->mlfq_nxt != 0)
  {
    if (it->pid == p->pid)
    {
      printf("already present!!!\n");
      return;
    }
    it = it->mlfq_nxt;
  }
  it->mlfq_nxt = p;
  p->mlfq_nxt = 0;
  p->mlfq_q = q;
  p->queue_time = 0;
  p->in_mlfq = 1;
  p->queue_wait_time = 0;
  return;
}

void remove_from_queue(struct proc *p)
{
  struct proc *it;
  struct queue *q = p->mlfq_q;
  it = q->begin;
  if (it == p)
  {
    q->begin = it->mlfq_nxt;
    p->mlfq_q = 0;
    p->queue_time = 0;
    p->in_mlfq = 0;
    return;
  }
  while (it->mlfq_nxt != p)
  {
    it = it->mlfq_nxt;
  }
  it->mlfq_nxt = it->mlfq_nxt->mlfq_nxt;
  p->mlfq_q = 0;
  p->in_mlfq = 0;
}

void remove_tmp_from_queue(struct proc *p)
{
  struct proc *it;
  struct queue *q = p->mlfq_q;
  it = q->begin;
  if (it == p)
  {
    q->begin = it->mlfq_nxt;
    p->mlfq_nxt = 0;
    p->in_mlfq = 0;
    return;
  }
  while (it->mlfq_nxt != p)
  {
    it = it->mlfq_nxt;
  }
  it->mlfq_nxt = it->mlfq_nxt->mlfq_nxt;
  p->mlfq_nxt = 0;
  p->in_mlfq = 0;
}

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

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
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

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

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

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->sigalarm_routine = 0;
  p->sigalarm_ticks = -1;
  p->ticks = 0;
  p->mask = 0;
  p->start_time = ticks;
  p->nice = 5;
  p->static_priority = 60;
  p->sleep_ticks = 0;
  p->running_ticks = 0;
  p->num_sched = 0;
  p->mlfq_nxt = 0;
  p->cycle = 0;
  p->in_mlfq = 0;
  p->mlfq_q = 0;

  if (p->parent && p->parent->tickets > 0)
  {
    p->tickets = p->parent->tickets;
  }
  else
  {
    p->tickets = 1;
  }
  // p->tickets = 1;
  // printf("the start time for process %d is %d", p->pid, ticks);

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Allocate a copy trapframe page
  if ((p->trapframe_cpy = (struct trapframe *)kalloc()) == 0)
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
  p->mlfq_q = 0;
  p->mlfq_nxt = 0;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
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

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
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
  // printf("trying to free pagetable of parent\n");
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
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

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
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
  // printf("current used pages in the ram : %d\n", (int)(curr_used_pages()));
  // Copy user memory from parent to child.
  if (uvmcopy_cow(p->pagetable, np->pagetable, p->sz) < 0)
  {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  // printf("current used pages in the ram : %d\n", (int)(curr_used_pages()));
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
  p->end_time = ticks;


  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}


#ifdef RR
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

    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}
#endif
#ifdef FCFS
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc *best_p = 0;
    int min_st_time = 2147483647;
    int id = 10;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      // acquire(&p->lock);
      // printf("The pid of the process is %d\n", p->pid);
      // printf("The start time is %d\n", p->start_time);
      // printf("The minimum start time is %d\n", min_st_time);
      // char state[64];
      // if (p->state == RUNNABLE)
      // {
      //   safestrcpy(state, "RUNNABLE", 9);
      // }
      // else if (p->state == SLEEPING)
      // {
      //   safestrcpy(state, "SLEEPING", 9);
      // }
      // else if (p->state == UNUSED)
      // {
      //   safestrcpy(state, "UNUSED", 7);
      // }
      // else if (p->state == RUNNING)
      // {
      //   safestrcpy(state, "RUNNING", 8);
      // }
      // else
      // {
      //   safestrcpy(state, "OTHERS", 7);
      // }
      // printf("The state of the process is %s\n", state);
      if (p->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        if (p->start_time < min_st_time)
        {
          best_p = p;
          min_st_time = p->start_time;
          id = p - proc;
        }
      }
      // release(&p->lock);
    }
    if (best_p == 0)
    {
      continue;
    }
    // intr_off();
    acquire(&best_p->lock);
    // printf("%d is about to be run\n", best_p->pid);
    if (best_p->state == RUNNABLE)
    {
      best_p->state = RUNNING;
      c->proc = best_p;
      swtch(&c->context, &best_p->context);
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&best_p->lock);
  }
}
#endif


int do_rand(unsigned long *ctx)
{
  /*
   * Compute x = (7^5 * x) mod (2^31 - 1)
   * without overflowing 31 bits:
   *      (2^31 - 1) = 127773 * (7^5) + 2836
   * From "Random number generators: good ones are hard to find",
   * Park and Miller, Communications of the ACM, vol. 31, no. 10,
   * October 1988, p. 1195.
   */
  long hi, lo, x;

  /* Transform to [1, 0x7ffffffe] range. */
  x = (*ctx % 0x7ffffffe) + 1;
  hi = x / 127773;
  lo = x % 127773;
  x = 16807 * lo - 2836 * hi;
  if (x < 0)
    x += 0x7fffffff;
  /* Transform to [0, 0x7ffffffd] range. */
  x--;
  *ctx = x;
  return (x);
}


#ifdef LBS
unsigned long seed = 3;

void scheduler(void)
{
  printf("here\n");
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
    int best_id = -1;
    intr_on();
    int ticket_cnt = 0;
    for (int i = 0; i < NPROC; i++)
    {
      acquire(&proc[i].lock);
      if (proc[i].state == RUNNABLE)
        ticket_cnt += proc[i].tickets;
      release(&proc[i].lock);
    }
    do_rand(&seed);
    int randn = seed % ticket_cnt;

    int sum = 0;
    for (int i = 0; i < NPROC; i++)
    {
      acquire(&proc[i].lock);
      if (proc[i].state == RUNNABLE)
      {
        sum += proc[i].tickets;
        if (sum >= randn)
        {
          best_id = i;
          release(&proc[i].lock);
          break;
        }
      }
      release(&proc[i].lock);
    }
    if (best_id < 0 || best_id > NPROC)
    {
      continue;
    }

    acquire(&proc[best_id].lock);
    if (proc[best_id].state == RUNNABLE)
    {
      proc[best_id].state = RUNNING;
      c->proc = &proc[best_id];
      swtch(&c->context, &proc[best_id].context);
      c->proc = 0;
    }
    release(&proc[best_id].lock);
  }
}
#endif

int waitx(uint64 addr, uint *rtime, uint *wtime)
{
  int pid;
  struct proc *p = myproc();

  acquire(&wait_lock);
  for (;;)
  {
    int havekids = 0;
    for (struct proc *np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          pid = np->pid;
          *rtime = np->running_ticks;
          *wtime = np->end_time - np->start_time - np->running_ticks;
          // printf("%d", np->end_time);
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
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }
    sleep(p, &wait_lock);
  }
}


#ifdef PBS
void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
    struct proc *best_p = 0;
    int max_dp = 101;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        int base = p->static_priority - p->nice + 5;
        int priority = base > 100 ? 100 : base;
        int dp = priority > 0 ? priority : 0;
        if (dp < max_dp)
        {
          best_p = p;
          max_dp = dp;
        }
        else if (dp == max_dp)
        {
          int num_sched1 = p->num_sched;
          int num_sched2 = best_p->num_sched;
          if (num_sched1 < num_sched2)
          {
            best_p = p;
          }
          else if (num_sched1 == num_sched2)
          {
            int start1 = p->start_time;
            int start2 = best_p->start_time;
            if (start1 > start2)
            {
              best_p = p;
            }
          }
        }

      }
      release(&p->lock);
    }
    if (best_p == 0)
    {
      continue;
    }

    acquire(&best_p->lock);
    if (best_p->state == RUNNABLE)
    {
      best_p->state = RUNNING;
      c->proc = best_p;
      best_p->num_sched++;
      // printf("%s %d", best_p->name, best_p->static_priority);
      swtch(&c->context, &best_p->context);
      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&best_p->lock);
  }
}
#endif

#ifdef MLFQ
void scheduler(void)
{
  struct cpu *c = mycpu();
  c->proc = 0;

  for (;;)
  {
  again:;
    intr_on();
    for (int i = 0; i < MLFQ_CNT; i++)
    {
      struct proc *it = mlfq[i].begin;
      while (it != 0)
      {
        if (it->state == SLEEPING)
        {
          struct proc *new_it = it->mlfq_nxt;
          remove_tmp_from_queue(it);
          it = new_it;
        }
        else if (it->state == ZOMBIE || it->state == UNUSED)
        {
          struct proc *new_it = it->mlfq_nxt;
          remove_from_queue(it);
          it = new_it;
        }
        else if (it->state == RUNNABLE && it->mlfq_q - mlfq > 0 && it->queue_wait_time >= max_age)
        {
          struct queue *new_q = it->mlfq_q - 1;
          remove_from_queue(it);
          add_to_queue(new_q, it);
        }
        else
        {
          it = it->mlfq_nxt;
        }
      }
    }
    // printf("completed the mlfq checking\n");
    for (int i = 0; i < NPROC; i++)
    {
      if (proc[i].state == RUNNABLE && proc[i].in_mlfq == 0 && proc[i].mlfq_q == 0)
      {
        add_to_queue(&mlfq[0], &proc[i]);
      }
      else if (proc[i].state == RUNNABLE && proc[i].in_mlfq == 0 && proc[i].mlfq_q)
      {
        add_to_queue(proc[i].mlfq_q, &proc[i]);
      }
    }
    for (int i = 0; i < MLFQ_CNT; i++)
    {
      struct proc *it = mlfq[i].begin;
      while (it != 0)
      {
        acquire(&it->lock);
        if (it->state == RUNNABLE)
        {
          if (it->queue_time == mlfq[i].ticks && it->mlfq_q - mlfq != MLFQ_CNT - 1)
          {
            struct queue *new_q = it->mlfq_q + 1;
            remove_from_queue(it);
            // printf("\n");
            // // printf("attempting add\n");
            // printf("process: %d\n", it->pid);
            // struct queue *q = it->mlfq_q;
            // printf("state before add: ");
            // print_queue(new_q);
            add_to_queue(new_q, it);
            // printf("state after add: ");
            // print_queue(new_q);
            // printf("add successful\n");
            release(&it->lock);
            goto again;
          }
          it->state = RUNNING;
          it->queue_wait_time = 0;
          c->proc = it;
          swtch(&c->context, &it->context);
          c->proc = 0;
          release(&it->lock);
          goto again;
        }
        release(&it->lock);
        it = it->mlfq_nxt;
      }
    }
  }
}

#endif

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
    panic("sched locks");
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
  if (p->sigalarm_routine == 0 && p->sigalarm_ticks != -1)
  {
    p->ticks++;
  }
  acquire(&p->lock);
  p->state = RUNNABLE;
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
  if(p->state != SLEEPING){
  p->last_sleep_tick = ticks;
  }
  // mlfq[p->mlfq_id][p->pos_in_q] = -1;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        // printf("in wakeup");
        p->sleep_ticks += ticks - (p->last_sleep_tick);
        // printf("ticks : %d sleep ticks : %d", ticks, p->last_sleep_tick);
        if(p->sleep_ticks != 0 || p->running_ticks != 0)
        p->nice = (p->sleep_ticks * 10) / (p->sleep_ticks + p->running_ticks);
        p->state = RUNNABLE;
        // (queue[p->mlfq_id]->end) = p; 
        // (queue[p->mlfq_id]->end)++;
      }
      release(&p->lock);
    }
  }
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

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
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
      [USED] "used",
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

int trace(int mask)
{
  struct proc *p = myproc();
  p->mask = mask;
  return 1;
}

int sigalarm(int ticks, uint64 fn)
{
  struct proc *p = myproc();
  p->ticks = 0;
  p->sigalarm_ticks = (ticks == 0) ? -1 : ticks;
  p->sigalarm_fn = fn;
  return 1;
}

int sigreturn(void)
{
  struct proc *p = myproc();
  p->sigalarm_routine = 0;
  // proc_freepagetable(p->pagetable, p->sz);
  // p->pagetable = p->pagetable_cpy;
  *(p->trapframe) = *(p->trapframe_cpy);
  return 1;
}

int set_priority(int new_priority, int pid)
{
  struct proc *p;
  printf("%d", myproc()->pid);
  int old_pr = -1;
  for (p = proc; p <= &proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      old_pr = p->static_priority;
      p->static_priority = new_priority;
      p->nice = 0;
      printf("new %d, old %d", p->static_priority, old_pr);
      if (old_pr > new_priority)
      {
        yield();
      }
      break;
    }
  }
  return old_pr;
}
