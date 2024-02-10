#define _LARGEFILE64_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR_SIZE 512
#define NTFS_TYPE 0x7
#define MFT_ENTRY_SIZE 0x400

uint64_t CLUSTER_SIZE;
int recoverfd;

typedef struct mbr_partition_table_entry {
  uint8_t status;
  uint8_t start_chs[3];
  uint8_t partition_type;
  uint8_t end_chs[3];
  uint32_t first_sector_lba; /* logical block address */
  uint32_t sector_count;
} MBR_partition_table_entry;

typedef struct disk_mbr {
  uint8_t code[440];
  uint32_t disk_signature;
  uint16_t reserved;
  MBR_partition_table_entry pt[4];
  uint8_t signature[2];
} __attribute__((packed)) DISK_mbr;

typedef struct { /* 25 bytes */
  uint16_t bytesPerSector;
  uint8_t sectorsPerCluster;
  uint8_t reserved0[7];
  uint8_t mediaDescriptor;
  uint8_t reserved1;
  uint8_t unused0[9];
  uint8_t reserved2[4];
} __attribute__((packed)) BPB;

typedef struct { /* 48 bytes */
  uint8_t unused0[4];
  uint64_t totalSectors;
  uint64_t MFTLogicalClusterNo;
  uint64_t MFTLogicalClusterNoCopy;
  uint8_t clusterPerMFTRecord;
  uint8_t unused1[3];
  uint8_t clustersPerIndexBuffer;
  uint8_t unused2[3];
  uint64_t volumeSerialNo;
  uint8_t unused3[4];
} __attribute__((packed)) ExtendedBPB;

typedef struct {
  uint8_t jump[3];
  uint64_t oemID;
  BPB bpb;
  ExtendedBPB ebpb;
  uint8_t bootstrapCode[426];
  uint16_t endOfSector;
} __attribute__((packed)) BR;

uint64_t getPartSize(int fd, int partitionNo) {
  DISK_mbr mbr;
  lseek64(fd, 0, SEEK_SET);
  if (read(fd, &mbr, SECTOR_SIZE) != SECTOR_SIZE) {
    printf("unable to read MBR\n");
    return -1;
  }
  off64_t addr = mbr.pt[partitionNo - 1].sector_count * SECTOR_SIZE;
  // printf("BR address: 0x%lx\n", addr);
  return addr;
}

off64_t getPartAdd(int fd, int partitionNo) { /* returns -1 on failure */
  DISK_mbr mbr;
  lseek64(fd, 0, SEEK_SET);
  if (read(fd, &mbr, SECTOR_SIZE) != SECTOR_SIZE) {
    printf("unable to read MBR\n");
    return -1;
  }
  off64_t addr = mbr.pt[partitionNo - 1].first_sector_lba * SECTOR_SIZE;
  // printf("BR address: 0x%lx\n", addr);
  return addr;
}

off64_t getMftAddr(int fd, int partitionNo) { /* returns -1 on failure */
  BR br;
  off64_t partAddr = getPartAdd(fd, partitionNo);
  if (partAddr < 0)
    return -1;
  if (lseek64(fd, partAddr, SEEK_SET) < 0) {
    perror("getMftAddr");
    return -1;
  }
  if (read(fd, &br, SECTOR_SIZE) != SECTOR_SIZE) {
    printf("could not read boot record");
    return -1;
  }
  CLUSTER_SIZE = br.bpb.sectorsPerCluster * SECTOR_SIZE;
  uint64_t MFTClusterNo =
      br.ebpb.MFTLogicalClusterNo < br.ebpb.MFTLogicalClusterNoCopy
          ? br.ebpb.MFTLogicalClusterNo
          : br.ebpb.MFTLogicalClusterNoCopy;
  off64_t addr = MFTClusterNo * CLUSTER_SIZE + partAddr;
  // printf("MFT address: 0x%lx\n", addr);
  return addr;
}

