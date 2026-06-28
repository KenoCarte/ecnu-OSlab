# LAB-6: 单进程走向多进程——进程调度与生命周期

**前言**

经过 lab-4 的初创和 lab-5 的完善，proczero 已经比较成熟了

从零到一很缓慢，但是从一到多很快：可以"复制 proczero"来产生更多进程

产生更多进程后，需要解决新产生的两个问题：

- 多个进程会竞争 CPU 资源（之前几乎由 proczero 独占）
- 进程新生与死亡的问题（之前的 proczero 诞生后永不死亡）

因此，本次实验主要关注两个主题：**进程调度** + **生命周期**

## 进程状态机

五种进程状态，从冷到热：

```
UNUSED ──→ RUNNABLE ──→ RUNNING ──→ ZOMBIE ──→ UNUSED
              ↑            │  │
              │   proc_yield  │  proc_exit
              │            ↓  │
              └──── RUNNABLE   ↓
                              ZOMBIE

              SLEEPING ──→ RUNNABLE
                  ↑           │
                  proc_sleep  │ proc_wakeup
                              │
              RUNNING ────────┘
```

| 状态 | 含义 |
|------|------|
| UNUSED | 槽位空闲，不持有任何资源 |
| RUNNABLE | 准备就绪，可以被调度 |
| RUNNING | 正在 CPU 上执行 |
| SLEEPING | 睡眠等待资源，不可调度 |
| ZOMBIE | 已退出，等待父进程回收 |

## 进程调度

### 核心机制

```
  调度器 (proc_scheduler)                用户进程 A
  ┌────────────────────────┐           ┌────────────────┐
  │ while(1) {              │  swtch   │ proc_return()  │
  │   find RUNNABLE         │ ───────→ │ trap_user_...  │
  │   swtch(cpu, proc)      │          │   → user mode  │
  │          ↑              │ ←─────── │   ... trap ... │
  │   swtch returns         │  swtch   │   proc_yield() │
  │   release lock          │          │   proc_sched() │
  │   find next RUNNABLE... │          └────────────────┘
  └────────────────────────┘
```

**关键**：用户进程 A → 用户进程 B 需要两次 swtch：
1. A `proc_sched()` → 调度器
2. 调度器找到 B → `swtch` 到 B

调度器充当**缓冲**，从初始化者变成调度选择者。

### 循环扫描 + 公平性

每个 CPU 记住上次调度的位置 `last_i`，下一次从 `(last_i + 1) % N_PROC` 开始扫描，避免低序号进程饿死高序号进程。

### 基于时钟的抢占

`trap_user_handler` 中，每次时钟中断或外设中断处理完毕后调用 `proc_yield()`：
- 设置 `state = RUNNABLE`
- 获取进程锁 → `proc_sched()` → swtch 回调度器
- 锁在 swtch 中传递给调度器，调度器释放

注意：`trap_kernel_handler` **不**调 `proc_yield()`——内核态持有进程锁时重复加锁会导致死锁。用户态路径通过 `trap_user_return` 从 trapframe 完整恢复 CSR，跨核安全。

## 进程数组管理

`proc_list[N_PROC]` 作为资源仓库，`proc_alloc` / `proc_free` 管理生命期：

| 函数 | 逻辑 |
|------|------|
| `proc_init` | 初始化所有槽位锁 + 状态，`global_pid = 1` |
| `proc_alloc` | 扫描找 UNUSED → 加锁 → 设置 RUNNABLE → 分配 trapframe/页表/内核栈 → 带锁返回 |
| `proc_free` | 调用者持锁 → 释放页表 → 清 mmap → 标记 UNUSED |

`proc_alloc` 统一负责 trapframe、页表、内核栈的分配，避免 `proc_make_first` 和 `proc_fork` 重复代码。

## 进程生命周期

### fork — 子进程复制

```c
proc_fork():
  c = proc_alloc()              // 带锁返回，state=RUNNABLE
  *c->tf = *p->tf               // 拷贝 trapframe
  c->tf->a0 = 0                 // 子进程返回值 = 0
  c->parent = p                 // 建立父子关系
  uvm_copy_pgtbl(...)           // 拷贝用户页表
  mmap 链表复制
  spinlock_release(&c->lk)      // 解锁，子进程可被调度
  return c->pid
```

### exit — 进程退出

```
proc_exit(exit_code):
  获取 wait_lk（全局）
  proc_reparent：子进程过继给 proczero
  proc_try_wakeup(parent)：唤醒等待的父进程
  获取自身 lk → state = ZOMBIE
  释放 wait_lk
  proc_sched() → 永不返回
```

