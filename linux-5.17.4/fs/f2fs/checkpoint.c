// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/checkpoint.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/mpage.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/f2fs_fs.h>
#include <linux/pagevec.h>
#include <linux/swap.h>
#include <linux/kthread.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "iostat.h"
#include <trace/events/f2fs.h>

#if DELAYED_MERGE
#include <linux/delay.h>
#include <linux/timer.h>
#endif 

#define DEFAULT_CHECKPOINT_IOPRIO (IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, 3))

#include "calclock.h"
unsigned long long wcpTime, wcpCnt;
unsigned long long wcp_waitTime, wcp_waitCnt;
unsigned long long wait_total_submit_time, wait_total_submit_cnt;
unsigned long long wait_total_wait_time, wait_total_submit_cnt;
unsigned long long docpTime, docpCnt;
unsigned long long sync_meta1_time, sync_meta1_cnt;
unsigned long long sync_meta2_time, sync_meta2_cnt;
unsigned long long wait_meta1_time, wait_meta1_cnt;
unsigned long long wait_data1_time, wait_data1_cnt;
unsigned long long wait_data2_time, wait_data2_cnt;
unsigned long long commit_cp_time, commit_cp_cnt;
unsigned long long unblockTime, unblockCnt;
unsigned long long zone_finTime, zone_finCnt;

static struct kmem_cache *ino_entry_slab;
struct kmem_cache *f2fs_inode_entry_slab;

void f2fs_stop_checkpoint(struct f2fs_sb_info *sbi, bool end_io)
{
	f2fs_build_fault_attr(sbi, 0, 0);
	set_ckpt_flags(sbi, CP_ERROR_FLAG);
	if (!end_io)
		f2fs_flush_merged_writes(sbi);
}

/*
 * We guarantee no failure on the returned page.
 */
struct page *f2fs_grab_meta_page(struct f2fs_sb_info *sbi, pgoff_t index)
{
	struct address_space *mapping = META_MAPPING(sbi);
	struct page *page;
repeat:
	page = f2fs_grab_cache_page(mapping, index, false);
	if (!page) {
		cond_resched();
		goto repeat;
	}
	f2fs_wait_on_page_writeback(page, META, true, true);
	if (!PageUptodate(page))
		SetPageUptodate(page);
	return page;
}

static struct page *__get_meta_page(struct f2fs_sb_info *sbi, pgoff_t index,
							bool is_meta)
{
	struct address_space *mapping = META_MAPPING(sbi);
	struct page *page;
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.type = META,
		.op = REQ_OP_READ,
		.op_flags = REQ_META | REQ_PRIO,
		.old_blkaddr = index,
		.new_blkaddr = index,
		.encrypted_page = NULL,
		.is_por = !is_meta,
	};
	int err;

	if (unlikely(!is_meta))
		fio.op_flags &= ~REQ_META;
repeat:
	page = f2fs_grab_cache_page(mapping, index, false);
	if (!page) {
		cond_resched();
		goto repeat;
	}
	if (PageUptodate(page))
		goto out;

	fio.page = page;

	err = f2fs_submit_page_bio(&fio);
	if (err) {
		f2fs_put_page(page, 1);
		return ERR_PTR(err);
	}

	f2fs_update_iostat(sbi, FS_META_READ_IO, F2FS_BLKSIZE);

	lock_page(page);
	if (unlikely(page->mapping != mapping)) {
		f2fs_put_page(page, 1);
		goto repeat;
	}

	if (unlikely(!PageUptodate(page))) {
		f2fs_put_page(page, 1);
		return ERR_PTR(-EIO);
	}
out:
	return page;
}

struct page *f2fs_get_meta_page(struct f2fs_sb_info *sbi, pgoff_t index)
{
	return __get_meta_page(sbi, index, true);
}

struct page *f2fs_get_meta_page_retry(struct f2fs_sb_info *sbi, pgoff_t index)
{
	struct page *page;
	int count = 0;

retry:
	page = __get_meta_page(sbi, index, true);
	if (IS_ERR(page)) {
		if (PTR_ERR(page) == -EIO &&
				++count <= DEFAULT_RETRY_IO_COUNT)
			goto retry;
		f2fs_stop_checkpoint(sbi, false);
	}
	return page;
}

/* for POR only */
struct page *f2fs_get_tmp_page(struct f2fs_sb_info *sbi, pgoff_t index)
{
	return __get_meta_page(sbi, index, false);
}

static bool __is_bitmap_valid(struct f2fs_sb_info *sbi, block_t blkaddr,
							int type)
{
	struct seg_entry *se;
	unsigned int segno, offset;
	bool exist;

	if (type != DATA_GENERIC_ENHANCE && type != DATA_GENERIC_ENHANCE_READ)
		return true;

	segno = GET_SEGNO(sbi, blkaddr);
	offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);
	se = get_seg_entry(sbi, segno);

	exist = f2fs_test_bit(offset, se->cur_valid_map);
	if (!exist && type == DATA_GENERIC_ENHANCE) {
		f2fs_err(sbi, "Inconsistent error blkaddr:%u, sit bitmap:%d",
			 blkaddr, exist);
		set_sbi_flag(sbi, SBI_NEED_FSCK);
		WARN_ON(1);
	}
	return exist;
}

bool f2fs_is_valid_blkaddr(struct f2fs_sb_info *sbi,
					block_t blkaddr, int type)
{
	switch (type) {
	case META_NAT:
		break;
	case META_SIT:
		if (unlikely(blkaddr >= SIT_BLK_CNT(sbi)))
			return false;
		break;
	case META_SSA:
		if (unlikely(blkaddr >= MAIN_BLKADDR(sbi) ||
			blkaddr < SM_I(sbi)->ssa_blkaddr))
			return false;
		break;
	case META_CP:
		if (unlikely(blkaddr >= SIT_I(sbi)->sit_base_addr ||
			blkaddr < __start_cp_addr(sbi)))
			return false;
		break;
	case META_POR:
		if (unlikely(blkaddr >= MAX_BLKADDR(sbi) ||
			blkaddr < MAIN_BLKADDR(sbi)))
			return false;
		break;
	case DATA_GENERIC:
	case DATA_GENERIC_ENHANCE:
	case DATA_GENERIC_ENHANCE_READ:
		if (unlikely(blkaddr >= MAX_BLKADDR(sbi) ||
				blkaddr < MAIN_BLKADDR(sbi))) {
			f2fs_warn(sbi, "access invalid blkaddr:%u",
				  blkaddr);
			set_sbi_flag(sbi, SBI_NEED_FSCK);
			WARN_ON(1);
			return false;
		} else {
			return __is_bitmap_valid(sbi, blkaddr, type);
		}
		break;
	case META_GENERIC:
		if (unlikely(blkaddr < SEG0_BLKADDR(sbi) ||
			blkaddr >= MAIN_BLKADDR(sbi)))
			return false;
		break;
	default:
		BUG();
	}

	return true;
}

/*
 * Readahead CP/NAT/SIT/SSA/POR pages
 */
int f2fs_ra_meta_pages(struct f2fs_sb_info *sbi, block_t start, int nrpages,
							int type, bool sync)
{
	struct page *page;
	block_t blkno = start;
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.type = META,
		.op = REQ_OP_READ,
		.op_flags = sync ? (REQ_META | REQ_PRIO) : REQ_RAHEAD,
		.encrypted_page = NULL,
		.in_list = false,
		.is_por = (type == META_POR),
	};
	struct blk_plug plug;
	int err;

	if (unlikely(type == META_POR))
		fio.op_flags &= ~REQ_META;
#if META_FOR_ZNS
  if (type == META_SSA) {
    down_read(&SM_I(sbi)->ssa_ltree_slock);
  }
#endif
	blk_start_plug(&plug);
	for (; nrpages-- > 0; blkno++) {

		if (!f2fs_is_valid_blkaddr(sbi, blkno, type))
			goto out;

		switch (type) {
		case META_NAT:
			if (unlikely(blkno >=
					NAT_BLOCK_OFFSET(NM_I(sbi)->max_nid)))
				blkno = 0;
			/* get nat block addr */
			fio.new_blkaddr = current_nat_addr(sbi,
					blkno * NAT_ENTRY_PER_BLOCK);
			break;
		case META_SIT:
			if (unlikely(blkno >= TOTAL_SEGS(sbi)))
				goto out;
			/* get sit block addr */
			fio.new_blkaddr = current_sit_addr(sbi,
					blkno * SIT_ENTRY_PER_BLOCK);
			break;
		case META_SSA:
#if META_FOR_ZNS
      blkno = blkno - SM_I(sbi)->ssa_blkaddr;
      fio.new_blkaddr = get_cur_meta_blkaddr(sbi, blkno, SM_I(sbi)->ssa_blkaddr,
        SM_I(sbi)->ssa_bitmap, 1); 
      break;
#endif
		case META_CP:
		case META_POR:
			fio.new_blkaddr = blkno;
			break;
		default:
			BUG();
		}

		page = f2fs_grab_cache_page(META_MAPPING(sbi),
						fio.new_blkaddr, false);
		if (!page)
			continue;
		if (PageUptodate(page)) {
			f2fs_put_page(page, 1);
			continue;
		}
#if META_FOR_ZNS
#if !NAIVE_MFZ
  if (type == META_SSA) {
    //lookup log tree
	  struct f2fs_summary_block *sum;
    struct ssa_set *head;
    struct radix_tree_root *root;
    sum = (struct f2fs_summary_block *) page_address(page);
//    down_read(&SM_I(sbi)->ssa_ltree_slock);
	  root = &SM_I(sbi)->ssa_log_root[SM_I(sbi)->cur_log_tree_idx];
    head = radix_tree_lookup(root, blkno);
    
    if (head) {
      memcpy(sum->entries, head->entries, SUM_ENTRY_SIZE);
      memcpy(&sum->footer, &head->footer, SUM_FOOTER_SIZE);
//      up_read(&SM_I(sbi)->ssa_ltree_slock);
      f2fs_put_page(page, 1);
      continue;
    }

    if (is_set_ckpt_flags(sbi, CP_SSA_MERGE_FLAG)) {
	    root = &SM_I(sbi)->ssa_log_root[SM_I(sbi)->cur_log_tree_idx ^ 0x1];
      head = radix_tree_lookup(root, blkno);

      if (head) {
        memcpy(sum->entries, head->entries, SUM_ENTRY_SIZE);
        memcpy(&sum->footer, &head->footer, SUM_FOOTER_SIZE);
//        up_read(&SM_I(sbi)->ssa_ltree_slock);
        f2fs_put_page(page, 1);
        continue;
      }
    }
//    up_read(&SM_I(sbi)->ssa_ltree_slock);
  }
#endif
#endif
		fio.page = page;
    //printk("(%s:%d) ssa blkaddr: %u", __func__, __LINE__, fio.new_blkaddr);
		err = f2fs_submit_page_bio(&fio);
		f2fs_put_page(page, err ? 1 : 0);
		if (!err)
			f2fs_update_iostat(sbi, FS_META_READ_IO, F2FS_BLKSIZE);
	}
out:
	blk_finish_plug(&plug);
#if META_FOR_ZNS
  if (type == META_SSA) {
    up_read(&SM_I(sbi)->ssa_ltree_slock);
  }
#endif
	return blkno - start;
}

void f2fs_ra_meta_pages_cond(struct f2fs_sb_info *sbi, pgoff_t index)
{
	struct page *page;
	bool readahead = false;

	page = find_get_page(META_MAPPING(sbi), index);
	if (!page || !PageUptodate(page))
		readahead = true;
	f2fs_put_page(page, 0);

	if (readahead)
		f2fs_ra_meta_pages(sbi, index, BIO_MAX_VECS, META_POR, true);
}
static int __f2fs_write_meta_page(struct page *page,
				struct writeback_control *wbc,
				enum iostat_type io_type)
{
	struct f2fs_sb_info *sbi = F2FS_P_SB(page);

	trace_f2fs_writepage(page, META);
	//printk("(%s:%d) page index : %lu",
	//		__func__, __LINE__, page->index);

	if (unlikely(f2fs_cp_error(sbi))){
		printk("(%s:%d) error : redirty out, page index : %lu",
				__func__, __LINE__, page->index);
		goto redirty_out;
	}
	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING))){
		goto redirty_out;
	}
#if META_FOR_ZNS
	if (wbc->for_reclaim && page->index < SM_I(sbi)->ssa_blkaddr)
#else
	if (wbc->for_reclaim && page->index < GET_SUM_BLOCK(sbi, 0))
#endif
  {
		goto redirty_out;
	}
	f2fs_do_write_meta_page(sbi, page, io_type);
	dec_page_count(sbi, F2FS_DIRTY_META);

	if (wbc->for_reclaim)
		f2fs_submit_merged_write_cond(sbi, NULL, page, 0, META);

	unlock_page(page);

	if (unlikely(f2fs_cp_error(sbi))){
		printk("(%s:%d) cp error, page index(%lu)",
				__func__, __LINE__, page->index);
		f2fs_submit_merged_write(sbi, META);
	}

	return 0;

redirty_out:
	printk("(%s:%d) error : redirty_out", __func__, __LINE__); 
	redirty_page_for_writepage(wbc, page);
	return AOP_WRITEPAGE_ACTIVATE;
}

#if META_FOR_ZNS
inline int f2fs_sync_single_meta_page(struct page *page){
	struct writeback_control wbc = {
		.for_reclaim = 0,
	};
	int ret;
	ret = __f2fs_write_meta_page(page, &wbc, FS_CP_META_IO);
	return ret;

}
#endif

