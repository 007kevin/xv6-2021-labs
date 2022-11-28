// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

// Size of the bcache bucket. Using a prime number reduces
// the likelihood of a collision.
#define NBUCKET 17

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;

  struct spinlock bucketlock[NBUCKET];
  struct buf bucket[NBUCKET];
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");
  for(int i = 0; i < NBUCKET; ++i){
    initlock(&bcache.bucketlock[i], "bcache.bucket");
    bcache.bucket[i].prev = &bcache.bucket[i];
    bcache.bucket[i].next = &bcache.bucket[i];
  }

  // Initialize buf sleeplocks
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  int key = blockno%NBUCKET;

  // Is the block already cached?
  acquire(&bcache.bucketlock[key]);
  for(b = bcache.bucket[key].next; b != &bcache.bucket[key]; b = b->next){
    if(b->dev == dev && b -> blockno == blockno){
      b->refcnt++;
      release(&bcache.bucketlock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  acquire(&bcache.lock);
  struct buf *lb = 0; // least recently used buf
  for(int i = 0; i < NBUF; ++i){
    b = &bcache.buf[i];
    if((lb == 0 && b->refcnt == 0) || (b->refcnt == 0 && lb->ticks > b->ticks)){
      lb = b;
    }
  }
  b = lb;
  if (b) {
    b->dev = dev;
    b->blockno = blockno;
    b->valid = 0;
    b->refcnt = 1;
    b->next = bcache.bucket[key].next;
    b->prev = &bcache.bucket[key];
    bcache.bucket[key].next->prev = b;
    bcache.bucket[key].next = b;
    release(&bcache.lock);
    release(&bcache.bucketlock[key]);
    acquiresleep(&b->lock);
    return b;
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int key = (b->blockno)%NBUCKET;
  acquire(&bcache.bucketlock[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->prev = 0;
    b->next = 0 ;
    b->ticks = ticks; // record negative value to compare against 0
  }
  release(&bcache.bucketlock[key]);
}

void
bpin(struct buf *b) {
  int key = (b->blockno)%NBUCKET;
  acquire(&bcache.bucketlock[key]);
  b->refcnt++;
  release(&bcache.bucketlock[key]);
}

void
bunpin(struct buf *b) {
  int key = (b->blockno)%NBUCKET;
  acquire(&bcache.bucketlock[key]);
  b->refcnt--;
  release(&bcache.bucketlock[key]);
}


