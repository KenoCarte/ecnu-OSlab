# LAB-8: 文件系统 之 数据组织与层次结构

## 前言

lab-7 建立了 block 级别的磁盘管理能力。在此基础上，lab-8 解决两个问题：

1. 大于 1 个 block 的数据如何组织？—— **inode**：通过三层索引映射（直接+一级间接+二级间接）管理文件数据块
2. 如何用人类可读的名称索引数据？—— **dentry**：通过目录项实现 name → inode_num 的翻译，再通过路径解析支持层次化目录树

## inode.c — 索引节点管理

### 索引结构

```
inode_disk_t.index[13]:
  [0..9]     直接映射 (10 个 data block, 40KB)
  [10..11]   一级间接映射 (2 个 index block, 各 1024 entry, 8MB)
  [12]       二级间接映射 (1 个 top block → 1024 mid block → 各 1024 data, 4GB)
```

`locate_or_add_block` 按逻辑块号在三层中逐级查找/分配，与页表生长的三层映射同构。

### 函数实现

| 函数 | 逻辑 |
|------|------|
| `inode_init` | 初始化 `lk_inode_cache`，N_INODE 个 slot 设为 ref=0/valid_info=false/inode_num=INVALID |
| `inode_rw` | `inode_firstblock + num/INODE_PER_BLOCK` 定位磁盘块，memmove 读写 disk_info |
| `inode_get` | 持 `lk_inode_cache` 扫描缓存 → 命中 ref++ 返回；未命中找 ref==0 槽位，标记后放锁，拿 slk 读盘 |
| `inode_create` | bitmap_alloc_inode → inode_get → 拿 slk 填 disk_info → inode_rw 写盘 → 放 slk 返回（调用者自行 lock） |
| `inode_dup` | 持 `lk_inode_cache` 对 ref++ |
| `inode_lock` | sleeplock_acquire；若 valid_info==false 则从磁盘读入 |
| `inode_unlock` | sleeplock_release |
| `inode_put` | 持 `lk_inode_cache` ref-- → 放锁 → 拿 slk → 若 ref=0&&nlink=0 → free_data_blocks + bitmap_free_inode + 清 valid_info |
| `inode_delete` | 持 slk，free_data_blocks → 清 disk_info → inode_rw 写回 → bitmap_free_inode |
| `inode_read_data` | 持 slk，按 offset 逐 block locate_or_add_block + buffer_get，copyout 或 memmove 到 dst |
| `inode_write_data` | 持 slk，逐 block locate_or_add_block + buffer_get，copyin 或 memmove 后 buffer_write，更新 size |

### 锁设计

```
lk_inode_cache (自旋锁)         slk (睡眠锁)
  ├── ref                         ├── disk_info
  ├── valid_info                  ├── inode_num (读/写磁盘时)
  └── inode_num (分配/回收)
```

**锁序**：`lk_inode_cache` → `slk`，持 `lk_inode_cache` 时不做磁盘 I/O。

`inode_get` 缓存未命中时先设 valid_info=true 再放 `lk_inode_cache`，然后才拿 slk 读盘。另一个 `inode_get` 并发查到同一 inode 时会命中缓存（ref++），它的 `inode_lock` 阻塞在 slk 上，等第一个读盘完成。

## dentry.c — 目录项与路径解析

### 数据结构

```
dentry_t (64B): name[60] + inode_num(4)
一个目录 inode 仅用 index[0]（一个 block），size 表示已用空间
空槽位：name[0]=='\0'
```

### 函数实现

| 函数 | 逻辑 |
|------|------|
| `dentry_search` | 遍历 size 范围内有效槽位，`strncmp(name, MAXLEN_FILENAME)` 匹配 |
| `dentry_create` | 查重名 → 找空槽位/新槽位 → 填入 name+inode_num → 更新 size → buffer_write |
| `dentry_delete` | 遍历匹配 → memset 清零 → buffer_write |
| `dentry_print` | 遍历所有有效槽位，打印 offset/inode_num/name |
| `get_element` | 从 path 提取第一个 `/` 分隔的 name，返回剩余路径 |
| `__path_to_inode` | 从 ROOT_INODE 出发，逐级 get_element → dentry_search → inode_get 下钻，最后一级按 find_parent 返回父或子 |
| `path_to_inode` | `__path_to_inode(path, name, false)` |
| `path_to_parent_inode` | `__path_to_inode(path, name, true)` |

