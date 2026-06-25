# LAB-5: 系统调用流程建立 + 用户态虚拟内存管理

## 用户地址空间完整布局

```
 用户页表                      内核页表
 ┌──────────────────────┐ ← VA_MAX          ┌──────────────────────┐ ← VA_MAX
 │  TRAMPOLINE  (1页)   │ 0x3FFFFFF000      │  TRAMPOLINE  (1页)   │
 │  权限: R+X           │                   │  权限: R+X           │
 ├──────────────────────┤                   ├──────────────────────┤
 │  TRAPFRAME   (1页)   │ 0x3FFFFFE000      │  KSTACK(0)   (1页)   │ 0x3FFFFFC000
 ├──────────────────────┤                   │  ...         ...     │
 │  ustack      (可增长)│ ← 向下增长        ├──────────────────────┤
 │  权限: R+W+U         │                   │  alloc region        │
 │  ...                  │                   │  (identity mapped)   │
 ├──────────────────────┤ ← MMAP_END        │  权限: R+W           │
 │                      │                   ├──────────────────────┤
 │  mmap_region (多块)  │ 离散映射区        │  kernel data         │
 │  权限: R+W+U         │ 64MB              │  权限: R+W           │
 │                      │                   ├──────────────────────┤
 ├──────────────────────┤ ← MMAP_BEGIN      │  kernel code         │
 │  heap         (可伸缩)│ ← 向上增长        │  权限: R+X           │
 │  权限: R+W+U         │                   ├──────────────────────┤
 │  ...                  │                   │  MMIO                │
 ├──────────────────────┤                   │  (UART/CLINT/PLIC)   │
 │  code+data   (1页)   │ USER_BASE=0x1000  └──────────────────────┘
 │  权限: R+W+X+U       │
 ├──────────────────────┤
 │  guard page (1页)    │ 0x0
 │  不可访问             │
 └──────────────────────┘
```

## 任务 1：用户态和内核态的数据迁移

### 背景

lab-4 的 `sys_helloworld` 无法传递参数——系统调用号通过 `a7` 寄存器传递，a0~a5 存放参数。对于值传递直接读 `tf->ax`；对于地址传递，用户传入的地址基于用户页表，内核使用内核页表，需要手动走查用户页表找到物理地址再做数据迁移。

### 实现

**uvm_copyin(pgtbl, dst, src, len)**：用户空间 → 内核空间。沿用户页表逐页查 `vm_getpte` 找到物理地址，`memmove` 分段拷贝。处理非页对齐的起始偏移。

**uvm_copyout(pgtbl, dst, src, len)**：内核空间 → 用户空间。与 copyin 对称，方向相反。

**uvm_copyin_str(pgtbl, dst, src, maxlen)**：用户空间字符串 → 内核空间。逐字节检查 `\0` 提前终止。

**trap_user_handler 改动**：ecall 分支改为调用统一的 `syscall()` 分发函数，epc+=4。

**sys_copyin / sys_copyout / sys_copyinstr**：通过 `arg_uint64` / `arg_uint32` 获取用户传入的地址参数，调用对应的 uvm 函数，copyin/copyinstr 将结果 printf 输出。

### 关键设计：syscall 分发

```c
// syscall.c — 跳转表驱动
static uint64 (*syscalls[])(void) = {
    [SYS_copyin]    sys_copyin,
    [SYS_copyout]   sys_copyout,
    [SYS_copyinstr] sys_copyinstr,
    [SYS_brk]       sys_brk,
    [SYS_mmap]      sys_mmap,
    [SYS_munmap]    sys_munmap,
};

void syscall() {
    int sys_num = p->tf->a7;
    p->tf->a0 = syscalls[sys_num]();   // 返回值写入 a0
}
```

## 任务 2：堆的手动管理与栈的自动管理

### 堆的管理

用户通过 `sys_brk(new_heap_top)` 手动伸缩堆：

