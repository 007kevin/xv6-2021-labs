// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);
uint64 kfreemem(void);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

// page reference counter for copy on write support.
int ref[PHYSTOP/PGSIZE];

int
getref(void *pa){
  return ref[((uint64) pa)/PGSIZE];
}

void
addref(void *pa){
  ref[((uint64) pa)/PGSIZE] = getref(pa) + 1;
}

void
subref(void *pa){
  int count = getref(pa);
  if (count > 0)
    ref[((uint64) pa)/PGSIZE] = count - 1;
}

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");


  r = (struct run*)pa;

  /* if (getref(pa) > 0) */
  /*     printf("kfreemem: %d\n", kfreemem()); */

  acquire(&kmem.lock);
  subref(pa);
  if (getref(pa) == 0){
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    addref(r);
    kmem.freelist = r->next;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// Returns the amount of free memory in bytes
uint64
kfreemem(void)
{
  uint64 freenodes = 0;
  acquire(&kmem.lock);
  struct run *current = kmem.freelist;
  while(current) {
    freenodes++;
    current = current->next;
  }
  release(&kmem.lock);
  return freenodes * PGSIZE;
}

