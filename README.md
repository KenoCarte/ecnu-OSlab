# LAB-4: 第一个用户进程的诞生

## 进程的定义和地址空间布局

### 用户进程的定义

用户进程由 `proc_t` 描述，包含以下关键资源:

```c
typedef struct proc {
    int pid;              // 进程标识符
    pgtbl_t pgtbl;        // 用户态页表 (含 trampoline + trapframe + 用户代码/栈/堆)
    uint64 heap_top;      // 用户堆顶
    uint64 ustack_npage;  // 用户栈页数
    trapframe_t *tf;      // 跨特权级上下文暂存区 (U-mode ↔ S-mode)
    uint64 kstack;        // 内核栈虚拟地址
    context_t ctx;         // 同特权级上下文暂存区 (S-mode ↔ S-mode)
} proc_t;
```

值得注意: 这里的"用户栈"指用户进程在 U-mode 使用的栈, "内核栈"指该进程陷入 S-mode 后使用的栈——每个用户进程拥有独立的内核栈。

### 用户进程的地址空间

```
 用户页表 (proczero.pgtbl)              内核页表 (kernel_pgtbl)
 ┌──────────────────────┐ ← VA_MAX     ┌──────────────────────┐ ← VA_MAX
 │  TRAMPOLINE  (1页)   │ 0x3FFFFFF000 │  TRAMPOLINE  (1页)   │ 0x3FFFFFF000
 │  权限: R+X           │              │  权限: R+X           │
 ├──────────────────────┤              ├──────────────────────┤
 │  TRAPFRAME   (1页)   │ 0x3FFFFFE000 │  KSTACK(0)   (1页)   │ 0x3FFFFFC000
 │  权限: R+W           │              │  权限: R+W           │
 ├──────────────────────┤              │  ...         ...     │
 │  ustack      (1页)   │              │                      │
 │  权限: R+W+U         │              ├──────────────────────┤
 ├──────────────────────┤              │  alloc region        │
 │  ...                  │              │  (identity mapped)   │
 │  heap_top →          │              │  权限: R+W           │
 ├──────────────────────┤              ├──────────────────────┤
 │  code+data   (1页)   │ USER_BASE    │  kernel data         │
 │  权限: R+W+X+U       │ = 0x1000     │  权限: R+W           │
 ├──────────────────────┤              ├──────────────────────┤
 │  guard page (1页)    │ 0x0          │  kernel code         │
 │  不可访问             │              │  权限: R+X           │
 └──────────────────────┘              ├──────────────────────┤
                                       │  MMIO (UART/CLINT/   │
                                       │   PLIC)              │
                                       └──────────────────────┘
```

**关键设计**: TRAMPOLINE 在内核页表和所有用户页表中映射到相同的虚拟地址, 使得切换页表时无需"远跳"——`csrw satp` 的下一条指令仍然有效。这是 xv6 经典设计的核心。

## 两种上下文切换

本次实验引入了两种截然不同的上下文切换机制:

| 维度 | `swtch` (context) | `trapframe` (trampoline) |
|------|-------------------|--------------------------|
| 特权级变化 | S-mode → S-mode | U-mode ↔ S-mode |
| 保存内容 | 仅 callee-saved 寄存器 (14个) | 全部 32 个通用寄存器 + 内核恢复信息 |
| 存储位置 | `proc->ctx` | `proc->tf` (通过 sscratch 指向) |
| 触发方式 | 函数调用 `swtch(old, new)` | ecall / 中断 / 异常 (硬件自动) |
| 返回方式 | `ret` | `sret` |

### swtch — S-mode 上下文切换

定义在 `proc/swtch.S`，是一个标准 C 可调函数 `swtch(context_t *old, context_t *new)`:

```
swtch:
    sd ra, 0(a0)       # 保存当前寄存器到 old
    sd sp, 8(a0)
    sd s0~s11, 16~104(a0)
    ld ra, 0(a1)       # 从 new 恢复寄存器
    ld sp, 8(a1)
    ld s0~s11, 16~104(a1)
    ret                # 返回 → ra 已指向新上下文
```

`ret` 后，PC 和栈已切换到新执行流——这就是"上下文切换"的本质。**swtch 不涉及特权级变化，也不处理 caller-saved 寄存器 (t0~t6, a0~a7)**——这些由编译器自动在调用者栈帧中保存。

### trapframe — 跨特权级上下文切换

由 `trap.S` 中的软件中断 `timer_vector` 触发 S-mode 软件中断 (SSIP)，在 S-mode 中完成滴答计数。

## 特权级间上下文切换 (trampoline)

