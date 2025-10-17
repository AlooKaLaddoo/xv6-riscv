#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

// Page replacement algorithm selection (Bonus Phase 8)
// Comment out to use FIFO, uncomment to use Clock
// #define USE_CLOCK_ALGORITHM

// Forward declarations for page replacement (Phase 3)
static int handle_page_replacement(void);
static void write_to_swap(struct proc *p, uint64 va, int slot);
static void read_from_swap(struct proc *p, uint64 va, int slot, uint64 pa);

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  struct proc *p = myproc();

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0) {  // has physical page been allocated?
      // Check if it's a swapped page and free the swap slot
      if (p && (*pte & PTE_SWAPPED)) {
        int slot = (*pte >> 10) & 0x3FF;  // Extract slot number
        free_swap_slot(p, slot);
        *pte = 0;
      }
      continue;
    }
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
      
      // Remove from resident set if this is the current process
      if (p && p->pagetable == pagetable) {
        remove_from_resident_set(p, a);
      }
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Lazily grow user memory from oldsz to newsz by creating page table
// entries marked as PTE_LAZY without allocating physical memory.
// Returns newsz on success, 0 on failure.
uint64
uvmlazygrow(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  uint64 a;
  pte_t *pte;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    // Create PTE without allocating physical memory
    if((pte = walk(pagetable, a, 1)) == 0){
      // Failed to create page table entry
      return 0;
    }
    if(*pte & PTE_V)
      panic("uvmlazygrow: page already mapped");
    
    // Mark as lazy (not valid, but user-accessible with permissions)
    *pte = PTE_U | PTE_LAZY | (xperm & (PTE_R | PTE_W | PTE_X));
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

// Handle page fault for text/data segments - load from executable
void
handle_exec_fault(uint64 va)
{
  struct proc *p = myproc();
  char *mem;
  pte_t *pte;
  uint64 file_offset = 0;
  int n;
  int found = 0;
  
  va = PGROUNDDOWN(va);
  
  // Allocate physical page
  mem = kalloc();
  if (mem == 0) {
    // Memory full - trigger page replacement (Phase 3)
    if(handle_page_replacement() < 0) {
      // Process was killed due to swap exhaustion or other error
      return;
    }
    mem = kalloc();
    if(mem == 0) {
      // Still no memory after replacement - kill process
      printf("[pid %d] KILL memory-exhausted\n", p->pid);
      setkilled(p);
      return;
    }
  }
  
  // Zero-fill first (in case we don't read a full page)
  memset(mem, 0, PGSIZE);
  
  // Need to find which ELF segment this VA belongs to by reading ELF headers
  struct elfhdr elf;
  struct proghdr ph;
  int i, off;
  
  begin_op();
  ilock(p->exec_inode);
  
  // Read ELF header
  if(readi(p->exec_inode, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) {
    iunlock(p->exec_inode);
    end_op();
    kfree(mem);
    panic("handle_exec_fault: failed to read ELF header");
  }
  
  // Find the program header that contains this virtual address
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(p->exec_inode, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) {
      iunlock(p->exec_inode);
      end_op();
      kfree(mem);
      panic("handle_exec_fault: failed to read program header");
    }
    
    if(ph.type != ELF_PROG_LOAD)
      continue;
    
    // Check if VA is in this segment
    if(va >= ph.vaddr && va < ph.vaddr + ph.memsz) {
      found = 1;
      
      // Calculate offset in this segment
      uint64 seg_offset = va - ph.vaddr;
      
      // Calculate file offset
      file_offset = ph.off + seg_offset;
      
      // Determine how much to read from file
      uint64 bytes_in_file = (seg_offset < ph.filesz) ? (ph.filesz - seg_offset) : 0;
      
      // Read from file (only if there's data in the file for this part)
      if(bytes_in_file > 0) {
        n = (bytes_in_file > PGSIZE) ? PGSIZE : bytes_in_file;
        if(readi(p->exec_inode, 0, (uint64)mem, file_offset, n) != n) {
          iunlock(p->exec_inode);
          end_op();
          kfree(mem);
          panic("handle_exec_fault: failed to read segment data");
        }
      }
      // Rest is already zeroed from memset above
      
      break;
    }
  }
  
  iunlock(p->exec_inode);
  end_op();
  
  if(!found) {
    kfree(mem);
    panic("handle_exec_fault: VA not in any ELF segment");
  }
  
  // Get PTE
  pte = walk(p->pagetable, va, 0);
  if (pte == 0) {
    kfree(mem);
    panic("handle_exec_fault: walk failed");
  }
  
  // Determine permissions based on segment
  int perm = PTE_U;
  if (va >= p->text_start && va < p->text_end) {
    // Text segment: readable and executable (not writable)
    perm |= PTE_R | PTE_X;
  } else {
    // Data segment: initially read-only for dirty tracking (Phase 5)
    // Will become writable on first write fault
    perm |= PTE_R;
  }
  
  // Map the page
  *pte = PA2PTE((uint64)mem) | perm | PTE_V;
  
  // Add to resident set
  add_to_resident_set(p, va, p->next_fifo_seq++, 0, -1);
  
  // Log
  printf("[pid %d] LOADEXEC va=0x%lx\n", p->pid, va);
  printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->next_fifo_seq - 1);
}

