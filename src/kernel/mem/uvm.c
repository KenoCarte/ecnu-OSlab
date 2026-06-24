#include "mod.h"

/*--------------------part-1: 关于内核空间<->用户空间的数据传递--------------------*/

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len) {
    uint64 va = ALIGN_DOWN(src, PGSIZE);
    uint32 off = src - va, copied = 0;
    while (copied < len) {
        uint32 to_copy = MIN(PGSIZE - off, len - copied);
        pte_t* pte = vm_getpte(pgtbl, va, false);
        assert(pte != NULL && (*pte) & PTE_V, "uvm_copyin: invalid pte");
        uint64 pa = (uint64)PTE_TO_PA(*pte);
        memmove((char*)(dst + copied), (char*)(pa + off), to_copy);
        copied += to_copy;
        off = 0;
        va += PGSIZE;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len) {
    uint64 va = ALIGN_DOWN(dst, PGSIZE);
    uint32 off = dst - va, copied = 0;
    while (copied < len) {
        uint32 to_copy = MIN(PGSIZE - off, len - copied);
        pte_t* pte = vm_getpte(pgtbl, va, false);
        assert(pte != NULL && (*pte) & PTE_V, "uvm_copyout: invalid pte");
        uint64 pa = (uint64)PTE_TO_PA(*pte);
        memmove((char*)(pa + off), (char*)(src + copied), to_copy);
        copied += to_copy;
        off = 0;
        va += PGSIZE;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen) {
    uint64 va = ALIGN_DOWN(src, PGSIZE);
    uint32 off = src - va, copied = 0;
    while (copied < maxlen) {
        uint32 to_copy = MIN(PGSIZE - off, maxlen - copied);
        pte_t* pte = vm_getpte(pgtbl, va, false);
        assert(pte != NULL && (*pte) & PTE_V, "uvm_copyin_str: invalid pte");
        uint64 pa = (uint64)PTE_TO_PA(*pte);
        for (uint32 i = 0; i < to_copy; i++) {
            char c = *(char*)(pa + off + i);
            *(char*)(dst + copied + i) = c;
            if (c == '\0') return;
        }
        copied += to_copy;
        off = 0;
        va += PGSIZE;
    }
}

/*--------------------part-2: mmap_region相关--------------------*/

// 打印以mmap为首的mmap链
// for debug
void uvm_show_mmaplist(mmap_region_t* mmap) {
    mmap_region_t* tmp = mmap;
    printf("\nalloced mmap_space:\n");
    if (tmp == NULL)
        printf("empty\n");
    while (tmp != NULL) {
        printf("alloced mmap_region: %p ~ %p\n", tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}

// 两个 mmap_region 区域合并
// 注意: 保留一个 释放一个 不操作 next 指针
// 由uvm_mmap调用
static void mmap_merge(mmap_region_t* mmap_1, mmap_region_t* mmap_2, bool keep_mmap_1) {
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");

    // merge
    if (keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    }
    else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 寻找一块足够大的区域(len), 作为 mmap_region
// 由uvm_mmap调用(处理begin==0的情况)
// 成功返回begin, 失败返回0
static uint64 uvm_mmap_find(mmap_region_t* head_mmap, uint64 len, mmap_region_t** p_last_mmap, mmap_region_t** p_tmp_mmap) {
    uint64 cur = MMAP_BEGIN;
    while (*p_tmp_mmap) {
        if (cur + len <= (*p_tmp_mmap)->begin) return cur;
        cur = (*p_tmp_mmap)->begin + (*p_tmp_mmap)->npages * PGSIZE;
        *p_last_mmap = *p_tmp_mmap;
        *p_tmp_mmap = (*p_tmp_mmap)->next;
    }
    if (cur + len <= MMAP_END) return cur;
    return 0;
}

// 在用户页表和进程mmap链里新增mmap区域 [begin, begin + npages * PGSIZE)
// 调用者保证begin是page-aligned的, 页面权限为perm
// 注意: 如果start==0, 意味着需要内核自主找一块足够大的空间
// 失败则panic卡死
void uvm_mmap(uint64 begin, uint32 npages, int perm) {
    proc_t* p = myproc();
    assert(p != NULL, "uvm_mmap: p is NULL");
    mmap_region_t* last_mmap = NULL;
    mmap_region_t** p_last_mmap = &last_mmap, ** p_tmp_mmap = &p->mmap;
    if (!begin) {
        begin = uvm_mmap_find(p->mmap, npages * PGSIZE, p_last_mmap, p_tmp_mmap);
        assert(begin != 0, "uvm_mmap: no enough space");
    }
    else {
        while (*p_tmp_mmap) {
            if (begin + npages * PGSIZE <= (*p_tmp_mmap)->begin) break;
            *p_last_mmap = *p_tmp_mmap;
            p_tmp_mmap = &((*p_tmp_mmap)->next);
        }
        assert(begin >= USER_BASE && begin + npages * PGSIZE <= MMAP_END, "uvm_mmap: out of range");
    }
    mmap_region_t* node = mmap_region_alloc();
    node->begin = begin;
    node->npages = npages;
    node->next = *p_tmp_mmap;
    if (*p_last_mmap) (*p_last_mmap)->next = node;
    else p->mmap = node;
    if (*p_tmp_mmap && begin + npages * PGSIZE == (*p_tmp_mmap)->begin) {
        node->next = (*p_tmp_mmap)->next;
        mmap_merge(node, *p_tmp_mmap, true);
    }
    if (*p_last_mmap && (*p_last_mmap)->begin + (*p_last_mmap)->npages * PGSIZE == begin) {
        (*p_last_mmap)->next = node->next;
        mmap_merge(*p_last_mmap, node, true);
    }
    for (uint64 va = begin; va < begin + npages * PGSIZE; va += PGSIZE) {
        uint64 pa = (uint64)pmem_alloc(false);
        assert(pa != 0, "uvm_mmap: pmem_alloc failed");
        vm_mappages(p->pgtbl, va, pa, PGSIZE, perm | PTE_U);
    }
}


// 在用户页表和进程mmap链里释放mmap区域 [begin, begin + npages * PGSIZE)
// 失败则panic卡死
void uvm_munmap(uint64 begin, uint32 npages) {
    proc_t* p = myproc();
    assert(p != NULL, "uvm_munmap: p is NULL");
    for (uint64 va = begin; va < begin + npages * PGSIZE; va += PGSIZE)
        vm_unmappages(p->pgtbl, va, PGSIZE, true);
    mmap_region_t* prev = NULL, * cur = p->mmap;
    while (cur) {
        if (begin >= cur->begin && begin + npages * PGSIZE <= cur->begin + cur->npages * PGSIZE) break;
        prev = cur;
        cur = cur->next;
    }
    assert(cur != NULL, "uvm_munmap: mmap region not found");
    if (begin == cur->begin && npages == cur->npages) {
        if (cur == p->mmap) p->mmap = cur->next;
        else prev->next = cur->next;
        mmap_region_free(cur);
    }
    else if (begin == cur->begin) {
        cur->begin += npages * PGSIZE;
        cur->npages -= npages;
    }
    else if (begin + npages * PGSIZE == cur->begin + cur->npages * PGSIZE) {
        cur->npages -= npages;
    }
    else {
        mmap_region_t* new_node = mmap_region_alloc();
        new_node->begin = begin + npages * PGSIZE;
        new_node->npages = (cur->begin + cur->npages * PGSIZE - new_node->begin) / PGSIZE;
        new_node->next = cur->next;
        cur->npages = (begin - cur->begin) / PGSIZE;
        cur->next = new_node;
    }
}

/*------------------part-3: 用户空间heap和stack管理相关------------------*/

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len) {
    uint64 new_top = cur_heap_top + len;
    if (new_top >= MMAP_BEGIN) return -1;
    for (uint64 va = ALIGN_UP(cur_heap_top, PGSIZE); va < ALIGN_UP(new_top, PGSIZE); va += PGSIZE) {
        uint64 pa = (uint64)pmem_alloc(false);
        assert(pa != 0, "uvm_heap_grow: pmem_alloc failed");
        vm_mappages(pgtbl, va, pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    return new_top;
}

// 用户堆空间减少, 返回新的堆顶地址
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len) {
    if (cur_heap_top < len) return 0;
    uint64 new_top = cur_heap_top - len;
    for (uint64 va = ALIGN_UP(new_top, PGSIZE); va < ALIGN_UP(cur_heap_top, PGSIZE); va += PGSIZE)
        vm_unmappages(pgtbl, va, PGSIZE, true);
    return new_top;
}

// 处理函数栈增长导致的page fault事件
// 成功返回new_ustack_npage，失败返回-1
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr) {
    uint64 stk_base = TRAPFRAME - old_ustack_npage * PGSIZE;
    if (fault_addr < MMAP_END || fault_addr >= stk_base) return -1;
    uint64 new_stk_base = ALIGN_DOWN(fault_addr, PGSIZE);
    uint64 new_ustack_npage = (TRAPFRAME - new_stk_base) / PGSIZE;
    for (uint64 va = new_stk_base; va < stk_base; va += PGSIZE) {
        uint64 pa = (uint64)pmem_alloc(false);
        assert(pa != 0, "uvm_ustack_grow: pmem_alloc failed");
        vm_mappages(pgtbl, va, pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    return new_ustack_npage;
}

/*----------------------part-4: 用户页表管理相关----------------------*/

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level) {
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++) {
        pte_t pte = pgtbl[i];
        if (!((pte)&PTE_V)) continue;
        uint64 pgtbl_1 = PTE_TO_PA(pte);
        if (level > 1 && PTE_CHECK(pte)) destroy_pgtbl((pgtbl_t)pgtbl_1, level - 1);
        else pmem_free(pgtbl_1, false);
    }
    pmem_free((uint64)pgtbl, true);
}

// 页表销毁
void uvm_destroy_pgtbl(pgtbl_t pgtbl) {
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, true);   // 可以释放，因为trapframe是每个进程独有的
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false); // 不能释放，因为所有进程共用区域
    destroy_pgtbl(pgtbl, 3);
}

// 连续虚拟空间的复制
// 在uvm_copy_pgtbl中使用
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end) {
    uint64 va, pa, page;
    int flags;
    pte_t* pte;

    for (va = begin; va < end; va += PGSIZE) {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");

        pa = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        memmove((char*)page, (const char*)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 拷贝页表 (拷贝并不包括 trapframe 和 trampoline)
// 拷贝的页表管理的物理页是原来页表的复制品
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint64 ustack_npage, mmap_region_t* mmap) {
    copy_range(old, new, USER_BASE, heap_top);
    copy_range(old, new, TRAPFRAME - ustack_npage * PGSIZE, TRAPFRAME);
    mmap_region_t* tmp = mmap;
    while (tmp) {
        copy_range(old, new, tmp->begin, tmp->begin + tmp->npages * PGSIZE);
        tmp = tmp->next;
    }
}