static int f2fs_write_meta_page(struct page *page,
				struct writeback_control *wbc)
{
	return __f2fs_write_meta_page(page, wbc, FS_META_IO);
}

static int f2fs_write_meta_pages(struct address_space *mapping,
				struct writeback_control *wbc)
{
	struct f2fs_sb_info *sbi = F2FS_M_SB(mapping);
	long diff, written;
#if META_FOR_ZNS && !DELAYED_MERGE
	int dirty_sum_pages = get_dirty_sum_pages(sbi);
	//printk("(%s:%d) dirty_sum_pages : %d", __func__, __LINE__, dirty_sum_pages); 
#endif
	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
		goto skip_write;

	/* collect a number of dirty meta pages and write together */
	if (wbc->sync_mode != WB_SYNC_ALL &&
			get_pages(sbi, F2FS_DIRTY_META) <
					nr_pages_to_skip(sbi, META))
		goto skip_write;

	/* if locked failed, cp will flush dirty pages instead */
	if (!down_write_trylock(&sbi->cp_global_sem))
		goto skip_write;
	
  trace_f2fs_writepages(mapping->host, wbc, META);
	diff = nr_pages_to_write(sbi, META, wbc);
	written = f2fs_sync_meta_pages(sbi, META, wbc->nr_to_write, FS_META_IO);
	up_write(&sbi->cp_global_sem);
	wbc->nr_to_write = max((long)0, wbc->nr_to_write - written - diff);
	
#if META_FOR_ZNS
#if !DELAYED_MERGE
	if (!has_curlog_space(sbi, dirty_sum_pages, SSA_LOG)){
		//printk("(%s:%d) issue cp", __func__, __LINE__); 
		//printk("(%s:%d) dirty_sum_pages : %d", __func__, __LINE__, dirty_sum_pages); 
		f2fs_issue_checkpoint(sbi);
	}
#endif
#endif
	return 0;

skip_write:
	wbc->pages_skipped += get_pages(sbi, F2FS_DIRTY_META);
	trace_f2fs_writepages(mapping->host, wbc, META);
	return 0;
}

long f2fs_sync_meta_pages(struct f2fs_sb_info *sbi, enum page_type type,
				long nr_to_write, enum iostat_type io_type)
{
	struct address_space *mapping = META_MAPPING(sbi);
	pgoff_t index = 0, prev = ULONG_MAX;
	struct pagevec pvec;
	long nwritten = 0;
	int nr_pages;
	struct writeback_control wbc = {
		.for_reclaim = 0,
	};
	struct blk_plug plug;
#if META_FOR_ZNS
	pgoff_t end = SIT_I(sbi)->sit_base_addr-1;
	//int dirty_sum_pages = SM_I(sbi)->cur_sum_log;
#if !DELAYED_MERGE
	int dirty_sum_pages = get_dirty_sum_pages(sbi); 
#endif
//	printk("(%s:%d) dirty_sum_pages : %d", __func__, __LINE__, dirty_sum_pages); 
#endif

	pagevec_init(&pvec);
	blk_start_plug(&plug);
#if META_FOR_ZNS
	while ((nr_pages = pagevec_lookup_range_tag(&pvec, mapping, &index,
				end, PAGECACHE_TAG_DIRTY))) {
		int i;
		//printk("(%s:%d) nr_pages %d : , first page index : %lu", 
		//	__func__, __LINE__, nr_pages, pvec.pages[0]->index);

		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];
			if (prev == ULONG_MAX)
				prev = page->index - 1;
			if (nr_to_write != LONG_MAX && page->index != prev + 1) {
				pagevec_release(&pvec);
				goto stop;
			}

			lock_page(page);

			if (unlikely(page->mapping != mapping)) {
continue_unlock:
				unlock_page(page);
				continue;
			}
			if (!PageDirty(page)) {
				/* someone wrote it for us */
				goto continue_unlock;
			}

			f2fs_wait_on_page_writeback(page, META, true, true);

			if (!clear_page_dirty_for_io(page))
				goto continue_unlock;

			if (__f2fs_write_meta_page(page, &wbc, io_type)) {
				printk("(%s:%d) error", __func__, __LINE__); 
				unlock_page(page);
				f2fs_bug_on(sbi, 1);
				break;
			}
			nwritten++;
			prev = page->index;
			if (unlikely(nwritten >= nr_to_write))
				break;
		}
		pagevec_release(&pvec);
		cond_resched();
	}
	if(io_type == FS_META_IO || io_type == FS_CP_META_IO){
#if DELAYED_MERGE
		//printk("(%s:%d) flush_sum during sync_meta", __func__, __LINE__); 
		__flush_sum_blks(sbi);
#else
		if(has_curlog_space(sbi, dirty_sum_pages, SSA_LOG)){
			__flush_sum_blks(sbi);
		}
#endif
		/*
		else {
			printk("(%s:%d) skip flush sum and issue cp later for merging SSA LOG",
					__func__, __LINE__); 
			printk("(%s:%d) dirty_sum_pages : %d",
					__func__, __LINE__, dirty_sum_pages); 
			//f2fs_issue_checkpoint(sbi);
		}
		*/
	}
#else
	while ((nr_pages = pagevec_lookup_tag(&pvec, mapping, &index,
				PAGECACHE_TAG_DIRTY))) {
		int i;
		for (i = 0; i < nr_pages; i++) {
			struct page *page = pvec.pages[i];

			if (prev == ULONG_MAX)
				prev = page->index - 1;
			if (nr_to_write != LONG_MAX && page->index != prev + 1) {
				pagevec_release(&pvec);
				goto stop;
			}

			lock_page(page);

			if (unlikely(page->mapping != mapping)) {
continue_unlock:
				unlock_page(page);
				continue;
			}
			if (!PageDirty(page)) {
				/* someone wrote it for us */
				goto continue_unlock;
			}

			f2fs_wait_on_page_writeback(page, META, true, true);

			if (!clear_page_dirty_for_io(page))
				goto continue_unlock;

			if (__f2fs_write_meta_page(page, &wbc, io_type)) {
				unlock_page(page);
				f2fs_bug_on(sbi, 1);
				break;
			}
			nwritten++;
			prev = page->index;
			if (unlikely(nwritten >= nr_to_write))
				break;
		}
		pagevec_release(&pvec);
		cond_resched();
	}
#endif
stop:
	if (nwritten)
		f2fs_submit_merged_write(sbi, type);

	blk_finish_plug(&plug);

	return nwritten;
}

static int f2fs_set_meta_page_dirty(struct page *page)
{
	trace_f2fs_set_page_dirty(page, META);

	if (!PageUptodate(page))
		SetPageUptodate(page);
	if (!PageDirty(page)) {
		__set_page_dirty_nobuffers(page);
		inc_page_count(F2FS_P_SB(page), F2FS_DIRTY_META);
		set_page_private_reference(page);
		return 1;
	}
	return 0;
}

const struct address_space_operations f2fs_meta_aops = {
	.writepage	= f2fs_write_meta_page,
	.writepages	= f2fs_write_meta_pages,
	.set_page_dirty	= f2fs_set_meta_page_dirty,
	.invalidatepage = f2fs_invalidate_page,
	.releasepage	= f2fs_release_page,
#ifdef CONFIG_MIGRATION
	.migratepage    = f2fs_migrate_page,
#endif
};

static void __add_ino_entry(struct f2fs_sb_info *sbi, nid_t ino,
						unsigned int devidx, int type)
{
	struct inode_management *im = &sbi->im[type];
	struct ino_entry *e = NULL, *new = NULL;

	if (type == FLUSH_INO) {
		rcu_read_lock();
		e = radix_tree_lookup(&im->ino_root, ino);
		rcu_read_unlock();
	}

retry:
	if (!e)
		new = f2fs_kmem_cache_alloc(ino_entry_slab,
						GFP_NOFS, true, NULL);

	radix_tree_preload(GFP_NOFS | __GFP_NOFAIL);

	spin_lock(&im->ino_lock);
	e = radix_tree_lookup(&im->ino_root, ino);
	if (!e) {
		if (!new) {
			spin_unlock(&im->ino_lock);
			goto retry;
		}
		e = new;
		if (unlikely(radix_tree_insert(&im->ino_root, ino, e)))
			f2fs_bug_on(sbi, 1);

		memset(e, 0, sizeof(struct ino_entry));
		e->ino = ino;

		list_add_tail(&e->list, &im->ino_list);
		if (type != ORPHAN_INO)
			im->ino_num++;
	}

	if (type == FLUSH_INO)
		f2fs_set_bit(devidx, (char *)&e->dirty_device);

	spin_unlock(&im->ino_lock);
	radix_tree_preload_end();

	if (new && e != new)
		kmem_cache_free(ino_entry_slab, new);
}

static void __remove_ino_entry(struct f2fs_sb_info *sbi, nid_t ino, int type)
{
	struct inode_management *im = &sbi->im[type];
	struct ino_entry *e;

	spin_lock(&im->ino_lock);
	e = radix_tree_lookup(&im->ino_root, ino);
	if (e) {
		list_del(&e->list);
		radix_tree_delete(&im->ino_root, ino);
		im->ino_num--;
		spin_unlock(&im->ino_lock);
		kmem_cache_free(ino_entry_slab, e);
		return;
	}
	spin_unlock(&im->ino_lock);
}

void f2fs_add_ino_entry(struct f2fs_sb_info *sbi, nid_t ino, int type)
{
	/* add new dirty ino entry into list */
	__add_ino_entry(sbi, ino, 0, type);
}

void f2fs_remove_ino_entry(struct f2fs_sb_info *sbi, nid_t ino, int type)
{
	/* remove dirty ino entry from list */
	__remove_ino_entry(sbi, ino, type);
}

/* mode should be APPEND_INO, UPDATE_INO or TRANS_DIR_INO */
bool f2fs_exist_written_data(struct f2fs_sb_info *sbi, nid_t ino, int mode)
{
	struct inode_management *im = &sbi->im[mode];
	struct ino_entry *e;

	spin_lock(&im->ino_lock);
	e = radix_tree_lookup(&im->ino_root, ino);
	spin_unlock(&im->ino_lock);
	return e ? true : false;
}

void f2fs_release_ino_entry(struct f2fs_sb_info *sbi, bool all)
{
	struct ino_entry *e, *tmp;
	int i;

	for (i = all ? ORPHAN_INO : APPEND_INO; i < MAX_INO_ENTRY; i++) {
		struct inode_management *im = &sbi->im[i];

		spin_lock(&im->ino_lock);
		list_for_each_entry_safe(e, tmp, &im->ino_list, list) {
			list_del(&e->list);
			radix_tree_delete(&im->ino_root, e->ino);
			kmem_cache_free(ino_entry_slab, e);
			im->ino_num--;
		}
		spin_unlock(&im->ino_lock);
	}
}

void f2fs_set_dirty_device(struct f2fs_sb_info *sbi, nid_t ino,
					unsigned int devidx, int type)
{
	__add_ino_entry(sbi, ino, devidx, type);
}

bool f2fs_is_dirty_device(struct f2fs_sb_info *sbi, nid_t ino,
					unsigned int devidx, int type)
{
	struct inode_management *im = &sbi->im[type];
	struct ino_entry *e;
	bool is_dirty = false;

	spin_lock(&im->ino_lock);
	e = radix_tree_lookup(&im->ino_root, ino);
	if (e && f2fs_test_bit(devidx, (char *)&e->dirty_device))
		is_dirty = true;
	spin_unlock(&im->ino_lock);
	return is_dirty;
}

int f2fs_acquire_orphan_inode(struct f2fs_sb_info *sbi)
{
	struct inode_management *im = &sbi->im[ORPHAN_INO];
	int err = 0;

	spin_lock(&im->ino_lock);

	if (time_to_inject(sbi, FAULT_ORPHAN)) {
		spin_unlock(&im->ino_lock);
		f2fs_show_injection_info(sbi, FAULT_ORPHAN);
		return -ENOSPC;
	}

	if (unlikely(im->ino_num >= sbi->max_orphans))
		err = -ENOSPC;
	else
		im->ino_num++;
	spin_unlock(&im->ino_lock);

	return err;
}

void f2fs_release_orphan_inode(struct f2fs_sb_info *sbi)
{
	struct inode_management *im = &sbi->im[ORPHAN_INO];

	spin_lock(&im->ino_lock);
	f2fs_bug_on(sbi, im->ino_num == 0);
	im->ino_num--;
	spin_unlock(&im->ino_lock);
}

void f2fs_add_orphan_inode(struct inode *inode)
{
	/* add new orphan ino entry into list */
	__add_ino_entry(F2FS_I_SB(inode), inode->i_ino, 0, ORPHAN_INO);
	f2fs_update_inode_page(inode);
}

void f2fs_remove_orphan_inode(struct f2fs_sb_info *sbi, nid_t ino)
{
	/* remove orphan entry from orphan list */
	__remove_ino_entry(sbi, ino, ORPHAN_INO);
}

static int recover_orphan_inode(struct f2fs_sb_info *sbi, nid_t ino)
{
	struct inode *inode;
	struct node_info ni;
	int err;

	inode = f2fs_iget_retry(sbi->sb, ino);
	if (IS_ERR(inode)) {
		/*
		 * there should be a bug that we can't find the entry
		 * to orphan inode.
		 */
		f2fs_bug_on(sbi, PTR_ERR(inode) == -ENOENT);
		return PTR_ERR(inode);
	}

	err = f2fs_dquot_initialize(inode);
	if (err) {
		iput(inode);
		goto err_out;
	}

	clear_nlink(inode);

	/* truncate all the data during iput */
	iput(inode);

	err = f2fs_get_node_info(sbi, ino, &ni, false);
	if (err)
		goto err_out;

	/* ENOMEM was fully retried in f2fs_evict_inode. */
	if (ni.blk_addr != NULL_ADDR) {
		err = -EIO;
		goto err_out;
	}
	return 0;

err_out:
	set_sbi_flag(sbi, SBI_NEED_FSCK);
	f2fs_warn(sbi, "%s: orphan failed (ino=%x), run fsck to fix.",
		  __func__, ino);
	return err;
}

