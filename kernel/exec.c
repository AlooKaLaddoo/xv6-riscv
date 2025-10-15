#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "fs.h"
#include "stat.h"
#include "file.h"

static int loadseg(pde_t *, uint64, struct inode *, uint, uint);
static int uvmlazymappages(pagetable_t pagetable, uint64 va, uint64 size, int perm);
static int create_swap_file(struct proc *p);

// map ELF permissions to PTE permission bits.
int flags2perm(int flags)
{
    int perm = 0;
    if(flags & 0x1)
      perm = PTE_X;
    if(flags & 0x2)
      perm |= PTE_W;
    return perm;
}

//
// the implementation of the exec() system call
//
int
kexec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();

  begin_op();

  // Open the executable file.
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);

  // Read the ELF header.
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf)) {
    goto bad;
  }

  // Is this really an ELF file?
  if(elf.magic != ELF_MAGIC) {
    goto bad;
  }

  if((pagetable = proc_pagetable(p)) == 0) {
    goto bad;
  }



  // LAZY LOADING: Create lazy page mappings instead of loading program into memory
  // Track segment boundaries for page fault handler
  uint64 text_start = 0xffffffffffffffff, text_end = 0;
  uint64 data_start = 0xffffffffffffffff, data_end = 0;
  
  
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph)) {
      goto bad;
    }
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.memsz < ph.filesz) {
      goto bad;
    }
    if(ph.vaddr + ph.memsz < ph.vaddr) {
      goto bad;
    }
    if(ph.vaddr % PGSIZE != 0) {
      goto bad;
    }
    
    
    // Determine permissions
    int perm = flags2perm(ph.flags);
    
    // Track segment boundaries
    uint64 seg_start = PGROUNDDOWN(ph.vaddr);
    uint64 seg_end = PGROUNDUP(ph.vaddr + ph.memsz);
    
    // Classify as text or data based on permissions
    if(perm & PTE_X) {
      // Executable segment (text)
      if(seg_start < text_start) text_start = seg_start;
      if(seg_end > text_end) text_end = seg_end;
    } else {
      // Non-executable segment (data)
      if(seg_start < data_start) data_start = seg_start;
      if(seg_end > data_end) data_end = seg_end;
    }
    
    // Create lazy mappings (NO physical memory allocation)
    if(uvmlazymappages(pagetable, ph.vaddr, ph.memsz, perm | PTE_R) < 0) {
      goto bad;
    }
    
    // Update size
    if(ph.vaddr + ph.memsz > sz)
      sz = ph.vaddr + ph.memsz;
  }
  

  // Keep reference to executable inode for lazy loading
  p->exec_inode = idup(ip);
  if(p->exec_inode == 0) {
    goto bad;
  }
  
  iunlockput(ip);
  end_op();
  ip = 0;

  p = myproc();
  uint64 oldsz = p->sz;


  // Set up stack region
  sz = PGROUNDUP(sz);
  uint64 stack_start = sz;
  uint64 stack_pages = USERSTACK + 1; // +1 for guard page
  

  // Create guard page (no permissions)
  pte_t *pte;
  if((pte = walk(pagetable, stack_start, 1)) == 0) {
    goto bad;
  }
  *pte = PTE_U; // Guard page: user but not valid
  
  // Allocate physical pages for the top of the stack where arguments go
  // We'll allocate USERSTACK pages (all stack pages need to be physical for arguments)
  sz = stack_start + stack_pages * PGSIZE;
  sp = sz;
  stackbase = sp - USERSTACK*PGSIZE;
  
  
  char *mem;
  for(int j = 0; j < USERSTACK; j++) {
    // Allocate from the top down: sp - (j+1)*PGSIZE
    uint64 stack_va = PGROUNDDOWN(sp - (j+1)*PGSIZE);
    if((mem = kalloc()) == 0) {
      goto bad;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, stack_va, PGSIZE, (uint64)mem, PTE_U | PTE_R | PTE_W) != 0) {
      kfree(mem);
      goto bad;
    }
  }
  
  // No lazy stack pages needed since USERSTACK is small (1 page)
  // If USERSTACK were larger, we'd create lazy mappings here


  // Copy argument strings into new stack, remember their
  // addresses in ustack[].
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG) {
      goto bad;
    }
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase) {
      goto bad;
    }
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0) {
      goto bad;
    }
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push a copy of ustack[], the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase) {
    goto bad;
  }
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0) {
    goto bad;
  }

  // a0 and a1 contain arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
  
  // Initialize demand paging fields
  p->text_start = (text_start != 0xffffffffffffffff) ? text_start : 0;
  p->text_end = text_end;
  p->data_start = (data_start != 0xffffffffffffffff) ? data_start : text_end;
  p->data_end = data_end;
  p->heap_start = sz; // Heap starts after stack
  p->stack_top = MAXVA;
  
  // Initialize FIFO sequence counter
  p->next_fifo_seq = 0;
  
  // Initialize resident set tracking
  p->num_resident = 0;
  for(i = 0; i < MAX_PAGES_INFO; i++) {
    p->resident_pages[i].va = 0;
    p->resident_pages[i].seq = 0;
    p->resident_pages[i].is_dirty = 0;
    p->resident_pages[i].swap_slot = -1;
  }
  
  // Log INIT-LAZYMAP
  printf("[pid %d] INIT-LAZYMAP text=[0x%lx,0x%lx) data=[0x%lx,0x%lx) heap_start=0x%lx stack_top=0x%lx\n",
         p->pid, p->text_start, p->text_end, p->data_start, p->data_end, p->heap_start, p->stack_top);
    
  // Commit to the user image.
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = ulib.c:start()
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);

  // Create per-process swap file (in separate transaction)
  // This is required for Phase 3/4 swapping, but optional for Phase 2
  if(create_swap_file(p) < 0) {
    // Continue without swap file for now
    // Will be needed when we implement swapping in Phase 3
    p->swapfile = 0;
  }

  return argc; // this ends up in a0, the first argument to main(argc, argv)

 bad:
  // Clean up exec_inode if it was set
  if(p->exec_inode){
    iput(p->exec_inode);
    p->exec_inode = 0;
  }
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load an ELF program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int __attribute__((unused))
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}