**self−other 模式**：进程不主动回收自己——标记 ZOMBIE，由父进程通过 `wait` 回收。

### wait — 父进程回收子进程

```
proc_wait(user_addr):
  获取 wait_lk
  while(1):
    扫描 proc_list
    找到 ZOMBIE 孩子 → proc_free → 写退出状态到用户空间 → 释放 wait_lk → 返回 pid
    有孩子但无 ZOMBIE → proc_sleep(self, &wait_lk)  // 睡眠等待
    无孩子 → 释放 wait_lk → 返回 -1
```

### sleep / wakeup

| 函数 | 逻辑 |
|------|------|
| `proc_sleep(sleep_space, lock)` | 获取自身 lk → 如果 lock 不是自身锁则释放 lock → `state = SLEEPING` → `proc_sched()` → 醒来后重新获取 lock |
| `proc_wakeup(sleep_space)` | 遍历进程数组 → 状态为 SLEEPING 且 sleep_space 匹配 → 设为 RUNNABLE |

用于 `proc_wait` 等场景：父进程等待子进程退出时睡眠，子进程退出时通过 `proc_try_wakeup` 唤醒。

### 睡眠锁（sleeplock）

```c
sleeplock_acquire(lk):
  spinlock_acquire(&lk->lock)
  while (lk->locked):
      proc_sleep(lk, &lk->lock)      // 释放 spinlock，睡眠
      spinlock_acquire(&lk->lock)    // 醒来后重新拿锁
  lk->locked = 1

sleeplock_release(lk):
  spinlock_acquire(&lk->lock)
  lk->locked = 0
  proc_wakeup(lk)                    // 唤醒等待者
  spinlock_release(&lk->lock)
```

## proc_return 与 trap 返回的两条路径

| 路径 | 来源 | 返回方式 | CSR 恢复 |
|------|------|----------|----------|
| 用户态 | `trap_user_handler` → `trap_user_return` | 从 trapframe 写 sepc/sstatus | ✅ 跨核安全 |
| 内核态 | `trap_kernel_handler` → `kernel_vector` sret | 用 CPU 当前 CSR | ❌ 跨核 CSR 丢失 |

**这就是 `trap_kernel_handler` 不能调 `proc_yield` 的根因**：`kernel_vector` 只保存通用寄存器（ra, sp, gp...），不保存 sepc/sstatus。proc_yield 跨核后 sret 读到错误 CPU 的 CSR。

## 主要改动文件

| 文件 | 改动 |
|------|------|
| `proc/proc.c` | 15+ 函数：进程数组管理、调度器、fork/exit/wait、sleep/wakeup |
| `proc/type.h` | `proc_t` 新增 `lk`/`state`/`parent`/`exit_code`/`sleep_space`/`name`；新增 `proc_state` 枚举 |
| `trap/trap_user.c` | ecall epc+=4 移到 syscall 前；中断路径加 `proc_yield` |
| `trap/trap_kernel.c` | 恢复 lab-5 中断处理逻辑，**不加** `proc_yield` |
| `trap/timer.c` | 新增 `timer_wait`，`timer_update` 加 `proc_wakeup` |
| `syscall/sysfunc.c` | 新增 `sys_fork`/`sys_exit`/`sys_wait`/`sys_sleep`/`sys_print_str`/`sys_print_int`/`sys_getpid` |
| `syscall/type.h` | 新系统调用号 SYS_brk=1 到 SYS_sleep=10 |
| `mem/kvm.c` | KSTACK 从单栈改为 N_PROC 个双页栈 |
| `lock/sleeplock.c` | 睡眠锁实现 |
| `main.c` | 调用 `proc_init` / `mmap_init`，末尾 `proc_scheduler()` |
| `lib/type.h` | include 顺序调整：lock/type.h 先于 proc/type.h |

## 踩坑记录

### 坑 1：内核态 proc_yield 跨核崩溃

**现象**：`panic! trap_kernel_handler: not from s-mode`

**原因**：`kernel_vector` asm 只保存通用寄存器，不保存 sepc/sstatus。proc_yield 后进程可能在另一核上恢复，sret 读到错误 CSR。

**解决**：只在 `trap_user_handler` 调 `proc_yield`（user 路径通过 trapframe 恢复 CSR）；`trap_kernel_handler` 不调。

### 坑 2：proc_return 中提前 w_stvec 导致竞态