| 情况 | 操作 |
|------|------|
| `new_top == 0` | 查询当前堆顶 |
| `new_top > heap_top` | `uvm_heap_grow`：对齐后逐页 `pmem_alloc(false)` + `vm_mappages` |
| `new_top < heap_top` | `uvm_heap_ungrow`：对齐后逐页 `vm_unmappages(..., true)` |
| `new_top == heap_top` | 直接返回 |

边界检查：堆顶不能超过 `MMAP_BEGIN`（进入 mmap 区域）。

### 栈的管理

栈向下增长，由内核自动管理。当用户访问未分配的栈空间时，触发 **13号（Load Page Fault）** 或 **15号（Store/AMO Page Fault）**。

`trap_user_handler` 中新增处理：

```c
case 13: case 15:
    new_npage = uvm_ustack_grow(p->pgtbl, p->ustack_npage, stval);
    if (new_npage < 0) panic(...);
    p->ustack_npage = new_npage;
    break;
```

**uvm_ustack_grow**：
1. 检查 `fault_addr` 是否在合法栈扩展区间 `[MMAP_END, stk_base)`
2. 合法性确认后，从 `fault_addr`（页对齐）到当前栈底逐页分配映射
3. 返回新的 `ustack_npage`

边界检查：栈底不能越过 `MMAP_END`（进入 mmap 区域）。

## 任务 3：mmap_region_node 仓库管理

### 数据结构

```c
typedef struct mmap_region {
    uint64 begin;
    uint32 npages;
    struct mmap_region *next;
} mmap_region_t;

typedef struct mmap_region_node {
    mmap_region_t mmap;              // 第一成员，与 node 同地址
    struct mmap_region_node *next;   // 仓库链表指针
} mmap_region_node_t;
```

全局仓库：256 个 `node_list[N_MMAP]` 节点 + 链表头 `list_head` + 自旋锁 `list_lk`。

### 实现

| 函数 | 逻辑 |
|------|------|
| `mmap_init` | 初始化锁，256 个节点全部头插入 `list_head` 空闲链表 |
| `mmap_region_alloc` | 加锁 → 取表头节点 → 解锁。空则 panic |
| `mmap_region_free` | `(mmap_region_node_t*)mmap` 强转 → 加锁 → 头插回链表 |

由于 `mmap_region_t` 是 `mmap_region_node_t` 的第一个成员，二者地址相同，free 时直接强转即可。

## 任务 4：mmap 与 munmap

### 离散内存映射的完整流程

```
用户 sys_mmap(begin, len)
  → uvm_mmap(begin, npages, perm)
    → begin==0 时: uvm_mmap_find 扫描链表找空洞
    → mmap_region_alloc 创建新节点
    → 插入有序链表 + 合并相邻节点
    → pmem_alloc + vm_mappages 分配映射物理页
```

### uvm_mmap 详细逻辑

1. **找插入位置**：沿 `p->mmap` 链表遍历，找到第一个 `begin < cur->begin` 的位置
2. **创建节点**：`mmap_region_alloc` + 填写 begin/npages/next
3. **插入链表**：维护 `prev`/`cur` 指针，正确插入保持有序
4. **合并相邻**：
   - 先与后继合并：`node->end == cur->begin` → `mmap_merge(node, cur, true)`
   - 再与前驱合并：`prev->end == begin` → `mmap_merge(prev, node, true)`
   - merge 前手动处理 next 指针（merge 不操作 next）
5. **映射物理页**：逐页 `pmem_alloc(false)` + `vm_mappages(pgtbl, ..., perm | PTE_U)`

### mmap_merge

保留一个节点，释放另一个。`keep_mmap_1=true` 时 mmap_1 吞并 mmap_2；否则反向。**不操作 next 指针**，由调用者处理。

### uvm_mmap_find

扫描 mmap 链表，在 `[MMAP_BEGIN, MMAP_END)` 范围内找第一个足够大的空洞。返回空洞起始地址，失败返回 0。

### uvm_munmap 详细逻辑

1. **解映射物理页**：逐页 `vm_unmappages(va, PGSIZE, true)`
2. **查找目标 mmap_region**：遍历链表找到包含 `[begin, begin+npages*PGSIZE)` 的节点
3. **按四种情况处理**：