// Handle page fault for heap - allocate and zero-fill
void
handle_heap_fault(uint64 va)
{
  struct proc *p = myproc();
  char *mem;
  pte_t *pte;
  
  va = PGROUNDDOWN(va);
  
  // Allocate physical page
  mem = kalloc();
  if (mem == 0) {
    // Memory full - trigger page replacement (Phase 3)
    if(handle_page_replacement() < 0) {
      // Process was killed due to swap exhaustion or other error
      return;
    }
    mem = kalloc();
    if(mem == 0) {
      // Still no memory after replacement - kill process
      printf("[pid %d] KILL memory-exhausted\n", p->pid);
      setkilled(p);
      return;
    }
  }
  
  // Zero-fill
  memset(mem, 0, PGSIZE);
  
  // Get PTE
  pte = walk(p->pagetable, va, 0);
  if (pte == 0) {
    kfree(mem);
    panic("handle_heap_fault: walk failed");
  }
  
  // Map page as read-only initially for dirty tracking (Phase 5)
  // Will become writable on first write fault
  *pte = PA2PTE((uint64)mem) | PTE_V | PTE_U | PTE_R;
  
  // Add to resident set (initially clean)
  add_to_resident_set(p, va, p->next_fifo_seq++, 0, -1);
  
  // Log
  printf("[pid %d] ALLOC va=0x%lx\n", p->pid, va);
  printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->next_fifo_seq - 1);
}

// Handle page fault for stack - validate and allocate
void
handle_stack_fault(uint64 va)
{
  struct proc *p = myproc();
  uint64 sp = p->trapframe->sp;
  
  va = PGROUNDDOWN(va);
  
  // Check if within one page below SP and below stack top
  if (va < sp - PGSIZE || va >= p->stack_top) {
    // Invalid stack access
    printf("[pid %d] KILL invalid-stack-access va=0x%lx sp=0x%lx\n", 
           p->pid, va, sp);
    setkilled(p);
    return;
  }
  
  // Valid stack access - same as heap allocation
  handle_heap_fault(va);
}

// Mark page as dirty in resident set (Phase 5)
void
mark_page_dirty(struct proc *p, uint64 va)
{
  int idx;
  va = PGROUNDDOWN(va);
  
  if (find_resident_page(p, va, &idx)) {
    p->resident_pages[idx].is_dirty = 1;
  }
}

// Select FIFO victim - page with lowest sequence number
// Returns virtual address of victim page
static uint64
select_fifo_victim(struct proc *p)
{
  if (p->num_resident == 0) {
    panic("select_fifo_victim: no resident pages to evict");
  }
  
  // Find page with lowest sequence number (oldest page)
  int victim_idx = 0;
  uint min_seq = p->resident_pages[0].seq;
  
  for (int i = 1; i < p->num_resident; i++) {
    // Handle wraparound: (int)(seq1 - seq2) < 0 means seq1 is older
    if ((int)(p->resident_pages[i].seq - min_seq) < 0) {
      min_seq = p->resident_pages[i].seq;
      victim_idx = i;
    }
  }
  
  return p->resident_pages[victim_idx].va;
}

// Clock algorithm victim selection (Bonus Phase 8)
// Uses second-chance algorithm with PTE_A (access bit)
// Circular scan giving accessed pages a second chance
static uint64 __attribute__((unused))
select_clock_victim(struct proc *p)
{
  if (p->num_resident == 0) {
    panic("select_clock_victim: no resident pages to evict");
  }
  
  // Ensure clock_hand is within bounds
  if (p->clock_hand >= p->num_resident) {
    p->clock_hand = 0;
  }
  
  int scanned = 0;  // Track how many pages we've scanned
  int max_scans = p->num_resident * 2;  // Allow up to 2 full rotations
  
  // Circular scan with second-chance
  while (scanned < max_scans) {
    int current_idx = p->clock_hand;
    
    uint64 va = p->resident_pages[current_idx].va;
    pte_t *pte = walk(p->pagetable, va, 0);
    
    if (pte == 0 || (*pte & PTE_V) == 0) {
      panic("select_clock_victim: invalid PTE");
    }
    
    // Check access bit (PTE_A)
    if (*pte & PTE_A) {
      // Give second chance: clear access bit and move on
      *pte &= ~PTE_A;
      p->clock_hand = (p->clock_hand + 1) % p->num_resident;
      scanned++;
    } else {
      // Not accessed recently - this is our victim
      // Update clock hand to next position for next eviction
      p->clock_hand = (p->clock_hand + 1) % p->num_resident;
      return va;
    }
  }
  
  // Safety: if all pages were accessed (shouldn't happen after 2 rotations)
  // Return page at current clock hand
  int victim_idx = p->clock_hand;
  p->clock_hand = (p->clock_hand + 1) % p->num_resident;
  return p->resident_pages[victim_idx].va;
}

