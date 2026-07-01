# LAB-9: 文件系统 之 文件管理与全系统整合

## 前言

lab-9 是文件系统最终章，也是全系统的粘合剂。在 lab-8 的 inode 和 dentry 基础上，构建了完整的文件抽象层、设备文件系统、进程与文件系统的深度绑定，以及 ELF 程序的加载执行能力。

## 第1步：准备工作

### pmem_stat

```c
void pmem_stat(uint32 *free_pages_in_kernel, uint32 *free_pages_in_user) {
    spinlock_acquire(&kern_region.lk);
    *free_pages_in_kernel = kern_region.allocable;
    spinlock_release(&kern_region.lk);
    spinlock_acquire(&user_region.lk);
    *free_pages_in_user = user_region.allocable;
    spinlock_release(&user_region.lk);
}
```

### uvm_heap_grow flags 参数

签名改为 `uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len, int flag)`，内部 `vm_mappages(..., flag)` 替代硬编码的 `PTE_R|PTE_W|PTE_U`。`sys_brk` 调用处传入 `PTE_R|PTE_W|PTE_U`。

### console 控制台

`print_init` 增加 `cons_init()` 调用；`uart_intr` 替换旧版字符回显为 `cons_edit(c)` 单行调用，实现行缓冲输入。

### initcode 编译基址

Makefile 中 `-Ttext 0` 改为 `-Ttext 0x1000`，与 `USER_BASE` 对齐，解决用户态字符串指针地址偏移问题。

## 第2步：文件系统补全

### dentry.c 新增函数

| 函数 | 逻辑 |
|------|------|
| `dentry_search_2` | 遍历目录项，按 inode_num 匹配，填回 name |
| `dentry_transmit` | 遍历有效目录项，紧凑打包到 dst（用户态用 uvm_copyout） |
| `inode_to_path` | 从目标 inode 通过 ".." 回溯到根，逆向填充路径缓冲区 |
| `path_create_inode` | 调用 path_to_parent_inode → inode_create → dentry_create；对目录类型额外创建 "." 和 ".." |
| `path_link` | 旧路径 inode nlink++，新路径 dentry_create；不允许目录类型链接 |
| `path_unlink` | dentry_delete + inode nlink--，nlink 归零时通过 inode_put 触发回收 |

### __path_to_inode 相对路径支持

```c
ip = (path[0] == '/') ? inode_get(ROOT_INODE) : inode_dup(myproc()->cwd);
```

遇到 `..` 指向自身时（root 的 ".." 指向 root），`inode_unlock(ip); inode_put(nxt); inode_lock(ip)` 先解锁再重锁，避免同一把 slk 重入死锁。

### 文件抽象 (fs.c)

```
file_table[N_FILE] + lk_file_table
```

| 函数 | 逻辑 |
|------|------|
| `file_init` | 初始化 lk_file_table，clear 所有 slot |
| `file_alloc` | 持锁遍历找 ref==0 的 slot，ref=1 返回 |
| `file_open` | path_to_inode → 不存在且带 CREATE 标志则 path_create_inode → 设备文件权限检查 → file_alloc → 填充 readable/writable |
| `file_close` | ip != NULL 则 inode_put，ref-- |
| `file_read` | inode_lock → switch type：DATA→inode_read_data，DIR→dentry_transmit，DEVICE→device_read_data → offset 递增（非 DIR） |
| `file_write` | 同上，DATA→inode_write_data，DEVICE→device_write_data |
| `file_lseek` | switch flag（SET/ADD/SUB），uint32 下溢自动被 >size 钳位兜底 |
| `file_dup` | 持 lk_file_table 对 ref++ |
| `file_get_stat` | 填入 file_stat_t（type/nlink/size/inode_num/offset），uvm_copyout 到用户空间 |

### 设备文件 (device.c)

6 种设备，major 0-5：

| 设备 | 权限 | 逻辑 |
|------|------|------|
| /dev/stdin | 只读 | cons_read |
| /dev/stdout | 只写 | cons_write |
| /dev/stderr | 只写 | 前置 "ERROR: " + cons_write |
| /dev/zero | 只读 | pmem_alloc(true) 清零页 → copyout，循环填充 |
| /dev/null | 读写 | 读返回 0，写返回 len（丢弃数据） |
| /dev/gpt0 | 只写 | 解析输入字符串，匹配关键词返回预设回答，含 pmem_stat 查询 |

`device_init`：先确保 `/dev` 目录存在，再逐一创建 6 个设备文件的 inode+dentry，最后 device_register 注册读写函数到 device_table。

`device_open_check`：按 major 和 open_mode 校验读写权限。

`device_read_data` / `device_write_data`：查 device_table 调用对应函数。

## 第3步：进程与文件系统集成

### proc.c 改动

| 函数 | 改动 |
|------|------|
| `proc_init` | open_file 数组 + cwd 初始化为 NULL |
| `proc_alloc` | 同上 |
| `proc_return` | proczero 打开 stdin/stdout/stderr（/dev/stdin/stdout/stderr），cwd 设为 root |
| `proc_fork` | 子进程 open_file = file_dup(父)，cwd = inode_dup(父) |
| `proc_free` | 释放 open_file（file_close）+ cwd（inode_put） |

