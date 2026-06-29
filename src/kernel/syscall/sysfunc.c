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
    char str[STR_MAXLEN + 1];
    arg_str(0, str, STR_MAXLEN);
    printf("%s\n", str);
    return 0;
}

/*
    打印一个32位整数
    int num
    成功返回0
*/
uint64 sys_print_int() {
    uint32 num;
    arg_uint32(0, &num);
    printf("num = %d\n", num);
    return 0;
}

/*
    进程复制
    返回子进程的pid
*/
uint64 sys_fork() {
    return proc_fork();
}

/*
    等待子进程退出
    uint64 addr_exit_state
*/
uint64 sys_wait() {
    uint64 addr;
    arg_uint64(0, &addr);
    return proc_wait(addr);
}

/*
    进程退出
    int exit_code
    不返回
*/
uint64 sys_exit() {
    uint32 exit_code;
    arg_uint32(0, &exit_code);
    proc_exit(exit_code);
    return 0;
}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep() {
    uint32 ntick;
    arg_uint32(0, &ntick);
    timer_wait(ntick);
    return 0;
}

/*
    返回当前进程的pid
*/
uint64 sys_getpid() {
    proc_t* p = myproc();
    assert(p != NULL, "sys_getpid: p is NULL");
    return p->pid;
}

/*
    从data_bitmap申请1个block (测试data_bitmap_alloc)
    返回block序号
*/
uint64 sys_alloc_block() {

}

/*
    向data_bitmap释放1个block (测试data_bitmap_free)
    uint32 block_num (目标block序号)
    成功返回0
*/
uint64 sys_free_block() {

}

/*
    从inode_bitmap申请1个inode (测试inode_bitmap_alloc)
    返回block序号
*/
uint64 sys_alloc_inode() {

}

/*
    向inode_bitmap释放1个inode (测试inode_bitmap_free)
    uint32 inode_num (目标inode序号)
    成功返回0
*/
uint64 sys_free_inode() {

}

/*
    输出目标bitmap的状态
    uint32 choose_bitmap (0->data_bitmap 1->inode_bitmap)
    成功返回0, 失败返回-1
*/
uint64 sys_show_bitmap() {

}

/*
    获取1个描述block的buffer (测试buffer_get)
    uint32 block_num 目标block的序号
    成功返回buffer地址, 失败返回-1
*/
uint64 sys_get_block() {

}

/*
    释放1个描述block的buffer (测试buffer_put)
    uint64 addr_buf 即将被释放的buffer
    成功返回0
*/
uint64 sys_put_block() {

}

/*
    将buf->data拷贝到用户空间 (测试buffer_read)
    uint64 addr_buf 使用的buffer
    uint64 addr_data 用户数据区 (copy dst)
    成功返回0
*/
uint64 sys_read_block() {

}

/*
    将用户空间数据同步到内核空间, 并通过buffer写入block (测试buffer_write)
    uint64 addr_buf 使用的buffer
    uint64 addr_data 用户数据区 (copy src)
    成功返回0
*/
uint64 sys_write_block() {

}

/*
    输出buffer链表的状态
    成功返回0
*/
uint64 sys_show_buffer() {

}

/*
    释放非活跃链表中buffer持有的物理内存资源
    uint32 buffer_count (希望释放的buffer数量)
    成功返回0
*/
uint64 sys_flush_buffer() {

}