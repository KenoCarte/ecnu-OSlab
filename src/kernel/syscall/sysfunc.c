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
    if (new_heap_top == 0)
        return p->heap_top;
    if (new_heap_top > p->heap_top) {
        uint64 ret = uvm_heap_grow(p->pgtbl, p->heap_top, new_heap_top - p->heap_top, PTE_R | PTE_W | PTE_U);
        if (ret == -1) return -1;
        p->heap_top = ret;
        return ret;
    }
    else if (new_heap_top < p->heap_top) {
        uint64 ret = uvm_heap_ungrow(p->pgtbl, p->heap_top, p->heap_top - new_heap_top);
        if (ret == -1) return -1;
        p->heap_top = ret;
        return ret;
    }
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
    执行ELF文件以替换当前进程的内容
    char *path
    char **argv
    成功返回argc, 失败返回-1
*/
uint64 sys_exec() {
    char path[STR_MAXLEN];
    uint64 argv_addr;
    arg_str(0, path, STR_MAXLEN);
    arg_uint64(1, &argv_addr);
    char* argv[ELF_MAXARGS];
    int argc = 0;
    proc_t* p = myproc();
    if (argv_addr != 0) {
        for (int i = 0;i < ELF_MAXARGS;i++) {
            uint64 arg_addr;
            uvm_copyin(p->pgtbl, (uint64)&arg_addr, argv_addr + i * sizeof(uint64), sizeof(uint64));
            if (arg_addr == 0) break;
            char* arg = pmem_alloc(true);
            assert(arg != NULL, "sys_exec: pmem_alloc failed");
            uvm_copyin_str(p->pgtbl, (uint64)arg, arg_addr, ELF_MAXARG_LEN);
            argv[i] = arg;
            argc++;
        }
    }
    argv[argc] = NULL;
    int ret = proc_exec(path, argv);
    for (int i = 0; i < argc; i++) pmem_free((uint64)argv[i], true);
    return ret;
}

/* 构建fd->file的映射, 返回fd */
static uint32 alloc_fd(file_t* file) {
    proc_t* p = myproc();
    for (uint32 i = 0; i < N_OPEN_FILE_PER_PROC; i++) {
        if (p->open_file[i] == NULL) {
            p->open_file[i] = file;
            return i;
        }
    }
    return -1;
}

/*
    打开或创建文件
    char *path
    uint32 open_mode
    成功返回fd, 失败返回-1
*/
uint64 sys_open() {
    char path[STR_MAXLEN + 1];
    uint32 open_mode;
    arg_str(0, path, STR_MAXLEN);
    arg_uint32(1, &open_mode);
    file_t* file = file_open(path, open_mode);
    if (file == NULL) return -1;
    return alloc_fd(file);
}

/*
    关闭文件
    uint32 fd
    成功返回0, 失败返回-1
*/
uint64 sys_close() {
    uint32 fd;
    arg_uint32(0, &fd);
    file_t* file;
    if (arg_fd(0, &fd, &file) == -1) return -1;
    myproc()->open_file[fd] = NULL;
    file_close(file);
    return 0;
}

/*
    读取文件内容
    uint32 fd
    uint32 len
    uint64 addr
    成功返回读到的字节数, 失败返回0
*/
uint64 sys_read() {
    uint32 fd;
    uint32 len;
    uint64 addr;
    arg_uint32(0, &fd);
    arg_uint32(1, &len);
    arg_uint64(2, &addr);
    file_t* file;
    if (arg_fd(0, &fd, &file) == -1) return 0;
    return file_read(file, len, addr, true);
}

/*
    写入文件内容
    uint32 fd
    uint32 len
    uint64 addr
    成功返回写入的字节数, 失败返回0
*/
uint64 sys_write() {
    uint32 fd;
    uint32 len;
    uint64 addr;
    arg_uint32(0, &fd);
    arg_uint32(1, &len);
    arg_uint64(2, &addr);
    file_t* file;
    if (arg_fd(0, &fd, &file) == -1) return 0;
    return file_write(file, len, addr, true);
}

