/*
 * Block allocation
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 2
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3.h"

#ifndef trace
#define trace trace_on
#endif

/*
 * Group counts
 */

static int countmap_load(struct sb *sb, block_t group)
{
	block_t block = group >> (sb->blockbits - 1);
	struct countmap_pin *pin = &sb->countmap_pin;

	if (!pin->buffer || bufindex(pin->buffer) != block) {
		if (pin->buffer)
			blockput(pin->buffer);
		pin->buffer = blockread(mapping(sb->countmap), block);
		if (!pin->buffer) {
			tux3_err(sb, "block read failed");
			return -EIO;
		}
	}
	return 0;
}

static int countmap_add(struct sb *sb, block_t group, int count)
{
	unsigned offset = group & (sb->blockmask >> 1);
	struct buffer_head *clone;
	__be16 *p;
	int err;

	err = countmap_load(sb, group);
	if (err)
		return err;
	trace("add %d to group %Lu", count, group);
	/*
	 * The countmap is modified only by backend.  blockdirty()
	 * should never return -EAGAIN.
	 */
	clone = blockdirty(sb->countmap_pin.buffer, sb->unify);
	if (IS_ERR(clone)) {
		err = PTR_ERR(clone);
		assert(err != -EAGAIN);
		return err;
	}
	sb->countmap_pin.buffer = clone;

	p = bufdata(sb->countmap_pin.buffer);
	be16_add_cpu(p + offset, count);

	return 0;
}

static int countmap_add_segment(struct sb *sb, block_t start, unsigned blocks,
				int set)
{
	block_t group = start >> sb->groupbits;

	/* Compile option: support cross-group segments */
	if (1 && group != (start + blocks) >> sb->groupbits) {
		unsigned groupsize = 1 << sb->groupbits;
		unsigned groupmask = groupsize - 1;

		while (blocks) {
			unsigned grouplen = (~start & groupmask) + 1;
			int len = min(grouplen, blocks);
			int err = countmap_add(sb, group++, set ? len : -len);
			if (err)
				return err;
			start += len;
			blocks -= len;
		}
		return 0;
	}

	return countmap_add(sb, group, set ? blocks : -blocks);
}

void countmap_put(struct countmap_pin *pin)
{
	if (pin->buffer) {
		blockput(pin->buffer);
		pin->buffer = NULL;
	}
}

static int countmap_used(struct sb *sb, block_t group)
{
	unsigned offset = group & (sb->blockmask >> 1);
	__be16 *p;
	int err;

	err = countmap_load(sb, group);
	if (err)
		return err;

	p = bufdata(sb->countmap_pin.buffer);
	return be16_to_cpup(p + offset);
}

#ifndef __KERNEL__
void countmap_dump(struct sb *sb, block_t start, block_t count)
{
	unsigned groupbits = sb->groupbits, groupsize = 1 << groupbits;

	for (block_t group = start; group < count; group++) {
		block_t block = group << groupbits;
		block_t blocks = min_t(block_t, sb->volblocks - block, groupsize);
		__tux3_dbg("%Lu: %i used, ", group, countmap_used(sb, group));
		bitmap_dump(sb->bitmap, block, blocks);
	}
}
#endif /* !__KERNEL__ */

/*
 * Bitmap
 */

#ifndef __KERNEL__
block_t count_range(struct inode *inode, block_t start, block_t count)
{
	unsigned char ones[256];

	assert(!(start & 7));

	for (int i = 0; i < sizeof(ones); i++)
		ones[i] = bytebits(i);

	struct sb *sb = tux_sb(inode->i_sb);
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	block_t limit = start + count;
	block_t blocks = (limit + mapmask) >> mapshift;
	block_t tail = (count + 7) >> 3, total = 0;
	unsigned offset = (start & mapmask) >> 3;

	for (block_t block = start >> mapshift; block < blocks; block++) {
		trace_off("count block %x/%x", block, blocks);
		struct buffer_head *buffer = blockread(mapping(inode), block);
		if (!buffer)
			return -1;
		unsigned bytes = sb->blocksize - offset;
		if (bytes > tail)
			bytes = tail;
		unsigned char *p = bufdata(buffer) + offset, *top = p + bytes;
		while (p < top)
			total += ones[*p++];
		blockput(buffer);
		tail -= bytes;
		offset = 0;
	}
	return total;
}

