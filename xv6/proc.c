#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "rand.h"
#include "pstat.h"
#include "ticketlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

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
  
  p->tickets=1;
  p->ticks=0;
  
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

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

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
  np->tickets = curproc->tickets;
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

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
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
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
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

//TODO
//calc total no of tickets for all processes
int
tickets_sum(void){
  struct proc *p;
  int ticketsTotal=0;

//loop over process table and increment total tickets if a runnable process is found 
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if(p->state==RUNNABLE){
      ticketsTotal+=p->tickets;
    }
  }
  return ticketsTotal;          // returning total number of tickets for runnable processes
}
//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  //edit
  long total_tickets = 0, counter = 0, winner = 0;

  
  c->proc = 0;  
  
  // 1 2 3 4 5    6 7 8 9   10 11 12 13 14 
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    
    total_tickets = tickets_sum();
    winner = random_at_most(total_tickets); // say 9
    counter = 0;
    
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;
	
      counter += p->tickets; //5 // 5+4 

      if (counter < winner) { 
            continue;
      }

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();
      p->ticks += 1;

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
      break;
    }
    release(&ptable.lock);
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
      p->state = RUNNABLE;
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
        p->state = RUNNABLE;
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

int 
settickets(int tickets)
{

  if(tickets < 1)
    return -1;
    
  struct proc *proc = myproc();
  
  acquire(&ptable.lock);
  ptable.proc[proc-ptable.proc].tickets = tickets;
  release(&ptable.lock);
  
  return 0;
}

int
getpinfo(struct pstat* ps) {
  int i = 0;
  struct proc *p;
  acquire(&ptable.lock);
  
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) 
  {
    ps->pid[i] = p->pid;
    ps->inuse[i] = p->state != UNUSED;
    ps->tickets[i] = p->tickets;
    ps->ticks[i] = p->ticks;
    i++;
  }
  
  release(&ptable.lock);
  
  return 0;
}

int
clone(void(*fcn)(void*, void*), void *arg1, void *arg2, void *stack)
{
  int i;
  struct proc *np;
  struct proc *curproc = myproc();
  
   //check if the stack address is page-aligned and have at least one page of memory
  if(((uint) stack % PGSIZE) != 0) return -1; 
  if((curproc->sz < PGSIZE + (uint) stack)) return -1;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
	
  // Copy process data to the new thread
  np->pgdir = curproc->pgdir; //make the thread share the parent's address space
  
  np->sz = curproc->sz;  //same size as parent
  
  np->parent = curproc;  //make calling process as the new thread parent 
  
  *np->tf = *curproc->tf;  //parent process and thread have the same trap frame



  uint user_stack[3];
  
  // This sets the first value in the stack, which the base pointer will be set to. It's a "fake" value because there is no previous base pointer.
  user_stack[0] = 0xffffffff;
  
  //This sets the first value after the base pointer to the first function argument. 
  user_stack[1] = (uint) arg1;
  
  //This sets the second value after the base pointer to the second function argument.
  user_stack[2] = (uint) arg2;
  
  // set top of the stack to the allocated page (stack is actually the bottom of the page)
  uint stack_top = (uint) stack + PGSIZE;
  
  // subtract 12 bytes from the stack top to make space for the three values being saved
  stack_top -= 12;
  // copy user stack values to np's memory
  if (copyout(np->pgdir, stack_top, user_stack, 12) < 0) {
	return -1;
  }


  np->tf->esp = (uint) stack;	 //putting the address of stack in the stack pointer
  np->tf->eax = 0;  
  np->threadstack = stack;   //saving the address of the stack
  
  // set stack base and stack pointers for return-from-trap
  // they will be the same value because we are returning into a function
  np->tf->ebp = (uint) stack_top; 	//The base pointer is set to the top of the newly-allocated memory.
  np->tf->esp = (uint) stack_top; 	// The stack pointer is also set to the of the page because that is where our function will enter. 
  

  np->tf->eip = (uint) fcn; 	// This sets the instruction pointer register. This ensures the cloned process will run fcn on start.
  
  
  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;	//This is the return register. The child process returning from clone should get 0 as a return value.
  
  // Duplicate the files used by the current process(thread) to be used 
  // also by the new thread
  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
      
  // Duplicate the current directory to be used by the new thread
  np->cwd = idup(curproc->cwd);
  // Make the two threads belong to the current process
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  acquire(&ptable.lock);
  
  // Make the state of the new thread to be runnable 
  np->state = RUNNABLE;

  release(&ptable.lock);

  return np->pid;
}

int 
join(void** stack) 
{
  struct proc *p;
  int havethreads, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
  
    // Scan through table looking for exited children (zombie children).
    havethreads = 0;
    
    //loop over processes in the process table and check if a process have the same pgdir as the current process 
    //If they share the same page directory it means that the p must be a "child" thread of the parent process (curproc).
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
          // If the current process is not my parent or share the same address space...  
      if(p->parent != curproc || p->pgdir != p->parent->pgdir)
        continue; // You are not a threads
      
      havethreads = 1; 
      
      // free the thread resources
      // the difference between join() and wait() is that join does not free the child thread's address space 
      // Because the child is a thread, this is also the parent's address space! Freeing it would break the parent process.
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        // Removing thread from the kernal stack
        kfree(p->kstack);
        p->kstack = 0;
        
        // Reseting thread from the process table
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        
        stack = p->threadstack;
        p->threadstack = 0;
        
        release(&ptable.lock);
        
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havethreads || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void ticket_sleep(void *chan)
{
	struct proc *p = myproc();

	if (p == 0)
		panic("sleep");

	acquire(&ptable.lock);
	
	p->chan = chan;
	p->state = SLEEPING;
	sched();
	p->chan = 0;
	
	release(&ptable.lock);
}

void initlock_t(struct ticketlock *lk)
{
    lk->next_ticket = 0;
    lk->current_turn = 0;
}

void acquire_t(struct ticketlock *lk)
{
    cli(); //clear inturrupt flag (IF) Disable inturrupts
    int myTicket = fetch_and_add(&lk->next_ticket, 1);
    
    while (lk->current_turn != myTicket)
        ticket_sleep(lk); // to prevent busy waiting.
}

void release_t(struct ticketlock *lk)
{
  fetch_and_add(&lk->current_turn, 1);
  wakeup(lk); // wakup on release and reacquire lock.
  sti(); //set inturrupt flag (IF) Enable inturrupts
}

