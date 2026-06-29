#include "mod.h"

extern super_block_t sb;

/* 内存中的inode资源集合 */
static inode_t inode_cache[N_INODE];
static spinlock_t lk_inode_cache;

/* inode_cache初始化 */
void inode_init() {
	spinlock_init(&lk_inode_cache, "lk_inode_cache");
	for (int i = 0; i < N_INODE; i++) {
		inode_cache[i].valid_info = false;
		inode_cache[i].ref = 0;
		inode_cache[i].inode_num = INVALID_INODE_NUM;
		sleeplock_init(&inode_cache[i].slk, "slk_inode");
	}
}

/*--------------------关于inode->index的增删查操作-----------------*/

/*
	供free_data_blocks使用
	递归删除inode->index中的一个元素
	返回删除过程中是否遇到空的block_num (文件末尾)
*/
static bool __free_data_blocks(uint32 block_num, uint32 level) {
	if (block_num == 0 || block_num == INVALID_BLOCK_NUM) return true;
	if (level == 0) {
		bitmap_free_block(block_num);
		return false;
	}
	buffer_t* buf1 = buffer_get(block_num);
	bool meet_empty = false;
	for (int i = 0;i < BLOCK_SIZE / sizeof(uint32);i++) {
		uint32 next_block_num = ((uint32*)buf1->data)[i];
		meet_empty = __free_data_blocks(next_block_num, level - 1);
		if (meet_empty) break;
	}
	buffer_put(buf1);
	bitmap_free_block(block_num);
	return meet_empty;
}

/*
	释放inode管理的blocks
*/
static void free_data_blocks(uint32* inode_index) {
	unsigned int i;
	bool meet_empty = false;

	/* step-1: 释放直接映射的block */
	for (i = 0; i < INODE_INDEX_1; i++) {
		meet_empty = __free_data_blocks(inode_index[i], 0);
		if (meet_empty) return;
	}

	/* step-2: 释放一级间接映射的block */
	for (; i < INODE_INDEX_2; i++) {
		meet_empty = __free_data_blocks(inode_index[i], 1);
		if (meet_empty) return;
	}

	/* step-3: 释放二级间接映射的block */
	for (; i < INODE_INDEX_3; i++) {
		meet_empty = __free_data_blocks(inode_index[i], 2);
		if (meet_empty) return;
	}

	panic("free_data_blocks: impossible!");
}