void bitmap_dump(struct inode *inode, block_t start, block_t count)
{
	enum { show_used = 0 };
	struct sb *sb = tux_sb(inode->i_sb);
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapsize = 1 << mapshift;
	unsigned mapmask = mapsize - 1;
	unsigned offset = (start & mapmask) >> 3, bit = start & 7, total = 0;
	block_t limit = start + count, blocks = (limit + mapmask) >> mapshift;
	block_t tail = (count + bit + 7) >> 3, begin = -1;

	__tux3_dbg("%s regions in %Lu/%Lu: ", show_used ? "used" : "free", start, count);
	for (block_t block = start >> mapshift; block < blocks; block++) {
		struct buffer_head *buffer = blockread(mapping(inode), block);
		assert(buffer);
		unsigned bytes = sb->blocksize - offset;
		if (bytes > tail)
			bytes = tail;
		unsigned char *data = bufdata(buffer), *p = data + offset, *top = p + bytes;
		for (; p < top; p++, bit = 0) {
			unsigned c = *p;
			if ((!c && ((begin >= 0) ^ show_used)))
				continue;
			if (((c == 0xff) && ((begin < 0) ^ show_used)))
				continue;
			for (int i = bit, mask = 1 << bit; i < 8; i++, mask <<= 1) {
				if (!(c & mask) ^ (begin < 0) ^ show_used)
					continue;
				block_t found = i + ((p - data) << 3) + (block << mapshift);
				if (found == limit)
					break;
				if (begin < 0) {
					__tux3_dbg("%Lu", found);
					begin = found;
					total++;
				} else {
					__tux3_dbg("/%Lu ", found - begin);
					begin = -1;
				}
			}
		}
		blockput(buffer);
		tail -= bytes;
		offset = 0;
	}
	if ((begin >= 0))
		__tux3_dbg("/%Lu ", start + count - begin);
	__tux3_dbg("(%u)\n", total);
}
#endif /* !__KERNEL__ */

/*
 * Modify bits on one block, then adjust ->freeblocks.
 */
static int bitmap_modify_bits(struct sb *sb, struct buffer_head *buffer,
			      unsigned offset, unsigned blocks, int set)
{
	struct buffer_head *clone;
	void (*modify)(u8 *, unsigned, unsigned) = set ? set_bits : clear_bits;

	assert(blocks > 0);
	assert(offset + blocks <= sb->blocksize << 3);

	/*
	 * The bitmap is modified only by backend.
	 * blockdirty() should never return -EAGAIN.
	 */
	clone = blockdirty(buffer, sb->unify);
	if (IS_ERR(clone)) {
		int err = PTR_ERR(clone);
		assert(err != -EAGAIN);
		return err;
	}

	modify(bufdata(clone), offset, blocks);

	mark_buffer_dirty_non(clone);
	blockput(clone);

	if (set)
		sb->freeblocks -= blocks;
	else
		sb->freeblocks += blocks;

	return 0;
}

/*
 * If bits on multiple blocks is excepted state, modify bits.
 *
 * FIXME: If error happened on middle of blocks, modified bits and
 * ->freeblocks are not restored to original. What to do?
 */
static int __bitmap_modify(struct sb *sb, block_t start, unsigned blocks,
			   int set, int (*test)(u8 *, unsigned, unsigned))
{
	struct inode *bitmap = sb->bitmap;
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapsize = 1 << mapshift;
	unsigned mapmask = mapsize - 1;
	unsigned mapoffset = start & mapmask;
	block_t mapblock, mapblocks = (start + blocks + mapmask) >> mapshift;
	unsigned orig_blocks = blocks;

	assert(blocks > 0);
	assert(start + blocks <= sb->volblocks);

	for (mapblock = start >> mapshift; mapblock < mapblocks; mapblock++) {
		struct buffer_head *buffer;
		unsigned len;
		int err;

		buffer = blockread(mapping(bitmap), mapblock);
		if (!buffer) {
			tux3_err(sb, "block read failed");
			return -EIO; /* FIXME: error handling */
		}

		len = min(mapsize - mapoffset, blocks);
		if (test && !test(bufdata(buffer), mapoffset, len)) {
			blockput(buffer);

			tux3_fs_error(sb, "%s: start 0x%Lx, count %x",
				      set ? "already allocated" : "double free",
				      start, blocks);
			return -EIO; /* FIXME: error handling */
		}

		err = bitmap_modify_bits(sb, buffer, mapoffset, len, set);
		if (err) {
			blockput(buffer);
			return err; /* FIXME: error handling */
		}

		mapoffset = 0;
		blocks -= len;
	}

	return countmap_add_segment(sb, start, orig_blocks, set);
}

