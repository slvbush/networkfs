#pragma once

#include <string>

#define wstr(s) s, strlen(s)

inline void ino_to_string(char (&buf)[21], uint64_t ino) {
  // Root inode is 1000 t the server, but FUSE hardcodes it as 1.
  if (ino == 1) ino = 1000;
  snprintf(buf, sizeof(buf), "%lu", ino);
}

// Custom error codes
#define ESOCKNOCREATE 0x2001
#define ESOCKNOCONNECT 0x2002
#define ESOCKNOMSGSEND 0x2003
#define ESOCKNOMSGRECV 0x2004
#define EHTTPBADCODE 0x2005
#define EHTTPMALFORMED 0x2006
#define EPROTMALFORMED 0x2007

enum networkfs_status {
  NFS_SUCCESS = 0,
  NFS_ENOENT = 1,
  NFS_ENOTFILE = 2,
  NFS_ENOTDIR = 3,
  NFS_ENOENT_DIR = 4,
  NFS_EEXIST = 5,
  NFS_EFBIG = 6,
  NFS_ENOSPC_DIR = 7,
  NFS_ENOTEMPTY = 8,
  NFS_ENAMETOOLONG = 9,
};