/*
	获取inode第logical_block_num个block的物理序号block_num
	调用者保证输入的logical_block_num只有两种情况:
	1. 属于已经分配的区域 (返回block_num)
	2. 将已经分配出去的区域往外扩展1个block (申请block并返回block_num)
	成功返回block_num, 失败返回-1
*/
static uint32 locate_or_add_block(uint32* inode_index, uint32 logical_block_num) {
	const uint32 P = BLOCK_SIZE / sizeof(uint32);
	if (logical_block_num < INODE_INDEX_1) {
		if (inode_index[logical_block_num] == INVALID_BLOCK_NUM) {
			uint32 new_block_num = bitmap_alloc_block();
			if (new_block_num == -1) return -1;
			inode_index[logical_block_num] = new_block_num;
		}
		return inode_index[logical_block_num];
	}
	else if (logical_block_num < INODE_INDEX_2) {
		uint32 idx = (logical_block_num - INODE_INDEX_1) / P;
		buffer_t* buf1 = NULL;
		if (inode_index[idx] == INVALID_BLOCK_NUM) {
			uint32 new_block_num = bitmap_alloc_block();
			if (new_block_num == -1) return -1;
			inode_index[idx] = new_block_num;
			buf1 = buffer_get(new_block_num);
			memset(buf1->data, 0, BLOCK_SIZE);
		}
		else buf1 = buffer_get(inode_index[idx]);
		uint32* inode_idx_1 = (uint32*)buf1->data;
		uint32 idx_1 = (logical_block_num - INODE_INDEX_1) % P;
		if (inode_idx_1[idx_1] == INVALID_BLOCK_NUM) {
			uint32 new_block_num = bitmap_alloc_block();
			if (new_block_num == -1) {
				buffer_write(buf1);
				buffer_put(buf1);
				return -1;
			}
			inode_idx_1[idx_1] = new_block_num;
		}
		uint32 block_num = inode_idx_1[idx_1];
		buffer_write(buf1);
		buffer_put(buf1);
		return block_num;
	}
	else if (logical_block_num < INODE_INDEX_3) {
		buffer_t* buf1 = NULL;
		if (inode_index[INODE_INDEX_3] == INVALID_BLOCK_NUM) {
			uint32 new_block_num = bitmap_alloc_block();
			if (new_block_num == -1) return -1;
			inode_index[INODE_INDEX_3] = new_block_num;
			buf1 = buffer_get(new_block_num);
			memset(buf1->data, 0, BLOCK_SIZE);
		}
		else buf1 = buffer_get(inode_index[idx]);
		uint32* inode_idx_1 = (uint32*)buf1->data;
		uint32 idx_1 = (logical_block_num - INODE_INDEX_2) / P;
		buffer_t* buf2 = NULL;
		if (inode_idx_1[idx_1] == INVALID_BLOCK_NUM) {
			uint32 new_block_num = bitmap_alloc_block();
			if (new_block_num == -1) {
				buffer_write(buf1);
				buffer_put(buf1);
				return -1;
			}
			inode_idx_1[idx_1] = new_block_num;
			buf2 = buffer_get(new_block_num);
			memset(buf2->data, 0, BLOCK_SIZE);
		}
		else buf2 = buffer_get(inode_idx_1[idx_1]);
		uint32* inode_idx_2 = (uint32*)buf2->data;
		uint32 idx_2 = (logical_block_num - INODE_INDEX_2) % P;
		if (inode_idx_2[idx_2] == INVALID_BLOCK_NUM) {
			uint32 new_block_num = bitmap_alloc_block();
			if (new_block_num == -1) {
				buffer_write(buf2);
				buffer_put(buf2);
				buffer_write(buf1);
				buffer_put(buf1);
				return -1;
			}
			inode_idx_2[idx_2] = new_block_num;
		}
		uint32 block_num = inode_idx_2[idx_2];
		buffer_write(buf2);
		buffer_put(buf2);
		buffer_write(buf1);
		buffer_put(buf1);
		return block_num;
	}
}
/*---------------------关于inode的管理: get dup lock unlock put----------------------*/

/*
	磁盘里的inode <-> 内存里的inode
	调用者需要持有ip->slk并设置合理的inode_num
*/
void inode_rw(inode_t* ip, bool write) {

}

/*
	尝试在inode_cache里寻找是否存在目标inode
	如果不存在则申请一个空闲的inode
	如果没有空闲位置直接panic
	核心逻辑: ref++
*/
inode_t* inode_get(uint32 inode_num) {

}

/*
	在磁盘里创建1个新的inode
	1. 查询和修改inode_bitmap
	2. 填充inode_region对应位置的inode
	注意: 返回的inode未上锁
*/
inode_t* inode_create(uint16 type, uint16 major, uint16 minor) {

}

/*
	ip->ref++ with lock proctect
*/
inode_t* inode_dup(inode_t* ip) {

}

/*
	锁住inode
	如果inode->disk_info无效则更新一波
*/
void inode_lock(inode_t* ip) {

}

/*
	解锁inode
*/
void inode_unlock(inode_t* ip) {

}

/*
	与inode_get相对应, 调用者释放inode资源
	如果达成某些条件, 可能触发彻底删除
*/
void inode_put(inode_t* ip) {

}

/*
	在磁盘里删除1个inode
	1. 修改inode_bitmap释放inode_region资源
	2. 修改block_bitmap释放block_region资源
	注意: 调用者需要持有ip->slk
*/
void inode_delete(inode_t* ip) {

}

