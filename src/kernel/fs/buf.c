#include "mod.h"

static buffer_node_t buf_cache[N_BUFFER];
static buffer_node_t buf_head_active, buf_head_inactive;
static spinlock_t lk_buf_cache;

/*
	将一个节点拿出来并插入
	1. 活跃链表的头部 buf_head_active->next
	2. 活跃链表的尾部 buf_head_active->prev
	3. 不活跃链表的头部 buf_head_inactive->next
	4. 不活跃链表的尾部 buf_head_inactive->prev
*/
static void insert_node(buffer_node_t* node, bool insert_active, bool insert_next) {
	/* 如果有需要, 让node先离开当前位置 */
	if (node->next != NULL && node->prev != NULL) {
		node->next->prev = node->prev;
		node->prev->next = node->next;
	}

	/* 选择目标双向循环链表 */
	buffer_node_t* head = &buf_head_inactive;
	if (insert_active)
		head = &buf_head_active;

	/* 然后将node插入head->next or head->prev */
	if (insert_next) {
		node->next = head->next;
		node->next->prev = node;
		node->prev = head;
		head->next = node;
	}
	else {
		node->prev = head->prev;
		node->prev->next = node;
		node->next = head;
		head->prev = node;
	}
}

/*
	buffer系统初始化：
	1. 初始化全局的lk_buf_cache + buf_head_active + buf_head_inactive
	2. 初始化buf_cache中的所有node, 并将他们放在不活跃链表中
*/
void buffer_init() {
	spinlock_init(&lk_buf_cache, "buf_cache");
	buf_head_active.next = buf_head_active.prev = &buf_head_active;
	buf_head_inactive.next = buf_head_inactive.prev = &buf_head_inactive;
	for (int i = 0;i < N_BUFFER;i++) {
		buf_cache[i].buf.ref = 0;
		buf_cache[i].buf.block_num = BLOCK_NUM_UNUSED;
		buf_cache[i].buf.data = NULL;
		sleeplock_init(&buf_cache[i].buf.slk, "buf_cache");
		buf_cache[i].next = buf_cache[i].prev = NULL;
		insert_node(&buf_cache[i], false, true);
	}
}

/* 磁盘读取: block -> buf */
static void buffer_read(buffer_t* buf) {
	assert(buf->data != NULL, "buffer_read: buf->data is NULL");
	assert(buf->block_num != BLOCK_NUM_UNUSED, "buffer_read: buf->block_num is BLOCK_NUM_UNUSED");
	assert(sleeplock_holding(&buf->slk), "buffer_read: buf->slk is not held");
	virtio_disk_rw(buf, false);
}

/* 磁盘写入: buf -> block */
void buffer_write(buffer_t* buf) {
	assert(buf->data != NULL, "buffer_write: buf->data is NULL");
	assert(buf->block_num != BLOCK_NUM_UNUSED, "buffer_write: buf->block_num is BLOCK_NUM_UNUSED");
	assert(sleeplock_holding(&buf->slk), "buffer_write: buf->slk is not held");
	virtio_disk_rw(buf, true);

}

/* 从buf_cache中获取一个buf */
buffer_t* buffer_get(uint32 block_num) {
	spinlock_acquire(&lk_buf_cache);
	for (buffer_node_t* node = buf_head_active.next; node != &buf_head_active; node = node->next) {
		if (node->buf.block_num == block_num) {
			insert_node(node, true, true);
			node->buf.ref++;
			spinlock_release(&lk_buf_cache);
			sleeplock_acquire(&node->buf.slk);
			return &node->buf;
		}
	}
	for (buffer_node_t* node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
		if (node->buf.block_num == block_num) {
			insert_node(node, true, true);
			node->buf.ref++;
			spinlock_release(&lk_buf_cache);
			sleeplock_acquire(&node->buf.slk);
			return &node->buf;
		}
	}
	buffer_node_t* node = buf_head_inactive.prev;
	assert(node != &buf_head_inactive, "buffer_get: no free buffer");
	insert_node(node, true, true);
	node->buf.block_num = block_num;
	node->buf.ref = 1;
	spinlock_release(&lk_buf_cache);
	sleeplock_acquire(&node->buf.slk);
	if (node->buf.data == NULL) {
		node->buf.data = pmem_alloc(false);
		assert(node->buf.data != NULL, "buffer_get: pmem_alloc failed");
	}
	buffer_read(&node->buf);
	return &node->buf;
}

/* 向buf_cache归还一个buf */
void buffer_put(buffer_t* buf) {
	spinlock_acquire(&lk_buf_cache);
	assert(buf->ref > 0, "buffer_put: buf->ref is 0");
	buf->ref--;
	if (buf->ref == 0) {
		buffer_node_t* node = (buffer_node_t*)buf;
		insert_node(node, false, true);
	}
	spinlock_release(&lk_buf_cache);
	sleeplock_release(&buf->slk);
}

/*
	从后向前遍历非活跃链表, 尝试释放buffer_count个buffer持有的物理内存(data)
	返回成功释放资源的buffer数量
*/
uint32 buffer_freemem(uint32 buffer_count) {
	uint32 freed_count = 0;
	spinlock_acquire(&lk_buf_cache);
	for (buffer_node_t* node = buf_head_inactive.prev; node != &buf_head_inactive && buffer_count > 0; node = node->prev) {
		if (node->buf.ref == 0 && node->buf.data != NULL) {
			if (node->buf.data) pmem_free((uint64)node->buf.data, false);
			node->buf.data = NULL;
			node->buf.block_num = BLOCK_NUM_UNUSED;
			buffer_count--;
			freed_count++;
		}
	}
	spinlock_release(&lk_buf_cache);
	return freed_count;
}

/* 输出buffer_cache的信息 (for test) */
void buffer_print_info() {
	buffer_node_t* node;

	assert(N_BUFFER == N_BUFFER_TEST, "buffer_print_info: invalid N_BUFFER");

	spinlock_acquire(&lk_buf_cache);

	printf("buffer_cache information:\n");

	printf("1.active list:\n");
	for (node = buf_head_active.next; node != &buf_head_active; node = node->next) {
		printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
			(int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
	}
	printf("over!\n");

	printf("2.inactive list:\n");
	for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
		printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
			(int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
	}
	printf("over!\n");

	spinlock_release(&lk_buf_cache);
}
