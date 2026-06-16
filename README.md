# Lab-2: 内存管理初步

## 实验目标

实现物理内存管理（空闲链表）和 SV39 内核态虚拟内存（三级页表），为后续进程管理打下内存基础。

## 内存布局

```
0x80000000 ─── KERNEL_DATA ─── ALLOC_BEGIN ─────────────────── ALLOC_END
    [内核代码]     [内核数据]      [      可分配物理页区域          ]
                                 前 1024 页 = kern_region
                                 剩余      = user_region
```

所有符号（KERNEL_DATA、ALLOC_BEGIN、ALLOC_END）由 kernel.ld 的 PROVIDE 提供，代码通过 `extern char[]` 引用。

## 第一阶段：物理内存管理 (pmem.c)

### 核心数据结构

空闲页通过**单链表**串联。链表节点 `page_node_t` 直接复用空闲页的前 8 字节——页在链表上时存 next 指针，分配出去后那 8 字节就当普通内存用。`alloc_region_t` 描述一个分配区域：起止地址、自旋锁、空闲计数、链表头。

链表的头插入策略意味着分配和释放都是 O(1)，但分配出去的页不保证地址顺序。

### 三个函数

**`pmem_init()`**：取 ALLOC_BEGIN/ALLOC_END 等符号地址，算出 kern_region 和 user_region 的起止。前 KERN_PAGES(1024) 页给内核区域，剩余给用户区域。遍历每个区域的每一页，以头插法建好空闲链表。

**`pmem_alloc(in_kernel)`**：根据 in_kernel 选区域，加锁，从链表头取下一页，allocable 减一，解锁，memset 清零后返回。链表为空或计数归零时 panic。

**`pmem_free(page, in_kernel)`**：选区域，加锁，范围校验（`page < begin || page > end`），把 page 当 page_node_t 头插入链表，allocable 加一，解锁。

### 踩坑

- kernel.ld 提供的 ALLOC_END = 0x88000000 刚好是 QEMU 配的 128MB 内存边界。pmem_init 的循环如果用 `<= end`，会尝试写地址 0x88000000（越界），QEMU 直接 access fault。改用 `< end` 解决。

## 第二阶段：SV39 虚拟内存 (kvm.c)

### SV39 三级页表

```
虚拟地址 (39 bits)
┌──────────┬──────────┬──────────┬────────────┐
│  VPN[2]  │  VPN[1]  │  VPN[0]  │   offset   │
│  9 bits  │  9 bits  │  9 bits  │  12 bits   │
└────┬─────┴────┬─────┴────┬─────┴──────┬─────┘
     │          │          │            │
     ▼          ▼          ▼            ▼
  L2 页表 ──→ L1 页表 ──→ L0 页表 ──→ 物理页 (4KB)
  (顶级)      (次级)      (低级)         数据
```

每级页表 512 项 × 8 字节 = 4KB，刚好一页。虚拟地址最大 512GB（2^39），但实际只用低 38 bits（VA_MAX）。

### 页表项 (PTE)

```
┌───────────┬──────────────────────────────┬────────────┐
│ reserved  │            PPN               │   flags    │
│  10 bits  │          44 bits             │  10 bits   │
└───────────┴──────────────────────────────┴────────────┘
```

关键 flags: `V`(0), `R`(1), `W`(2), `X`(3), `U`(4)

指向**下一级页表**的 PTE 必须满足 `PTE_CHECK`（R=W=X=0），硬件以此来区分"这是页表页还是数据页"。PTE 中不存 R/W/X 不影响 CPU 遍历页表，只影响直接用该页做数据访问。

PA ↔ PTE 转换：`PA_TO_PTE(pa) = (pa >> 12) << 10`，`PTE_TO_PA(pte) = (pte >> 10) << 12`。

### 实现

**`vm_getpte(pgtbl, va, alloc)`**：三级循环（idx=2,1,0），每级用 `VA_TO_VPN` 取索引，读 PTE。如果 PTE.V=0 且 alloc=true，调 pmem_alloc 申请新物理页当次级页表，写入 PTE（只设 PTE_V，不设 RWX）。每级下钻前 assert PTE_CHECK。idx=0 时返回指向低级页表项的指针。

**`vm_mappages(pgtbl, va, pa, len, perm)`**：用 offset 遍历 `[0, len)`，步长 PGSIZE。每次调 vm_getpte 拿到 PTE 指针，assert `!(*pte & perm)` 防止重映射冲突，然后写入 `PA_TO_PTE(pa+offset) | perm | PTE_V`。offset 方式避免了 len 非页对齐时 uint64 下溢。

**`vm_unmappages(pgtbl, va, len, freeit)`**：同样 offset 遍历。vm_getpte 找到 PTE，assert 存在且 V 位有效。freeit=true 时先 pmem_free 释放物理页（传 false 走用户区域），再清 PTE。

### 内核页表初始化 (kvm_init)

`kvm_init` 将硬件寄存器区域和内核全部内存做直接映射（va = pa），分三块设不同权限：

