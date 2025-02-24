#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

#ifdef MLFQ
struct queue mlfq[NMLFQ];
#endif

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
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
#ifdef MLFQ
  for (int i = 0; i < NMLFQ; i++)
  {
    mlfq[i].size = 0;
    mlfq[i].head = 0;
    mlfq[i].tail = 0;
  }
#endif
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
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
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();

  p->ctime = ticks;
  p->rtime = 0;
  p->stime = 0;
  p->sched_count = 0;

  #ifdef LBS
  p->tickets = 1;
  #endif

  #ifdef PBS
  p->priority = 60;
  #endif

  #ifdef MLFQ
  p->priority = 0;
  p->in_queue = 0;
  p->quanta = 1;
  p->q_in_time = ticks;
  for(int i = 0; i < NMLFQ; i++)
  {
    p->qrtime[i] = 0;
  }
  #endif


  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }
  //Make a trapframe page backup for timer interrupt
  if((p->bkuptframe = (struct trapframe *)kalloc()) == 0) {
    release(&p->lock);
    return 0;
  }
  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  //Added for initialisation
  p->timepassed = 0;
  p->ticks = 0;
  p->hndlr = 0;
  p->handling = 0;

  p->state = USED;
  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->bkuptframe){
    kfree((void *)p->bkuptframe);
    p->bkuptframe = 0;
  }
  if(p->pagetable)
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
  p->strace_mask_bits = 0;
  p->rtime = 0;
  p->stime = 0;
  p->sched_count = 0;
  p->endtime = 0;
  #ifdef PBS
  p->priority = 60;
  #endif
  #ifdef LBS
  p->tickets = 1;
  #endif
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
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
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
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
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;
  np->strace_mask_bits = p->strace_mask_bits;
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  #ifdef LBS
  np->tickets = p->tickets;
  #endif
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
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
  p->endtime = ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
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
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
waitx(uint64 addr, uint* rtime, uint* wtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->endtime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
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
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

#ifdef ROUND_ROBIN
void
round_robin(struct cpu *c) {
  for(struct proc *p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        p->sched_count++;
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
#endif

#ifdef FCFS
void
fcfs(struct cpu *c) {
  int minimum_time = 0;
  struct proc *proc_with_min_time = 0;

  // go through all processes and find the one with the minimum creation time
  for(struct proc *p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == RUNNABLE) {
      // if there is no current process, or the process p has a greater creation time than the current process,
      // then we set the current minimum process to the process p
      if(proc_with_min_time == 0 || p->ctime < minimum_time) {
        if(proc_with_min_time!= 0) {
          release(&proc_with_min_time->lock); // release the lock of the previous minimum process so that new process can acquire it
        }
        proc_with_min_time = p;
      }
    }
    // if current process is not the same as the minimum process, then release the lock of the current process
    if(proc_with_min_time != p) {
      release(&p->lock);
    }
  }

  if(proc_with_min_time != 0) {
    proc_with_min_time->sched_count++;
    //Switch to chosen process.  It is the process's job
    // to release its lock and then reacquire it
    // before jumping back to us.
    proc_with_min_time->state = RUNNING;
    c->proc = proc_with_min_time;
    swtch(&c->context, &proc_with_min_time->context);

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    // release the lock once process is done running
    release(&proc_with_min_time->lock);
  }
}
#endif

#ifdef LBS

void settickets(int tickets)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->tickets = tickets;
  release(&p->lock);
}

// from FreeBSD.
int
do_rand(unsigned long *ctx)
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

unsigned long rand_next = 1;

int
rand(void)
{
    return (do_rand(&rand_next));
}

int
get_random_ticket(int a, int b)
{
    if(a > b) {
        int temp = a;
        a = b;
        b = temp;
    }

    int range = b - a + 1;
    int r = rand() % range;
    return (a + r);
}

void
lbs(struct cpu *c)
{
  int total_tickets = 0;
  struct proc *lucky_proc = 0;

  for(struct proc *p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if(p->state == RUNNABLE)
    {
      total_tickets += p->tickets;
    }
    release(&p->lock);
  }

  int lucky_ticket = get_random_ticket(0, total_tickets);

  total_tickets = 0;

  for(struct proc *p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if(p->state == RUNNABLE)
    {
      total_tickets += p->tickets;
      if(total_tickets >= lucky_ticket)
      {
        lucky_proc = p;
        break;
      }
    }
    release(&p->lock);
  }

  if(lucky_proc != 0)
  {
    lucky_proc->sched_count++;
    lucky_proc->state = RUNNING;
    c->proc = lucky_proc;
    swtch(&c->context, &lucky_proc->context);
    c->proc = 0;
    release(&lucky_proc->lock);
  }
}


#endif

#ifdef PBS
void set_priority(int priority, int pid, int* old_priority)
{
  for(struct proc *p = proc; p < &proc[NPROC]; p++) {
    if(p->pid == pid) {
      acquire(&p->lock);
      *old_priority = p->priority;
      p->priority = priority;
      p->rtime = 0;
      p->stime = 0;
      release(&p->lock);
      if(*old_priority > priority) {
        yield();
      }
    }
  }
}

int dynamic_priority(struct proc *p)
{
  int niceness;
  int dp;

  if(p->rtime + p->stime != 0)
    niceness = (p->stime / (p->rtime + p->stime)) * 10;
  else
  {
    niceness = 5;
  }

  dp = p->priority - niceness + 5;

  if(dp > 100)
  {
    dp = 100;
  }
  return dp;
}

void
pbs(struct cpu *c)
{
  struct proc *minproc = 0;
  for(struct proc *p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if(p->state == RUNNABLE)
    {
      if(minproc == 0)
      {
        minproc = p;
        continue;
      }

      if(dynamic_priority(minproc) > dynamic_priority(p))
      {
        release(&minproc->lock);
        minproc = p;
        continue;
      }

      if(dynamic_priority(minproc) == dynamic_priority(p))
      {
        if(minproc->sched_count > p->sched_count)
        {
          release(&minproc->lock);
          minproc = p;
          continue;
        }
        else if(minproc->sched_count == p->sched_count)
        {
          if(minproc->ctime > p->ctime)
          {
            release(&minproc->lock);
            minproc = p;
            continue;
          }
        }
      }
    }
    release(&p->lock);
  }

  if(minproc != 0)
  {
    minproc->sched_count++;
    minproc->state = RUNNING;
    minproc->rtime = 0;
    minproc->stime = 0;
    c->proc = minproc;
    swtch(&c->context, &minproc->context);
    c->proc = 0;
    release(&minproc->lock);
  }
}
#endif

#ifdef MLFQ
void
mlfq_sched(struct cpu *c)
{
  struct proc *minproc = 0;
  // reset priority of old procs
  for(struct proc *p = proc; p < &proc[NPROC];p++ )
  {
    if(p->state != RUNNABLE)
    {
      continue;
    }

    if(ticks - p->q_in_time >= AGETICKS)
    {
      p->q_in_time = ticks;
      if(p->in_queue)
      {
        queue_remove(&mlfq[p->priority], p->pid);
        p->in_queue = 0;
      }

      if(p->priority > 0) p->priority--;
    }
  }

  for(struct proc *p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if(p->state == RUNNABLE && !p->in_queue)
    {
      queue_push(&mlfq[p->priority], p);
      p->in_queue = 1;
    }
    release(&p->lock);
  }

  for(int i = 0; i < NMLFQ; i++)
  {
    while(mlfq[i].size)
    {
      struct proc *p = top(&mlfq[i]);
      acquire(&p->lock);
      queue_pop(&mlfq[i]);
      p->in_queue = 0;

      if(p->state == RUNNABLE)
      {
        p->q_in_time = ticks;
        minproc = p;
        break;
      }
      release(&p->lock);
    }
    if(minproc != 0)
    {
      break;
    }
  }

  if(minproc != 0)
  {
    minproc->quanta = 1 << minproc->priority;
    minproc->sched_count++;
    minproc->state = RUNNING;
    c->proc = minproc;
    swtch(&c->context, &minproc->context);
    c->proc = 0;
    minproc->q_in_time = ticks;
    release(&minproc->lock);
  }
}
#endif
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    #ifdef ROUND_ROBIN
    round_robin(c);
    #endif

    #ifdef FCFS
    fcfs(c);
    #endif

    #ifdef LBS
    lbs(c);
    #endif

    #ifdef PBS
    pbs(c);
    #endif

    #ifdef MLFQ
    mlfq_sched(c);
    #endif
  }
}

