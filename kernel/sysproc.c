#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "memstat.h"

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
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;

  // Always use lazy allocation through growproc()
  // growproc() will create lazy PTEs for growth or free pages for shrinking
  if(growproc(n) < 0) {
    return -1;
  }
  
  return addr;
}

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

// Helper function to count swapped pages
static int
count_swapped_pages(struct proc *p)
{
  int count = 0;
  pte_t *pte;
  
  // Iterate through all possible pages in the process address space
  for (uint64 va = p->text_start; va < p->sz; va += PGSIZE) {
    pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_SWAPPED)) {
      count++;
    }
  }
  
  return count;
}

// memstat() system call implementation (Phase 6)
uint64
sys_memstat(void)
{
  uint64 addr;
  
  // Get the user-space address where to copy the result
  argaddr(0, &addr);
  
  struct proc_mem_stat info;
  struct proc *p = myproc();
  
  // Fill in basic info
  info.pid = p->pid;
  info.next_fifo_seq = p->next_fifo_seq;
  info.num_resident_pages = p->num_resident;
  info.num_swapped_pages = count_swapped_pages(p);
  
  // Calculate total pages (from text start to current size)
  info.num_pages_total = (p->sz - p->text_start) / PGSIZE;
  
  // Fill page information for each page
  int page_count = 0;
  for (uint64 va = p->text_start; 
       va < p->sz && page_count < MAX_PAGES_INFO; 
       va += PGSIZE) {
    
    info.pages[page_count].va = va;
    
    // Get PTE for this virtual address
    pte_t *pte = walk(p->pagetable, va, 0);
    
    if (pte && (*pte & PTE_V)) {
      // Page is resident (valid)
      info.pages[page_count].state = RESIDENT;
      
      // Find in resident set to get dirty bit and sequence
      int idx;
      if (find_resident_page(p, va, &idx)) {
        info.pages[page_count].is_dirty = p->resident_pages[idx].is_dirty;
        info.pages[page_count].seq = p->resident_pages[idx].seq;
        info.pages[page_count].swap_slot = -1;
      } else {
        // Shouldn't happen, but handle gracefully
        info.pages[page_count].is_dirty = 0;
        info.pages[page_count].seq = -1;
        info.pages[page_count].swap_slot = -1;
      }
    } else if (pte && (*pte & PTE_SWAPPED)) {
      // Page is swapped out
      info.pages[page_count].state = SWAPPED;
      info.pages[page_count].swap_slot = (*pte >> 10) & 0x3FF;  // Extract slot from PTE
      info.pages[page_count].is_dirty = 0;  // Not in memory
      info.pages[page_count].seq = -1;      // Not in resident set
    } else {
      // Page is unmapped (lazy allocated but not yet faulted)
      info.pages[page_count].state = UNMAPPED;
      info.pages[page_count].is_dirty = 0;
      info.pages[page_count].seq = -1;
      info.pages[page_count].swap_slot = -1;
    }
    
    page_count++;
  }
  
  // Copy the filled structure to user space
  if (copyout(p->pagetable, addr, (char *)&info, sizeof(info)) < 0)
    return -1;
  
  return 0;
}
