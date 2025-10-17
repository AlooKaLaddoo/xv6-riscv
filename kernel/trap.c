#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "memstat.h"

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

// Helper function to check if a page is swapped
int
is_swapped_page(struct proc *p, uint64 va)
{
  va = PGROUNDDOWN(va);
  
  // Don't call walk() on invalid addresses (>= MAXVA or kernel addresses)
  if (va >= MAXVA || va >= KERNBASE) {
    return 0;
  }
  
  pte_t *pte = walk(p->pagetable, va, 0);
  if (pte && (*pte & PTE_SWAPPED)) {
    return 1;
  }
  return 0;
}

// Step 2.5: Check if a page fault address is in a valid range
// Valid ranges are:
// - Text segment [text_start, text_end)
// - Data segment [data_start, data_end)
// - Heap [heap_start, sz)
// - Stack (one page below SP up to stack_top)
// - Swapped pages
int
is_valid_access(struct proc *p, uint64 va)
{
  va = PGROUNDDOWN(va);
  
  // Reject kernel addresses (>= MAXVA or >= KERNBASE)
  // KERNBASE is 0x80000000 where kernel memory starts
  if (va >= MAXVA || va >= KERNBASE) return 0;
  
  // Check text segment
  if (va >= p->text_start && va < p->text_end) return 1;
  
  // Check data segment
  if (va >= p->data_start && va < p->data_end) return 1;
  
  // Check heap (up to process size)
  if (va >= p->heap_start && va < p->sz) return 1;
  
  // Check stack (one page below SP up to stack_top)
  uint64 sp = p->trapframe->sp;
  if (va >= sp - PGSIZE && va < p->stack_top) return 1;
  
  // Check if swapped
  if (is_swapped_page(p, va)) return 1;
  
  return 0;
}

// Classify the cause of a page fault
char*
classify_fault_cause(struct proc *p, uint64 va)
{
  va = PGROUNDDOWN(va);
  
  // Check if swapped
  if (is_swapped_page(p, va)) {
    return "swap";
  }
  
  // Check text segment
  if (va >= p->text_start && va < p->text_end) {
    return "exec";
  }
  
  // Check data segment
  if (va >= p->data_start && va < p->data_end) {
    return "exec";
  }
  
  // Check heap
  if (va >= p->heap_start && va < p->sz) {
    return "heap";
  }
  
  // Check stack
  uint64 sp = p->trapframe->sp;
  if (va >= sp - PGSIZE && va < p->stack_top) {
    return "stack";
  }
  
  return "invalid";
}

// Handle page fault (Steps 2.3, 2.4, 2.5, Phase 5)
void
handle_page_fault(struct proc *p, uint64 va, uint64 scause)
{
  va = PGROUNDDOWN(va);
  
  // Phase 5: Check for write fault on read-only page (dirty tracking)
  // This happens when a page is valid but not writable (first write to clean page)
  if (scause == 15 && va < MAXVA && va < KERNBASE) {  // Store/write page fault on valid address
    pte_t *pte = walk(p->pagetable, va, 0);
    
    // If page is valid but not writable, this is first write - mark dirty
    if (pte && (*pte & PTE_V) && !(*pte & PTE_W)) {
      // Check if this is a legitimate writable region (not text segment)
      if (!(va >= p->text_start && va < p->text_end)) {
        // Mark page as dirty
        mark_page_dirty(p, va);
        
        // Make page writable
        *pte |= PTE_W;
        
        // Flush TLB for this page
        sfence_vma();
        
        // Continue execution - no need to log, this is transparent
        return;
      }
      // If trying to write to text segment, fall through to error handling
    }
  }
  
  // Determine access type (Step 2.3)
  char *access_type;
  if (scause == 12) {
    access_type = "exec";
  } else if (scause == 13) {
    access_type = "read";
  } else {  // scause == 15
    access_type = "write";
  }
  
  // Classify fault cause (Step 2.3)
  char *cause = classify_fault_cause(p, va);
  
  // Log page fault (Step 2.3)
  printf("[pid %d] PAGEFAULT va=0x%lx access=%s cause=%s\n",
         p->pid, va, access_type, cause);
  
  // Step 2.5: Handle invalid page faults
  // Check if access is to a valid address range
  if (!is_valid_access(p, va)) {
    // Invalid access - log and terminate process
    printf("[pid %d] KILL invalid-access va=0x%lx access=%s\n",
           p->pid, va, access_type);
    setkilled(p);
    return;
  }
  
  // Step 2.5: Check for invalid execute access to non-executable regions
  if (scause == 12) {  // Instruction page fault
    // Only text segment can be executed
    if (!(va >= p->text_start && va < p->text_end)) {
      printf("[pid %d] KILL invalid-exec va=0x%lx\n", p->pid, va);
      setkilled(p);
      return;
    }
  }
  
  // Step 2.5: Check for invalid write access to read-only (text) segment
  if (scause == 15) {  // Store/write page fault
    // Cannot write to text segment
    if (va >= p->text_start && va < p->text_end) {
      printf("[pid %d] KILL write-to-text va=0x%lx\n", p->pid, va);
      setkilled(p);
      return;
    }
  }
  
  // Call appropriate handler based on cause
  if (is_swapped_page(p, va)) {
    // Swapped page - swap in from disk (Phase 4.4)
    handle_swap_fault(va);
  } else if ((va >= p->text_start && va < p->text_end) || 
             (va >= p->data_start && va < p->data_end)) {
    // Text or data segment - load from executable
    handle_exec_fault(va);
  } else if (va >= p->heap_start && va < p->sz) {
    // Heap - allocate and zero-fill
    handle_heap_fault(va);
  } else if (va >= p->trapframe->sp - PGSIZE && va < p->stack_top) {
    // Stack - validate and allocate
    handle_stack_fault(va);
  } else {
    // Should not reach here if is_valid_access works correctly
    printf("[pid %d] KILL unexpected-fault va=0x%lx\n", p->pid, va);
    setkilled(p);
  }
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
  } else if((which_dev = devintr()) != 0){
    // ok
  } else if(r_scause() == 12 || r_scause() == 13 || r_scause() == 15) {
    // Page fault: 12 = instruction, 13 = load, 15 = store
    uint64 va = r_stval();
    handle_page_fault(p, va, r_scause());
  } else {
    printf("usertrap(): unexpected scause 0x%lx pid=%d\n", r_scause(), p->pid);
    printf("            sepc=0x%lx stval=0x%lx\n", r_sepc(), r_stval());
    setkilled(p);
  }

  if(killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

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
  if(which_dev == 2 && myproc() != 0)
    yield();

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