int f2fs_recover_orphan_inodes(struct f2fs_sb_info *sbi)
{
	block_t start_blk, orphan_blocks, i, j;
	unsigned int s_flags = sbi->sb->s_flags;
	int err = 0;
#ifdef CONFIG_QUOTA
	int quota_enabled;
#endif

	if (!is_set_ckpt_flags(sbi, CP_ORPHAN_PRESENT_FLAG))
		return 0;

	if (bdev_read_only(sbi->sb->s_bdev)) {
		f2fs_info(sbi, "write access unavailable, skipping orphan cleanup");
		return 0;
	}

	if (s_flags & SB_RDONLY) {
		f2fs_info(sbi, "orphan cleanup on readonly fs");
		sbi->sb->s_flags &= ~SB_RDONLY;
	}

#ifdef CONFIG_QUOTA
	/*
	 * Turn on quotas which were not enabled for read-only mounts if
	 * filesystem has quota feature, so that they are updated correctly.
	 */
	quota_enabled = f2fs_enable_quota_files(sbi, s_flags & SB_RDONLY);
#endif

	start_blk = __start_cp_addr(sbi) + 1 + __cp_payload(sbi);
	orphan_blocks = __start_sum_addr(sbi) - 1 - __cp_payload(sbi);

	f2fs_ra_meta_pages(sbi, start_blk, orphan_blocks, META_CP, true);

	for (i = 0; i < orphan_blocks; i++) {
		struct page *page;
		struct f2fs_orphan_block *orphan_blk;

		page = f2fs_get_meta_page(sbi, start_blk + i);
		if (IS_ERR(page)) {
			err = PTR_ERR(page);
			goto out;
		}

		orphan_blk = (struct f2fs_orphan_block *)page_address(page);
		for (j = 0; j < le32_to_cpu(orphan_blk->entry_count); j++) {
			nid_t ino = le32_to_cpu(orphan_blk->ino[j]);

			err = recover_orphan_inode(sbi, ino);
			if (err) {
				f2fs_put_page(page, 1);
				goto out;
			}
		}
		f2fs_put_page(page, 1);
	}
	/* clear Orphan Flag */
	clear_ckpt_flags(sbi, CP_ORPHAN_PRESENT_FLAG);
out:
	set_sbi_flag(sbi, SBI_IS_RECOVERED);

#ifdef CONFIG_QUOTA
	/* Turn quotas off */
	if (quota_enabled)
		f2fs_quota_off_umount(sbi->sb);
#endif
	sbi->sb->s_flags = s_flags; /* Restore SB_RDONLY status */

	return err;
}

static void write_orphan_inodes(struct f2fs_sb_info *sbi, block_t start_blk)
{
	struct list_head *head;
	struct f2fs_orphan_block *orphan_blk = NULL;
	unsigned int nentries = 0;
	unsigned short index = 1;
	unsigned short orphan_blocks;
	struct page *page = NULL;
	struct ino_entry *orphan = NULL;
	struct inode_management *im = &sbi->im[ORPHAN_INO];

	orphan_blocks = GET_ORPHAN_BLOCKS(im->ino_num);

	/*
	 * we don't need to do spin_lock(&im->ino_lock) here, since all the
	 * orphan inode operations are covered under f2fs_lock_op().
	 * And, spin_lock should be avoided due to page operations below.
	 */
	head = &im->ino_list;

	/* loop for each orphan inode entry and write them in Jornal block */
	list_for_each_entry(orphan, head, list) {
		if (!page) {
			page = f2fs_grab_meta_page(sbi, start_blk++);
			orphan_blk =
				(struct f2fs_orphan_block *)page_address(page);
			memset(orphan_blk, 0, sizeof(*orphan_blk));
		}

		orphan_blk->ino[nentries++] = cpu_to_le32(orphan->ino);

		if (nentries == F2FS_ORPHANS_PER_BLOCK) {
			/*
			 * an orphan block is full of 1020 entries,
			 * then we need to flush current orphan blocks
			 * and bring another one in memory
			 */
			orphan_blk->blk_addr = cpu_to_le16(index);
			orphan_blk->blk_count = cpu_to_le16(orphan_blocks);
			orphan_blk->entry_count = cpu_to_le32(nentries);
			set_page_dirty(page);
			f2fs_put_page(page, 1);
			index++;
			nentries = 0;
			page = NULL;
		}
	}

	if (page) {
		orphan_blk->blk_addr = cpu_to_le16(index);
		orphan_blk->blk_count = cpu_to_le16(orphan_blocks);
		orphan_blk->entry_count = cpu_to_le32(nentries);
		set_page_dirty(page);
		f2fs_put_page(page, 1);
	}
}

static __u32 f2fs_checkpoint_chksum(struct f2fs_sb_info *sbi,
						struct f2fs_checkpoint *ckpt)
{
	unsigned int chksum_ofs = le32_to_cpu(ckpt->checksum_offset);
	__u32 chksum;

	chksum = f2fs_crc32(sbi, ckpt, chksum_ofs);
	if (chksum_ofs < CP_CHKSUM_OFFSET) {
		chksum_ofs += sizeof(chksum);
		chksum = f2fs_chksum(sbi, chksum, (__u8 *)ckpt + chksum_ofs,
						F2FS_BLKSIZE - chksum_ofs);
	}
	return chksum;
}

static int get_checkpoint_version(struct f2fs_sb_info *sbi, block_t cp_addr,
		struct f2fs_checkpoint **cp_block, struct page **cp_page,
		unsigned long long *version)
{
	size_t crc_offset = 0;
	__u32 crc;

	*cp_page = f2fs_get_meta_page(sbi, cp_addr);
	if (IS_ERR(*cp_page))
		return PTR_ERR(*cp_page);

	*cp_block = (struct f2fs_checkpoint *)page_address(*cp_page);

	crc_offset = le32_to_cpu((*cp_block)->checksum_offset);
	if (crc_offset < CP_MIN_CHKSUM_OFFSET ||
			crc_offset > CP_CHKSUM_OFFSET) {
		f2fs_put_page(*cp_page, 1);
		f2fs_warn(sbi, "invalid crc_offset: %zu", crc_offset);
		return -EINVAL;
	}

	crc = f2fs_checkpoint_chksum(sbi, *cp_block);
	if (crc != cur_cp_crc(*cp_block)) {
		f2fs_put_page(*cp_page, 1);
		f2fs_warn(sbi, "invalid crc value");
		return -EINVAL;
	}

	*version = cur_cp_version(*cp_block);
	return 0;
}

static struct page *validate_checkpoint(struct f2fs_sb_info *sbi,
				block_t cp_addr, unsigned long long *version)
{
	struct page *cp_page_1 = NULL, *cp_page_2 = NULL;
	struct f2fs_checkpoint *cp_block = NULL;
	unsigned long long cur_version = 0, pre_version = 0;
	unsigned int cp_blocks;
	int err;

	err = get_checkpoint_version(sbi, cp_addr, &cp_block,
					&cp_page_1, version);
	if (err)
		return NULL;

	cp_blocks = le32_to_cpu(cp_block->cp_pack_total_block_count);

	if (cp_blocks > sbi->blocks_per_seg || cp_blocks <= F2FS_CP_PACKS) {
		f2fs_warn(sbi, "invalid cp_pack_total_block_count:%u",
			  le32_to_cpu(cp_block->cp_pack_total_block_count));
		goto invalid_cp;
	}
	pre_version = *version;

	cp_addr += cp_blocks - 1;
	err = get_checkpoint_version(sbi, cp_addr, &cp_block,
					&cp_page_2, version);
	if (err)
		goto invalid_cp;
	cur_version = *version;

	if (cur_version == pre_version) {
		*version = cur_version;
		f2fs_put_page(cp_page_2, 1);
		return cp_page_1;
	}
	f2fs_put_page(cp_page_2, 1);
invalid_cp:
	f2fs_put_page(cp_page_1, 1);
	return NULL;
}

int f2fs_get_valid_checkpoint(struct f2fs_sb_info *sbi)
{
	struct f2fs_checkpoint *cp_block;
	struct f2fs_super_block *fsb = sbi->raw_super;
	struct page *cp1, *cp2, *cur_page;
	unsigned long blk_size = sbi->blocksize;
	unsigned long long cp1_version = 0, cp2_version = 0;
	unsigned long long cp_start_blk_no;
	unsigned int cp_blks = 1 + __cp_payload(sbi);
	block_t cp_blk_no;
	int i;
	int err;

	sbi->ckpt = f2fs_kvzalloc(sbi, array_size(blk_size, cp_blks),
				  GFP_KERNEL);
	if (!sbi->ckpt)
		return -ENOMEM;
	/*
	 * Finding out valid cp block involves read both
	 * sets( cp pack 1 and cp pack 2)
	 */
	cp_start_blk_no = le32_to_cpu(fsb->cp_blkaddr);
	cp1 = validate_checkpoint(sbi, cp_start_blk_no, &cp1_version);

	/* The second checkpoint pack should start at the next segment */
	cp_start_blk_no += ((unsigned long long)1) <<
				le32_to_cpu(fsb->log_blocks_per_seg);
	cp2 = validate_checkpoint(sbi, cp_start_blk_no, &cp2_version);

	if (cp1 && cp2) {
		if (ver_after(cp2_version, cp1_version))
			cur_page = cp2;
		else
			cur_page = cp1;
	} else if (cp1) {
		cur_page = cp1;
	} else if (cp2) {
		cur_page = cp2;
	} else {
		err = -EFSCORRUPTED;
		goto fail_no_cp;
	}

	cp_block = (struct f2fs_checkpoint *)page_address(cur_page);
	memcpy(sbi->ckpt, cp_block, blk_size);

	if (cur_page == cp1)
		sbi->cur_cp_pack = 1;
	else
		sbi->cur_cp_pack = 2;

	/* Sanity checking of checkpoint */
	if (f2fs_sanity_check_ckpt(sbi)) {
		err = -EFSCORRUPTED;
		goto free_fail_no_cp;
	}

	if (cp_blks <= 1)
		goto done;

	cp_blk_no = le32_to_cpu(fsb->cp_blkaddr);
	if (cur_page == cp2)
		cp_blk_no += 1 << le32_to_cpu(fsb->log_blocks_per_seg);

	for (i = 1; i < cp_blks; i++) {
		void *sit_bitmap_ptr;
		unsigned char *ckpt = (unsigned char *)sbi->ckpt;

		cur_page = f2fs_get_meta_page(sbi, cp_blk_no + i);
		if (IS_ERR(cur_page)) {
			err = PTR_ERR(cur_page);
			goto free_fail_no_cp;
		}
		sit_bitmap_ptr = page_address(cur_page);
		memcpy(ckpt + i * blk_size, sit_bitmap_ptr, blk_size);
		f2fs_put_page(cur_page, 1);
	}
done:
	f2fs_put_page(cp1, 1);
	f2fs_put_page(cp2, 1);
	return 0;

free_fail_no_cp:
	f2fs_put_page(cp1, 1);
	f2fs_put_page(cp2, 1);
fail_no_cp:
	kvfree(sbi->ckpt);
	return err;
}

static void __add_dirty_inode(struct inode *inode, enum inode_type type)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	int flag = (type == DIR_INODE) ? FI_DIRTY_DIR : FI_DIRTY_FILE;

	if (is_inode_flag_set(inode, flag))
		return;

	set_inode_flag(inode, flag);
	if (!f2fs_is_volatile_file(inode))
		list_add_tail(&F2FS_I(inode)->dirty_list,
						&sbi->inode_list[type]);
	stat_inc_dirty_inode(sbi, type);
}

static void __remove_dirty_inode(struct inode *inode, enum inode_type type)
{
	int flag = (type == DIR_INODE) ? FI_DIRTY_DIR : FI_DIRTY_FILE;

	if (get_dirty_pages(inode) || !is_inode_flag_set(inode, flag))
		return;

	list_del_init(&F2FS_I(inode)->dirty_list);
	clear_inode_flag(inode, flag);
	stat_dec_dirty_inode(F2FS_I_SB(inode), type);
}

void f2fs_update_dirty_page(struct inode *inode, struct page *page)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	enum inode_type type = S_ISDIR(inode->i_mode) ? DIR_INODE : FILE_INODE;

	if (!S_ISDIR(inode->i_mode) && !S_ISREG(inode->i_mode) &&
			!S_ISLNK(inode->i_mode))
		return;

	spin_lock(&sbi->inode_lock[type]);
	if (type != FILE_INODE || test_opt(sbi, DATA_FLUSH))
		__add_dirty_inode(inode, type);
	inode_inc_dirty_pages(inode);
	spin_unlock(&sbi->inode_lock[type]);

	set_page_private_reference(page);
}

void f2fs_remove_dirty_inode(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	enum inode_type type = S_ISDIR(inode->i_mode) ? DIR_INODE : FILE_INODE;

	if (!S_ISDIR(inode->i_mode) && !S_ISREG(inode->i_mode) &&
			!S_ISLNK(inode->i_mode))
		return;

	if (type == FILE_INODE && !test_opt(sbi, DATA_FLUSH))
		return;

	spin_lock(&sbi->inode_lock[type]);
	__remove_dirty_inode(inode, type);
	spin_unlock(&sbi->inode_lock[type]);
}

