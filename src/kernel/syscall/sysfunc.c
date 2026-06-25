#include "mod.h"

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

/*
    打印一个字符串
    char *str
    成功返回0
*/
uint64 sys_print_str() {

}

/*
    打印一个32位整数
    int num
    成功返回0
*/
uint64 sys_print_int() {

}

/*
    进程复制
    返回子进程的pid
*/
uint64 sys_fork() {

}

/*
    等待子进程退出
    uint64 addr_exit_state
*/
uint64 sys_wait() {

}

/*
    进程退出
    int exit_code
    不返回
*/
uint64 sys_exit() {

}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep() {

}

/*
    返回当前进程的pid
*/
uint64 sys_getpid() {

}