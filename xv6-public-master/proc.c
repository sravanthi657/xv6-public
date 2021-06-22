#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

struct proc *queue[5][NPROC];
int queue_popu[5]={-1,-1,-1,-1,-1};
int clockperiod[5]={1,2,4,8,16};
void remove_proc_from_queue(int pid,int curq);
void ageing(void);
void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->cur_queue =0;
  p->ticks[0]=0;
  p->ticks[1]=0;
  p->ticks[2]=0;
  p->ticks[3]=0;
  p->ticks[4]=0;
  p->waittime=15;
  queue_popu[0]++; //deafult schedule(RR) is size updated
  queue[0][queue_popu[0]]=p;
  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to enter executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
  //Waitx code is initialized here
  acquire(&ptable.lock);
  p-> ctime = ticks; // limited/locked the value of ticks
  p-> etime =0;
  p->iotime = 0;
  p-> rtime =0;
  p-> num_run=0;
  release(&ptable.lock);
  p-> priority=60;//deafult

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  p->enter=ticks; //now (sun 13:40) I added

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np-> enter =ticks;
  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  curproc->etime = ticks; // protect with a lock so val of ticks doesn't change
  cprintf("Total Time taken: %d\n", curproc->etime - curproc->ctime);
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        #ifdef MLFQ
        remove_proc_from_queue(p->pid,p->cur_queue); //Removing zombie processses from the queue
        #endif
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//     ****waitx function enters here ****