int f2fs_sync_dirty_inodes(struct f2fs_sb_info *sbi, enum inode_type type)
{
	struct list_head *head;
	struct inode *inode;
	struct f2fs_inode_info *fi;
	bool is_dir = (type == DIR_INODE);
	unsigned long ino = 0;

	trace_f2fs_sync_dirty_inodes_enter(sbi->sb, is_dir,
				get_pages(sbi, is_dir ?
				F2FS_DIRTY_DENTS : F2FS_DIRTY_DATA));
retry:
	if (unlikely(f2fs_cp_error(sbi))) {
		trace_f2fs_sync_dirty_inodes_exit(sbi->sb, is_dir,
				get_pages(sbi, is_dir ?
				F2FS_DIRTY_DENTS : F2FS_DIRTY_DATA));
		return -EIO;
	}

	spin_lock(&sbi->inode_lock[type]);

	head = &sbi->inode_list[type];
	if (list_empty(head)) {
		spin_unlock(&sbi->inode_lock[type]);
		trace_f2fs_sync_dirty_inodes_exit(sbi->sb, is_dir,
				get_pages(sbi, is_dir ?
				F2FS_DIRTY_DENTS : F2FS_DIRTY_DATA));
		return 0;
	}
	fi = list_first_entry(head, struct f2fs_inode_info, dirty_list);
	inode = igrab(&fi->vfs_inode);
	spin_unlock(&sbi->inode_lock[type]);
	if (inode) {
		unsigned long cur_ino = inode->i_ino;

		F2FS_I(inode)->cp_task = current;

		filemap_fdatawrite(inode->i_mapping);

		F2FS_I(inode)->cp_task = NULL;

		iput(inode);
		/* We need to give cpu to another writers. */
		if (ino == cur_ino)
			cond_resched();
		else
			ino = cur_ino;
	} else {
		/*
		 * We should submit bio, since it exists several
		 * wribacking dentry pages in the freeing inode.
		 */
		f2fs_submit_merged_write(sbi, DATA);
		cond_resched();
	}
	goto retry;
}

int f2fs_sync_inode_meta(struct f2fs_sb_info *sbi)
{
	struct list_head *head = &sbi->inode_list[DIRTY_META];
	struct inode *inode;
	struct f2fs_inode_info *fi;
	s64 total = get_pages(sbi, F2FS_DIRTY_IMETA);

	while (total--) {
		if (unlikely(f2fs_cp_error(sbi)))
			return -EIO;

		spin_lock(&sbi->inode_lock[DIRTY_META]);
		if (list_empty(head)) {
			spin_unlock(&sbi->inode_lock[DIRTY_META]);
			return 0;
		}
		fi = list_first_entry(head, struct f2fs_inode_info,
							gdirty_list);
		inode = igrab(&fi->vfs_inode);
		spin_unlock(&sbi->inode_lock[DIRTY_META]);
		if (inode) {
			sync_inode_metadata(inode, 0);

			/* it's on eviction */
			if (is_inode_flag_set(inode, FI_DIRTY_INODE))
				f2fs_update_inode_page(inode);
			iput(inode);
		}
	}
	return 0;
}

static void __prepare_cp_block(struct f2fs_sb_info *sbi)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	nid_t last_nid = nm_i->next_scan_nid;

	next_free_nid(sbi, &last_nid);
	ckpt->valid_block_count = cpu_to_le64(valid_user_blocks(sbi));
	ckpt->valid_node_count = cpu_to_le32(valid_node_count(sbi));
	ckpt->valid_inode_count = cpu_to_le32(valid_inode_count(sbi));
	ckpt->next_free_nid = cpu_to_le32(last_nid);
}

static bool __need_flush_quota(struct f2fs_sb_info *sbi)
{
	bool ret = false;

	if (!is_journalled_quota(sbi))
		return false;

	if (!down_write_trylock(&sbi->quota_sem))
		return true;
	if (is_sbi_flag_set(sbi, SBI_QUOTA_SKIP_FLUSH)) {
		ret = false;
	} else if (is_sbi_flag_set(sbi, SBI_QUOTA_NEED_REPAIR)) {
		ret = false;
	} else if (is_sbi_flag_set(sbi, SBI_QUOTA_NEED_FLUSH)) {
		clear_sbi_flag(sbi, SBI_QUOTA_NEED_FLUSH);
		ret = true;
	} else if (get_pages(sbi, F2FS_DIRTY_QDATA)) {
		ret = true;
	}
	up_write(&sbi->quota_sem);
	return ret;
}

/*
 * Freeze all the FS-operations for checkpoint.
 */
static int block_operations(struct f2fs_sb_info *sbi)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = LONG_MAX,
		.for_reclaim = 0,
	};
	int err = 0, cnt = 0;

	/*
	 * Let's flush inline_data in dirty node pages.
	 */
	f2fs_flush_inline_data(sbi);

retry_flush_quotas:
	f2fs_lock_all(sbi);
	if (__need_flush_quota(sbi)) {
		int locked;

		if (++cnt > DEFAULT_RETRY_QUOTA_FLUSH_COUNT) {
			set_sbi_flag(sbi, SBI_QUOTA_SKIP_FLUSH);
			set_sbi_flag(sbi, SBI_QUOTA_NEED_FLUSH);
			goto retry_flush_dents;
		}
		f2fs_unlock_all(sbi);

		/* only failed during mount/umount/freeze/quotactl */
		locked = down_read_trylock(&sbi->sb->s_umount);
		f2fs_quota_sync(sbi->sb, -1);
		if (locked)
			up_read(&sbi->sb->s_umount);
		cond_resched();
		goto retry_flush_quotas;
	}

retry_flush_dents:
	/* write all the dirty dentry pages */
	if (get_pages(sbi, F2FS_DIRTY_DENTS)) {
		f2fs_unlock_all(sbi);
		err = f2fs_sync_dirty_inodes(sbi, DIR_INODE);
		if (err)
			return err;
		cond_resched();
		goto retry_flush_quotas;
	}
	
	/*
	 * POR: we should ensure that there are no dirty node pages
	 * until finishing nat/sit flush. inode->i_blocks can be updated.
	 */
	down_write(&sbi->node_change);

	if (get_pages(sbi, F2FS_DIRTY_IMETA)) {
		up_write(&sbi->node_change);
		f2fs_unlock_all(sbi);
		err = f2fs_sync_inode_meta(sbi);
		if (err)
			return err;
		cond_resched();
		goto retry_flush_quotas;
	}
retry_flush_nodes:
	down_write(&sbi->node_write);

	if (get_pages(sbi, F2FS_DIRTY_NODES)) {
		up_write(&sbi->node_write);
		atomic_inc(&sbi->wb_sync_req[NODE]);
		err = f2fs_sync_node_pages(sbi, &wbc, false, FS_CP_NODE_IO);
		atomic_dec(&sbi->wb_sync_req[NODE]);
		if (err) {
			up_write(&sbi->node_change);
			f2fs_unlock_all(sbi);
			return err;
		}
		cond_resched();
		goto retry_flush_nodes;
	}
	/*
	 * sbi->node_change is used only for AIO write_begin path which produces
	 * dirty node blocks and some checkpoint values by block allocation.
	 */
	__prepare_cp_block(sbi);
	up_write(&sbi->node_change);
	return err;
}

static void unblock_operations(struct f2fs_sb_info *sbi)
{
	up_write(&sbi->node_write);
	f2fs_unlock_all(sbi);
}

void f2fs_wait_on_all_pages(struct f2fs_sb_info *sbi, int type)
{
	DEFINE_WAIT(wait);

	//struct timespec64 ts[2];
	//struct timespec64 ts_total[2];
	//unsigned long long submitTime = 0, submitCnt = 0;
	//unsigned long long totalTime = 0, totalCnt = 0;

	//ktime_get_raw_ts64(&ts_total[0]);
	for (;;) {
		if (!get_pages(sbi, type))
			break;

		if (unlikely(f2fs_cp_error(sbi)))
			break;

		if (type == F2FS_DIRTY_META)
			f2fs_sync_meta_pages(sbi, META, LONG_MAX,
							FS_CP_META_IO);
		else if (type == F2FS_WB_CP_DATA) {
			//ktime_get_raw_ts64(&ts[0]);
			f2fs_submit_merged_write(sbi, DATA);
			//ktime_get_raw_ts64(&ts[1]);
			//calclock(ts, &submitTime, &submitCnt);
		}
#if DELAYED_MERGE
    else if (type == F2FS_MERGE_META) {
//		  printk("(%s:%d) merge meta type pages : %lld", __func__, __LINE__, get_pages(sbi, type));
      f2fs_submit_merged_write(sbi, DATA);
    }
#endif
		//printk("(%s:%d) get_page(%d):%lld", __func__, __LINE__, type, get_pages(sbi, type));
		prepare_to_wait(&sbi->cp_wait, &wait, TASK_UNINTERRUPTIBLE);
		io_schedule_timeout(DEFAULT_IO_TIMEOUT);
	}
	finish_wait(&sbi->cp_wait, &wait);
	//ktime_get_raw_ts64(&ts_total[1]);
	//calclock(ts_total, &totalTime, &totalCnt);
/*
	if(type==F2FS_WB_CP_DATA || type==F2FS_MERGE_META){
		printk("(%s:%d) %llu, %llu", __func__, __LINE__, submitTime, totalTime - submitTime);
		wait_total_submit_time += submitTime;
		wait_total_wait_time += (totalTime - submitTime);
	}
*/
}

static void update_ckpt_flags(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	unsigned long orphan_num = sbi->im[ORPHAN_INO].ino_num;
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	unsigned long flags;

	if (cpc->reason & CP_UMOUNT) {
		if (le32_to_cpu(ckpt->cp_pack_total_block_count) +
			NM_I(sbi)->nat_bits_blocks > sbi->blocks_per_seg) {
			clear_ckpt_flags(sbi, CP_NAT_BITS_FLAG);
			f2fs_notice(sbi, "Disable nat_bits due to no space");
		} else if (!is_set_ckpt_flags(sbi, CP_NAT_BITS_FLAG) &&
						f2fs_nat_bitmap_enabled(sbi)) {
			f2fs_enable_nat_bits(sbi);
			set_ckpt_flags(sbi, CP_NAT_BITS_FLAG);
			f2fs_notice(sbi, "Rebuild and enable nat_bits");
		}
	}

	spin_lock_irqsave(&sbi->cp_lock, flags);

	if (cpc->reason & CP_TRIMMED)
		__set_ckpt_flags(ckpt, CP_TRIMMED_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_TRIMMED_FLAG);

	if (cpc->reason & CP_UMOUNT)
		__set_ckpt_flags(ckpt, CP_UMOUNT_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_UMOUNT_FLAG);

	if (cpc->reason & CP_FASTBOOT)
		__set_ckpt_flags(ckpt, CP_FASTBOOT_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_FASTBOOT_FLAG);

	if (orphan_num)
		__set_ckpt_flags(ckpt, CP_ORPHAN_PRESENT_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_ORPHAN_PRESENT_FLAG);

	if (is_sbi_flag_set(sbi, SBI_NEED_FSCK))
		__set_ckpt_flags(ckpt, CP_FSCK_FLAG);

	if (is_sbi_flag_set(sbi, SBI_IS_RESIZEFS))
		__set_ckpt_flags(ckpt, CP_RESIZEFS_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_RESIZEFS_FLAG);

	if (is_sbi_flag_set(sbi, SBI_CP_DISABLED))
		__set_ckpt_flags(ckpt, CP_DISABLED_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_DISABLED_FLAG);

	if (is_sbi_flag_set(sbi, SBI_CP_DISABLED_QUICK))
		__set_ckpt_flags(ckpt, CP_DISABLED_QUICK_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_DISABLED_QUICK_FLAG);

	if (is_sbi_flag_set(sbi, SBI_QUOTA_SKIP_FLUSH))
		__set_ckpt_flags(ckpt, CP_QUOTA_NEED_FSCK_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_QUOTA_NEED_FSCK_FLAG);

	if (is_sbi_flag_set(sbi, SBI_QUOTA_NEED_REPAIR))
		__set_ckpt_flags(ckpt, CP_QUOTA_NEED_FSCK_FLAG);

	/* set this flag to activate crc|cp_ver for recovery */
	__set_ckpt_flags(ckpt, CP_CRC_RECOVERY_FLAG);
	__clear_ckpt_flags(ckpt, CP_NOCRC_RECOVERY_FLAG);

	spin_unlock_irqrestore(&sbi->cp_lock, flags);
}

static void commit_checkpoint(struct f2fs_sb_info *sbi,
	void *src, block_t blk_addr)
{
	struct writeback_control wbc = {
		.for_reclaim = 0,
	};

	/*
	 * pagevec_lookup_tag and lock_page again will take
	 * some extra time. Therefore, f2fs_update_meta_pages and
	 * f2fs_sync_meta_pages are combined in this function.
	 */
	struct page *page = f2fs_grab_meta_page(sbi, blk_addr);
	int err;

	f2fs_wait_on_page_writeback(page, META, true, true);

	memcpy(page_address(page), src, PAGE_SIZE);

	set_page_dirty(page);
	if (unlikely(!clear_page_dirty_for_io(page)))
		f2fs_bug_on(sbi, 1);

	/* writeout cp pack 2 page */
	err = __f2fs_write_meta_page(page, &wbc, FS_CP_META_IO);
	if (unlikely(err && f2fs_cp_error(sbi))) {
		f2fs_put_page(page, 1);
		return;
	}
	f2fs_bug_on(sbi, err);
	f2fs_put_page(page, 0);

	/* submit checkpoint (with barrier if NOBARRIER is not set) */
	f2fs_submit_merged_write(sbi, META_FLUSH);
}