static int bitmap_modify(struct sb *sb, block_t start, unsigned blocks, int set)
{
	return __bitmap_modify(sb, start, blocks, set, NULL);
}

static int bitmap_test_and_modify(struct sb *sb, block_t start, unsigned blocks,
				  int set)
{
	int (*test)(u8 *, unsigned, unsigned) = set ? all_clear : all_set;
	return __bitmap_modify(sb, start, blocks, set, test);
}

static inline int mergable(struct block_segment *seg, block_t block)
{
	return seg->block + seg->count == block;
}

/*
 * Find blocks available in the specified range.
 *
 * NOTE: Caller must check "*block" to know how many blocks were
 * found. This returns 0 even if no blocks found.
 *
 * return value:
 * < 0 - error
 *   0 - succeed to check
 */
int balloc_find_range(struct sb *sb,
	struct block_segment *seg, int maxsegs, int *segs,
	block_t start, block_t range, unsigned *blocks)
{
	struct inode *bitmap = sb->bitmap;
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapsize = 1 << mapshift;
	unsigned mapmask = mapsize - 1;
	struct buffer_head *buffer;

	trace("find %u blocks in [%Lu+%Lu], segs = %d",
	      *blocks, start, range, *segs);

	assert(*blocks > 0);
	assert(start < sb->volblocks);
	assert(start + range <= sb->volblocks);
	assert(*segs < maxsegs);
	assert(tux3_under_backend(sb));

	/* Search across blocks */
	while (range > 0) {
		block_t mapblock = start >> mapshift;
		block_t mapbase = mapblock << mapshift;
		unsigned offset = start & mapmask;
		unsigned maplimit = min_t(block_t, mapsize, offset + range);
		unsigned chunk = maplimit - offset;
		char *data;

		buffer = blockread(mapping(bitmap), mapblock);
		if (!buffer) {
			tux3_err(sb, "block read failed");
			return -EIO;
			/* FIXME: error handling */
		}
		data = bufdata(buffer);
		/* Search within block */
		do {
			block_t end = (block_t)offset + *blocks;
			unsigned limit, next;

			limit = min_t(block_t, end, maplimit);
			next = find_next_bit_le(data, limit, offset);

			if (next != offset) {
				unsigned count = next - offset;
				block_t found = mapbase + offset;

				if (*segs && mergable(&seg[*segs - 1], found)) {
					trace("append seg [%Lu+%u]", found, count);
					seg[*segs - 1].count += count;
				} else {
					trace("balloc seg [%Lu+%u]", found, count);
					seg[(*segs)++] = (struct block_segment){
						.block = found,
						.count = count,
					};
				}
				*blocks -= count;

				if (!*blocks || *segs == maxsegs) {
					blockput(buffer);
					return 0;
				}
			}

			offset = find_next_zero_bit_le(data, maplimit, next + 1);
			/* Remove after tested on arm. (next + 1 can
			 * be greater than maplimit) */
			assert(offset <= maplimit);
		} while (offset != maplimit);

		assert(start + chunk == mapbase + maplimit);
		start += chunk;
		range -= chunk;
		blockput(buffer);
	}

	return 0;
}

/*
 * Allocate block segments from entire volume.  Wrap around volume if needed.
 * Returns negative if error, zero if at least one block found
 *
 * Scan entire volume exactly once. Start at current goal, continue to end
 * of group, then continue scanning a group at a time, wrapping around to
 * volume base if necessary. Skip any groups with less than some threshold
 * of free blocks, depending on original request size. The first and last
 * partial groups are scanned regardless of threshold in the first pass
 * and never in the second pass. The second pass scans groups skipped in
 * the first pass that are not completely full.
 *
 * return value:
 * < 0 - error
 *   0 - succeed to allocate blocks, at least >= 1
 */