int waitx(int *wtime ,int *rtime)
{
  struct proc *p;
  int havekids,pid;
  struct proc *curproc = myproc();
  acquire(&ptable.lock);
  while(1)
  {
    havekids=0;
    for(p=ptable.proc;p< &ptable.proc[NPROC];p++)
    {
      if(p-> parent != curproc)continue;
      havekids=1;
      if(p -> state == ZOMBIE)
      {
        *rtime = p-> rtime;
        *wtime = p-> etime - p->ctime - p-> rtime - p->iotime;
        pid = p-> pid;
        kfree(p-> kstack);
        p-> kstack =0;
        freevm(p-> pgdir);
        #ifdef MLFQ
        remove_proc_from_queue(p->pid,p->cur_queue); //Removing zombie processses from the queue
        #endif
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }
    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to enter running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p,*cp_p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    #ifdef MLFQ
    acquire(&ptable.lock);
    ageing();
    struct proc *m_proc;
    int i, j;
    if (queue_popu[0] != -1) // ENTERING QUEUE 0
    {
      for (i = 0; i <= queue_popu[0]; i++)
      {
        if (queue[0][i]->state != RUNNABLE)
          continue;
        queue[0][i]->enter=ticks;
        m_proc = queue[0][i];
        c->proc = m_proc;
        switchuvm(m_proc);
        m_proc->state = RUNNING;
        m_proc->num_run++;
        swtch((&c->scheduler), m_proc->context);
        switchkvm();
        c->proc = 0;
        //Make sure you do not exceed given time period and wait time
        if ((m_proc->ticks[0] >= clockperiod[0]) || m_proc->killed!=0)
        {
          //copying  proc to lower priority queue
          if (m_proc->killed == 0)
          {
            queue_popu[1]++;
            m_proc->cur_queue++;
            queue[1][queue_popu[1]] = m_proc;
          }
          //deleting proc from queue[0]
          for (j = i; j <= queue_popu[0] - 1; j++)
            queue[0][j] = queue[0][j + 1];
          m_proc->ticks[0] = 0;
          queue_popu[0]--;
        }
      }
    }
    if (queue_popu[1] != -1) //ENTERING QUEUE 1
    {
      for (i = 0; i <= queue_popu[1]; i++)
      {
        if (queue[1][i]->state != RUNNABLE)
          continue;
        queue[1][i]->enter=ticks;
        m_proc = queue[1][i];
        c->proc = m_proc;
        switchuvm(m_proc);
        m_proc->state = RUNNING;
        m_proc->num_run++;
        swtch(&c->scheduler, m_proc->context);
        switchkvm();
        c->proc = 0;
        if (m_proc->ticks[1] >= clockperiod[1] || m_proc->killed!=0)
        {
          // copying proc to lower priority queue
          if (m_proc->killed == 0)
          {
            queue_popu[2]++;
            m_proc->cur_queue++;
            queue[2][queue_popu[2]] = m_proc;
          }
          /*deleting proc from queue[1]*/
          for (j = i; j <= queue_popu[1] - 1; j++)
            queue[1][j] = queue[1][j + 1];
          m_proc->ticks[1] = 0;
          queue_popu[1]--;
        }
      }
    }
    if (queue_popu[2] != -1)
    {
      for (i = 0; i <= queue_popu[2]; i++)
      {
        if (queue[2][i]->state != RUNNABLE)
          continue;
        queue[2][i]->enter=ticks;
        m_proc = queue[2][i];
        c->proc = m_proc;
        switchuvm(m_proc);
        m_proc->state = RUNNING;
        m_proc->num_run++;
        swtch(&c->scheduler, m_proc->context);
        switchkvm();
        c->proc = 0;
        if (m_proc->ticks[2] >= clockperiod[2]|| m_proc->killed!=0)
        {
          //  copying proc to lower priority queue
          if (m_proc->killed == 0)
          {
            queue_popu[3]++;
            m_proc->cur_queue++;
            queue[3][queue_popu[3]] = m_proc;
          }
          /*deleting proc from queue[2]*/
          for (j = i; j <= queue_popu[2] - 1; j++)
            queue[2][j] = queue[2][j + 1];
          m_proc->ticks[2] = 0;
          queue_popu[2]--;
        }
      }
    }
    if (queue_popu[3] != -1)
    {
      for (i = 0; i <= queue_popu[3]; i++)
      {
        if (queue[3][i]->state != RUNNABLE)
          continue;
        queue[3][i]->enter=ticks;
        m_proc = queue[3][i];
        c->proc = m_proc;
        switchuvm(m_proc);
        m_proc->state = RUNNING;
        m_proc->num_run++;
        swtch(&c->scheduler, m_proc->context);
        switchkvm();
        c->proc = 0;
        if (m_proc->ticks[3] >= clockperiod[3] || m_proc->killed!=0)
        {

          // copying proc to lower priority queue
          if (m_proc->killed == 0)
          {
            queue_popu[4]++;
            m_proc->cur_queue++;
            queue[4][queue_popu[4]] = m_proc;
          }
          /*deleting proc from queue[1]*/
          for (j = i; j <= queue_popu[3] - 1; j++)
            queue[3][j] = queue[3][j + 1];
          m_proc->ticks[3] = 0;
          queue_popu[3]--;
        }
      }
    }
    if (queue_popu[4] != -1)
    {
      for (i = 0; i <= queue_popu[4]; i++)
      {
        if (queue[4][i]->state != RUNNABLE)
          continue;
        queue[4][i]->enter=ticks;
        m_proc = queue[4][i];
        c->proc = m_proc;
        switchuvm(m_proc);
        m_proc->state = RUNNING;
        m_proc->num_run++;
        swtch(&c->scheduler, m_proc->context);
        switchkvm();
        c->proc = 0;

        if(m_proc->killed!=0)
        {
          for (j = i; j <= queue_popu[4] - 1; j++)
            queue[4][j] = queue[4][j + 1];
        }

        else if (m_proc->killed == 0)
        {
          for (j = i; j <= queue_popu[4] - 1; j++)
            queue[4][j] = queue[4][j + 1];
          queue[4][queue_popu[4]] = m_proc;
        }
      }
    }
    release(&ptable.lock);

#else
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
  #ifdef RR
      if (p->state != RUNNABLE)
        continue;
      p->waittime=0;
  #else
  #ifdef PBS

      struct proc *highest_priority = 0;
      struct proc *p1 = 0;
      if (p->state != RUNNABLE)continue;
      p->waittime=0;highest_priority = p;
      for (p1 = ptable.proc; p1 < &ptable.proc[NPROC]; p1++)
      {
        if ((p1->state == RUNNABLE) && (highest_priority->priority > p1->priority))
          highest_priority = p1;
      }

      p = highest_priority;

  #else
  #ifdef FCFS
      struct proc *min_process = p;

      if (p->state != RUNNABLE)
        continue;
      p->waittime =0;
      for (cp_p = ptable.proc; cp_p < &ptable.proc[NPROC]; cp_p++)
      {
        if (cp_p->state != RUNNABLE)continue;
        if (cp_p->ctime < p->ctime) min_process = cp_p;
      }
      p = min_process;

  #endif
  #endif
  #endif
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      p->num_run++;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      // }
    }

    release(&ptable.lock);
#endif
    }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  myproc()->enter=ticks;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
    {
      p->state = RUNNABLE;
      p->enter=ticks;
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
      {
        p->state = RUNNABLE;
        p->enter=ticks;
      }
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

void remove_proc_from_queue(int pid,int curq)
{

  int i=0;
  while(i<=queue_popu[curq])
  {
    if(queue[curq][i]-> pid ==pid)
    {
      for(int j=i;j<=queue_popu[0]-1;j++)queue[curq][j]=queue[curq][j+1];
        queue_popu[curq]--;
    }
    i++;
  }
}

int cps(void)
{
  struct proc *p;
  sti();
  acquire(&ptable.lock);
  cprintf("name \t Pid \t Priority \t State \t \tr_time \t w_time  n_run \t ");
  #ifdef MLFQ
  cprintf("cur_q \t q0 \t q1 \t q2 \t q3 \t q4 \t");
  #endif
  cprintf(" \n\n");
  int i=0;
  for(p=ptable.proc;p<&ptable.proc[NPROC];p++)
  {
    if(p ->state == SLEEPING)
    {
      cprintf("%s \t  %d  \t  %d \t \tSLEEPING \t %d \t  %d \t  %d \t",p-> name ,p-> pid, p-> priority ,p-> rtime,p-> waittime,p-> num_run);
      #ifdef MLFQ
        cprintf("%d \t %d \t %d \t %d \t %d \t %d \t", p-> cur_queue,p->ticks[0],p-> ticks[1],p->ticks[2], p->ticks[3],p-> ticks[4]);
      #endif
      cprintf("\n");  
    }

    else if(p ->state == RUNNING)
    {
      cprintf("%s \t  %d  \t  %d \t \tRUNNING \t %d \t  %d \t  %d \t",p-> name ,p-> pid, p-> priority ,p-> rtime,p-> waittime,p-> num_run);
      #ifdef MLFQ
        cprintf("%d \t %d \t %d \t %d \t %d \t %d \t", p-> cur_queue,p->ticks[0],p-> ticks[1],p->ticks[2], p->ticks[3],p-> ticks[4]);
      #endif
      cprintf("\n");}
    else if(p ->state == ZOMBIE)
    {
      cprintf("%s \t  %d  \t  %d \t \tZOMBIE \t %d \t  %d \t  %d \t",p-> name ,p-> pid, p-> priority ,p-> rtime,p-> waittime,p-> num_run);
      #ifdef MLFQ
        cprintf("%d \t %d \t %d \t %d \t %d \t %d \t", p-> cur_queue,p->ticks[0],p-> ticks[1],p->ticks[2], p->ticks[3],p-> ticks[4]);
      #endif
      cprintf("\n");}
    i++;
  }
  release(&ptable.lock);
  return 23;
}


void ageing(void)
{
  //checking queue
  for(int index=1;index<=4;index++)
  {
    for(int i=0;i<=queue_popu[index];i++)
    {
      if(queue[index][i]->state!=RUNNABLE)continue;
      if((ticks-queue[index][i]->enter)>queue[index][i]->waittime)
      {
          cprintf("%s %d switching to queue %d  as waittime %ds exceeded\n\033[0m","\033[1;32m",queue[1][i]->pid,index-1,queue[1][i]->waittime);
          queue_popu[index-1]++;
          queue[index][i]->cur_queue--;
          queue[index][i]->ticks[queue[index][i]->cur_queue]=0;
          queue[index][i]->enter=ticks;
          queue[index-1][queue_popu[index-1]]=queue[index][i];
          queue[index-1][i]-> waittime =0;
          remove_proc_from_queue(queue[index][i]->pid,queue[index][i]->cur_queue);
      }
    }
  }
}

int set_priority(int priority,int pid)
{
  struct proc *p;
  int to_yield = 0, old_priority = 0;
  if(priority<0 || priority>100)return -3;
  
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if(p->pid == pid)
    {
      to_yield = 0;
      acquire(&ptable.lock);
      old_priority = p->priority;p->priority = priority;
      cprintf("\033[0;34mChanged priority of process %d from %d to %d\n", p->pid, old_priority, p->priority);
      if (old_priority > p->priority)to_yield = 1;
      release(&ptable.lock);
      break;
    }
  }
    if (to_yield == 1)yield(); 
    return old_priority;
}