static inline u64 get_sectors_written(struct block_device *bdev)
{
	return (u64)part_stat_read(bdev, sectors[STAT_WRITE]);
}

u64 f2fs_get_sectors_written(struct f2fs_sb_info *sbi)
{
	if (f2fs_is_multi_device(sbi)) {
		u64 sectors = 0;
		int i;

		for (i = 0; i < sbi->s_ndevs; i++)
			sectors += get_sectors_written(FDEV(i).bdev);

		return sectors;
	}

	return get_sectors_written(sbi->sb->s_bdev);
}

#if META_FOR_ZNS
static int do_checkpoint(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	unsigned long orphan_num = sbi->im[ORPHAN_INO].ino_num, flags;
	block_t start_blk;
	unsigned int data_sum_blocks, orphan_blocks;
	__u32 crc32 = 0;
	int i;
	int cp_payload_blks = __cp_payload(sbi);
	struct curseg_info *seg_i = CURSEG_I(sbi, CURSEG_HOT_NODE);
	u64 kbytes_written;
	int err;

//	struct timespec64 ts[2];

	long nwritten;

	/* Flush all the NAT/SIT pages */

	//ktime_get_raw_ts64(&ts[0]);
	nwritten = f2fs_sync_meta_pages(sbi, META, LONG_MAX, FS_CP_META_IO);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &sync_meta1_time, &sync_meta1_cnt);

	/* start to update checkpoint, cp ver is already updated previously */
	ckpt->elapsed_time = cpu_to_le64(get_mtime(sbi, true));
	ckpt->free_segment_count = cpu_to_le32(free_segments(sbi));
	for (i = 0; i < NR_CURSEG_NODE_TYPE; i++) {
		ckpt->cur_node_segno[i] =
			cpu_to_le32(curseg_segno(sbi, i + CURSEG_HOT_NODE));
		ckpt->cur_node_blkoff[i] =
			cpu_to_le16(curseg_blkoff(sbi, i + CURSEG_HOT_NODE));
		ckpt->alloc_type[i + CURSEG_HOT_NODE] =
				curseg_alloc_type(sbi, i + CURSEG_HOT_NODE);
	}
	for (i = 0; i < NR_CURSEG_DATA_TYPE; i++) {
		ckpt->cur_data_segno[i] =
			cpu_to_le32(curseg_segno(sbi, i + CURSEG_HOT_DATA));
		ckpt->cur_data_blkoff[i] =
			cpu_to_le16(curseg_blkoff(sbi, i + CURSEG_HOT_DATA));
		ckpt->alloc_type[i + CURSEG_HOT_DATA] =
				curseg_alloc_type(sbi, i + CURSEG_HOT_DATA);
	}
	//TODO: 4k striping -> store all cursegs
	/* 2 cp + n data seg summary + orphan inode blocks */
	data_sum_blocks = f2fs_npages_for_summary_flush(sbi, false);
	spin_lock_irqsave(&sbi->cp_lock, flags);
	if (data_sum_blocks < NR_CURSEG_DATA_TYPE)
		__set_ckpt_flags(ckpt, CP_COMPACT_SUM_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_COMPACT_SUM_FLAG);
	spin_unlock_irqrestore(&sbi->cp_lock, flags);

	orphan_blocks = GET_ORPHAN_BLOCKS(orphan_num);
	ckpt->cp_pack_start_sum = cpu_to_le32(1 + cp_payload_blks +
			orphan_blocks);

	if (__remain_node_summaries(cpc->reason))
		ckpt->cp_pack_total_block_count = cpu_to_le32(F2FS_CP_PACKS +
				cp_payload_blks + data_sum_blocks +
				orphan_blocks + NR_CURSEG_NODE_TYPE);
	else
		ckpt->cp_pack_total_block_count = cpu_to_le32(F2FS_CP_PACKS +
				cp_payload_blks + data_sum_blocks +
				orphan_blocks);

	/* update ckpt flag for checkpoint */
	update_ckpt_flags(sbi, cpc);

	/* update SIT/NAT bitmap */
	//META_FOR_ZNS: for metadata merge
	get_sit_bitmap(sbi, __bitmap_ptr(sbi, SIT_BITMAP));
	get_nat_bitmap(sbi, __bitmap_ptr(sbi, NAT_BITMAP));
	get_ssa_bitmap(sbi, __bitmap_ptr(sbi, SSA_BITMAP));

	crc32 = f2fs_checkpoint_chksum(sbi, ckpt);
	*((__le32 *)((unsigned char *)ckpt +
				le32_to_cpu(ckpt->checksum_offset)))
				= cpu_to_le32(crc32);

	start_blk = __start_cp_next_addr(sbi);

	/* write out checkpoint buffer at block 0 */
	f2fs_update_meta_page(sbi, ckpt, start_blk++);

	for (i = 1; i < 1 + cp_payload_blks; i++){
		//printk("(%s : %d) start_blk : %u", __func__, __LINE__, start_blk);
		f2fs_update_meta_page(sbi, (char *)ckpt + i * F2FS_BLKSIZE,
							start_blk++);
	}

	if (orphan_num) {
		write_orphan_inodes(sbi, start_blk);
		start_blk += orphan_blocks;
	}

	//META_FOR_ZNS : write current sumblk in "CP"
	f2fs_write_data_summaries(sbi, start_blk);
	start_blk += data_sum_blocks;

	//printk("(%s : %d) start_blk : %u", __func__, __LINE__, start_blk);
	/* Record write statistics in the hot node summary */
	kbytes_written = sbi->kbytes_written;
	kbytes_written += (f2fs_get_sectors_written(sbi) -
				sbi->sectors_written_start) >> 1;
	seg_i->journal->info.kbytes_written = cpu_to_le64(kbytes_written);

	if (__remain_node_summaries(cpc->reason)) {
	//META_FOR_ZNS : write current sumblk in "CP"
		f2fs_write_node_summaries(sbi, start_blk);
		start_blk += NR_CURSEG_NODE_TYPE;
	}
	//printk("(%s : %d) start_blk : %u", __func__, __LINE__, start_blk);

	/* update user_block_counts */
	sbi->last_valid_block_count = sbi->total_valid_block_count;
	percpu_counter_set(&sbi->alloc_valid_block_count, 0);

	
	/* write nat bits */
	// originally write nat bits on the last block in the checkpoint segment
	// but for zns, just write consecutive block with other
	if ((cpc->reason & CP_UMOUNT) &&
			is_set_ckpt_flags(sbi, CP_NAT_BITS_FLAG)) {
		__u64 cp_ver = cur_cp_version(ckpt);
		block_t blk;

		cp_ver |= ((__u64)crc32 << 32);
		*(__le64 *)nm_i->nat_bits = cpu_to_le64(cp_ver);

		blk = start_blk;
		for (i = 0; i < nm_i->nat_bits_blocks; i++) {
			f2fs_update_meta_page(sbi, nm_i->nat_bits +
					(i << F2FS_BLKSIZE_BITS), blk + i);
			//printk("(%s : %d) start_blk : %u", __func__, __LINE__, start_blk);
		}
		start_blk += nm_i->nat_bits_blocks;
	}

	/* Here, we have one bio having CP pack except cp pack 2 page */
	//ktime_get_raw_ts64(&ts[0]);
	f2fs_sync_meta_pages(sbi, META, LONG_MAX, FS_CP_META_IO);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &sync_meta2_time, &sync_meta2_cnt);

	/* Wait for all dirty meta pages to be submitted for IO */
	//ktime_get_raw_ts64(&ts[0]);
	f2fs_wait_on_all_pages(sbi, F2FS_DIRTY_META);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &wait_meta1_time, &wait_meta1_cnt);
	
/* wait for previous submitted meta pages writeback */
	//ktime_get_raw_ts64(&ts[0]);
	f2fs_wait_on_all_pages(sbi, F2FS_WB_CP_DATA);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &wait_data1_time, &wait_data1_cnt);

#if NAIVE_MFZ
  f2fs_wait_on_all_pages(sbi, F2FS_MERGE_META);  
#else
  if (cpc->reason & CP_UMOUNT) {
   f2fs_wait_on_all_pages(sbi, F2FS_MERGE_META);  
  }
#endif

	/* flush all device cache */
	err = f2fs_flush_device_cache(sbi);
	if (err){
		//debug 
		printk("(%s::%d) error here : %d", __func__, __LINE__, err);
		return err;
	}

	/* barrier and flush checkpoint cp pack 2 page if it can */
	//ktime_get_raw_ts64(&ts[0]);
	commit_checkpoint(sbi, ckpt, start_blk);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &commit_cp_time, &commit_cp_cnt);
	
	//ktime_get_raw_ts64(&ts[0]);
	f2fs_wait_on_all_pages(sbi, F2FS_WB_CP_DATA);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &wait_data2_time, &wait_data2_cnt);
	
	/*
	 * invalidate intermediate page cache borrowed from meta inode which are
	 * used for migration of encrypted, verity or compressed inode's blocks.
	 */
	if (f2fs_sb_has_encrypt(sbi) || f2fs_sb_has_verity(sbi) ||
		f2fs_sb_has_compression(sbi))
		invalidate_mapping_pages(META_MAPPING(sbi),
				MAIN_BLKADDR(sbi), MAX_BLKADDR(sbi) - 1);

	f2fs_release_ino_entry(sbi, false);
	f2fs_reset_fsync_node_info(sbi);

	clear_sbi_flag(sbi, SBI_IS_DIRTY);
	clear_sbi_flag(sbi, SBI_NEED_CP);
	clear_sbi_flag(sbi, SBI_QUOTA_SKIP_FLUSH);

	spin_lock(&sbi->stat_lock);
	sbi->unusable_block_count = 0;
	spin_unlock(&sbi->stat_lock);

	__set_cp_next_pack(sbi);

	/*
	 * redirty superblock if metadata like node page or inode cache is
	 * updated during writing checkpoint.
	 */
	if (get_pages(sbi, F2FS_DIRTY_NODES) ||
			get_pages(sbi, F2FS_DIRTY_IMETA))
		set_sbi_flag(sbi, SBI_IS_DIRTY);

	f2fs_bug_on(sbi, get_pages(sbi, F2FS_DIRTY_DENTS));

	//printk("(%s::%d) do checkpoint end", __func__, __LINE__);
	return unlikely(f2fs_cp_error(sbi)) ? -EIO : 0;
}

