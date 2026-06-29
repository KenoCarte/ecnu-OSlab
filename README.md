# LAB-7: 文件系统 之 磁盘管理

## 前言

lab-6 构建了多进程支持，现在需要为这些进程提供持久化存储能力。

本次实验引入虚拟磁盘外设（virtio-blk），围绕它构建从底层驱动到上层管理接口的完整栈：

1. 磁盘驱动程序（virtio.c，直接使用）
2. 块缓冲系统（buf.c）——内存与磁盘之间的缓存桥梁
3. bitmap 资源管理（bitmap.c）——以 bit 为粒度管理 block 和 inode 的分配
4. 文件系统初始化（fs.c）——读入超级块，验证文件系统

缓冲系统是本次实验的核心：它不仅是磁盘 I/O 的性能关键，还涉及多核环境下的并发控制（自旋锁+睡眠锁的双层锁设计）。

## 缓冲系统 (buf.c)

### 数据结构

```
        活跃链表 (ref > 0)                 非活跃链表 (ref == 0)
   buf_head_active                   buf_head_inactive
        │                                  │
   ┌────┴────┐                       ┌────┴────┐
   │  head   │                       │  head   │
   └─────────┘                       └─────────┘
   next  prev                        next  prev
    │     ▲                           │     ▲
    ↓     │                           ↓     │
   bufA  bufC                        bufD  bufF
    │     ▲                           │     ▲
    ↓     │                           ↓     │
   bufB  ...                         bufE  ...
```

- `lk_buf_cache`：全局自旋锁，保护链表结构、block_num、ref
- `slk`：每 buffer 的睡眠锁，持有者方可做磁盘 I/O
- ref>0 → 活跃链表；ref==0 → 非活跃链表
- LRU 语义：head->next 最活跃，head->prev 最不活跃

### 函数实现

| 函数 | 逻辑 |
|------|------|
| `buffer_init` | 初始化锁和链表头；N_BUFFER 个 buffer 全部插入非活跃链表 |
| `buffer_get` | 持 lk_buf_cache → 遍历活跃链表 → 遍历非活跃链表 → 命中则移到活跃链表头、ref++、放锁、拿 slk；未命中取非活跃链表尾（最久未用）、设 block_num/ref=1、移到活跃链表、放锁、拿 slk、按需 pmem_alloc、disk read |
| `buffer_put` | 持 lk_buf_cache → ref-- → ref==0 则移到非活跃链表头 → 放锁 → 放 slk |
| `buffer_read` | 断言 data!=NULL、block_num 有效、slk 持有 → virtio_disk_rw(buf, false) |
| `buffer_write` | 同上 → virtio_disk_rw(buf, true) |
| `buffer_freemem` | 全程持 lk_buf_cache → 从非活跃链表尾向前 → ref==0 且 data!=NULL → pmem_free → data=NULL → block_num=UNUSED |
| `buffer_print_info` | 断言 N_BUFFER == N_BUFFER_TEST → 打印活跃/非活跃链表 |

### 关键设计点

**insert_node 统一复用**：通过 `next==NULL && prev==NULL` 判断节点是否已在链表中，首次插入和跨链表移动共用同一逻辑。`insert_active` 选目标链表，`insert_next` 选插入头部(true)还是尾部(false)。

**buf 到 node 的转换**：buf 是 buffer_node_t 首个字段，`(buffer_node_t*)buf` 即可从 buf 指针还原 node 指针，无需 offsetof（避免引入 stddef.h）。

**pmem_alloc 时机**：在 `spinlock_release(lk_buf_cache)` 之后、`sleeplock_acquire` 之后调用。此时 ref=1 且在活跃链表，无竞态，且不持自旋锁。

**buffer_get 锁顺序**：先放 lk_buf_cache，再拿 slk。lk_buf_cache 只在链表操作期间持有，磁盘 I/O（virtio_disk_rw）期间仅持 slk，允许其他 CPU 并发访问不同 buffer。

## bitmap 管理 (bitmap.c)

### 核心逻辑

```
bitmap_search_and_set(block_num, valid_count):
  逐字节扫描 buf->data
    ├── byte == 0xFF → 跳过 min(valid_count, 8) bit
    └── byte != 0xFF → 逐 bit 找 0
          ├── 找到 → 置 1 → buffer_write → return 局部索引
          └── 未找到 → 继续下一字节
  未找到 → buffer_put → return -1
```

| 函数 | 逻辑 |
|------|------|
| `bitmap_search_and_set` | 在单个 bitmap block 中扫描空闲 bit（0），置 1，写回磁盘，返回局部索引 |
| `bitmap_clear` | `index/BIT_PER_BYTE` 定位字节，`index%BIT_PER_BYTE` 定位 bit，清 0，写回 |
| `bitmap_alloc_block` | 遍历 data_bitmap 所有 block → search_and_set → 返回 `局部索引 + 块偏移 + sb.data_firstblock` |
| `bitmap_alloc_inode` | 遍历 inode_bitmap 所有 block → search_and_set → 返回 `局部索引 + 块偏移` |
| `bitmap_free_block` | `(block_num - firstblock) / BIT_PER_BLOCK` → 定位 bitmap block；`% BIT_PER_BLOCK` → 定位 bit |
| `bitmap_free_inode` | `inode_num / BIT_PER_BLOCK` + `% BIT_PER_BLOCK` 定位 |
| `bitmap_print` | 遍历所有 bitmap block，逐字节逐 bit 检查，打印已分配元素的全局序号 |

