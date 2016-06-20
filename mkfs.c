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

int nbitmap = PARTSIZE/(BSIZE*8) + 1;
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
uint master_freeblock;
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
  int i, cc, fd;
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
  nblocks = PARTSIZE - nmeta;

  //make sure no junk is in mbr before setting it
  memset(&mbr.bootstrap[0],0,sizeof(uchar)*446);
  memset(&mbr.partitions[0],0,sizeof(struct dpartition)*NPARTITIONS);
  memset(&mbr.magic[0],0,sizeof(uchar)*2);

  // allocate partition 0
  mbr.partitions[0].flags = PART_ALLOCATED | PART_BOOTABLE;
  mbr.partitions[0].type = FS_INODE;
  mbr.partitions[0].offset = 1;
  mbr.partitions[0].size = PARTSIZE;

  memset(&partitions, 0, sizeof(struct dpartition) * 4);
  partitions[0].offset = 1;
  partitions[1].offset = 1 + PARTSIZE;
  partitions[2].offset = 1 + PARTSIZE * 2;
  partitions[3].offset = 1 + PARTSIZE * 3;

  // initialize super blocks
  for (i = 0; i < NPARTITIONS; i++) {
    sbs[i].size = xint(PARTSIZE);
    sbs[i].nblocks = xint(nblocks);
    sbs[i].ninodes = xint(NINODES);
    sbs[i].nlog = xint(nlog);
    sbs[i].logstart = xint(1);
    sbs[i].inodestart = xint(1 + nlog);
    sbs[i].bmapstart = xint(1 + nlog + ninodeblocks);
    sbs[i].offset = partitions[i].offset;
  }


  printf("Each partition has the following composition: nmeta %d (boot, super, log blocks %u inode blocks %u, bitmap blocks %u) blocks %d total %d\n",nmeta, nlog, ninodeblocks, nbitmap, nblocks, PARTSIZE);

  freeblock = nmeta;     // the first free block that we can allocate
  master_freeblock = freeblock; // this is to remember the first free block in every partition

  for(i = 0; i < FSSIZE; i++)
    wsect(i, zeroes, 1); // TODO zero all partitions


  //writing MBR to disk
  memset(buf, 0, sizeof(buf));
  memmove(buf, &mbr, sizeof(mbr));
  wsect(0, buf, 1); // writes to absolute block #0

  //writing sb to disk
  memset(buf, 0, sizeof(buf));
  memmove(buf, &sbs[current_partition], sizeof(sbs[current_partition]));
  wsect(0, buf, 0); // writes to relative blcok #0, which is absolute #1 for partition 0

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

  for(i = 2; i < argc; i++){
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
  // fix size of root inode dir
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
  uint off =  mbr ? 0 : partitions[current_partition].offset;
  if(lseek(fsfd, (off + sec) * BSIZE, 0) != (off + sec) * BSIZE){
    perror("lseek");
    exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE){
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
  printf("balloc: write bitmap block at sector %d\n", sbs[current_partition].bmapstart + sbs[current_partition].offset + 1);
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