#else
static int do_checkpoint(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_nm_info *nm_i = NM_I(sbi);
	unsigned long orphan_num = sbi->im[ORPHAN_INO].ino_num, flags;
	block_t start_blk;
	unsigned int data_sum_blocks, orphan_blocks;
	__u32 crc32 = 0;
	int i;
	int cp_payload_blks = __cp_payload(sbi);
	struct curseg_info *seg_i = CURSEG_I(sbi, CURSEG_HOT_NODE);
	u64 kbytes_written;
	int err;

	/* Flush all the NAT/SIT pages */
	f2fs_sync_meta_pages(sbi, META, LONG_MAX, FS_CP_META_IO);

	/* start to update checkpoint, cp ver is already updated previously */
	ckpt->elapsed_time = cpu_to_le64(get_mtime(sbi, true));
	ckpt->free_segment_count = cpu_to_le32(free_segments(sbi));
	for (i = 0; i < NR_CURSEG_NODE_TYPE; i++) {
		ckpt->cur_node_segno[i] =
			cpu_to_le32(curseg_segno(sbi, i + CURSEG_HOT_NODE));
		ckpt->cur_node_blkoff[i] =
			cpu_to_le16(curseg_blkoff(sbi, i + CURSEG_HOT_NODE));
		ckpt->alloc_type[i + CURSEG_HOT_NODE] =
				curseg_alloc_type(sbi, i + CURSEG_HOT_NODE);
	}
	for (i = 0; i < NR_CURSEG_DATA_TYPE; i++) {
		ckpt->cur_data_segno[i] =
			cpu_to_le32(curseg_segno(sbi, i + CURSEG_HOT_DATA));
		ckpt->cur_data_blkoff[i] =
			cpu_to_le16(curseg_blkoff(sbi, i + CURSEG_HOT_DATA));
		ckpt->alloc_type[i + CURSEG_HOT_DATA] =
				curseg_alloc_type(sbi, i + CURSEG_HOT_DATA);
	}

	/* 2 cp + n data seg summary + orphan inode blocks */
	data_sum_blocks = f2fs_npages_for_summary_flush(sbi, false);
	spin_lock_irqsave(&sbi->cp_lock, flags);
	if (data_sum_blocks < NR_CURSEG_DATA_TYPE)
		__set_ckpt_flags(ckpt, CP_COMPACT_SUM_FLAG);
	else
		__clear_ckpt_flags(ckpt, CP_COMPACT_SUM_FLAG);
	spin_unlock_irqrestore(&sbi->cp_lock, flags);

	orphan_blocks = GET_ORPHAN_BLOCKS(orphan_num);
	ckpt->cp_pack_start_sum = cpu_to_le32(1 + cp_payload_blks +
			orphan_blocks);

	if (__remain_node_summaries(cpc->reason))
		ckpt->cp_pack_total_block_count = cpu_to_le32(F2FS_CP_PACKS +
				cp_payload_blks + data_sum_blocks +
				orphan_blocks + NR_CURSEG_NODE_TYPE);
	else
		ckpt->cp_pack_total_block_count = cpu_to_le32(F2FS_CP_PACKS +
				cp_payload_blks + data_sum_blocks +
				orphan_blocks);

	/* update ckpt flag for checkpoint */
	update_ckpt_flags(sbi, cpc);

	/* update SIT/NAT bitmap */
	//META_FOR_ZNS: for metadata merge
	get_sit_bitmap(sbi, __bitmap_ptr(sbi, SIT_BITMAP));
	get_nat_bitmap(sbi, __bitmap_ptr(sbi, NAT_BITMAP));

	crc32 = f2fs_checkpoint_chksum(sbi, ckpt);
	*((__le32 *)((unsigned char *)ckpt +
				le32_to_cpu(ckpt->checksum_offset)))
				= cpu_to_le32(crc32);

	start_blk = __start_cp_next_addr(sbi);

	/* write nat bits */
	if ((cpc->reason & CP_UMOUNT) &&
			is_set_ckpt_flags(sbi, CP_NAT_BITS_FLAG)) {
		__u64 cp_ver = cur_cp_version(ckpt);
		block_t blk;

		cp_ver |= ((__u64)crc32 << 32);
		*(__le64 *)nm_i->nat_bits = cpu_to_le64(cp_ver);

		blk = start_blk + sbi->blocks_per_seg - nm_i->nat_bits_blocks;
		for (i = 0; i < nm_i->nat_bits_blocks; i++)
			f2fs_update_meta_page(sbi, nm_i->nat_bits +
					(i << F2FS_BLKSIZE_BITS), blk + i);
	}

	/* write out checkpoint buffer at block 0 */
	f2fs_update_meta_page(sbi, ckpt, start_blk++);

	for (i = 1; i < 1 + cp_payload_blks; i++)
		f2fs_update_meta_page(sbi, (char *)ckpt + i * F2FS_BLKSIZE,
							start_blk++);

	if (orphan_num) {
		write_orphan_inodes(sbi, start_blk);
		start_blk += orphan_blocks;
	}

	//META_FOR_ZNS : write current sumblk in "CP"
	f2fs_write_data_summaries(sbi, start_blk);
	start_blk += data_sum_blocks;

	/* Record write statistics in the hot node summary */
	kbytes_written = sbi->kbytes_written;
	kbytes_written += (f2fs_get_sectors_written(sbi) -
				sbi->sectors_written_start) >> 1;
	seg_i->journal->info.kbytes_written = cpu_to_le64(kbytes_written);

	if (__remain_node_summaries(cpc->reason)) {
	//META_FOR_ZNS : write current sumblk in "CP"
		f2fs_write_node_summaries(sbi, start_blk);
		start_blk += NR_CURSEG_NODE_TYPE;
	}

	/* update user_block_counts */
	sbi->last_valid_block_count = sbi->total_valid_block_count;
	percpu_counter_set(&sbi->alloc_valid_block_count, 0);

	/* Here, we have one bio having CP pack except cp pack 2 page */
	f2fs_sync_meta_pages(sbi, META, LONG_MAX, FS_CP_META_IO);
	/* Wait for all dirty meta pages to be submitted for IO */
	f2fs_wait_on_all_pages(sbi, F2FS_DIRTY_META);

	/* wait for previous submitted meta pages writeback */
	f2fs_wait_on_all_pages(sbi, F2FS_WB_CP_DATA);

	/* flush all device cache */
	err = f2fs_flush_device_cache(sbi);
	if (err){
		return err;
	}

	/* barrier and flush checkpoint cp pack 2 page if it can */
	commit_checkpoint(sbi, ckpt, start_blk);
	f2fs_wait_on_all_pages(sbi, F2FS_WB_CP_DATA);

	/*
	 * invalidate intermediate page cache borrowed from meta inode which are
	 * used for migration of encrypted, verity or compressed inode's blocks.
	 */
	if (f2fs_sb_has_encrypt(sbi) || f2fs_sb_has_verity(sbi) ||
		f2fs_sb_has_compression(sbi))
		invalidate_mapping_pages(META_MAPPING(sbi),
				MAIN_BLKADDR(sbi), MAX_BLKADDR(sbi) - 1);

	f2fs_release_ino_entry(sbi, false);

	f2fs_reset_fsync_node_info(sbi);

	clear_sbi_flag(sbi, SBI_IS_DIRTY);
	clear_sbi_flag(sbi, SBI_NEED_CP);
	clear_sbi_flag(sbi, SBI_QUOTA_SKIP_FLUSH);

	spin_lock(&sbi->stat_lock);
	sbi->unusable_block_count = 0;
	spin_unlock(&sbi->stat_lock);

	__set_cp_next_pack(sbi);

	/*
	 * redirty superblock if metadata like node page or inode cache is
	 * updated during writing checkpoint.
	 */
	if (get_pages(sbi, F2FS_DIRTY_NODES) ||
			get_pages(sbi, F2FS_DIRTY_IMETA))
		set_sbi_flag(sbi, SBI_IS_DIRTY);

	f2fs_bug_on(sbi, get_pages(sbi, F2FS_DIRTY_DENTS));

	return unlikely(f2fs_cp_error(sbi)) ? -EIO : 0;
}
#endif
int f2fs_write_checkpoint(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	unsigned long long ckpt_ver;
	int err = 0;
#if META_FOR_ZNS
	block_t cp_blkaddr;
	struct f2fs_dev_info* zbd;
	sector_t zone_sectors;
#endif
	//struct timespec64 ts[2];
	//struct timespec64 ts_total[2];
	if (f2fs_readonly(sbi->sb) || f2fs_hw_is_readonly(sbi))
		return -EROFS;

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
		if (cpc->reason != CP_PAUSE)
			return 0;
		f2fs_warn(sbi, "Start checkpoint disabled!");
	}
	if (cpc->reason != CP_RESIZE)
		down_write(&sbi->cp_global_sem);
	if (!is_sbi_flag_set(sbi, SBI_IS_DIRTY) &&
		((cpc->reason & CP_FASTBOOT) || (cpc->reason & CP_SYNC) ||
		((cpc->reason & CP_DISCARD) && !sbi->discard_blks)))
		goto out;
	if (unlikely(f2fs_cp_error(sbi))) {
		err = -EIO;
		goto out;
	}
	trace_f2fs_write_checkpoint(sbi->sb, cpc->reason, "start block_ops");

#if 0
	printk("(%s : %d) CP strat\n", 
	  __func__, __LINE__);
#endif
  //ktime_get_raw_ts64(&ts_total[0]);

#if META_FOR_ZNS
	// before starting checkpoint, reset target zone
	// now, only consider one zns
	if (f2fs_is_multi_device(sbi)){
		printk("(%s : %d) error! : not support multi device!", 
				__func__, __LINE__);
		f2fs_bug_on(sbi, 1);
	}
	
	cp_blkaddr = __start_cp_next_addr(sbi);
	
	if (f2fs_blkz_is_seq(sbi, 0, cp_blkaddr)){
		zbd = &FDEV(0);
		zone_sectors = SECTOR_FROM_BLOCK(sbi->blocks_per_blkz); 
		if(!zbd || !zbd->bdev){
			f2fs_bug_on(sbi, 1);
			printk("(%s : %d) error here", __func__, __LINE__);
		} else {
			//printk("(%s : %d) blk zone mgmt RESET", __func__, __LINE__);
//			printk("(%s : %d) blk zone mgmt cp_blkaddr(%u)", __func__, __LINE__, cp_blkaddr);
			blkdev_zone_mgmt(zbd->bdev, REQ_OP_ZONE_RESET, 
					SECTOR_FROM_BLOCK(cp_blkaddr), 
					zone_sectors, GFP_NOFS);
		}
	} else {
		f2fs_warn(sbi, "error : not ZNS SSD");
		printk("(%s : %d) cp_blkaddr : %u", __func__, __LINE__, cp_blkaddr);
	}
#endif

	err = block_operations(sbi);
//	printk("(%s:%d) block_ops end", __func__, __LINE__);
	if (err)
		goto out;

	trace_f2fs_write_checkpoint(sbi->sb, cpc->reason, "finish block_ops");

	f2fs_flush_merged_writes(sbi);

	/* this is the case of multiple fstrims without any changes */
	if (cpc->reason & CP_DISCARD) {
		if (!f2fs_exist_trim_candidates(sbi, cpc)) {
			unblock_operations(sbi);
			goto out;
		}

		if (NM_I(sbi)->nat_cnt[DIRTY_NAT] == 0 &&
				SIT_I(sbi)->dirty_sentries == 0 &&
				prefree_segments(sbi) == 0) {
	    printk("(%s : %d) no nat flush checkpoint", __func__, __LINE__);
			f2fs_flush_sit_entries(sbi, cpc);
			f2fs_clear_prefree_segments(sbi, cpc);
			unblock_operations(sbi);
			goto out;
		}
	}
	/*
	 * update checkpoint pack index
	 * Increase the version number so that
	 * SIT entries and seg summaries are written at correct place
	 */
	ckpt_ver = cur_cp_version(ckpt);
	ckpt->checkpoint_ver = cpu_to_le64(++ckpt_ver);
	/* write cached NAT/SIT entries to NAT/SIT area */
	err = f2fs_flush_nat_entries(sbi, cpc);
	if (err) {
		f2fs_err(sbi, "f2fs_flush_nat_entries failed err:%d, stop checkpoint", err);
		f2fs_bug_on(sbi, !f2fs_cp_error(sbi));
		goto stop;
	}
	f2fs_flush_sit_entries(sbi, cpc);
#if META_FOR_ZNS
#if !NAIVE_MFZ
	if (cpc->reason & CP_UMOUNT) {
#else 
  {
#endif
		err = flush_sum_blks(sbi, cpc);
		if (err) {
			f2fs_err(sbi, "flush_sum_blks failed err:%d, stop checkpoint", err);
			f2fs_bug_on(sbi, !f2fs_cp_error(sbi));
			goto stop;
		}
	}
#endif
	/* save inmem log status */
	f2fs_save_inmem_curseg(sbi);
	
	//ktime_get_raw_ts64(&ts[0]);
	err = do_checkpoint(sbi, cpc);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &docpTime, &docpCnt);
	
	if (err) {
		f2fs_err(sbi, "do_checkpoint failed err:%d, stop checkpoint", err);
		f2fs_bug_on(sbi, !f2fs_cp_error(sbi));
		f2fs_release_discard_addrs(sbi);
	} else {
		f2fs_clear_prefree_segments(sbi, cpc);
	}

#if DELAYED_MERGE
	// invoke merge thread
	if (is_set_ckpt_flags(sbi, CP_SIT_MERGE_DONE_FLAG)) {
		reset_meta_zone_towrite(sbi, SM_I(sbi)->cur_sit_log ^ 0x1, SIT_LOG);
		clear_ckpt_flags(sbi, CP_SIT_MERGE_DONE_FLAG);
	}
	if (is_set_ckpt_flags(sbi, CP_NAT_MERGE_DONE_FLAG)) {
		reset_meta_zone_towrite(sbi, NM_I(sbi)->cur_nat_log ^ 0x1, NAT_LOG);
		clear_ckpt_flags(sbi, CP_NAT_MERGE_DONE_FLAG);
	}
	if (is_set_ckpt_flags(sbi, CP_SSA_MERGE_DONE_FLAG)) {
		//reset log zone
		reset_meta_zone_towrite(sbi, SM_I(sbi)->cur_sum_log ^ 0x1, SSA_LOG);
		clear_ckpt_flags(sbi, CP_SSA_MERGE_DONE_FLAG);
	}

	if (cpc->merge & 0x1) {
		//printk("(%s : %d) invoke merge for sit", __func__, __LINE__);
		cpc->merge = cpc->merge ;
    memcpy(SIT_I(sbi)->sit_merge_bitmap, SIT_I(sbi)->sit_log_bitmap, f2fs_bitmap_size(MAIN_SEGS(sbi)));
		set_ckpt_flags(sbi, CP_SIT_MERGE_FLAG);
		down_write(&SM_I(sbi)->sit_ltree_slock);
		SM_I(sbi)->sit_ltree_idx ^= 0x1;
		up_write(&SM_I(sbi)->sit_ltree_slock);

		//printk("(%s : %d) switch sit_ltree_idx to %d", 
		//		__func__, __LINE__, SM_I(sbi)->sit_ltree_idx);
		if (!radix_tree_empty(&SM_I(sbi)->sit_log_root[SM_I(sbi)->sit_ltree_idx])) {
			printk("(%s : %d) this is not empty tree", __func__, __LINE__);
		}
		
	}
	
	if (cpc->merge & 0x2) {
		//printk("(%s : %d) invoke merge for nat", __func__, __LINE__);
		cpc->merge = 0;
		set_ckpt_flags(sbi, CP_NAT_MERGE_FLAG);
		down_write(&NM_I(sbi)->nat_ltree_slock);
		NM_I(sbi)->nat_ltree_idx ^= 0x1;
		up_write(&NM_I(sbi)->nat_ltree_slock);

		//printk("(%s : %d) switch nat_ltree_idx to %d", 
		//		__func__, __LINE__, NM_I(sbi)->nat_ltree_idx);
		if (!radix_tree_empty(&NM_I(sbi)->nat_log_root[NM_I(sbi)->nat_ltree_idx])) {
			printk("(%s : %d) this is not empty tree", __func__, __LINE__);
		}
		
	}
	if (is_set_ckpt_flags(sbi, CP_SSA_MERGE_PREPARE_FLAG)) {

		down_write(&SM_I(sbi)->ssa_ltree_slock);
		SM_I(sbi)->cur_log_tree_idx ^= 0x1;
		up_write(&SM_I(sbi)->ssa_ltree_slock);

		//printk("(%s : %d) switch cur_log_tree_idx to %d", 
		//		__func__, __LINE__, SM_I(sbi)->cur_log_tree_idx);
		if (!radix_tree_empty(&SM_I(sbi)->ssa_log_root[SM_I(sbi)->cur_log_tree_idx])) {
			printk("(%s : %d) this is not empty tree", __func__, __LINE__);
		}
		clear_ckpt_flags(sbi, CP_SSA_MERGE_PREPARE_FLAG);
		set_ckpt_flags(sbi, CP_SSA_MERGE_FLAG);
// 		invoke merge thread
	}

