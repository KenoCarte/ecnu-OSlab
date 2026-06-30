#include "mod.h"

// 内核空间和用户空间的可分配物理页分开描述
static alloc_region_t kern_region, user_region;

// 物理内存的初始化
// 本质上就是填写kern_region和user_region, 包括基本数值和空闲链表
void pmem_init(void) {
    kern_region.begin = (uint64)ALLOC_BEGIN;
    kern_region.end = (uint64)ALLOC_BEGIN + PGSIZE * KERN_PAGES;
    kern_region.allocable = KERN_PAGES;
    user_region.begin = (uint64)ALLOC_BEGIN + PGSIZE * KERN_PAGES;
    user_region.end = (uint64)ALLOC_END;
    user_region.allocable = (user_region.end - user_region.begin) / PGSIZE;
    spinlock_init(&kern_region.lk, "kern_region");
    spinlock_init(&user_region.lk, "user_region");
    kern_region.list_head.next = NULL;
    user_region.list_head.next = NULL;
    for (uint64 page = kern_region.end - PGSIZE; page >= kern_region.begin; page -= PGSIZE) {
        page_node_t* node = (page_node_t*)page;
        node->next = kern_region.list_head.next;
        kern_region.list_head.next = node;
    }
    for (uint64 page = user_region.end - PGSIZE; page >= user_region.begin; page -= PGSIZE) {
        page_node_t* node = (page_node_t*)page;
        node->next = user_region.list_head.next;
        user_region.list_head.next = node;
    }
}

// 尝试返回一个可分配的清零后的物理页
// 失败则panic锁死
void* pmem_alloc(bool in_kernel) {
    page_node_t* page;
    spinlock_t* lk;
    if (in_kernel) {
        lk = &kern_region.lk;
        spinlock_acquire(lk);
        if (kern_region.allocable == 0) {
            panic("pmem_alloc: no more kernel page\n");
        }
        page = kern_region.list_head.next;
        kern_region.list_head.next = page->next;
        kern_region.allocable--;
    }
    else {
        lk = &user_region.lk;
        spinlock_acquire(lk);
        if (user_region.allocable == 0) {
            panic("pmem_alloc: no more user page\n");
        }
        page = user_region.list_head.next;
        user_region.list_head.next = page->next;
        user_region.allocable--;
    }
    spinlock_release(lk);
    memset(page, 0, PGSIZE);
    return page;
}

// 释放一个物理页
// 失败则panic锁死
void pmem_free(uint64 page, bool in_kernel) {
    page_node_t* node = (page_node_t*)page;
    spinlock_t* lk;
    if (in_kernel) {
        lk = &kern_region.lk;
        spinlock_acquire(lk);
        if (page < kern_region.begin || page > kern_region.end) {
            panic("pmem_free: invalid kernel page\n");
        }
        node->next = kern_region.list_head.next;
        kern_region.list_head.next = node;
        kern_region.allocable++;
    }
    else {
        lk = &user_region.lk;
        spinlock_acquire(lk);
        if (page < user_region.begin || page > user_region.end) {
            panic("pmem_free: invalid user page\n");
        }
        node->next = user_region.list_head.next;
        user_region.list_head.next = node;
        user_region.allocable++;
    }
    spinlock_release(lk);
}

void pmem_stat(uint32* free_pages_in_kernel, uint32* free_pages_in_user) {
    spinlock_acquire(&kern_region.lk);
    *free_pages_in_kernel = kern_region.allocable;
    spinlock_release(&kern_region.lk);
    spinlock_acquire(&user_region.lk);
    *free_pages_in_user = user_region.allocable;
    spinlock_release(&user_region.lk);
}
