// User-space memory statistics structures for memstat() syscall
// Part of PagedOut Inc. demand paging implementation

#ifndef _USER_MEMSTAT_H
#define _USER_MEMSTAT_H

// Maximum number of pages that can be reported per syscall
#define MAX_PAGES_INFO 128

// Page states
#define UNMAPPED 0  // Page is not mapped (lazy/reserved but not allocated)
#define RESIDENT 1  // Page is in physical memory
#define SWAPPED  2  // Page has been swapped out to disk

// Information about a single page
struct page_stat {
  unsigned int va;         // Virtual address (page-aligned)
  int state;               // Page state: UNMAPPED, RESIDENT, or SWAPPED
  int is_dirty;            // 1 if page has been written to, 0 otherwise
  int seq;                 // FIFO sequence number (for resident pages)
  int swap_slot;           // Swap slot number (if swapped out, -1 otherwise)
};

// Process memory statistics
struct proc_mem_stat {
  int pid;                                  // Process ID
  int num_pages_total;                      // Total virtual pages mapped
  int num_resident_pages;                   // Number of pages in physical memory
  int num_swapped_pages;                    // Number of pages in swap
  int next_fifo_seq;                        // Next FIFO sequence number to assign
  struct page_stat pages[MAX_PAGES_INFO];  // Array of page information
};

#endif // _USER_MEMSTAT_H