### 路径解析示例

```
"/AABBC/aaabb/file.txt"
  get_element → "AABBC" + "aaabb/file.txt"
    dentry_search(root, "AABBC") → inode_AABBC
    get_element → "aaabb" + "file.txt"
      dentry_search(AABBC, "aaabb") → inode_aaabb
      get_element → "file.txt" + (end)
        dentry_search(aaabb, "file.txt") → inode_file
```

## 主要改动文件

| 文件 | 改动 |
|------|------|
| `fs/inode.c` | 完整实现：cache 管理 + 三层索引 + 数据读写 + 生命周期 |
| `fs/dentry.c` | 完整实现：dentry 增删查 + 路径解析 |
| `fs/fs.c` | `fs_init` 增加 `inode_init()` + 测试代码 |
| `fs/type.h` | 新增 inode_disk_t、inode_t、dentry_t、相关常量 |
| `fs/method.h` | 新增 inode/dentry 函数声明 |
| `lib/utils.c` | 新增 `strlen` |
| `lib/method.h` | 新增 `strlen` 声明 |
| `proc/proc.c` | `proc_return` 调 `fs_init`（先放 p->lk） |
| `mkfs/mkfs.c` | 创建 root inode + 预置目录项 |
| `mkfs/mkfs.h` | 新磁盘布局参数 |

## 踩坑记录

### 坑 1：inode_get 扫描时 slk 泄漏

现象：第二次 `inode_get` 死锁。

原因：缓存扫描循环对非匹配条目 `sleeplock_acquire` 后没 `sleeplock_release`，下次扫描遇到同一 slot 时 slk 仍锁着 → sleeplock_acquire 永久阻塞。

解决：非匹配分支加 `sleeplock_release`。

### 坑 2：inode_get 持自旋锁做磁盘 I/O

现象：持 `lk_inode_cache` 进 `inode_rw` → `buffer_get` → `virtio_disk_rw` → `proc_sleep`，自旋锁未放就睡眠。

解决：缓存未命中时先设 `valid_info=true` + 放 `lk_inode_cache`，再拿 slk 读盘。

### 坑 3：locate_or_add_block 用 BLOCK_NUM_UNUSED 判空

现象：`virtio_disk_intr status` panic——用无效 block 号读写磁盘。

原因：`inode_create` 用 0 初始化 index（`memset(..., 0, ...)`），但 `locate_or_add_block` 用 `BLOCK_NUM_UNUSED`(0xFFFFFFFF) 判空。0 ≠ 0xFFFFFFFF，导致返回 0（超级块号）当数据块写入，破坏超级块。

解决：统一用 0 作"未分配"哨兵，`memset(... 0 ...)` 天然匹配。

### 坑 4：inode_put 持自旋锁做删除

现象：`panic! spinlock_release`。

原因：`inode_put` 持 `lk_inode_cache` 进入 `free_data_blocks` → 大量 buffer_get/put → `sleeplock_acquire` → 锁序冲突。

解决：先放 `lk_inode_cache` 再拿 slk 做删除，与 `inode_get` 对齐。

### 坑 5：dentry 路径解析 name_buf 覆盖

现象：`__path_to_inode` 中间分量找不到。

原因：`get_element(remaining, name_buf)` 覆盖了当前分量名，`dentry_search` 用错 name。

解决：用 `elem` 和 `next_elem` 两个独立 buffer。

### 坑 6：dentry_delete 没有真正删除

现象：删除目录项后仍然存在。

原因：找到匹配 dentry 后只 `buffer_put`，没 `memset` 清零也没 `buffer_write`。

解决：加 `memset(de, 0, sizeof(dentry_t))` + `buffer_write`。

### 坑 7：inode_index 下标越界

现象：二级间接映射区域操作数组越界。

原因：`inode_index[INODE_INDEX_3]` 访问 index[13]，合法下标 [0..12]。

解决：改为 `inode_index[INODE_INDEX_2]`。

## 测试

test-1（inode 创建+删除）、test-2（小/大批量数据读写）、test-3（dentry 增删查）、test-4（路径解析）全部通过。

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