/*----------------------基于inode的数据读写操作--------------------*/

/*
	基于inode的数据读取
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝data[offset,offset+len)到dst(用户态地址/内核态地址)
	返回读取的数据量(字节)
*/
uint32 inode_read_data(inode_t* ip, uint32 offset, uint32 len, void* dst, bool is_user_dst) {
	assert(sleeplock_holding(&ip->slk), "inode_read_data: slk");
	if (offset >= ip->disk_info.size) return 0;
	if (offset + len > ip->disk_info.size) len = ip->disk_info.size - offset;
	if (len == 0) return 0;
	uint64 dst_bytes = (uint64)dst;
	uint32 cur_block = offset / BLOCK_SIZE, res = 0;
	offset %= BLOCK_SIZE;
	while (res < len) {
		uint32 block_num = locate_or_add_block(ip->disk_info.index, cur_block);
		if (block_num == -1) return res;
		uint32 to_copy = BLOCK_SIZE - offset > len - res ? len - res : BLOCK_SIZE - offset;
		buffer_t* buf = buffer_get(block_num);
		if (is_user_dst)
			uvm_copyout(myproc()->pgtbl, (uint64)dst_bytes, (uint64)(buf->data + offset), to_copy);
		else
			memmove((void*)dst_bytes, (void*)(buf->data + offset), to_copy);
		buffer_put(buf);
		res += to_copy;
		dst_bytes += to_copy;
		offset = 0;
		cur_block++;
	}
	return res;
}

/*
	基于inode的数据写入
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝src(用户态地址/内核态地址)到data[offset,offset+len)
	返回写入的数据量(字节)
*/
uint32 inode_write_data(inode_t* ip, uint32 offset, uint32 len, void* src, bool is_user_src) {
	assert(sleeplock_holding(&ip->slk), "inode_write_data: slk");
	if (offset + len > MAX_FILE_SIZE) return 0;
	if (offset + len > ip->disk_info.size) ip->disk_info.size = offset + len;
	if (len == 0) return 0;
	uint64 src_bytes = (uint64)src;
	uint32 cur_block = offset / BLOCK_SIZE, res = 0;
	offset %= BLOCK_SIZE;
	while (res < len) {
		uint32 block_num = locate_or_add_block(ip->disk_info.index, cur_block);
		if (block_num == -1) return res;
		uint32 to_copy = BLOCK_SIZE - offset > len - res ? len - res : BLOCK_SIZE - offset;
		buffer_t* buf = buffer_get(block_num);
		if (is_user_src)
			uvm_copyin(myproc()->pgtbl, (uint64)(buf->data + offset), src_bytes, to_copy);
		else
			memmove((void*)(buf->data + offset), (void*)src_bytes, to_copy);
		buffer_write(buf);
		buffer_put(buf);
		res += to_copy;
		src_bytes += to_copy;
		offset = 0;
		cur_block++;
	}
	inode_rw(ip, true);
	return res;
}

static char* inode_type_list[] = { "DATA", "DIR", "DEVICE" };

/* 输出inode信息(for debug) */
void inode_print(inode_t* ip, char* name) {
	assert(sleeplock_holding(&ip->slk), "inode_print: slk");

	spinlock_acquire(&lk_inode_cache);

	printf("inode %s:\n", name);
	printf("ref = %d, inode_num = %d, valid_info = %d\n", ip->ref, ip->inode_num, ip->valid_info);
	printf("type = %s, major = %d, minor = %d, nlink = %d, size = %d\n", inode_type_list[ip->disk_info.type],
		ip->disk_info.major, ip->disk_info.minor, ip->disk_info.nlink, ip->disk_info.size);

	printf("index_list = [ ");
	for (int i = 0; i < INODE_INDEX_1; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_1; i < INODE_INDEX_2; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_2; i < INODE_INDEX_3; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("]\n\n");

	spinlock_release(&lk_inode_cache);
}