| 区域 | 映射范围 | 权限 |
|------|---------|------|
| UART MMIO | 0x10000000, 4KB | R+W |
| CLINT | 0x02000000, 64KB (16页) | R+W |
| PLIC | 0x0c000000, 4MB (1024页) | R+W |
| 内核代码段 | KERNEL_BASE → KERNEL_DATA | R+X |
| 内核数据段 | KERNEL_DATA → ALLOC_BEGIN | R+W |
| 可分配区 | ALLOC_BEGIN → ALLOC_END | R+W |

代码段不设 W（防止意外改写指令），数据段不设 X（防止 jump 到数据区）。

`kvm_inithart()` 已由骨架提供：`w_satp(MAKE_SATP(kernel_pgtbl))` 后 `sfence_vma()` 刷新 TLB。

### 踩坑

- 测试程序会先映射 PTE_R 再重映射 PTE_W，因此 assert 不能写 `!(*pte & PTE_V)`（那会阻止所有重映射）。当前用 `!(*pte & perm)`，检查新权限和现有权限不重叠。

## 测试结果

### pmem测试

#### 测试一

```
cpu 0 is booting!
panic! pmem_alloc: no more user page
```

#### 测试二

```
cpu 0 is booting!
=== test_case_2: Phase 1 - Allocate User Pages ===
Allocated user page[0] @ 87fff000
Allocated user page[1] @ 87ffe000
Allocated user page[2] @ 87ffd000
Allocated user page[3] @ 87ffc000
Allocated user page[4] @ 87ffb000
Allocated user page[5] @ 87ffa000
Allocated user page[6] @ 87ff9000
Allocated user page[7] @ 87ff8000
Allocated user page[8] @ 87ff7000
Allocated user page[9] @ 87ff6000
=== test_case_2: Phase 2 - Pre-free Check ===
Expected allocable: 31729, Actual: 31729
=== test_case_2: Phase 3 - Free Pages ===
Free user page[0] @ 87fff000
Free user page[1] @ 87ffe000
Free user page[2] @ 87ffd000
Free user page[3] @ 87ffc000
Free user page[4] @ 87ffb000
Free user page[5] @ 87ffa000
Free user page[6] @ 87ff9000
Free user page[7] @ 87ff8000
Free user page[8] @ 87ff7000
Free user page[9] @ 87ff6000
=== test_case_2: Phase 4 - Post-free Check ===
Expected allocable: 31739, Actual: 31739
Free list head @ 87ff6000
=== test_case_2: Phase 5 - Reallocate & Verify Zero ===
Reallocated page[0] @ 87ff6000
Zero verification passed
Reallocated page[1] @ 87ff7000
Zero verification passed
Reallocated page[2] @ 87ff8000
Zero verification passed
Reallocated page[3] @ 87ff9000
Zero verification passed
Reallocated page[4] @ 87ffa000
Zero verification passed
Reallocated page[5] @ 87ffb000
Zero verification passed
Reallocated page[6] @ 87ffc000
Zero verification passed
Reallocated page[7] @ 87ffd000
Zero verification passed
Reallocated page[8] @ 87ffe000
Zero verification passed
Reallocated page[9] @ 87fff000
Zero verification passed
test_case_2 passed!
```

### kvm 测试

#### 测试一

```
cpu 0 is booting!

test-1

level-2 pgtbl: pa = 803bd000
.. level-1 pgtbl 0: pa = 803bc000
.. .. level-0 pgtbl 0: pa = 803bb000
.. .. .. physical page 0: pa = 87fff000 flags = 3
.. .. .. physical page 10: pa = 87ffe000 flags = 7
.. .. level-0 pgtbl 1: pa = 803ba000
.. .. .. physical page 0: pa = 87ffd000 flags = 11
.. level-1 pgtbl 1: pa = 803b9000
.. .. level-0 pgtbl 0: pa = 803b8000
.. .. .. physical page 0: pa = 87ffd000 flags = 11
.. level-1 pgtbl 255: pa = 803b7000
.. .. level-0 pgtbl 511: pa = 803b6000
.. .. .. physical page 511: pa = 87ffb000 flags = 5

test-2

level-2 pgtbl: pa = 803bd000
.. level-1 pgtbl 0: pa = 803bc000
.. .. level-0 pgtbl 0: pa = 803bb000
.. .. .. physical page 0: pa = 87fff000 flags = 5
.. .. level-0 pgtbl 1: pa = 803ba000
.. level-1 pgtbl 1: pa = 803b9000
.. .. level-0 pgtbl 0: pa = 803b8000
.. .. .. physical page 0: pa = 87ffd000 flags = 11
.. level-1 pgtbl 255: pa = 803b7000
.. .. level-0 pgtbl 511: pa = 803b6000
.. .. .. physical page 511: pa = 87ffb000 flags = 5
```

#### 测试二

- va_1=0x100000, va_2=0x8000 分别映射到用户物理页
- 验证 PTE.V、PA 匹配、权限匹配
- 解映射后验证 PTE.V 已清零
- test_mapping_and_unmapping passed!

## 构建与运行

```bash
make build
make run     # Ctrl+A, X 退出
make debug   # 调试模式
```

## 环境

- OS: WSL2 + Ubuntu 22.04
- 编译器: riscv64-linux-gnu-gcc
- 模拟器: qemu-system-riscv64
- 参考: xv6-riscv-2020 (util 分支)