void
update_time()
{
  struct proc *p;
  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    switch(p->state){
      case RUNNING:
        p->rtime++;
        #ifdef MLFQ
          p->qrtime[p->priority]++;
          p->quanta--;
        #endif
      break;
      case SLEEPING:
        p->stime++;
      break;
      default:
      break;
    }
    release(&p->lock);
  }
}
// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
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
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
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
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
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

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
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
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  #ifdef ROUND_ROBIN
  printf("PID State Name\n");
  #endif
  #ifdef FCFS
    printf("PID State Name ctime\n");
  #endif
  #ifdef PBS
    printf("PID Priority State Name rtime stime sched_count\n");
  #endif
  #ifdef MLFQ
    printf("PID Priority State rtime stime sched_count q0 q1 q2 q3 q4\n");
  #endif
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    #ifdef ROUND_ROBIN
    printf("%d %s %s", p->pid, state, p->name);
    #endif
    #ifdef FCFS
    printf("%d %s %s %d", p->pid, state, p->name, p->ctime);
    #endif
    #ifdef LBS
    printf("%d %s %s %d", p->pid, state, p->name, p->tickets);
    #endif
    #ifdef PBS
    printf("%d %d %s %s %d %d %d", p->pid, dynamic_priority(p), state, p->name, p->rtime, p->stime, p->sched_count);
    #endif
    #ifdef MLFQ
    printf("%d %d %s %d %d %d %d %d %d %d %d", p->pid, p->priority, state, p->rtime, p->stime, p->sched_count, p->qrtime[0], p->qrtime[1], p->qrtime[2], p->qrtime[3], p->qrtime[4]);
    #endif
    printf("\n");
  }
}

// queue functions
#ifdef MLFQ
struct proc *top(struct queue *q) {
  if(q->size == 0) {
    return 0;
  }
  return q->procs[q->head];
}

void queue_push(struct queue *q, struct proc *p) {
  if(q->size == NPROC) {
    panic("queue is full");
  }
  q->procs[q->tail] = p;
  q->tail = (q->tail + 1) % NPROC;
  q->size++;
}

void queue_pop(struct queue *q) {
  if(q->size == 0) {
    panic("queue is empty");
  }
  q->head = (q->head + 1) % NPROC;
  q->size--;
}

void queue_remove(struct queue* q, int pid)
{
  for(int curr = q->head; curr != q->tail; curr = (curr+1) % NPROC)
  {
    if(q->procs[curr]->pid == pid)
    {
      struct proc *temp = q->procs[curr];
      q->procs[curr] = q->procs[(curr+1) % NPROC];
      q->procs[(curr+1) % NPROC] = temp;
    }
  }

  q->tail--;
  q->size--;

  if(q->tail < 0)
  {
    q->tail = NPROC - 1;
  }
}
#endif


#ifdef MLFQ
void
printstats()
{
  struct proc *p = myproc();
  printf("pid: %d--->priority: %d\n", p->pid, p->priority);
}
#endif