**现象**：timer 中断在 `intr_off` 前触发 → `user_vector` 用 `sscratch=0` 当 trapframe → 双 fault

**解决**：删掉 `proc_return` 中冗余的 `w_stvec`，让 `trap_user_return` 内部（先 `intr_off` 再 `w_stvec`）安全处理。

### 坑 3：fork 级联爆炸

**现象**：fork 一次产生 30+ 进程

**原因**：`trap_user_handler` 先调 `syscall()`（内含 `proc_fork` 复制 trapframe），后 `epc+=4`。子进程拿到的 `user_to_kern_epc` 指向 ecall 本身，首次 sret 又跑一次 fork。

**解决**：`epc+=4` 移到 `syscall()` 之前。

### 坑 4：proc_wait 自锁死锁

**现象**：`panic! spinlock_acquire`

**原因**：`proc_wait` 先 `spinlock_acquire(&p->lk)` 获取自身锁，循环扫描时又对 `proc_list[i] == p` 再次 `spinlock_acquire` 同一把锁。

**解决**：扫描时 `if (c == p) continue;` 跳过自己，或使用全局 `wait_lk` 统一保护 wait/exit 操作。

### 坑 5：include 循环依赖

**现象**：`unknown type name 'proc_t'`

**原因**：`lib/type.h` include `proc/type.h` 时，`proc/type.h` 里的 `spinlock_t lk` 需要的 `spinlock_t` 还未定义。

**解决**：`lib/type.h` 中 `#include "../lock/type.h"` 放在 `#include "../proc/type.h"` 之前；`proc/type.h` 不直接 include lock/type.h（避免循环）。

### 坑 6：proc_wait 与 proc_exit 的唤醒竞态

**现象**：父进程 wait 永远等不到子进程退出，系统看起来卡死。

**原因**：父进程 `proc_wait` 扫描完所有子进程后发现没人 ZOMBIE，准备睡觉。但子进程恰好在"扫描完毕"到"父进程进入 SLEEPING"这个无锁窗口内调了 `proc_exit` → `proc_try_wakeup`。父进程此时还是 RUNNING，wakeup 不做事。随后父进程把自己设为 SLEEPING，再也等不到唤醒。

**解决**：引入全局 `wait_lk`。`proc_wait` 和 `proc_exit` 都持同一把全局锁，保证"扫描+睡眠"和"标记 ZOMBIE+唤醒"之间没有竞态窗口。这就是 `wait_lk` 的由来——用锁缩小窗口是解决丢失唤醒问题最直接的手段。

### 坑 7：内核栈仅 1 页导致溢出

**现象**：`Store/AMO page fault` at stack guard page

**原因**：深层调用链（user_vector → syscall → printf → timer → proc_yield）溢出 4KB 内核栈。

**解决**：每个进程内核栈 2 页，`KSTACK` 映射两个连续物理页。

## 测试

### 测试 1：getpid + print

```c
int main() {
    int pid = syscall(SYS_getpid);
    if (pid == 1) {
        syscall(SYS_print_str, "\nproczero: hello ");
        syscall(SYS_print_str, "world!\n");
    }
    while (1);
}
```

验证：基本系统调用（getpid、print_str）和调度器正常运行。

### 测试 2：fork 基础

```c
int main() {
    syscall(SYS_print_str, "level-1!\n");
    syscall(SYS_fork);
    syscall(SYS_print_str, "level-2!\n");
    syscall(SYS_fork);
    syscall(SYS_print_str, "level-3!\n");
    while(1);
}
```

验证：fork 创建子进程，父子进程各自继续执行，多进程并发打印。

### 测试 3：fork + wait + exit

```
--------test begin--------
child proc: hello!
parent proc: hello!
good boy!
--------test end----------
```

验证：fork → 子进程 exit → 父进程 wait 回收 → 退出状态正确传递。

### 测试 4：sleep

```
Ready to sleep!          ← 子进程
Ready to exit!           ← 30 ticks 后
Child exit!              ← 父进程 wait 返回
```

验证：`proc_sleep` / `proc_wakeup` 机制 + `timer_wait` 精确睡眠。

## 构建与运行

```bash
make build    # 编译
make run      # QEMU 运行 (Ctrl+A, X 退出)
make debug    # 调试模式 (gdb-multiarch)
make clean    # 清理
```

## 环境

- **OS**：WSL2 + Ubuntu 22.04
- **编译器**：riscv64-linux-gnu-gcc
- **模拟器**：qemu-system-riscv64
- **参考**：xv6-riscv-2020 (util 分支)