/*
    调整读写指针位置
    uint32 fd
    uint32 offset
    uint32 flag
    成功返回新的偏移量, 失败返回-1
*/
uint64 sys_lseek() {
    uint32 fd;
    uint32 offset;
    uint64 flag;
    arg_uint32(0, &fd);
    arg_uint32(1, &offset);
    arg_uint64(2, &flag);
    file_t* file;
    if (arg_fd(0, &fd, &file) == -1) return -1;
    return file_lseek(file, offset, flag);
}

/*
    复制文件控制权
    uinr32 fd
    成功返回new_fd, 失败返回-1
*/
uint64 sys_dup() {
    uint32 fd;
    arg_uint32(0, &fd);
    file_t* file;
    if (arg_fd(0, &fd, &file) == -1) return -1;
    file_dup(file);
    return alloc_fd(file);
}

/*
    获取文件信息
    uint32 fd
    uint64 addr
    成功返回0, 失败返回-1
*/
uint64 sys_fstat() {
    uint32 fd;
    uint64 addr;
    arg_uint32(0, &fd);
    arg_uint64(1, &addr);
    file_t* file;
    if (arg_fd(0, &fd, &file) == -1) return -1;
    return file_get_stat(file, addr);
}

/*
    获取目录中的所有目录项信息
    uint32 fd
    uint64 addr
    uint32 buffer_len
    成功返回读到的字节数, 失败返回-1
*/
uint64 sys_get_dentries() {
    uint32 fd;
    uint64 addr;
    uint32 buffer_len;
    arg_uint32(0, &fd);
    arg_uint64(1, &addr);
    arg_uint32(2, &buffer_len);
    file_t* file;
    if (arg_fd(0, &fd, &file) == -1) return -1;
    return file_read(file, buffer_len, addr, true);
}

/*
    创建目录
    char *path
    成功返回0, 失败返回-1
*/
uint64 sys_mkdir() {
    char path[STR_MAXLEN + 1];
    arg_str(0, path, STR_MAXLEN);
    inode_t* ip = path_create_inode(path, INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    if (ip == NULL) return -1;
    inode_put(ip);
    return 0;
}

/*
    修改当前工作目录
    char *new_path
    成功返回0, 失败返回-1
*/
uint64 sys_chdir() {
    char path[STR_MAXLEN + 1];
    arg_str(0, path, STR_MAXLEN);
    inode_t* ip = path_to_inode(path);
    if (ip == NULL) return -1;
    if (myproc()->cwd != NULL) inode_put(myproc()->cwd);
    myproc()->cwd = ip;
    return 0;
}

/*
    打印当前工作目录的绝对路径
    成功返回0, 失败返回-1
*/
uint64 sys_print_cwd() {
    proc_t* p = myproc();
    char path[MAXLEN_FILENAME + 8];
    memset(path, 0, sizeof(path));
    if (p->cwd == NULL) return -1;
    uint32 offset = inode_to_path(p->cwd, path, sizeof(path));
    if (offset == -1) return -1;
    printf("current work directory = %s\n", path + offset);
    return 0;
}

/*
    新建链接
    char *old_path
    char *new_path
    成功返回0, 失败返回-1
*/
uint64 sys_link() {
    char old_path[STR_MAXLEN + 1];
    char new_path[STR_MAXLEN + 1];
    arg_str(0, old_path, STR_MAXLEN);
    arg_str(1, new_path, STR_MAXLEN);
    return path_link(old_path, new_path);
}


/*
    删除链接 (可能触发删除文件)
    char *path
    成功返回0, 失败返回-1
*/
uint64 sys_unlink() {
    char path[STR_MAXLEN + 1];
    arg_str(0, path, STR_MAXLEN);
    return path_unlink(path);
}