// UFS2 on-disk structures, as FreeBSD defines them in sys/ufs/ffs/fs.h,
// sys/ufs/ufs/dinode.h and sys/ufs/ufs/dir.h:
//
// Copyright (c) 2002 Networks Associates Technology, Inc.
// Copyright (c) 1982, 1986, 1993 The Regents of the University of California.
// SPDX-License-Identifier: BSD-3-Clause
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the University nor the names of its contributors may
//    be used to endorse or promote products derived from this software without
//    specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES.
//
// Only the fields' layout is taken; pointers in the in-core part of struct fs
// are written as plain 64-bit words, as on the 64-bit systems that read it.

#ifndef RIPPER_UFS2_LAYOUT_H_
#define RIPPER_UFS2_LAYOUT_H_

#include <stdint.h>

#define UFS2_MAGIC 0x19540119
#define UFS_CG_MAGIC 0x090255
#define UFS_SBLOCK 65536
#define UFS_SBLOCKSIZE 8192
#define UFS_ROOTINO 2
#define UFS_NDADDR 12
#define UFS_NIADDR 3
#define UFS_NXADDR 2
#define UFS_DIRBLKSIZ 512
#define UFS_FS_44INODEFMT 2
#define UFS_FS_FLAGS_UPDATED 0x80
#define UFS_FS_ISCLEAN 1
#define UFS_MAXFRAG 8

struct ufs_csum {
  int32_t cs_ndir;
  int32_t cs_nbfree;
  int32_t cs_nifree;
  int32_t cs_nffree;
};

struct ufs_csum_total {
  int64_t cs_ndir;
  int64_t cs_nbfree;
  int64_t cs_nifree;
  int64_t cs_nffree;
  int64_t cs_numclusters;
  int64_t cs_spare[3];
};

struct ufs_fs {
  int32_t fs_firstfield;
  int32_t fs_unused_1;
  int32_t fs_sblkno;
  int32_t fs_cblkno;
  int32_t fs_iblkno;
  int32_t fs_dblkno;
  int32_t fs_old_cgoffset;
  int32_t fs_old_cgmask;
  int32_t fs_old_time;
  int32_t fs_old_size;
  int32_t fs_old_dsize;
  uint32_t fs_ncg;
  int32_t fs_bsize;
  int32_t fs_fsize;
  int32_t fs_frag;
  int32_t fs_minfree;
  int32_t fs_old_rotdelay;
  int32_t fs_old_rps;
  int32_t fs_bmask;
  int32_t fs_fmask;
  int32_t fs_bshift;
  int32_t fs_fshift;
  int32_t fs_maxcontig;
  int32_t fs_maxbpg;
  int32_t fs_fragshift;
  int32_t fs_fsbtodb;
  int32_t fs_sbsize;
  int32_t fs_spare1[2];
  int32_t fs_nindir;
  uint32_t fs_inopb;
  int32_t fs_old_nspf;
  int32_t fs_optim;
  int32_t fs_old_npsect;
  int32_t fs_old_interleave;
  int32_t fs_old_trackskew;
  int32_t fs_id[2];
  int32_t fs_old_csaddr;
  int32_t fs_cssize;
  int32_t fs_cgsize;
  int32_t fs_spare2;
  int32_t fs_old_nsect;
  int32_t fs_old_spc;
  int32_t fs_old_ncyl;
  int32_t fs_old_cpg;
  uint32_t fs_ipg;
  int32_t fs_fpg;
  struct ufs_csum fs_old_cstotal;
  int8_t fs_fmod;
  int8_t fs_clean;
  int8_t fs_ronly;
  int8_t fs_old_flags;
  uint8_t fs_fsmnt[468];
  uint8_t fs_volname[32];
  uint64_t fs_swuid;
  int32_t fs_pad;
  int32_t fs_cgrotor;
  uint64_t fs_ocsp[15];  // pointers in core
  uint64_t fs_si;        // pointer in core
  int32_t fs_old_cpc;
  int32_t fs_maxbsize;
  int64_t fs_unrefs;
  int64_t fs_providersize;
  int64_t fs_metaspace;
  uint64_t fs_save_maxfilesize;
  int64_t fs_sparecon64[12];
  int64_t fs_sblockactualloc;
  int64_t fs_sblockloc;
  struct ufs_csum_total fs_cstotal;
  int64_t fs_time;
  int64_t fs_size;
  int64_t fs_dsize;
  int64_t fs_csaddr;
  int64_t fs_pendingblocks;
  uint32_t fs_pendinginodes;
  uint32_t fs_snapinum[20];
  uint32_t fs_avgfilesize;
  uint32_t fs_avgfpdir;
  uint32_t fs_available_spare;
  int64_t fs_mtime;
  int32_t fs_sujfree;
  int32_t fs_sparecon32[21];
  uint32_t fs_ckhash;
  uint32_t fs_metackhash;
  int32_t fs_flags;
  int32_t fs_contigsumsize;
  int32_t fs_maxsymlinklen;
  int32_t fs_old_inodefmt;
  uint64_t fs_maxfilesize;
  int64_t fs_qbmask;
  int64_t fs_qfmask;
  int32_t fs_state;
  int32_t fs_old_postblformat;
  int32_t fs_old_nrpos;
  int32_t fs_spare5[2];
  int32_t fs_magic;
};
_Static_assert(sizeof(struct ufs_fs) == 1376, "struct fs is 1376 bytes in FreeBSD");

struct ufs_cg {
  int32_t cg_firstfield;
  int32_t cg_magic;
  int32_t cg_old_time;
  uint32_t cg_cgx;
  int16_t cg_old_ncyl;
  int16_t cg_old_niblk;
  uint32_t cg_ndblk;
  struct ufs_csum cg_cs;
  uint32_t cg_rotor;
  uint32_t cg_frotor;
  uint32_t cg_irotor;
  uint32_t cg_frsum[UFS_MAXFRAG];
  int32_t cg_old_btotoff;
  int32_t cg_old_boff;
  uint32_t cg_iusedoff;
  uint32_t cg_freeoff;
  uint32_t cg_nextfreeoff;
  uint32_t cg_clustersumoff;
  uint32_t cg_clusteroff;
  uint32_t cg_nclusterblks;
  uint32_t cg_niblk;
  uint32_t cg_initediblk;
  uint32_t cg_unrefs;
  int32_t cg_sparecon32[1];
  uint32_t cg_ckhash;
  int64_t cg_time;
  int64_t cg_sparecon64[3];
};
_Static_assert(sizeof(struct ufs_cg) == 168, "struct cg's fixed part is 168 bytes");

struct ufs2_dinode {
  uint16_t di_mode;
  uint16_t di_nlink;
  uint32_t di_uid;
  uint32_t di_gid;
  uint32_t di_blksize;
  uint64_t di_size;
  uint64_t di_blocks;  // in 512-byte units
  int64_t di_atime;
  int64_t di_mtime;
  int64_t di_ctime;
  int64_t di_birthtime;
  int32_t di_mtimensec;
  int32_t di_atimensec;
  int32_t di_ctimensec;
  int32_t di_birthnsec;
  uint32_t di_gen;
  uint32_t di_kernflags;
  uint32_t di_flags;
  uint32_t di_extsize;
  int64_t di_extb[UFS_NXADDR];
  int64_t di_db[UFS_NDADDR];
  int64_t di_ib[UFS_NIADDR];
  uint64_t di_modrev;
  uint32_t di_dirdepth;
  uint32_t di_ckhash;
  uint32_t di_spare[2];
};
_Static_assert(sizeof(struct ufs2_dinode) == 256, "a UFS2 inode is 256 bytes");

#endif