#endif

	f2fs_restore_inmem_curseg(sbi);
stop:
	//ktime_get_raw_ts64(&ts[0]);
	unblock_operations(sbi);
	//ktime_get_raw_ts64(&ts[1]);
	//calclock(ts, &unblockTime, &unblockCnt);

	stat_inc_cp_count(sbi->stat_info);

	if (cpc->reason & CP_RECOVERY)
		f2fs_notice(sbi, "checkpoint: version = %llx", ckpt_ver);

	/* update CP_TIME to trigger checkpoint periodically */
	f2fs_update_time(sbi, CP_TIME);
	trace_f2fs_write_checkpoint(sbi->sb, cpc->reason, "finish checkpoint");

#if 0
	if (f2fs_is_multi_device(sbi)){
		printk("(%s : %d) error! : not support multi device!", 
				__func__, __LINE__);
		f2fs_bug_on(sbi, 1);
	}
	if (f2fs_blkz_is_seq(sbi, 0, cp_blkaddr)){
		if(!zbd || !zbd->bdev){
			f2fs_bug_on(sbi, 1);
			printk("(%s : %d) error here", __func__, __LINE__);
		} else {
			//printk("(%s : %d) blk zone mgmt FINISH", __func__, __LINE__);
			//printk("(%s : %d) blk zone mgmt cp_blkaddr(%u)", __func__, __LINE__, cp_blkaddr);
			ktime_get_raw_ts64(&ts[0]);
			blkdev_zone_mgmt(zbd->bdev, REQ_OP_ZONE_CLOSE, 
					SECTOR_FROM_BLOCK(cp_blkaddr), 
					zone_sectors, GFP_NOFS);
			ktime_get_raw_ts64(&ts[1]);
			calclock(ts, &zone_finTime, &zone_finCnt);
		}
	} else {
		f2fs_warn(sbi, "error : not ZNS SSD");
		printk("(%s : %d) cp_blkaddr : %u", __func__, __LINE__, cp_blkaddr);
	}
#endif
#if 0
	printk("(%s : %d) write checkpoint end\n", __func__, __LINE__);
#endif
	//ktime_get_raw_ts64(&ts_total[1]);
	//calclock(ts_total, &wcpTime, &wcpCnt);
out:
	if (cpc->reason != CP_RESIZE)
		up_write(&sbi->cp_global_sem);
	return err;
}

void f2fs_init_ino_entry_info(struct f2fs_sb_info *sbi)
{
	int i;

	for (i = 0; i < MAX_INO_ENTRY; i++) {
		struct inode_management *im = &sbi->im[i];

		INIT_RADIX_TREE(&im->ino_root, GFP_ATOMIC);
		spin_lock_init(&im->ino_lock);
		INIT_LIST_HEAD(&im->ino_list);
		im->ino_num = 0;
	}

	sbi->max_orphans = (sbi->blocks_per_seg - F2FS_CP_PACKS -
			NR_CURSEG_PERSIST_TYPE - __cp_payload(sbi)) *
				F2FS_ORPHANS_PER_BLOCK;
}

int __init f2fs_create_checkpoint_caches(void)
{
	ino_entry_slab = f2fs_kmem_cache_create("f2fs_ino_entry",
			sizeof(struct ino_entry));
	if (!ino_entry_slab)
		return -ENOMEM;
	f2fs_inode_entry_slab = f2fs_kmem_cache_create("f2fs_inode_entry",
			sizeof(struct inode_entry));
	if (!f2fs_inode_entry_slab) {
		kmem_cache_destroy(ino_entry_slab);
		return -ENOMEM;
	}
	return 0;
}

void f2fs_destroy_checkpoint_caches(void)
{
	kmem_cache_destroy(ino_entry_slab);
	kmem_cache_destroy(f2fs_inode_entry_slab);
}

static int __write_checkpoint_sync(struct f2fs_sb_info *sbi)
{
	struct cp_control cpc = { .reason = CP_SYNC, };
	int err;

	down_write(&sbi->gc_lock);
	err = f2fs_write_checkpoint(sbi, &cpc);
	up_write(&sbi->gc_lock);

	return err;
}

static void __checkpoint_and_complete_reqs(struct f2fs_sb_info *sbi)
{
	struct ckpt_req_control *cprc = &sbi->cprc_info;
	struct ckpt_req *req, *next;
	struct llist_node *dispatch_list;
	u64 sum_diff = 0, diff, count = 0;
	int ret;

	dispatch_list = llist_del_all(&cprc->issue_list);
	if (!dispatch_list)
		return;
	dispatch_list = llist_reverse_order(dispatch_list);

	ret = __write_checkpoint_sync(sbi);
	atomic_inc(&cprc->issued_ckpt);

	llist_for_each_entry_safe(req, next, dispatch_list, llnode) {
		diff = (u64)ktime_ms_delta(ktime_get(), req->queue_time);
		req->ret = ret;
		complete(&req->wait);

		sum_diff += diff;
		count++;
	}
	atomic_sub(count, &cprc->queued_ckpt);
	atomic_add(count, &cprc->total_ckpt);

	spin_lock(&cprc->stat_lock);
	cprc->cur_time = (unsigned int)div64_u64(sum_diff, count);
	if (cprc->peak_time < cprc->cur_time)
		cprc->peak_time = cprc->cur_time;
	spin_unlock(&cprc->stat_lock);
}

static int issue_checkpoint_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct ckpt_req_control *cprc = &sbi->cprc_info;
	wait_queue_head_t *q = &cprc->ckpt_wait_queue;
repeat:
	if (kthread_should_stop())
		return 0;

	if (!llist_empty(&cprc->issue_list))
		__checkpoint_and_complete_reqs(sbi);

	wait_event_interruptible(*q,
		kthread_should_stop() || !llist_empty(&cprc->issue_list));
	goto repeat;
}

static void flush_remained_ckpt_reqs(struct f2fs_sb_info *sbi,
		struct ckpt_req *wait_req)
{
	struct ckpt_req_control *cprc = &sbi->cprc_info;

	if (!llist_empty(&cprc->issue_list)) {
		__checkpoint_and_complete_reqs(sbi);
	} else {
		/* already dispatched by issue_checkpoint_thread */
		if (wait_req)
			wait_for_completion(&wait_req->wait);
	}
}

static void init_ckpt_req(struct ckpt_req *req)
{
	memset(req, 0, sizeof(struct ckpt_req));

	init_completion(&req->wait);
	req->queue_time = ktime_get();
}

int f2fs_issue_checkpoint(struct f2fs_sb_info *sbi)
{
	struct ckpt_req_control *cprc = &sbi->cprc_info;
	struct ckpt_req req;
	struct cp_control cpc;

	cpc.reason = __get_cp_reason(sbi);
	if (!test_opt(sbi, MERGE_CHECKPOINT) || cpc.reason != CP_SYNC) {
		int ret;

		down_write(&sbi->gc_lock);
		ret = f2fs_write_checkpoint(sbi, &cpc);
		up_write(&sbi->gc_lock);

		return ret;
	}

	if (!cprc->f2fs_issue_ckpt)
		return __write_checkpoint_sync(sbi);

	init_ckpt_req(&req);

	llist_add(&req.llnode, &cprc->issue_list);
	atomic_inc(&cprc->queued_ckpt);

	/*
	 * update issue_list before we wake up issue_checkpoint thread,
	 * this smp_mb() pairs with another barrier in ___wait_event(),
	 * see more details in comments of waitqueue_active().
	 */
	smp_mb();

	if (waitqueue_active(&cprc->ckpt_wait_queue))
		wake_up(&cprc->ckpt_wait_queue);

	if (cprc->f2fs_issue_ckpt)
		wait_for_completion(&req.wait);
	else
		flush_remained_ckpt_reqs(sbi, &req);

	return req.ret;
}

int f2fs_start_ckpt_thread(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	struct ckpt_req_control *cprc = &sbi->cprc_info;

	if (cprc->f2fs_issue_ckpt)
		return 0;

	cprc->f2fs_issue_ckpt = kthread_run(issue_checkpoint_thread, sbi,
			"f2fs_ckpt-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(cprc->f2fs_issue_ckpt)) {
		cprc->f2fs_issue_ckpt = NULL;
		return -ENOMEM;
	}

	set_task_ioprio(cprc->f2fs_issue_ckpt, cprc->ckpt_thread_ioprio);

	return 0;
}

void f2fs_stop_ckpt_thread(struct f2fs_sb_info *sbi)
{
	struct ckpt_req_control *cprc = &sbi->cprc_info;

	if (cprc->f2fs_issue_ckpt) {
		struct task_struct *ckpt_task = cprc->f2fs_issue_ckpt;

		cprc->f2fs_issue_ckpt = NULL;
		kthread_stop(ckpt_task);

		flush_remained_ckpt_reqs(sbi, NULL);
	}
}

#if DELAYED_MERGE
int f2fs_merge(void *data)
{
	struct f2fs_sb_info *sbi = data;
	long time_ms = 100;
	int ret = 0;
	bool done = false;

	while (!kthread_should_stop()) {
		//do merge
		//ssa
		done = false;

		if (is_set_ckpt_flags(sbi, CP_SSA_MERGE_FLAG)) {
		  if (is_set_ckpt_flags(sbi, CP_SSA_IN_MERGE_FLAG)) {
        msleep(time_ms);
        continue;
      }
//			f2fs_lock_op(sbi);
			set_ckpt_flags(sbi, CP_SSA_IN_MERGE_FLAG); 
			clear_ckpt_flags(sbi, CP_SSA_MERGE_FLAG);

			// do ssa merge
//			printk("(%s : %d) merge ssa", __func__, __LINE__);
			down_write(&SM_I(sbi)->ssa_ltree_slock);
			ret = merge_ssa(sbi, 0); 
			up_write(&SM_I(sbi)->ssa_ltree_slock);
			if (!ret) {
				//printk("(%s : %d) merge ssa success"
				//		, __func__, __LINE__);
				set_ckpt_flags(sbi, CP_SSA_MERGE_DONE_FLAG);
				clear_ckpt_flags(sbi, CP_SSA_IN_MERGE_FLAG);
			} else {
				set_ckpt_flags(sbi, CP_SSA_MERGE_DONE_FLAG);
				clear_ckpt_flags(sbi, CP_SSA_IN_MERGE_FLAG);
				printk("(%s : %d) merge ssa failed"
						, __func__, __LINE__);
			}
//			f2fs_unlock_op(sbi);
			done = true;
		}

		//nat
		if (is_set_ckpt_flags(sbi, CP_NAT_MERGE_FLAG)) {

			set_ckpt_flags(sbi, CP_NAT_IN_MERGE_FLAG); 
			clear_ckpt_flags(sbi, CP_NAT_MERGE_FLAG);

//			printk("(%s : %d) merge nat", __func__, __LINE__);
			down_read(&NM_I(sbi)->nat_ltree_slock);
			ret = merge_nat(sbi, 0); 
			up_read(&NM_I(sbi)->nat_ltree_slock);

			if (!ret) {
				printk("(%s : %d) merge nat success"
						, __func__, __LINE__);
				set_ckpt_flags(sbi, CP_NAT_MERGE_DONE_FLAG);
				clear_ckpt_flags(sbi, CP_NAT_IN_MERGE_FLAG);
			} else {
				printk("(%s : %d) merge nat failed"
						, __func__, __LINE__);
			}
			done = true;
		}

		//sit
		
		if (is_set_ckpt_flags(sbi, CP_SIT_MERGE_FLAG)) {
			set_ckpt_flags(sbi, CP_SIT_IN_MERGE_FLAG); 
			clear_ckpt_flags(sbi, CP_SIT_MERGE_FLAG);

//			printk("(%s : %d) merge sit", __func__, __LINE__);
			down_read(&SM_I(sbi)->sit_ltree_slock);
			ret = merge_sit(sbi, 0); 
			up_read(&SM_I(sbi)->sit_ltree_slock);

			if (!ret) {
				printk("(%s : %d) merge sit success"
						, __func__, __LINE__);
				set_ckpt_flags(sbi, CP_SIT_MERGE_DONE_FLAG);
				clear_ckpt_flags(sbi, CP_SIT_IN_MERGE_FLAG);
			} else {
				printk("(%s : %d) merge sit failed"
						, __func__, __LINE__);
			}
			done = true;
		}

		//sleep
		if (done) {
			f2fs_submit_merged_write(sbi, META);
      f2fs_wait_on_all_pages(sbi, F2FS_MERGE_META);
	//		printk("(%s : %d) merge wait done", __func__, __LINE__);
    }
		msleep(time_ms);
	}
	return 0;
}

int f2fs_start_merge_thread(struct f2fs_sb_info *sbi)
{
	printk("(%s : %d) start merge thread", __func__, __LINE__);
	sbi->merge_thread = kthread_run(f2fs_merge, sbi, "f2fs_merge"); 

	if (IS_ERR(sbi->merge_thread)) {
		printk("(%s : %d) start merge thread failed", __func__, __LINE__);
		sbi->merge_thread = NULL;
		return -ENOMEM;
	}

	printk("(%s : %d) start merge thread success", __func__, __LINE__);
	return 0;
}