## 文件系统初始化 (fs.c)

### 调用时机

`fs_init` 在 proczero 首次返回用户态前调用：

```
proc_return()
  ├── pid==1 && is_inited==0
  │     ├── spinlock_release(&p->lk)   // 先放锁
  │     ├── is_inited = 1
  │     └── fs_init()
  │           ├── buffer_init()
  │           ├── buffer_get(0) → 读入超级块
  │           ├── assert(magic == FS_MAGIC)
  │           └── sb_print()
  └── trap_user_return()
```

不能在 main 中调：磁盘 I/O 涉及 proc_sleep/wakeup，必须在进程上下文中。

**先放 p->lk 再调 fs_init**：fs_init → buffer_get → spinlock_acquire(lk_buf_cache)。若有路径先拿 lk_buf_cache 再拿 p->lk，持锁调 fs_init 会死锁。

## 基础设施改动

| 文件 | 改动 |
|------|------|
| kvm.c | kernel_pgtbl 去 static；vm_getpte 处理 pgtbl==NULL → 用内核页表；kvm_init 新增 VIRTIO_BASE MMIO 映射 |
| plic.c | VIRTIO_IRQ(1) 设优先级、阈值、使能 |
| trap_kernel.c | external_interrupt_handler 增加 `virtio_disk_intr()` 分支 |
| trap/mod.h | include `../fs/mod.h` |
| proc/proc.c | proc_return 中首次调用 fs_init（先放锁） |
| proc/mod.h | include `../fs/mod.h` |
| syscall/syscall.c | 跳转表增加 11 个条目（11-21） |
| syscall/type.h | SYS_alloc_block ~ SYS_flush_buffer 共 11 个新号 |
| syscall/method.h | 11 个新 syscall 函数声明 |
| syscall/mod.h | include `../proc/mod.h` |
| syscall/sysfunc.c | 11 个新系统调用实现 |
| main.c | 增加 `virtio_disk_init()` |
| user/syscall_num.h | 对应用户态系统调用号 |

## 新增系统调用 (11 个)

| 编号 | 名称 | 功能 |
|------|------|------|
| 11 | SYS_alloc_block | bitmap_alloc_block → 返回 block 号 |
| 12 | SYS_free_block | bitmap_free_block(block_num) |
| 13 | SYS_alloc_inode | bitmap_alloc_inode → 返回 inode 号 |
| 14 | SYS_free_inode | bitmap_free_inode(inode_num) |
| 15 | SYS_show_bitmap | bitmap_print(choose_bitmap == 0) |
| 16 | SYS_get_block | buffer_get(block_num) → 返回 buffer 地址 |
| 17 | SYS_read_block | buffer_read + uvm_copyout → 用户空间 |
| 18 | SYS_write_block | uvm_copyin → buffer_write |
| 19 | SYS_put_block | buffer_put(addr_buf) |
| 20 | SYS_show_buffer | buffer_print_info() |
| 21 | SYS_flush_buffer | buffer_freemem(count) |

## 踩坑记录

### 坑 1：proc_return 持锁调 fs_init 死锁

现象：`panic! spinlock_acquire`

原因：fs_init → buffer_get → spinlock_acquire(lk_buf_cache)，与可能存在 lk_buf_cache → p->lk 的路径形成锁序环。

解决：fs_init 调用前先 `spinlock_release(&p->lk)`。

## 测试

### test-1：超级块读取

```c
#include "sys.h"
int main() {
    syscall(SYS_print_str, "hello, world!\n");
    while(1);
}
```

预期：启动后输出磁盘布局信息（superblock 各区域起止块号、块大小、总容量、inode 数）。

### test-2：bitmap 分配释放

```c
// 分配 20 个 block → 查看 data bitmap → 释放偶数号 → 查看 → 释放奇数号 → 查看
// 分配 20 个 inode → 查看 inode bitmap → 全部释放 → 查看
```

预期：bitmap 中已分配 bit 的序号与申请返回值一致，释放后对应 bit 消失。

### test-3：buffer LRU 读写

```c
// 阶段一：向 BLOCK_BASE 写入 "ABCDEFGH\n"，flush 后重新读取并比对
// 阶段二：GET 5 个不同 block → 查看链表 → PUT 其中 3 个 → 查看链表 → FLUSH 3 个 → 查看链表
```

预期：写入和读出字符串一致；活跃/非活跃链表顺序符合 LRU（最近访问在最前）；flush 清除非活跃 buffer 的物理页。

## 构建与运行

```bash
make build    # 编译
make run      # QEMU 运行 (Ctrl+A, X 退出)
make debug    # 调试模式 (gdb-multiarch)
make clean    # 清理
```

## 环境

- OS：WSL2 + Ubuntu 22.04
- 编译器：riscv64-linux-gnu-gcc
- 模拟器：qemu-system-riscv64
- 参考：xv6-riscv-2020 (util 分支)