### 整体流程: A-B-C-D 结构

相比 `kernel_vector` → `trap_kernel_handler` → `sret` 的 **A-B-A** 结构, 用户态 trap 构成了更复杂的 **A-B-C-D** 结构:

```
        user_vector          trap_user_handler    trap_user_return      user_return
 U-mode ───→ S-mode ────────→ 处理trap ──────────→ 准备返回 ──────────→ S-mode → U-mode
  (A)     trampoline    (B)  内核代码       (C)   内核代码       (D)   trampoline
```

**首次启动**时, proczero 直接从 C-D 开始 (`swtch` → `trap_user_return` → `user_return` → `sret`)。  
当用户态发生 trap 后, 才会走完 A-B-C-D 的完整过程。

### 首次进入用户态 (C→D)

`proc_make_first` 设置 `proczero.ctx.ra = trap_user_return`, 然后 `swtch(&cpu.ctx, &proczero.ctx)` 切换到 proczero:

```
trap_user_return():
  1. intr_off()
  2. w_stvec(user_vector)         ← S-mode trap 入口指向用户态 handler
  3. 保存内核信息到 tf:          ← user_to_kern_satp/sp/trapvector/hartid
  4. w_sepc(tf->user_to_kern_epc) ← 返回用户态时 PC = USER_BASE
  5. w_sstatus(SPP=0, SPIE=1)     ← 假装上一个状态是 U-mode
  6. jmp(TRAPFRAME, satp)         ← 调用 user_return
```

`user_return` (trampoline.S):

```asm
    csrw satp, a1              # 切换到用户页表
    sfence.vma zero, zero
    ld t0, 112(a0)             # 恢复用户 a0
    csrw sscratch, t0          # sscratch ← 用户 a0
    # 恢复全部 32 个通用寄存器...
    csrrw a0, sscratch, a0     # a0 ← sscratch, sscratch ← a0(=TRAPFRAME)
    sret                       # PC→sepc, 切换到 U-mode
```

**关键**: `csrrw` 的副作用是 `sscratch = TRAPFRAME`——这是 user_vector 能找到 trapframe 的唯一途径。

### 用户态 trap 进入 (A→B)

当用户在 U-mode 执行 `ecall` 或发生中断:

```
硬件自动:
  sepc ← user_PC
  scause ← 异常/中断原因
  sstatus.SPP ← 0 (来自U-mode)
  sstatus.SIE ← 0 (关闭中断)
  PC ← stvec = user_vector
```

`user_vector` (trampoline.S):

```asm
    csrrw a0, sscratch, a0     # a0 ← sscratch(=TRAPFRAME), sscratch ← 用户a0
    # 保存全部通用寄存器到 tf (偏移量 40~280)
    sd ra, 40(a0) ... sd t6, 280(a0)
    csrr t0, sscratch          # t0 ← 用户 a0
    sd t0, 112(a0)             # 单独保存 a0
    # 恢复内核执行环境
    ld sp, 8(a0)               # sp = tf->user_to_kern_sp (内核栈)
    ld tp, 32(a0)              # tp = tf->user_to_kern_hartid
    ld t0, 16(a0)              # t0 = tf->user_to_kern_trapvector (C函数地址)
    ld t1, 0(a0)               # t1 = tf->user_to_kern_satp (内核页表)
    csrw satp, t1              # 切换到内核页表!
    sfence.vma zero, zero
    jr t0                       # → trap_user_handler
```

**注意**: 所有 `ld` 必须在 `csrw satp` 之前完成——切换页表后 a0 指向的虚拟地址可能失效 (内核页表中不一定有 TRAPFRAME 的映射)。

### 从内核返回用户态 (C→D)

trap 处理完成后, 调用 `trap_user_return()` → `user_return()` → `sret`, 与首次进入用户态的路径一致, 形成一个完整的闭环。

## 用户态陷阱处理 (trap_user_handler)

### 与 trap_kernel_handler 的区别

| | trap_kernel_handler | trap_user_handler |
|---|---|---|
| 来自 | S-mode | U-mode (assert SPP==0) |
| 进入后 | 直接处理 | 先 `w_stvec(kernel_vector)` 切换 trap 入口 |
| 中断处理 | 相同 | 相同 (timer + external) |
| 异常处理 | 仅 panic | 多了 ecall(syscall) 处理 |
| 返回前 | `sret` 直接返回 | 调用 `trap_user_return()` 走完整 A-B-C-D |

### 系统调用约定