void f2fs_stop_merge_thread(struct f2fs_sb_info *sbi)
{
	printk("(%s : %d) stop merge thread", __func__, __LINE__);
	if (sbi->merge_thread) {
		kthread_stop(sbi->merge_thread);
	}
}
#endif /* DELAYED_MERGE */

void f2fs_init_ckpt_req_control(struct f2fs_sb_info *sbi)
{
	struct ckpt_req_control *cprc = &sbi->cprc_info;

	atomic_set(&cprc->issued_ckpt, 0);
	atomic_set(&cprc->total_ckpt, 0);
	atomic_set(&cprc->queued_ckpt, 0);
	cprc->ckpt_thread_ioprio = DEFAULT_CHECKPOINT_IOPRIO;
	init_waitqueue_head(&cprc->ckpt_wait_queue);
	init_llist_head(&cprc->issue_list);
	spin_lock_init(&cprc->stat_lock);
}

#if META_FOR_ZNS
/*
 * lookup get current meta area log 
 */
inline pgoff_t next_log_addr(struct f2fs_sb_info *sbi, int log_type){
	
	pgoff_t log_addr;
	int off_in_zone = 0;
	int stripe_idx = 0;
	int stripe_cnt = 1;
#if META_LOG_STRIPE
  stripe_cnt = META_STRIPE_CNT;
#endif
	if (log_type == SIT_LOG){
#if 0//META_LOG_STRIPE
		off_in_zone = SM_I(sbi)->sit_blks_in_log / stripe_cnt;
		stripe_idx = SM_I(sbi)->sit_blks_in_log % stripe_cnt;
#else 
		off_in_zone = SM_I(sbi)->sit_blks_in_log;
#endif
		log_addr = SM_I(sbi)->sit_log_blkaddr + stripe_idx * sbi->blocks_per_blkz;
		log_addr += off_in_zone;
//		log_addr = SM_I(sbi)->sit_log_blkaddr + SM_I(sbi)->sit_blks_in_log;
		SM_I(sbi)->sit_blks_in_log++;
#if DELAYED_MERGE
		log_addr = log_addr + SM_I(sbi)->cur_sit_log * sbi->blocks_per_blkz;
#endif
	} else if (log_type == NAT_LOG) {
#if 0//META_LOG_STRIPE
		off_in_zone = NM_I(sbi)->nat_blks_in_log / stripe_cnt;
		stripe_idx = NM_I(sbi)->nat_blks_in_log % stripe_cnt;
#else 
		off_in_zone = NM_I(sbi)->nat_blks_in_log;
#endif
		log_addr = NM_I(sbi)->nat_log_blkaddr + stripe_idx * sbi->blocks_per_blkz;
		log_addr += off_in_zone;
//		log_addr = NM_I(sbi)->nat_log_blkaddr + NM_I(sbi)->nat_blks_in_log;
		NM_I(sbi)->nat_blks_in_log++;
		
//		NM_I(sbi)->nat_stripe_idx++;
//		if (NM_I(sbi)->nat_stripe_idx > 4);
//			NM_I(sbi)->nat_stripe_idx = 0;
#if DELAYED_MERGE
		log_addr = log_addr + NM_I(sbi)->cur_nat_log * sbi->blocks_per_blkz;
#endif		
	} else if (log_type == SSA_LOG) {
		off_in_zone = SM_I(sbi)->sum_blks_in_log / stripe_cnt;
		stripe_idx = SM_I(sbi)->sum_blks_in_log % stripe_cnt;

		log_addr = SM_I(sbi)->sum_log_blkaddr + stripe_idx * sbi->blocks_per_blkz;
		log_addr += off_in_zone;
//		log_addr = SM_I(sbi)->sum_log_blkaddr + SM_I(sbi)->sum_blks_in_log;
		SM_I(sbi)->sum_blks_in_log++;
#if DELAYED_MERGE
		log_addr = log_addr + SM_I(sbi)->cur_sum_log * stripe_cnt * sbi->blocks_per_blkz;
#endif		
	} else {
		f2fs_bug_on(sbi, 1);
	}
	return log_addr;
}

struct page *get_next_log_page(struct f2fs_sb_info *sbi, int log_type){
	struct page *page;
	pgoff_t off;

	if(unlikely(log_type < 0 || log_type > SSA_LOG)) {
		f2fs_bug_on(sbi, 1);
		return NULL;
	}

	off = next_log_addr(sbi, log_type);
	
	if (log_type == SIT_LOG){
		if (off >= NM_I(sbi)->nat_log_blkaddr){
			f2fs_bug_on(sbi, 1);
			return NULL;
		}

	} else if (log_type == NAT_LOG) {
		if (off >= SM_I(sbi)->sum_log_blkaddr){
			f2fs_bug_on(sbi, 1);
			return NULL;
		}
	} else if (log_type == SSA_LOG) {
		if (off >= SM_I(sbi)->main_blkaddr) {
			f2fs_bug_on(sbi, 1);
			return NULL;
		}
	}

	if (unlikely(off < SM_I(sbi)->sit_log_blkaddr)){
		f2fs_bug_on(sbi, 1);
		return NULL;
	}

	page = f2fs_grab_meta_page(sbi, off);
	set_page_dirty(page);
	//printk("(%s:%d) page index : %lu, page dirty : %d", 
	//	__func__, __LINE__, page->index, PageDirty(page));

	return page;
}
static int __move_metadata_page(struct f2fs_sb_info *sbi, 
		pgoff_t src_off, pgoff_t dst_off){
	
	struct page *src_page, *dst_page;
	int ret;
	//read src and dst page
	src_page = f2fs_get_meta_page(sbi, src_off); //read src page
	if(IS_ERR(src_page)){
		printk("(%s : %d) error while reading src page(%lu off)"
				, __func__, __LINE__, src_off);
		return -EIO;
	}
	dst_page = f2fs_grab_meta_page(sbi, dst_off);
	f2fs_copy_page(src_page, dst_page);
	f2fs_put_page(src_page, 1);
	//write page
	inc_page_count(sbi, F2FS_DIRTY_META);
	//printk("(%s : %d) dst_page(idx: %lu)"	, __func__, __LINE__, dst_page->index);
	if((ret = f2fs_sync_single_meta_page(dst_page))){
		printk("(%s : %d) write error while moving clean metadata in dirty zone(idx: %lu)"
				, __func__, __LINE__, dst_page->index);
		unlock_page(dst_page);
	}
	f2fs_put_page(dst_page, 0);

	return ret;	
}
static bool check_end_of_meta(struct f2fs_sb_info *sbi, 
		block_t block_off, int type)
{
	block_t blk_cnt;
	if(type == NAT || type == NAT_LOG){
		blk_cnt = NM_I(sbi)->nat_blocks;
	} else if(type == SIT || type == SIT_LOG){
		blk_cnt = SIT_I(sbi)->sit_blocks;
	} else if(type == SSA || type == SSA_LOG){
		return false;
	} else {
		return false;
	}
	if(block_off >= blk_cnt){
//		printk("(%s : %d) end of meta, block_off(%u), end blk(%u)",
//				__func__, __LINE__, block_off, blk_cnt);
		return true;
	}
	return false;

}
int advance_meta_zone_wp(struct f2fs_sb_info *sbi,
		block_t zoff, int cur_wp, int add, 
		int type){
	int i, ret, full = 0;
	block_t meta_off, base;
	struct f2fs_dev_info *zbd;
	sector_t zone_sectors;
	char *bitmap;
	int ssa = 0;

	meta_off = meta_zoff_to_boff(sbi, zoff) + cur_wp;
	for(i=0;i<add;i++){
		if(check_end_of_meta(sbi, meta_off + i, type)){
			full = 1;
			break;
		}
		ret = move_metadata_page(sbi, meta_off + i, type);
		if(ret){
			return -1;
		}
	}
	if(full){
		//printk("(%s : %d) meta zone full",
		//			__func__, __LINE__);
		zbd = &FDEV(0);
		zone_sectors = SECTOR_FROM_BLOCK(sbi->blocks_per_blkz);
		
		if(type == NAT || type == NAT_LOG){
			base = NM_I(sbi)->nat_blkaddr;
			bitmap = NM_I(sbi)->nat_bitmap;
		} else if (type == SIT || type == SIT_LOG){
			base = SIT_I(sbi)->sit_base_addr;
			bitmap = SIT_I(sbi)->sit_bitmap;
		} else if(type == SSA || type == SSA_LOG){
			base = SM_I(sbi)->ssa_blkaddr;
			bitmap = SM_I(sbi)->ssa_bitmap;
			ssa = 1;
		}
		base = get_cur_meta_blkaddr(sbi, 
				meta_zoff_to_boff(sbi, zoff), 
				base, bitmap, ssa);
		if(ret){
			printk("(%s : %d) zone finish failed. error : %d, base addr : %u",
					__func__, __LINE__, ret, base);
		}
		return SECTOR_TO_BLOCK(zone_sectors);
	}

	return cur_wp+add;
}
int move_metadata_page(struct f2fs_sb_info *sbi, 
		block_t meta_off, int type){

	int ret = -1;
	pgoff_t base;
	char *bitmap;
	pgoff_t src_off, dst_off;
	int ssa = 0;

	if(type == NAT || type == NAT_LOG){
		base = NM_I(sbi)->nat_blkaddr;
		bitmap = NM_I(sbi)->nat_bitmap;
	} else if (type == SIT || type == SIT_LOG){
		base = SIT_I(sbi)->sit_base_addr;
		bitmap = SIT_I(sbi)->sit_bitmap;
	} else if(type == SSA || type == SSA_LOG){
		base = SM_I(sbi)->ssa_blkaddr;
		bitmap = SM_I(sbi)->ssa_bitmap;
		ssa = 1;
	}

	src_off= get_cur_meta_blkaddr(sbi, meta_off, base, bitmap, ssa);
	dst_off= get_next_meta_blkaddr(sbi, meta_off, base, bitmap, ssa);
//  if (type == NAT || type == NAT_LOG) {
//    printk("(%s : %d) move page(%lu) to page(%lu)"
//        , __func__, __LINE__, src_off, dst_off);
//  }
	ret = __move_metadata_page(sbi, src_off, dst_off);
	if(type != SSA_LOG && type != SSA)
		f2fs_change_bit(meta_off, bitmap);

	return ret;
}
int reset_meta_zone_towrite(struct f2fs_sb_info *sbi,
		block_t zone_off, int type)
{
	struct block_device *bdev;
	block_t blkstart, blklen;
	block_t base;
	char *bitmap;
	unsigned int offset = 0;
	int log = 0, ret, i;
	
	bdev = FDEV(0).bdev;
	
	switch(type){
		case SIT_LOG:
			base = SM_I(sbi)->sit_log_blkaddr;
			log = 1;
			break;
		case NAT_LOG:
			base = NM_I(sbi)->nat_log_blkaddr;
			log = 1;
			break;
		case SSA_LOG:
			base = SM_I(sbi)->sum_log_blkaddr;
			log = 1;
			break;
		case SIT:
			base = SIT_I(sbi)->sit_base_addr;
			bitmap = SIT_I(sbi)->sit_bitmap;
			offset = meta_zoff_to_boff(sbi, zone_off);
			break;
		case NAT:
			base = NM_I(sbi)->nat_blkaddr;
			bitmap = NM_I(sbi)->nat_bitmap;
			offset = meta_zoff_to_boff(sbi, zone_off);
			break;
		case SSA:
			base = SM_I(sbi)->ssa_blkaddr; 
			bitmap = SM_I(sbi)->ssa_bitmap;
			offset = zone_off;
			break;
		default:
			f2fs_bug_on(sbi, 1);
			return -1;
	}

	if (log) {
		blkstart = base;
#if DELAYED_MERGE
		if (type == SSA_LOG){
#if META_LOG_STRIPE
			blkstart = blkstart + (SM_I(sbi)->cur_sum_log ^ 0x1) * 
        META_STRIPE_CNT * sbi->blocks_per_blkz;
#else
			blkstart = blkstart + (SM_I(sbi)->cur_sum_log ^ 0x1) * sbi->blocks_per_blkz;
#endif
    }
		else if (type == NAT_LOG)
			blkstart = blkstart + (NM_I(sbi)->cur_nat_log ^ 0x1) * sbi->blocks_per_blkz;
		else if (type == SIT_LOG)
			blkstart = blkstart + (SM_I(sbi)->cur_sit_log ^ 0x1) * sbi->blocks_per_blkz;
				
#endif
	} else {
		blkstart = base + 2 * zone_off * sbi->blocks_per_blkz;
		if(f2fs_test_bit(offset, bitmap) == 0)
			blkstart += sbi->blocks_per_blkz;
	}
	blklen = sbi->blocks_per_blkz; 
#if META_LOG_STRIPE
  if (type == SSA_LOG) {
    for (i=0;i<META_STRIPE_CNT;i++){
      ret = f2fs_issue_discard_zone(sbi, bdev, blkstart, blklen);
      //printk("(%s:%d) issue discard zone start block %x", __func__, __LINE__, blkstart);
      blkstart += blklen;
      if (ret)
        return ret;
    }
  } else {
    ret = f2fs_issue_discard_zone(sbi, bdev, blkstart, blklen);
    //printk("(%s:%d) issue discard zone start block %x", __func__, __LINE__, blkstart);
  }
#else
  ret = f2fs_issue_discard_zone(sbi, bdev, blkstart, blklen);
#endif
	return ret;
}
#endif