uint8_t *getMftEntry(int fd, int partitionNo, int entryNo) {
  uint8_t *MFTEntry = malloc(MFT_ENTRY_SIZE);
  off64_t MFTAddr = getMftAddr(fd, partitionNo);
  off64_t MFTEntryAddr = MFTAddr + MFT_ENTRY_SIZE * entryNo;
  if (MFTAddr == -1) {
    perror("unable to get mft address");
    return NULL;
  }
  if (lseek64(fd, MFTEntryAddr, SEEK_SET) < 0) {
    perror("lseek64");
    return NULL;
  }
  if (read(fd, MFTEntry, MFT_ENTRY_SIZE) != MFT_ENTRY_SIZE) {
    perror("read");
    return NULL;
  }
  return MFTEntry;
}

int copyBlocks(int fd, int clusterNo, int clusterLen, int partitionNo,
               uint64_t fileLeft) {
  uint64_t partAddr = getPartAdd(fd, partitionNo), realAddr, amountRead = 0,
           amountLeft;
  const uint64_t fragSize = CLUSTER_SIZE * clusterLen;
  uint8_t buf[CLUSTER_SIZE];
  if (partAddr < 0)
    return -1;
  realAddr = partAddr + CLUSTER_SIZE * clusterNo;
  if (lseek64(fd, realAddr, SEEK_SET) < 0)
    return -1;
  while (amountRead < fragSize) {
    amountLeft = fileLeft - amountRead;
    if (amountLeft < CLUSTER_SIZE) {
      if (read(fd, buf, CLUSTER_SIZE) < 0) {
        perror("read");
        return -1;
      }
      if (write(recoverfd, buf, amountLeft) < 0) {
        perror("write");
        return -1;
      }
      amountRead += amountLeft;
      break;
    } else {
      if (read(fd, buf, CLUSTER_SIZE) < 0) {
        perror("read");
        return -1;
      }
      if (write(recoverfd, buf, CLUSTER_SIZE) < 0) {
        perror("write");
        return -1;
      }
      amountRead += CLUSTER_SIZE;
    }
  }
  return 0;
}

char *getName(int fd, int partitionNo, int fileNo) {
  uint8_t nameLen, nonresident;
  char *name;
  const uint8_t *MFTEntry = getMftEntry(fd, partitionNo, fileNo),
                *nameAttrHeader, *nameAttr;
  uint16_t offsetToAttr, flags;
  uint32_t attrType, attrLen, currentOffset = {0};
  uint64_t parent = {0};
  // = {0} to zero out higher order bits that are not used for all arithmetic
  bool endOfEntry = false;
  memcpy(&flags, &MFTEntry[0x16], 2);
  memcpy(&currentOffset, MFTEntry + 0x14, 2);
  while (1) {
    memcpy(&attrType, MFTEntry + currentOffset + 0x0, 4);
    memcpy(&attrLen, MFTEntry + currentOffset + 0x4, 4);
    if (attrType == 0xffffffff) {
      endOfEntry = true;
      break;
    }
    if (attrType == 0x30)
      break;
    currentOffset += attrLen;
  }
  if (endOfEntry) {
    puts("no name attirbute found");
    return NULL;
  }
  nameAttrHeader = MFTEntry + currentOffset;
  memcpy(&nonresident, nameAttrHeader + 0x8, 1);
  if (nonresident == 0x0)
    memcpy(&offsetToAttr, nameAttrHeader + 0x14, 2);
  else
    memcpy(&offsetToAttr, nameAttrHeader + 0x20, 2);
  nameAttr = nameAttrHeader + offsetToAttr;
  memcpy(&parent, nameAttr + 0x0, 6);
  memcpy(&nameLen, nameAttr + 0x40, 1);
  name = malloc(nameLen + 1);
  name[nameLen - 1] = 0;            // set c null terminator
  for (int i = 0; i < nameLen; i++) // copy name
    name[i] = nameAttr[0x41 + 1 + i * 2];

  free((void *)MFTEntry);
  return name;
}

