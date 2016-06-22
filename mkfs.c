#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>

#define stat xv6_stat  // avoid clash with host struct stat
#include "types.h"
#include "fs.h"
#include "stat.h"
#include "param.h"
#include "mbr.h"


#ifndef static_assert
#define static_assert(a, b) do { switch (0) case 0: case (a): ; } while (0)
#endif

#define NINODES 200

// Disk layout:
// [ boot block | sb block | log | inode blocks | free bit map | data blocks ]

int nbitmap = FSSIZE/(BSIZE*8) + 1;
int ninodeblocks = NINODES / IPB + 1;
int nlog = LOGSIZE;
int nmeta;    // Number of meta blocks (boot, sb, nlog, inode, bitmap)
int nblocks;  // Number of data blocks

int fsfd;
struct superblock sbs[4];
struct mbr mbr;
char zeroes[BSIZE];
uint freeinode = 1;
uint freeblock;
int current_partition = 0;
struct dpartition partitions[4];


void balloc(int);
void wsect(uint, void*, int);
void winode(uint, struct dinode*);
void rinode(uint inum, struct dinode *ip);
void rsect(uint sec, void *buf, int);
uint ialloc(ushort type);
void iappend(uint inum, void *p, int n);

// convert to intel byte order
ushort
xshort(ushort x)
{
  ushort y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  return y;
}

uint
xint(uint x)
{
  uint y;
  uchar *a = (uchar*)&y;
  a[0] = x;
  a[1] = x >> 8;
  a[2] = x >> 16;
  a[3] = x >> 24;
  return y;
}


int
main(int argc, char *argv[])
{
  int i, cc, fd,fd_bootblock,fd_kernel,blocks_for_kernel;
  uint rootino, inum, off;
  struct dirent de;
  char buf[BSIZE];
  struct dinode din;

  static_assert(sizeof(int) == 4, "Integers must be 4 bytes!");

  if(argc < 2){
    fprintf(stderr, "Usage: mkfs fs.img files...\n");
    exit(1);
  }

  assert((BSIZE % sizeof(struct dinode)) == 0);
  assert((BSIZE % sizeof(struct dirent)) == 0);
  fsfd = open(argv[1], O_RDWR|O_CREAT|O_TRUNC, 0666);
  if(fsfd < 0){
    perror(argv[1]);
    exit(1);
  }

  // 1 fs block = 1 disk sector
  nmeta = 1 + nlog + ninodeblocks + nbitmap;
  nblocks = FSSIZE - nmeta;

  //make sure no junk is in mbr before setting it
  memset(&mbr, 0, sizeof(mbr));

  for(i = 0; i < 2; i++)
    wsect(i, zeroes, 1);


    //write kernel to block 1
  fd_kernel = open(argv[3], O_RDONLY, 0666);
  memset(buf, 0, sizeof(buf));
  blocks_for_kernel = 0;
  while((read(fd_kernel, buf, sizeof(buf))) > 0){
    blocks_for_kernel++;
    wsect(blocks_for_kernel,buf,1); // writes to absolute block #block_for_kernel
  }
  close(fd_kernel);

  //copy bootblock into mbr bootstart
  fd_bootblock = open(argv[2], O_RDONLY, 0666);
  read(fd_bootblock, &mbr.bootstrap[0], sizeof(mbr.bootstrap));

  //set boot signature
  lseek(fd_bootblock, sizeof(mbr) - sizeof(mbr.magic), SEEK_SET);
  read(fd_bootblock, mbr.magic, sizeof(mbr.magic));
  close(fd_bootblock);

  // allocate partitions
  memset(&partitions, 0, sizeof(struct dpartition) * 4);
  for (i = 0; i < NPARTITIONS; i++) {
    mbr.partitions[i].flags = i == 0 ? PART_ALLOCATED | PART_BOOTABLE : PART_ALLOCATED;
    mbr.partitions[i].type = FS_INODE;
    mbr.partitions[i].offset = i == 0 ? blocks_for_kernel + 1 : mbr.partitions[i-1].offset + mbr.partitions[i-1].size;
    mbr.partitions[i].size = FSSIZE;
    partitions[i].offset = mbr.partitions[i].offset;
  }

  // initialize super blocks
  for (i = 0; i < NPARTITIONS; i++) {
    sbs[i].size = xint(FSSIZE);
    sbs[i].nblocks = xint(nblocks);
    sbs[i].ninodes = xint(NINODES);
    sbs[i].nlog = xint(nlog);
    sbs[i].logstart = xint(1);
    sbs[i].inodestart = xint(1 + nlog);
    sbs[i].bmapstart = xint(1 + nlog + ninodeblocks);
    sbs[i].offset = partitions[i].offset;
  }
  printf("Each partition has the following composition: nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",nmeta, nlog, ninodeblocks, nbitmap, nblocks, FSSIZE);

  freeblock = nmeta;     // the first free block that we can allocate

  //writing MBR to disk
  memset(buf, 0, sizeof(buf));
  memmove(buf, &mbr, sizeof(mbr));
  wsect(0, buf, 1); // writes to absolute block #0


  // allocate partitions
  for(i = 0; i < FSSIZE * NPARTITIONS; i++)
    wsect(i + 1 + blocks_for_kernel, zeroes, 1);

  //writing super blocks to disk
  for (i = 0; i < NPARTITIONS; i++) {
    memset(buf, 0, sizeof(buf));
    memmove(buf, &sbs[i], sizeof(sbs[i]));
    current_partition = i;
    wsect(0, buf, 0);
  }
  current_partition = 0;

  // allocate inode for root directory in each partition
  for (i = 0; i < NPARTITIONS; i++, current_partition++, freeinode = 1) {
    rootino = ialloc(T_DIR);
    assert(rootino == ROOTINO);

    bzero(&de, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, ".");
    iappend(rootino, &de, sizeof(de));

    bzero(&de, sizeof(de));
    de.inum = xshort(rootino);
    strcpy(de.name, "..");
    iappend(rootino, &de, sizeof(de));
  }
  current_partition = 0;
  freeinode = 4;

  // populate the blocks bitmap for partitions 1-3
  for (i = 1; i < NPARTITIONS; i++) {
    current_partition = i;
    // fix size of root inode dir
    rinode(rootino, &din);
    off = xint(din.size);
    off = ((off/BSIZE) + 1) * BSIZE;
    din.size = xint(off);
    winode(rootino, &din);

    balloc(freeblock);
  }
  current_partition = 0;

  for(i = 4; i < argc; i++){
    assert(index(argv[i], '/') == 0);
    if((fd = open(argv[i], 0)) < 0){
      perror(argv[i]);
      exit(1);
    }
    // Skip leading _ in name when writing to file system.
    // The binaries are named _rm, _cat, etc. to keep the
    // build operating system from trying to execute them
    // in place of system binaries like rm and cat.
    if(argv[i][0] == '_'){
      ++argv[i];
    }
    inum = ialloc(T_FILE);
    bzero(&de, sizeof(de));
    de.inum = xshort(inum);
    
    strncpy(de.name, argv[i], DIRSIZ);
    iappend(rootino, &de, sizeof(de));
    while((cc = read(fd, buf, sizeof(buf))) > 0)
      iappend(inum, buf, cc);
    close(fd);
  }

  rinode(rootino, &din);
  off = xint(din.size);
  off = ((off/BSIZE) + 1) * BSIZE;
  din.size = xint(off);
  winode(rootino, &din);
  balloc(freeblock);

  exit(0);
}