| 情况 | 操作 |
|------|------|
| 完全匹配 | 摘链 → `mmap_region_free` |
| 左对齐（截左边） | `cur->begin +=`，`cur->npages -=` |
| 右对齐（截右边） | 仅 `cur->npages -=` |
| 中间挖洞（分裂） | `mmap_region_alloc` 新节点 → 分裂为两个 |

## 任务 5：页表的复制与销毁

### uvm_destroy_pgtbl

1. 先 `vm_unmappages(TRAPFRAME, ..., true)` 释放 trapframe 物理页
2. 再 `vm_unmappages(TRAMPOLINE, ..., false)` 仅清映射（不释放——所有进程共享）
3. 调用 `destroy_pgtbl(pgtbl, 3)` 递归销毁

### destroy_pgtbl（递归）

从 level=3（根页表）开始：
- 遍历 512 个 PTE，对每个有效项：
  - 若 `level > 1` 且 `PTE_CHECK`（指向子页表）：递归进入下一级
  - 否则（数据页）：`pmem_free` 释放
- 最后 `pmem_free(pgtbl, true)` 释放当前页表物理页

### uvm_copy_pgtbl

利用骨架提供的 `copy_range`（逐页 `vm_getpte` → `pmem_alloc` + `memmove` + `vm_mappages`），分三段复制：

1. 代码+堆区：`[USER_BASE, heap_top)`
2. 栈区：`[TRAPFRAME - ustack_npage * PGSIZE, TRAPFRAME)`
3. mmap 区：遍历 mmap 链表，每段调 `copy_range`

## 系统调用一览

| 调用号 | 名称 | 参数 | 功能 |
|--------|------|------|------|
| 1 | `SYS_copyin` | a0=addr, a1=len | 用户→内核拷贝 int 数组 |
| 2 | `SYS_copyout` | a0=addr | 内核→用户拷贝 int 数组 |
| 3 | `SYS_copyinstr` | a0=addr | 用户→内核拷贝字符串 |
| 4 | `SYS_brk` | a0=new_top | 伸缩堆顶 |
| 5 | `SYS_mmap` | a0=begin, a1=len | 创建内存映射 |
| 6 | `SYS_munmap` | a0=begin, a1=len | 解除内存映射 |
| 7 | `SYS_test_pgtbl` | — | 测试页表复制销毁 |

## 踩坑记录

### 坑 1：uvm_mmap 插入位置判断错误

**现象**：测试 4 的 mmap 链 merge 全部失效，256 个 node 快速耗尽 panic。

**原因**：链表遍历时用新区间终点与已有节点起点比较：

```c
// ❌ 错误
if (begin + npages * PGSIZE <= nxt->begin) break;
```

当新区间终点超出已有节点起点时，循环跨越了答案，导致插入到错误位置。

**解决**：改用新区间起点比较：

```c
// ✅ 正确
if (begin < nxt->begin) break;
```

### 坑 2：sys_brk 相等情况返回 -1

**现象**：测试 2 第三步 `syscall(SYS_brk, heap_top)` 传入相等的值，返回 -1，后续 syscall 拿到 -1 当 heap_top，连锁崩溃。

**原因**：if-else if 结构最后 `return -1` 会覆盖相等情况。

**解决**：相等时 `return p->heap_top`。

### 坑 3：uvm_mmap merge 后 next 指针悬空

**现象**：merge 后链表出现已释放节点的悬空引用。

**原因**：`mmap_merge` 不操作 next 指针，调用者必须在 merge 前手动接管 `next`：

```c
node->next = nxt->next;       // 先接管
mmap_merge(node, nxt, true);    // 再释放
```

### 坑 4：kvm.c 缺少 trampoline 声明

**现象**：编译报错 `'trampoline' undeclared`。

**原因**：lab-5 骨架的 `mem/type.h` 删除了 `extern char trampoline[]` 声明。

**解决**：在 `kvm.c` 顶部手动加 `extern char trampoline[];`。

## 测试

### 测试 1：用户态和内核态的数据迁移

