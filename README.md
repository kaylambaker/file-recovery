# Overview 

This program is able to copy a deleted file from an NTFS file system with MBR partition scheme given the inode of the file that you want to recover. The program will try to find the name of the file you want to recover. If the name is found, the output file will have the same name as the deleted file. If no name is found, the output file will be named ```out```. The program runs on Linux.

# Usage

Clone the repository with command ```git clone https://github.com/kaylambaker/file-recovery.git```

Build the program with the command ```make all```

You need to run the program as sudo. The programs usage is as follows: 

```sudo ./main <drivename> <partition No.> <file entry No.> <save path>```

* ```<drivename>``` - The drive with the file you want to recover. e.g. ```/dev/sdb```
* ```<partition No.>``` - Partition number that has the file you want to recover.
* ```<file entry No.>``` - The inode of the file you want to recover
* ```<save path>``` - The location you want to save the recovered file. e.g. ```/home/user/```

## Example Usage

```sudo ./main /dev/sdb 1 147 /run/media/kayla/1ae5e7cf-8e6d-4b5b-b1d1-4489200a2fea/```

In this example the deleted file is on /dev/sdb1. The inode of the file is 147 and the output file will be written in the directory /run/media/kayla/1ae5e7cf-8e6d-4b5b-b1d1-4489200a2fea/.

# Limitations

## The file may have been overwritten

The Master File Table (MFT) entry for the file, or some or all of the file you want to recover may have been overwritten. The NTFS file system stores information on the file's location in the MFT. The MFT entry attribute 0x80 has the address of where a file is located on a drive. This program uses attribute 0x80 of the deleted file to find the location of the file and copy the data. If the MFT entry is overwritten by another file, the program would not be able to find the original file's data. Additionally, even if the MFT entry has not been overwritten, the actual data of the file may have been overwritten.

## Requires inode number

This program requires you know the inode number of the file before you delete it, so it is not very practical for most people. A different implementation could take a file name as input and scan the NTFS MFT for a file with a matching name and recover that file. This would be a more practical implementation of a file recovery program. The drawback of this implementation is if the file name was deleted too. Deleting a file on an NTFS drive with the Linux command ```rm``` causes the name attribute to be deleted. The name may also not be present if the MFT entry has been overwritten.

## Will not work for files that have too many fragments

An MFT entry is 1KB, and the MFT attribute 0x80 has the address of where a file is located on a drive. The file may be fragmented, so the MFT entry may have multiple addresses for the file's data. A file may have more fragment addresses than will fit into a single 1KB MFT entry. If this is the case, the program will only recover the fragemnts in the 1KB MFT entry. The remainder of the file will not be recovered. MFT attribute 0x20 can be used to find the remainder of the file.