```
 用户侧 (initcode.c):             内核侧 (trap_user_handler):
 ┌──────────────────────┐         ┌───────────────────────────┐
 │ a7 = 系统调用号       │  ecall  │ scause = 8 (U-mode ecall) │
 │ syscall(SYS_xxx)     │ ──────→ │ switch(tf->a7):           │
 │                      │         │   case 0: printf(...)     │
 │                      │  sret   │ tf->user_to_kern_epc += 4 │
 │ PC += 4, 继续用户代码 │ ←────── │ trap_user_return()       │
 └──────────────────────┘         └───────────────────────────┘
```

- **系统调用号**: 通过 `a7` 寄存器传递
- **返回值**: 通过 `a0` 寄存器返回
- **epc + 4**: 系统调用虽然是异常, 但返回时应跳过 ecall 指令 (行为类似中断)

支持 0~6 个参数 (`__syscall0` ~ `__syscall6`), 通过 `a0`~`a5` 寄存器传递。

### 用户态中断处理

在 `trap_user_handler` 中, 中断处理与内核态一致:

| scause | 类型 | 处理函数 |
|--------|------|----------|
| 0x8000000000000001/5 | 时钟中断 (S-mode software/timer) | `timer_interrupt_handler()` |
| 0x8000000000000009 | 外设中断 (UART) | `external_interrupt_handler()` |

时钟中断到达 U-mode 后经 M-mode `timer_vector` 转发为 SSIP, 在 S-mode 中处理。外设中断经 PLIC 路由到 S-mode。

## 实验内容

### 1. kvm.c — 内核页表新增映射

在 `kvm_init()` 中新增两项映射:

```c
vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)&trampoline, PGSIZE, PTE_R | PTE_X);
void* kstk0 = pmem_alloc(true);
vm_mappages(kernel_pgtbl, KSTACK(0), (uint64)kstk0, PGSIZE, PTE_R | PTE_W);
```

- **TRAMPOLINE**: 内核也需要执行 trampoline 代码 (首次进入用户态时从 `trap_user_return` 跳转到 `user_return`)
- **KSTACK(0)**: proc0 的内核栈, 物理页按需分配

### 2. proc.c — 第一个用户进程创建

`proc_make_first()` 的核心流程:

```
1. 设置 pid = 0
2. pmem_alloc 申请 trapframe 物理页
3. proc_pgtbl_init(trapframe) → 创建用户页表, 映射 TRAMPOLINE 和 TRAPFRAME
4. pmem_alloc 申请 ustack 物理页 → 映射到 TRAPFRAME - PGSIZE
5. pmem_alloc 申请 code 物理页 → memmove(elf, initcode, len) → 映射到 USER_BASE
6. 设置 tf 字段: user_to_kern_sp, user_to_kern_epc(=USER_BASE), sp(=TRAPFRAME)
7. 设置 ctx: ra = trap_user_return, sp = kstack + PGSIZE
8. mycpu()->proc = &proczero
9. swtch(&mycpu()->ctx, &proczero.ctx) → 切换到 proczero 执行流
```

`proc_pgtbl_init()` 映射 TRAMPOLINE 和 TRAPFRAME 到用户页表:

```c
vm_mappages(pgtbl, TRAMPOLINE, (uint64)&trampoline, PGSIZE, PTE_R | PTE_X);
vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);
```

**注意**: 此处**不能**加 `PTE_U`。详见下方踩坑记录。

### 3. trap_user.c — 用户态陷阱处理

**trap_user_handler()**: 读 sepc/sstatus/scause/stval 寄存器 → assert SPP==0 → 切换 stvec 为 `kernel_vector` → 保存 epc 到 tf → 根据 scause 分发:

- 中断: timer (case 1/5) → `timer_interrupt_handler()`, external (case 9) → `external_interrupt_handler()`
- 异常: ecall (case 8) → 读 `tf->a7` 获取系统调用号, 当前支持 `SYS_helloworld(0)` 打印 helloworld, 然后 `tf->user_to_kern_epc += 4`

**trap_user_return()**: 关中断 → 设置 stvec = user_vector → 保存内核信息到 tf → 恢复用户 sepc 和 sstatus → 跳转到 `user_return`

## 踩坑记录

### 坑1: 用户页表中 TRAMPOLINE/TRAPFRAME 不能加 PTE_U

**现象**: 用户进程进入 S-mode 后, 在 `user_vector` 第一条指令 `csrrw a0, sscratch, a0` 处无限循环。