void
wsect(uint sec, void *buf, int mbr)
{
  uint off =  mbr ? 0 : partitions[current_partition].offset;
  if(lseek(fsfd, (off + sec) * BSIZE, 0) != (off + sec) * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE){
    perror("write");
    exit(1);
  }
}

void
winode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;


  bn = IBLOCK(inum, sbs[current_partition]);
  rsect(bn, buf, 0);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf, 0);
}

void
rinode(uint inum, struct dinode *ip)
{
  char buf[BSIZE];
  uint bn;
  struct dinode *dip;
  bn = IBLOCK(inum, sbs[current_partition]);
  rsect(bn, buf, 0);
  dip = ((struct dinode*)buf) + (inum % IPB);
  *ip = *dip;
}

void
rsect(uint sec, void *buf, int mbr)
{
  int i = -111;
  uint off =  mbr ? 0 : partitions[current_partition].offset;
  if(lseek(fsfd, (off + sec) * BSIZE, 0) != (off + sec) * BSIZE){
    perror("lseek");
    exit(1);
  }
  if((i = read(fsfd, buf, BSIZE)) != BSIZE){
    perror("read");
    exit(1);
  }
}

uint
ialloc(ushort type)
{
  uint inum = freeinode++;
  struct dinode din;

  bzero(&din, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

void
balloc(int used)
{
  uchar buf[BSIZE];
  int i;

  printf("balloc: first %d blocks have been allocated\n", used);
  assert(used < BSIZE*8);
  bzero(buf, BSIZE);
  for(i = 0; i < used; i++){
    buf[i/8] = buf[i/8] | (0x1 << (i%8));
  }

  printf("balloc: write bitmap block at partition %d, sector %d\n", current_partition , sbs[current_partition].bmapstart + sbs[current_partition].offset + 1);
  wsect(sbs[current_partition].bmapstart, buf, 0);
}

#define min(a, b) ((a) < (b) ? (a) : (b))

void
iappend(uint inum, void *xp, int n)
{
  char *p = (char*)xp;
  uint fbn, off, n1;
  struct dinode din;
  char buf[BSIZE];
  uint indirect[NINDIRECT];
  uint x;

  rinode(inum, &din);
  off = xint(din.size);
  // printf("append inum %d at off %d sz %d\n", inum, off, n);
  while(n > 0){
    fbn = off / BSIZE;
    assert(fbn < MAXFILE);
    if(fbn < NDIRECT){
      if(xint(din.addrs[fbn]) == 0){
        din.addrs[fbn] = xint(freeblock++);
      }
      x = xint(din.addrs[fbn]);
    } else {
      if(xint(din.addrs[NDIRECT]) == 0){
        din.addrs[NDIRECT] = xint(freeblock++);
      }
      rsect(xint(din.addrs[NDIRECT]), (char*)indirect, 0);
      if(indirect[fbn - NDIRECT] == 0){
        indirect[fbn - NDIRECT] = xint(freeblock++);
        wsect(xint(din.addrs[NDIRECT]), (char*)indirect, 0);
      }
      x = xint(indirect[fbn-NDIRECT]);
    }
    n1 = min(n, (fbn + 1) * BSIZE - off);
    rsect(x, buf, 0);
    bcopy(p, buf + off - (fbn * BSIZE), n1);
    wsect(x, buf, 0);
    n -= n1;
    off += n1;
    p += n1;
  }
  din.size = xint(off);
  winode(inum, &din);
}