### proc_exec (exec.c)

执行流程：

```
step-0: pmem_alloc 新 trapframe + proc_pgtbl_init 新页表
step-1: path_to_inode → ELF inode
step-2: inode_read_data → elf_header，校验 magic
step-3: prepare_heap → 遍历 program headers → load_segment 载入代码/数据
step-4: inode_put(ip)
step-5: prepare_stack → 分配栈页，填入 argv
step-6: uvm_destroy_pgtbl(旧) → 释放旧代码/数据/PTE页
step-7: 设置 trapframe：satp/sp/epc/a0(argc)/a1(argv)
step-8: 更新 heap_top/ustack_npage/name
```

**关键修正**：
- `c->tf->a0 = 0`：proc_fork 复制父进程 trapframe 后必须覆盖 a0，否则子进程误认为自己是父进程
- `prepare_heap` 的 PTE flags 需包含 `PTE_U`，根据 `ph.flags`（ELF_PROG_FLAG_READ/WRITE/EXEC）映射
- mmap 清理：uvm_destroy_pgtbl（destroy_pgtbl 递归释放旧页表所有 leaf 物理页，含 mmap 区域）→ while 循环 uvm_munmap（释放链表节点）；uvm_munmap 在新页表中 vm_unmappages 找不到对应 VA，不会二次释放物理页
- 旧 trapframe 物理页由 uvm_destroy_pgtbl 中 vm_unmappages(TRAPFRAME, true) 统一释放

## 第4步：系统调用

新增 14 个系统调用（SYS_exec=9 到 SYS_unlink=22）：

| 编号 | 名称 | 核心逻辑 |
|------|------|----------|
| 9 | SYS_exec | arg_str 读 path → uvm_copyin 读 argv 指针数组 → proc_exec → 释放 argv 内核缓冲区 |
| 10 | SYS_open | arg_str + arg_uint32 → file_open → alloc_fd |
| 11 | SYS_close | arg_fd → open_file[fd]=NULL → file_close |
| 12 | SYS_read | arg_fd + arg_uint32(len) + arg_uint64(addr) → file_read |
| 13 | SYS_write | 同上 → file_write |
| 14 | SYS_lseek | arg_fd + arg_uint32(offset) + arg_uint32(flag) → file_lseek |
| 15 | SYS_dup | arg_fd → file_dup → alloc_fd |
| 16 | SYS_fstat | arg_fd + arg_uint64(addr) → file_get_stat |
| 17 | SYS_get_dentries | arg_fd + arg_uint64(addr) + arg_uint32(len) → file_read(DIR→dentry_transmit) |
| 18 | SYS_mkdir | arg_str → path_create_inode(DIR) |
| 19 | SYS_chdir | arg_str → path_to_inode → 替换 cwd（释放旧 cwd） |
| 20 | SYS_print_cwd | inode_to_path(cwd) → printf |
| 21 | SYS_link | arg_str×2 → path_link |
| 22 | SYS_unlink | arg_str → path_unlink |

## 踩坑记录

### 坑 1：`-Ttext 0` 导致 argv 地址错误

现象：`uvm_copyin_str: src=68`，panic invalid PTE。

原因：Makefile 中 initcode.elf 链接参数 `-Ttext 0`，但 proc_make_first 把 initcode 加载到 USER_BASE=0x1000。所有字符串地址偏了一页。

解决：Makefile 改为 `-Ttext 0x1000`。

### 坑 2：uart_intr 未调 cons_edit

现象：终端输入后无反应。

原因：uart_intr 只回显字符，未调 cons_edit 入队，cons_read 永远等不到数据。

解决：uart_intr 的 while 循环体替换为单行 `cons_edit(c)`。

### 坑 3：新目录缺少 "." 和 ".."

现象：相对路径 `./hello.txt` 和 `../xxx` 找不到文件。

原因：path_create_inode 创建目录时没有填入 "." 和 ".." dentry。

解决：目录类型创建时追加 `dentry_create(ip, ip->inode_num, ".")` 和 `dentry_create(ip, parent->inode_num, "..")`。

### 坑 4：root ".." 重入锁死锁

现象：`dentry_search(root, "..")` 返回 root 自身，再 `inode_lock(root)` 重复加锁，sleeplock 不可重入→死锁。

解决：`inode_unlock(ip); inode_put(nxt); inode_lock(ip)` 先解锁再锁定同一 inode。

## 测试

initcode fork + exec 四个用户态测试程序：

- test_1：stdin/stdout/stderr 输入输出 + exec 传参
- test_2：文件 open/close/dup/fstat + read/write/lseek + 目录 get_dentries
- test_3：mkdir/chdir/print_cwd + link/unlink 硬链接
- test_4：设备文件 zero/null/gpt0 读写

## 构建与运行

```bash
make clean && make build && make run
```

## 环境

- OS：WSL2 + Ubuntu 22.04
- 编译器：riscv64-linux-gnu-gcc
- 模拟器：qemu-system-riscv64
- 参考：xv6-riscv-2020 (util 分支)