int balloc_find(struct sb *sb,
	struct block_segment *seg, int maxsegs, int *segs,
	unsigned *blocks)
{
	block_t goal = sb->nextblock, volsize = sb->volblocks, start = goal;
	block_t topgroup = volsize >> sb->groupbits;
	unsigned groupsize = 1 << sb->groupbits, groupmask = groupsize - 1;
	unsigned need = *blocks;
	unsigned threshold = min(need, groupsize >> 2);
	int err, newsegs = 0, pass = 0;

	trace("scan volume for %u blocks, goal = %Lu", need, goal);
	trace("groupsize = %u, topgroup = %Lu, threshold = %u",
	      groupsize, topgroup, threshold);
	//bitmap_dump(sb->bitmap, 0, volsize);
	//groups_dump(sb, 0, (volsize + groupmask) >> sb->groupbits);

	do {
		block_t tail = volsize;
		trace("--- pass%i ---", pass + 1);
		trace("group %Lu: start", goal >> sb->groupbits);
		do {
			block_t group = goal >> sb->groupbits;
			block_t next = goal + groupsize;
			int skip = pass > 0, top = group == topgroup;
			if (tail != volsize) {
				unsigned size, used;

				if (tail < groupsize && goal + tail == start) {
					trace("group %Lu: back to start",
					      goal >> sb->groupbits);
					next = goal + tail;
					goto last;
				}
				size = top ? (volsize & groupmask) : groupsize;
				used = countmap_used(sb, group);
				trace("group %Lu: check used", group);
				assert(used <= size);
				skip = used == size || (size - used < threshold) ^ pass;
				next = goal + size;
			}
			next &= ~groupmask;
			if (top)
				next = volsize;
last:
			trace("goal = %Lu, next = %Lu, skip = %i",
			      goal, next, skip);
			if (!skip) {
				err = balloc_find_range(sb, seg, maxsegs,
							&newsegs, goal,
							next - goal, &need);
				if (err)
					return err;
				if (!need || newsegs == maxsegs)
					goto done;
				trace("=> result: segs = %d, need = %u, tail = %Lu",
				      newsegs, need, tail);
			}

			tail -= next - goal;
			/* skip reserved at bottom? */
			goal = next == volsize ? 0 : next;
		} while (tail);
	} while (++pass < 2);

done:
	*segs = newsegs;
	*blocks = need;

	return 0;
}

int balloc_use(struct sb *sb, struct block_segment *seg, int segs)
{
	block_t goal;
	int i;

	assert(segs > 0);

	for (i = 0; i < segs; i++) {
		/* FIXME: error handling */
		int err = bitmap_modify(sb, seg[i].block, seg[i].count, 1);
		if (err)
			return err;
	}

	goal = seg[segs - 1].block + seg[segs - 1].count;
	sb->nextblock = goal == sb->volblocks ? 0 : goal;

	return 0;
}

int balloc_segs(struct sb *sb,
	struct block_segment *seg, int maxsegs, int *segs,
	unsigned *blocks)
{
	int err = balloc_find(sb, seg, maxsegs, segs, blocks);
	if (!err)
		err = balloc_use(sb, seg, *segs);
	return err;
}

block_t balloc_one(struct sb *sb)
{
	struct block_segment seg;
	unsigned blocks = 1;
	int err, segs;

	err = balloc_segs(sb, &seg, 1, &segs, &blocks);
	if (err)
		return err;
	assert(segs == 1 && blocks == 0 && seg.count == 1);
	return seg.block;
}

int bfree(struct sb *sb, block_t start, unsigned blocks)
{
	assert(tux3_under_backend(sb));
	trace("bfree extent [%Lu+%u], ", start, blocks);
	return bitmap_test_and_modify(sb, start, blocks, 0);
}

int bfree_segs(struct sb *sb, struct block_segment *seg, int segs)
{
	int i;
	for (i = 0; i < segs; i++) {
		/* FIXME: error handling */
		int err = bfree(sb, seg[i].block, seg[i].count);
		if (err)
			return err;
	}
	return 0;
}

int replay_update_bitmap(struct replay *rp, block_t start, unsigned blocks,
			 int set)
{
	return bitmap_test_and_modify(rp->sb, start, blocks, set);
}
