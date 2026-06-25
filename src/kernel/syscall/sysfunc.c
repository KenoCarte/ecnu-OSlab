#include "mod.h"

/*
    测试: 从用户空间传入一个int类型的数组
    uint64 addr 数组起始地址
    uint32 len  元素数量
    成功返回0
*/
uint64 sys_copyin() {
    uint64 addr;
    uint32 len;
    arg_uint64(0, &addr);
    arg_uint32(1, &len);
    int buf[SYS_MAX_NUM];
    proc_t* p = myproc();
    assert(p != NULL, "sys_copyin: p is NULL");
    uvm_copyin(p->pgtbl, (uint64)buf, addr, len * sizeof(int));
    // for (int i = 0; i < len; i++)
    //     printf("get a number from user：%d\n", buf[i]);
    return 0;
}

/*
    测试: 向用户空间传出一个int类型的数组
    uint64 addr 数组起始地址
    成功返回拷贝的元素数量
*/
uint64 sys_copyout() {
    uint64 addr;
    arg_uint64(0, &addr);
    static int buf[] = { 1, 2, 3, 4, 5 };
    proc_t* p = myproc();
    assert(p != NULL, "sys_copyout: p is NULL");
    uvm_copyout(p->pgtbl, addr, (uint64)buf, sizeof(buf));
    return sizeof(buf) / sizeof(int);
}

/*
    测试: 从用户空间传入一个字符串
    uint64 addr 字符串起始地址
    成功返回0
*/
uint64 sys_copyinstr() {
    uint64 addr;
    arg_uint64(0, &addr);
    char buf[STR_MAXLEN + 1];
    proc_t* p = myproc();
    assert(p != NULL, "sys_copyinstr: p is NULL");
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, STR_MAXLEN);
    buf[STR_MAXLEN] = '\0';
    // printf("get string from user: %s\n", buf);
    return 0;
}

/*
    用户堆空间伸缩
    uint64 new_heap_top (如果是0, 代表查询当前堆顶位置)
    成功返回new_heap_top, 失败返回-1
*/
uint64 sys_brk() {
    uint64 new_heap_top;
    arg_uint64(0, &new_heap_top);
    proc_t* p = myproc();
    assert(p != NULL, "sys_brk: p is NULL");
    if (new_heap_top == 0) {
        // printf("look event：ret_heap_top = %x\n", p->heap_top);
        // vm_print(p->pgtbl);
        // printf("\n");
        return p->heap_top;
    }
    if (new_heap_top > p->heap_top) {
        uint64 ret = uvm_heap_grow(p->pgtbl, p->heap_top, new_heap_top - p->heap_top);
        if (ret == -1) return -1;
        p->heap_top = ret;
        // printf("grow event：ret_heap_top = %x\n", new_heap_top);
        // vm_print(p->pgtbl);
        // printf("\n");
        return ret;
    }
    else if (new_heap_top < p->heap_top) {
        uint64 ret = uvm_heap_ungrow(p->pgtbl, p->heap_top, p->heap_top - new_heap_top);
        if (ret == -1) return -1;
        p->heap_top = ret;
        // printf("ungrow event：ret_heap_top = %x\n", new_heap_top);
        // vm_print(p->pgtbl);
        // printf("\n");
        return ret;
    }
    // printf("equal event：ret_heap_top = %x\n", new_heap_top);
    // vm_print(p->pgtbl);
    // printf("\n");
    return new_heap_top;
}

/*
    增加一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节,需检查是否是page-aligned)
    成功返回映射空间的起始地址, 失败返回-1
*/
uint64 sys_mmap() {
    uint64 start;
    uint32 len;
    arg_uint64(0, &start);
    arg_uint32(1, &len);
    if (len == 0 || len % PGSIZE != 0) return -1;
    proc_t* p = myproc();
    assert(p != NULL, "sys_mmap: p is NULL");
    uvm_mmap(start, len / PGSIZE, PTE_R | PTE_W | PTE_U);
    // uvm_show_mmaplist(p->mmap);
    // vm_print(p->pgtbl);
    // printf("\n");
    mmap_region_t* tmp = p->mmap;
    while (tmp) {
        if (start > tmp->begin) start = tmp->begin;
        tmp = tmp->next;
    }
    return start;
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_munmap() {
    uint64 begin;
    uint32 len;
    arg_uint64(0, &begin);
    arg_uint32(1, &len);
    if (len == 0 || len % PGSIZE != 0) return -1;
    proc_t* p = myproc();
    assert(p != NULL, "sys_munmap: p is NULL");
    uvm_munmap(begin, len / PGSIZE);
    // uvm_show_mmaplist(p->mmap);
    // vm_print(p->pgtbl);
    // printf("\n");
    return 0;
}