// Write page contents to swap file at specified slot (Phase 4.3)
// va: virtual address of page to swap out
// slot: swap slot number (0-1023)
static void
write_to_swap(struct proc *p, uint64 va, int slot)
{
  pte_t *pte;
  uint64 pa;
  uint64 offset;
  int n;
  
  va = PGROUNDDOWN(va);
  
  // Get PTE and physical address
  pte = walk(p->pagetable, va, 0);
  if (pte == 0 || (*pte & PTE_V) == 0) {
    panic("write_to_swap: page not valid");
  }
  
  pa = PTE2PA(*pte);
  
  // Calculate offset in swap file (slot * page size)
  offset = slot * PGSIZE;
  
  // Write page to swap file
  begin_op();
  ilock(p->swapfile->ip);
  
  // Write PGSIZE bytes from physical address to swap file at offset
  if((n = writei(p->swapfile->ip, 0, pa, offset, PGSIZE)) != PGSIZE) {
    iunlock(p->swapfile->ip);
    end_op();
    panic("write_to_swap: writei failed");
  }
  
  iunlock(p->swapfile->ip);
  end_op();
}

// Read page contents from swap file at specified slot (Phase 4.4)
// va: virtual address of page to swap in
// slot: swap slot number (0-1023)
// pa: physical address where to load the page
static void
read_from_swap(struct proc *p, uint64 va, int slot, uint64 pa)
{
  uint64 offset;
  int n;
  
  va = PGROUNDDOWN(va);
  
  // Calculate offset in swap file (slot * page size)
  offset = slot * PGSIZE;
  
  // Read page from swap file
  begin_op();
  ilock(p->swapfile->ip);
  
  // Read PGSIZE bytes from swap file at offset to physical address
  if((n = readi(p->swapfile->ip, 0, pa, offset, PGSIZE)) != PGSIZE) {
    iunlock(p->swapfile->ip);
    end_op();
    panic("read_from_swap: readi failed");
  }
  
  iunlock(p->swapfile->ip);
  end_op();
}

// Handle swap-in operation (Phase 4.4)
// Called when a page fault occurs on a swapped-out page
void
handle_swap_fault(uint64 va)
{
  struct proc *p = myproc();
  pte_t *pte;
  char *mem;
  int slot;
  
  va = PGROUNDDOWN(va);
  
  // Get PTE to extract swap slot number
  pte = walk(p->pagetable, va, 0);
  if (pte == 0 || (*pte & PTE_SWAPPED) == 0) {
    panic("handle_swap_fault: page not swapped");
  }
  
  // Extract slot number from PTE (stored in bits 10 and above)
  slot = (*pte >> 10) & 0x3FF;  // 0x3FF = 1023, max slot number
  
  // Allocate physical page
  mem = kalloc();
  if (mem == 0) {
    // Memory full - trigger page replacement
    if(handle_page_replacement() < 0) {
      // Process was killed due to swap exhaustion or other error
      return;
    }
    mem = kalloc();
    if(mem == 0) {
      // Still no memory after replacement - kill process
      printf("[pid %d] KILL memory-exhausted\n", p->pid);
      setkilled(p);
      return;
    }
  }
  
  // Read page from swap file
  read_from_swap(p, va, slot, (uint64)mem);
  
  // Free the swap slot
  free_swap_slot(p, slot);
  
  // Determine permissions based on address range (Phase 5)
  // Initially map as read-only for dirty tracking
  int perm = PTE_U | PTE_V;
  if (va >= p->text_start && va < p->text_end) {
    // Text segment: readable and executable (not writable)
    perm |= PTE_R | PTE_X;
  } else {
    // Data/heap/stack: readable only, will become writable on first write
    perm |= PTE_R;
  }
  
  // Map page into page table
  *pte = PA2PTE((uint64)mem) | perm;
  
  // Add to resident set with new FIFO sequence
  add_to_resident_set(p, va, p->next_fifo_seq++, 0, -1);
  
  // Log swap-in
  printf("[pid %d] SWAPIN va=0x%lx slot=%d\n", p->pid, va, slot);
  printf("[pid %d] RESIDENT va=0x%lx seq=%d\n", p->pid, va, p->next_fifo_seq - 1);
}