int TraverseRuns(int fd, int partitionNo, int fileNo) {
  uint8_t *MFTEntry = getMftEntry(fd, partitionNo, fileNo), *dataAttrHeader,
          nonresident, *currentRun, runHeader, runHeaderLen, runHeaderClusterNo,
          *msb;
  uint16_t offsetToAttr;
  uint32_t attrType, attrLen, currentOffset = 0;
  uint64_t clusterLen = 0, actualFileSize = 0, amountLeft;
  int64_t relativeClusterNoSignExtend = 0, prevClusterNo = 0, currentClusterNo,
          relativeClusterNoOG = 0, amountRead = 0;
  // = 0 to zero out higher order bits that are not used for all arithmetic
  bool endOfEntry = false;
  if (!MFTEntry) {
    puts("error reading mft entry");
    return -1;
  }
  memcpy(&currentOffset, MFTEntry + 0x14, 2);
  while (1) {
    memcpy(&attrType, MFTEntry + currentOffset + 0x0, 4);
    memcpy(&attrLen, MFTEntry + currentOffset + 0x4, 4);
    if (attrType == 0xffffffff) {
      endOfEntry = true;
      break;
    }
    if (attrType == 0x80)
      break;
    currentOffset += attrLen;
  }
  if (endOfEntry) {
    puts("no data attirbute found");
    return -1;
  }
  dataAttrHeader = MFTEntry + currentOffset;
  memcpy(&nonresident, dataAttrHeader + 0x8, 1);
  switch (nonresident) {
  case 0x0:
    memcpy(&offsetToAttr, dataAttrHeader + 0x14, 2);
    memcpy(&actualFileSize, dataAttrHeader + 0x10, 4);
    currentRun = dataAttrHeader + offsetToAttr;
    for (int i = 0; i < actualFileSize; i++)
      write(recoverfd, currentRun + i, 1);
    break;
  case 0x1:
    memcpy(&actualFileSize, dataAttrHeader + 0x30, 8);
    memcpy(&offsetToAttr, dataAttrHeader + 0x20, 2);
    currentRun = dataAttrHeader + offsetToAttr;
    while (1) {
      memcpy(&runHeader, currentRun, 1);
      if (runHeader == 0)
        break;
      // shift over first nibble
      runHeaderClusterNo = runHeader >> 4;
      // shift to get second nibble
      runHeaderLen = (uint8_t)(runHeader << 4) >> 4;
      // copy my stuff
      memcpy(&clusterLen, currentRun + 1, runHeaderLen);
      memcpy(&relativeClusterNoOG, currentRun + 1 + runHeaderLen,
             runHeaderClusterNo);
      memcpy(&relativeClusterNoSignExtend, &relativeClusterNoOG,
             sizeof(int64_t));
      // get MSB to check sign
      msb = (uint8_t *)&relativeClusterNoSignExtend + (runHeaderClusterNo - 1);
      if (*msb & 0x80) // if negative
        // set some bits to 1
        memset(++msb, 0xff, sizeof(int64_t) - runHeaderClusterNo);
      currentClusterNo = prevClusterNo + relativeClusterNoSignExtend;
      if (copyBlocks(fd, currentClusterNo, clusterLen, partitionNo,
                     actualFileSize - amountRead) < 0) {

        puts("error in copyBlocks");
        return -1;
      }
      amountLeft = actualFileSize - amountRead;
      if (amountLeft < CLUSTER_SIZE)
        amountRead += actualFileSize - amountRead;
      else
        amountRead += clusterLen * CLUSTER_SIZE;
      currentRun += runHeaderLen + runHeaderClusterNo + 1;
      prevClusterNo =
          currentClusterNo; // so can calculate using the relative cluster No
      relativeClusterNoOG = clusterLen =
          0; // zero out MSBs in case next run doesn't fill them
    }
    break;
  }
  free(MFTEntry);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 5) {
    printf(
        "usage: %s <drivename> <partition No.> <file entry No.> <save path>\n",
        argv[0]);
    return -1;
  }
  const char *drive = argv[1], *savePath = argv[4];
  char buf[256], *name, *usedName;
  const int partitionNo = atoi(argv[2]), fileNo = atoi(argv[3]),
            fd = open(drive, O_RDONLY);
  int res;
  if (fd < 0) {
    perror("unable to open file");
    return -1;
  }
  name = getName(fd, partitionNo, fileNo);
  if (!name) {
    usedName = "out";
    puts("writing to file 'out'");
  } else
    usedName = name;
  sprintf(buf, "%s/%s", savePath, usedName);
  recoverfd = open(buf, O_WRONLY | O_CREAT, 0777);
  if (recoverfd < 0) {
    perror("unable to create file in recovery path");
    return -1;
  }

  res = TraverseRuns(fd, atoi(argv[2]), atoi(argv[3]));
  close(fd);
  close(recoverfd);
  free(name);

  return res;
}
