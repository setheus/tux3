/* Lifted from Ext2, blush. GPLv2. Portions (c) Daniel Phillips 2008 */

/*
 *  linux/include/linux/ext2_fs.h
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 *  linux/fs/ext2/dir.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/dir.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext2 directory handling functions
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 * All code that works with directory layout had been switched to pagecache
 * and moved here. AV
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hexdump.c"
#include "buffer.h"
#include "tux3.h"

#define le16_to_cpu(x) x
#define cpu_to_le16(x) x
#define le32_to_cpu(x) x
#define cpu_to_le32(x) x
#define mark_inode_dirty(x)

typedef u16 le16;

#define CURRENT_TIME_SEC 123
#define EXT2_DIR_PAD 3
#define EXT2_REC_LEN(name_len) (((name_len) + 8 + EXT2_DIR_PAD) & ~EXT2_DIR_PAD)
#define EXT2_MAX_REC_LEN ((1<<16)-1)
#define EXT2_NAME_LEN 255

typedef struct { u32 inode; u16 rec_len; u8 name_len, type; char name[]; } ext2_dirent;

static inline unsigned ext2_rec_len_from_disk(le16 dlen)
{
	unsigned len = le16_to_cpu(dlen);
	if (len == EXT2_MAX_REC_LEN)
		return 1 << 16;
	return len;
}

static inline le16 ext2_rec_len_to_disk(unsigned len)
{
	if (len == (1 << 16))
		return cpu_to_le16(EXT2_MAX_REC_LEN);
	else if (len > (1 << 16))
		error("oops");
	return cpu_to_le16(len);
}

static inline int ext2_match(int len, const char *const name, ext2_dirent *de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

static inline ext2_dirent *next_entry(ext2_dirent *p)
{
	return (ext2_dirent *)((char *)p + ext2_rec_len_from_disk(p->rec_len));
}

enum {
	EXT2_UNKNOWN,
	EXT2_REG,
	EXT2_DIR,
	EXT2_CHR,
	EXT2_BLK,
	EXT2_FIFO,
	EXT2_SOCK,
	EXT2_LNK,
	EXT2_TYPES
};

#define STAT_SHIFT 12

static unsigned char ext2_type_by_mode[S_IFMT >> STAT_SHIFT] = {
	[S_IFREG >> STAT_SHIFT] = EXT2_REG,
	[S_IFDIR >> STAT_SHIFT] = EXT2_DIR,
	[S_IFCHR >> STAT_SHIFT] = EXT2_CHR,
	[S_IFBLK >> STAT_SHIFT] = EXT2_BLK,
	[S_IFIFO >> STAT_SHIFT] = EXT2_FIFO,
	[S_IFSOCK >> STAT_SHIFT] = EXT2_SOCK,
	[S_IFLNK >> STAT_SHIFT] = EXT2_LNK,
};

void ext2_dump_entries(struct buffer *buffer, unsigned blocksize)
{
	printf("dirents <%Lx:%Lx>: ", buffer->map->inode->inum, buffer->index);
	ext2_dirent *dirent = (ext2_dirent *)buffer->data;
	ext2_dirent *limit = buffer->data + blocksize;
	while (dirent < limit) {
		if (dirent->inode)
			printf("%.*s (%x:%i) ",
				dirent->name_len,
				dirent->name,
				dirent->inode,
				dirent->type);
		dirent = next_entry(dirent);
	}
	printf("\n");
	brelse(buffer);
}

int ext2_create_entry(struct inode *inode, char *name, int len, unsigned inum, unsigned mode)
{
	ext2_dirent *dirent;
	struct buffer *buffer;
	unsigned reclen = EXT2_REC_LEN(len), rec_len, name_len;
	unsigned blocksize = 1 << inode->map->dev->bits;
	unsigned blocks = inode->i_size >> inode->map->dev->bits, block;
	for (block = 0; block < blocks; block++) {
		buffer = getblk(inode->map, block);
		dirent = buffer->data;
		ext2_dirent *limit = buffer->data + blocksize - reclen;
		while (dirent <= limit) {
			if (dirent->rec_len == 0) {
				warn("zero-length directory entry");
				brelse(buffer);
				return -EIO;
			}
			name_len = EXT2_REC_LEN(dirent->name_len);
			rec_len = ext2_rec_len_from_disk(dirent->rec_len);
			if (!dirent->inode && rec_len >= reclen)
				goto create;
			if (rec_len >= name_len + reclen)
				goto create;
			dirent = (ext2_dirent *)((char *)dirent + rec_len);
		}
		brelse(buffer);
	}
	buffer = getblk(inode->map, blocks);
	dirent = buffer->data;
	name_len = 0;
	rec_len = blocksize;
	*dirent = (ext2_dirent){ .rec_len = ext2_rec_len_to_disk(blocksize) };
	inode->i_size += blocksize;
	set_buffer_dirty(buffer);
create:
	if (dirent->inode) {
		ext2_dirent *newent = (ext2_dirent *)((char *)dirent + name_len);
		newent->rec_len = ext2_rec_len_to_disk(rec_len - name_len);
		dirent->rec_len = ext2_rec_len_to_disk(name_len);
		dirent = newent;
	}
	dirent->name_len = len;
	memcpy(dirent->name, name, len);
	dirent->inode = cpu_to_le32(inum);
	dirent->type = ext2_type_by_mode[(mode & S_IFMT) >> STAT_SHIFT];
	inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
	mark_inode_dirty(inode);
	brelse(buffer);
	return 0;
}

ext2_dirent *ext2_find_entry(struct inode *inode, char *name, int len, struct buffer **result)
{
	unsigned reclen = EXT2_REC_LEN(len);
	unsigned blocksize = 1 << inode->map->dev->bits;
	unsigned blocks = inode->i_size >> inode->map->dev->bits, block;
	for (block = 0; block < blocks; block++) {
		struct buffer *buffer = getblk(inode->map, block);
		ext2_dirent *dirent = buffer->data;
		ext2_dirent *limit = (void *)dirent + blocksize - reclen;
		while (dirent <= limit) {
			if (dirent->rec_len == 0) {
				brelse(buffer);
				warn("zero length dirent at <%Lx:%x>", inode->inum, block);
				return NULL;
			}
			if (ext2_match(len, name, dirent)) {
				*result = buffer;
				return dirent;
			}
			dirent = next_entry(dirent);
		}
		brelse(buffer);
	}
	return NULL;
}

enum {DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK };

static unsigned char filetype[EXT2_TYPES] = {
	[EXT2_UNKNOWN] = DT_UNKNOWN,
	[EXT2_REG] = DT_REG,
	[EXT2_DIR] = DT_DIR,
	[EXT2_CHR] = DT_CHR,
	[EXT2_BLK] = DT_BLK,
	[EXT2_FIFO] = DT_FIFO,
	[EXT2_SOCK] = DT_SOCK,
	[EXT2_LNK] = DT_LNK,
};

typedef int (filldir_t)(void *dirent, char *name, unsigned namelen, loff_t offset, unsigned inode, unsigned type);

static int ext2_readdir(struct file *filp, void *dirents, filldir_t filldir)
{
	loff_t pos = filp->f_pos;
	struct inode *inode = filp->f_inode;
	int revalidate = filp->f_version != inode->i_version;
	unsigned blockbits = inode->map->dev->bits;
	unsigned blocksize = 1 << blockbits;
	unsigned blockmask = blocksize - 1;
	unsigned blocks = inode->i_size >> blockbits;
	unsigned offset = pos & blockmask;

	for (unsigned block = pos >> blockbits ; block < blocks; block++) {
		struct buffer *buffer = getblk(inode->map, block);
		void *base = buffer->data;
		if (!buffer)
			return -EIO;
		if (revalidate) {
			if (offset) {
				ext2_dirent *entry = base + offset;
				ext2_dirent *p = base + (offset & blockmask);
				while (p < entry && p->rec_len)
					p = next_entry(p);
				offset = (void *)p - base;
				filp->f_pos = (block << blockbits) + offset;
			}
			filp->f_version = inode->i_version;
			revalidate = 0;
		}
		unsigned size = inode->i_size - (block << blockbits);
		ext2_dirent *limit = base + (size > blocksize ? blocksize : size) - EXT2_REC_LEN(1);
		for (ext2_dirent *entry = base + offset; entry <= limit; entry = next_entry(entry)) {
			if (entry->rec_len == 0) {
				brelse(buffer);
				warn("zero length entry at <%Lx:%x>", inode->inum, block);
				return -EIO;
			}
			if (entry->inode) {
				unsigned type = (entry->type < EXT2_TYPES) ? filetype[entry->type] : DT_UNKNOWN;
				int full = filldir(
					dirents, entry->name, entry->name_len,
					(block << blockbits) | ((void *)entry - base),
					le32_to_cpu(entry->inode), type);
				if (full) {
					brelse(buffer);
					return 0;
				}
			}
			filp->f_pos += ext2_rec_len_from_disk(entry->rec_len);
		}
		brelse(buffer);
		offset = 0;
	}
	return 0;
}

int ext2_delete_entry(struct buffer *buffer, ext2_dirent *dirent)
{
	ext2_dirent *prev = NULL, *this = buffer->data;
	while ((char *)this < (char *)dirent) {
		if (this->rec_len == 0) {
			warn("zero-length directory entry");
			brelse(buffer);
			return -EIO;
		}
		prev = this;
		this = next_entry(this);
	}
	if (prev)
		prev->rec_len = ext2_rec_len_to_disk((void *)dirent +
		ext2_rec_len_from_disk(dirent->rec_len) - (void *)prev);
	dirent->inode = 0;
	brelse(buffer);
	return 0;
}

int filldir(void *dirent, char *name, unsigned namelen, loff_t offset, unsigned inode, unsigned type)
{
	printf("\"%.*s\"\n", namelen, name);
	return 0;
}

int main(int argc, char *argv[])
{
	struct dev *dev = &(struct dev){ .bits = 8 };
	struct map *map = new_map(dev, NULL);
	init_buffers(dev, 1 << 20);
	struct buffer *buffer;
	map->inode = &(struct inode){ .map = map, .i_mode = S_IFDIR };
	ext2_create_entry(map->inode, "hello", 5, 0x666, S_IFREG);
	ext2_create_entry(map->inode, "world", 5, 0x777, S_IFLNK);
	ext2_dirent *entry = ext2_find_entry(map->inode, "hello", 5, &buffer);
	if (buffer)
		hexdump(entry, 16);
	ext2_dump_entries(getblk(map, 0), 1 << dev->bits);

	if (!ext2_delete_entry(buffer, entry)) {
		show_buffers(map);
		map->inode->i_ctime = map->inode->i_mtime = CURRENT_TIME_SEC;
		mark_inode_dirty(map->inode);
	}

	ext2_dump_entries(getblk(map, 0), 1 << dev->bits);
	show_buffers(map);
	struct file *file = &(struct file){ .f_inode = map->inode };
	for (int i = 0; i < 10; i++) {
		char name[100];
		sprintf(name, "file%i", i);
		ext2_create_entry(map->inode, name, strlen(name), 0x666, S_IFREG);
	}
	ext2_dump_entries(getblk(map, 0), 1 << dev->bits);
	char dents[10000];
	ext2_readdir(file, dents, filldir);
	return 0;
}