```c
#include "sys.h"

int main() {
    int L[5];
    char* s = "hello, world";
    syscall(SYS_copyout, L);
    syscall(SYS_copyin, L, 5);
    syscall(SYS_copyinstr, s);
    while (1);
    return 0;
}
```

预期输出：
```
get a number from user：1
get a number from user：2
get a number from user：3
get a number from user：4
get a number from user：5
get string from user: hello, world
```

### 测试 2：堆的手动管理与栈的自动管理

**堆测试**：

```c
#include "sys.h"
#define PGSIZE 4096

int main() {
    long long heap_top = 0;
    heap_top = syscall(SYS_brk, 0);
    heap_top = syscall(SYS_brk, heap_top + PGSIZE * 9);
    heap_top = syscall(SYS_brk, heap_top);
    heap_top = syscall(SYS_brk, heap_top - PGSIZE * 5);
    while (1);
    return 0;
}
```

**栈测试**：

```c
#include "sys.h"
#define PGSIZE 4096

int main() {
    char tmp[PGSIZE * 4];
    tmp[PGSIZE * 3] = 'h';
    tmp[PGSIZE * 3 + 1] = 'e';
    tmp[PGSIZE * 3 + 2] = 'l';
    tmp[PGSIZE * 3 + 3] = 'l';
    tmp[PGSIZE * 3 + 4] = 'o';
    tmp[PGSIZE * 3 + 5] = '\0';
    syscall(SYS_copyinstr, tmp + PGSIZE * 3);
    tmp[0] = 'w'; tmp[1] = 'o'; tmp[2] = 'r'; tmp[3] = 'l'; tmp[4] = 'd'; tmp[5] = '\0';
    syscall(SYS_copyinstr, tmp);
    while (1);
    return 0;
}
```

预期输出：page fault 触发栈自动扩展，两次 `copyinstr` 分别输出 "hello" 和 "world"。

### 测试 3：mmap_region_node 仓库管理

将 `main.c` 替换为 README 中测试 3 的版本。双核各申请 128 个 node 再释放，最后 `mmap_show_nodelist` 显示 256 个全部回到空闲链表，索引乱序证明并发正确。

### 测试 4：mmap 与 munmap

```c
#define MMAP_BEGIN (MMAP_END - 64 * 256 * PGSIZE)

int main() {
    // 7 次 mmap（含 merge）
    syscall(SYS_mmap, MMAP_BEGIN + 4*PGSIZE, 3*PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 10*PGSIZE, 2*PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 2*PGSIZE,  2*PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 12*PGSIZE, 1*PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN + 7*PGSIZE, 3*PGSIZE);
    syscall(SYS_mmap, MMAP_BEGIN, 2*PGSIZE);
    syscall(SYS_mmap, 0, 10*PGSIZE);

    // 7 次 munmap（含四种截断类型）
    syscall(SYS_munmap, MMAP_BEGIN + 10*PGSIZE, 5*PGSIZE);
    syscall(SYS_munmap, MMAP_BEGIN, 10*PGSIZE);
    // ... 其余 munmap
    while (1);
}
```

验证 merge 后链表归并正确、munmap 不 panic、vm_print 映射正常。

### 测试 5：页表的复制与销毁

通过 `SYS_test_pgtbl` 系统调用验证：创建新页表 → `uvm_copy_pgtbl` 复制 → 验证新旧物理页不同 → `uvm_destroy_pgtbl` 销毁 → 原进程继续正常工作。

```
[test_pgtbl] copy ok: old_pa=87ffe000, new_pa=87ff9000
[test_pgtbl] destroy ok
get string from user: hello, world      ← 原进程正常
```

## 构建与运行

```bash
make build    # 编译
make run      # QEMU 运行 (Ctrl+A, X 退出)
make debug    # 调试模式 (配合 gdb-multiarch)
make clean    # 清理
```

## 环境

- **OS**：WSL2 + Ubuntu 22.04
- **编译器**：riscv64-linux-gnu-gcc
- **模拟器**：qemu-system-riscv64
- **参考**：xv6-riscv-2020 (util 分支)