// Create lazy page table mappings without allocating physical memory
// Maps pages with PTE_U | PTE_LAZY flags (not PTE_V)
static int
uvmlazymappages(pagetable_t pagetable, uint64 va, uint64 size, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    return 0;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if((*pte & PTE_V) || (*pte & PTE_LAZY))
      panic("uvmlazymappages: remap");
    
    // Mark as lazy (not valid, but user-accessible)
    *pte = PTE_U | PTE_LAZY | (perm & (PTE_R | PTE_W | PTE_X));
    
    if(a == last)
      break;
    a += PGSIZE;
  }
  
  return 0;
}

// Create per-process swap file (Step 4.1)
// Creates /pgswpXXXXX where XXXXX is the process PID
static int
create_swap_file(struct proc *p)
{
  struct inode *ip;
  
  // Generate swap file path: /pgswpXXXXX where XXXXX is PID (zero-padded)
  // Example: /pgswp00001, /pgswp00023
  int n = 0;
  p->swappath[n++] = '/';
  p->swappath[n++] = 'p';
  p->swappath[n++] = 'g';
  p->swappath[n++] = 's';
  p->swappath[n++] = 'w';
  p->swappath[n++] = 'p';
  
  // Convert PID to 5-digit zero-padded string
  int pid = p->pid;
  int divisor = 10000;
  for(int i = 0; i < 5; i++) {
    p->swappath[n++] = '0' + ((pid / divisor) % 10);
    divisor /= 10;
  }
  p->swappath[n] = '\0';
  
  // Create the file using the create() function
  begin_op();
  
  // create() returns a locked inode
  if((ip = create(p->swappath, T_FILE, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  
  // Allocate file structure for read/write access
  if((p->swapfile = filealloc()) == 0) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  
  // Set up file structure
  p->swapfile->type = FD_INODE;
  p->swapfile->ip = ip;
  p->swapfile->off = 0;
  p->swapfile->readable = 1;
  p->swapfile->writable = 1;
  
  iunlock(ip);
  end_op();
  
  // Initialize swap bitmap (all slots free)
  // 32 integers * 32 bits = 1024 slots = 4MB swap capacity
  memset(p->swap_bitmap, 0, sizeof(p->swap_bitmap));
  
  return 0;
}