```
(gdb) p/x $pc      → 0x3ffffff000    # user_vector
(gdb) p/x $stvec   → 0x3ffffff000    # TRAMPOLINE
(gdb) p/x $scause  → 0xc             # Instruction page fault
(gdb) p/x $sstatus → 0x100           # SPP=1 (来自 S-mode)
(gdb) p/x $stval   → 0x3ffffff000    # 页错误地址 = TRAMPOLINE
```

**原因分析**: 当 U-mode 发生 trap 进入 S-mode 时, CPU 特权级切换但 satp **不切换**, 仍使用用户页表。此时 `stvec` 指向 `user_vector` (位于 TRAMPOLINE 页), CPU 需要从 TRAMPOLINE 取指。根据 RISC-V 特权规范 §4.3.1:

> **S-mode 下取指时, 若 PTE 的 U 位为 1, 硬件必须触发指令页错误 (Instruction Page Fault)——实际调试时发现的确如此。**

如果用户页表中 TRAMPOLINE 的映射带有 `PTE_U`, 在 S-mode 下取指会触发指令页错误, 而 `stvec` 又指向 TRAMPOLINE, 形成无限递归:

```
S-mode 取指 @TRAMPOLINE → PTE_U=1 → 指令页错误
  → 跳转到 stvec = TRAMPOLINE → 又是 PTE_U=1 → 再次页错误
  → 死循环
```

**解决**: `proc_pgtbl_init()` 中 TRAMPOLINE 和 TRAPFRAME **只设 `PTE_R | PTE_X` 或 `PTE_R | PTE_W`，不加 `PTE_U`**:

```c
vm_mappages(pgtbl, TRAMPOLINE, (uint64)&trampoline, PGSIZE, PTE_R | PTE_X);    // 无 PTE_U
vm_mappages(pgtbl, TRAPFRAME, trapframe, PGSIZE, PTE_R | PTE_W);               // 无 PTE_U
```

**但为什么用户页表映射 kernel code 时某些页又要加 PTE_U?**  
代码页 (`USER_BASE`) 确实加 `PTE_U`, 因为用户程序在 U-mode 执行它。但 TRAMPOLINE 和 TRAPFRAME **只在 S-mode 访问**:
- TRAMPOLINE 在 S-mode 取指 (user_vector / user_return)
- TRAPFRAME 在 S-mode 读写 (保存/恢复寄存器)
- 用户程序永远不会也不能访问这两个页面

## 测试

### 测试一: 系统调用

initcode.c 发出两次系统调用:

```c
#include "sys.h"

int main()
{
    syscall(SYS_helloworld);
    syscall(SYS_helloworld);
    while (1)
        ;
    return 0;
}
```

`trap_user_handler` 收到 `scause=8` (U-mode ecall), 根据 `a7=0` 匹配 `SYS_helloworld` 分支, 输出 `proczero: hello world!\n`。

### 测试二: 用户态的时钟中断和串口中断

LAB-3 中我们验证了内核态时钟中断和串口中断的响应。LAB-4 需要验证这些中断在用户态也能正常工作。

**时钟中断测试**: 用户态 `while(1)` 期间, M-mode 定时器持续触发 → `timer_vector` 更新 MTIMECMP 并设置 SSIP → SSIP 陷入 S-mode → `user_vector` → `trap_user_handler` → `timer_interrupt_handler` → `trap_user_return` → 回到 `while(1)`

**串口中断测试**: 在 QEMU 终端敲击键盘 → UART 产生 PLIC 中断 → 陷入 `trap_user_handler` → `external_interrupt_handler` → `uart_intr` 回显字符 → 返回用户态

可以在 `trap_user_handler` 的时钟中断分支加入计数器验证:

```c
case 1:case 5:
    timer_interrupt_handler();
    {
        static int tick = 0;
        if (++tick % 100 == 0)
            printf("[user] tick #%d\n", tick);
    }
    break;
```

### 测试结果

```
cpu 0 is booting!
proczero: hello world!
proczero: hello world!
[user] tick #100       ← 用户态时钟中断正常
[user] tick #200
[user] tick #300
<键盘输入回显>           ← 用户态串口中断正常
```

验证了两个 syscall 成功响应，且 `while(1)` 期间时钟中断和串口中断都能正常触发和返回。

## 构建与运行

```bash
make build    # 编译内核 + 用户程序
make run      # QEMU 运行 (Ctrl+A, X 退出)
make debug    # 调试模式 (配合 gdb-multiarch)
make clean    # 清理
```

## 环境

- **OS**: WSL2 + Ubuntu 22.04
- **编译器**: riscv64-linux-gnu-gcc
- **模拟器**: qemu-system-riscv64
- **参考**: xv6-riscv-2020 (util 分支)