// Handle page replacement when memory is full (Phase 3)
// Returns 0 on success, -1 on failure
static int
handle_page_replacement(void)
{
  struct proc *p = myproc();
  pte_t *pte;
  uint64 pa;
  uint64 victim_va;
  int idx;
  uint victim_seq;
  int is_dirty;
  const char *algo_name;
  int attempts = 0;
  const int MAX_ATTEMPTS = 10;  // Prevent infinite loops
  
  // Log memory full
  printf("[pid %d] MEMFULL\n", p->pid);
  
  // Loop to find a valid victim page
  while (attempts < MAX_ATTEMPTS) {
    // Check if there are any resident pages to evict
    if (p->num_resident == 0) {
      printf("[pid %d] KILL no-pages-to-evict\n", p->pid);
      setkilled(p);
      return -1;
    }
    
    // Select victim using configured algorithm
#ifdef USE_CLOCK_ALGORITHM
    victim_va = select_clock_victim(p);
    algo_name = "CLOCK";
#else
    victim_va = select_fifo_victim(p);
    algo_name = "FIFO";
#endif
    
    // Find victim in resident set to get its metadata
    if (!find_resident_page(p, victim_va, &idx)) {
      panic("handle_page_replacement: victim not in resident set");
    }
    
    victim_seq = p->resident_pages[idx].seq;
    is_dirty = p->resident_pages[idx].is_dirty;
    
    // Log victim selection
    printf("[pid %d] VICTIM va=0x%lx seq=%d algo=%s\n",
           p->pid, victim_va, victim_seq, algo_name);
    
    // Log eviction with state
    printf("[pid %d] EVICT va=0x%lx state=%s\n",
           p->pid, victim_va, is_dirty ? "dirty" : "clean");
    
    // Get PTE and physical address before eviction
    pte = walk(p->pagetable, victim_va, 0);
    if (pte == 0 || (*pte & PTE_V) == 0) {
      // Page is not valid - already evicted or never allocated
      // Just remove from resident set and try next victim
      printf("[pid %d] WARNING: victim page 0x%lx not valid, removing from resident set\n", p->pid, victim_va);
      remove_from_resident_set(p, victim_va);
      attempts++;
      continue;  // Try next victim
    }
    
    // Found a valid victim, break out of loop
    break;
  }
  
  // Check if we exhausted attempts
  if (attempts >= MAX_ATTEMPTS) {
    printf("[pid %d] KILL too-many-invalid-victims\n", p->pid);
    setkilled(p);
    return -1;
  }
  
  pa = PTE2PA(*pte);
  
  // Handle eviction based on dirty state
  if (is_dirty) {
    // Dirty page needs to be swapped out (Phase 4)
    if (p->swapfile == 0) {
      // Swap not initialized - cannot swap out
      printf("[pid %d] KILL swap-not-initialized\n", p->pid);
      setkilled(p);
      return -1;
    }
    
    // Allocate a swap slot
    int slot = allocate_swap_slot(p);
    if (slot < 0) {
      // Swap full - no free slots
      printf("[pid %d] SWAPFULL\n", p->pid);
      printf("[pid %d] KILL swap-exhausted\n", p->pid);
      setkilled(p);
      return -1;
    }
    
    // Write page to swap file
    write_to_swap(p, victim_va, slot);
    
    // Log swap-out
    printf("[pid %d] SWAPOUT va=0x%lx slot=%d\n", p->pid, victim_va, slot);
    
    // Mark PTE as swapped (store slot number in PTE)
    // Clear PTE_V but set PTE_SWAPPED and store slot in upper bits
    *pte = (slot << 10) | PTE_SWAPPED | PTE_U;
    
  } else {
    // Clean page - just discard it
    printf("[pid %d] DISCARD va=0x%lx\n", p->pid, victim_va);
    
    // Mark PTE as not present but lazy (can be reloaded from exec or re-allocated)
    // Determine if this is a text/data page or heap/stack page
    if (victim_va >= p->text_start && victim_va < p->data_end) {
      // Text/data - can be reloaded from executable
      *pte = PTE_U | PTE_LAZY;
    } else {
      // Heap/stack - mark as lazy
      *pte = PTE_U | PTE_LAZY | PTE_R | PTE_W;
    }
  }
  
  // Free the physical page
  kfree((void *)pa);
  
  // Remove from resident set
  remove_from_resident_set(p, victim_va);
  
  return 0;
}
