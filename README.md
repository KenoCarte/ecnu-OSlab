# Lab-1: 机器启动 + printf + 自旋锁

## 实验目标

完成 RISC-V 双核启动，实现 printf 和自旋锁，最终在 QEMU 上输出双核启动信息。

## 启动链路

```
QEMU BIOS ──→ 0x80000000 (_entry) ──→ start ──mret──→ main (S-mode)
              [M-mode]                [M-mode]          [printf输出]
```

| 阶段 | 文件 | 模式 | 职责 |
|------|------|------|------|
| 入口 | boot/entry.S | M-mode | 为每个核设独立栈, 跳转 start |
| 启动 | boot/start.c | M-mode | 写 mstatus.MPP=S、mepc=main、配置 PMP/中断委托，执行 mret |
| 内核 | main.c | S-mode | print_init → 双核同步 → printf 输出 |

## 实验内容

### 1. start.c — M-mode → S-mode 切换

- `w_satp(0)`: 暂时关闭分页，使用物理地址
- `r_mhartid() → w_tp(id)`: 把 hartid 存到 tp 寄存器，供 S-mode 使用
- `mstatus.MPP = S-mode`: 告诉硬件 mret 之后切到 S 模式
- `w_mepc(main)`: mret 后 PC 跳到 main
- **PMP 配置**: `w_pmpcfg0(0xf) + w_pmpaddr0(全地址)` — 允许 S-mode 访问全部物理内存
- **中断委托**: `w_medeleg(0xffff) + w_mideleg(0xffff)` — 异常和中断从 M-mode 委托到 S-mode
- `w_sie(...)`: 使能 S-mode 的外部/定时器/软件中断
- `asm volatile("mret")`: 触发状态迁移

### 2. spinlock.c — 自旋锁实现

| 函数 | 逻辑 |
|------|------|
| spinlock_init | locked=0, cpuid=-1, 设锁名 |
| spinlock_holding | 检查 locked && cpuid == mycpuid() |
| spinlock_acquire | push_off() → 原子CAS自旋等待 → 记录cpuid |
| spinlock_release | 清cpuid → 内存屏障 → 原子释放 → pop_off() |

关键点:
- **push_off/pop_off**: 嵌套中断开关，防止单核调度打断
- **__sync_lock_test_and_set**: GCC内置原子操作，对应 RISC-V amoswap 指令
- **mycpuid() vs r_mhartid()**: S-mode 下不能读 M-mode CSR，改用 tp 寄存器

### 3. print.c — printf 实现

- 支持格式串: `%d`(有符号十进制), `%p`(32位十六进制), `%x`(64位0x前缀十六进制), `%c`, `%s`
- 使用 `stdarg.h` 遍历可变参数
- printf 全程持有 print_lk 自旋锁，保证多核输出不交错
- assert 委托给 panic，panic 输出错误信息后设置 panicked 标志并永久自旋

### 4. main.c — 双核同步启动 + 并行加法

- 核0: 先初始化 UART 和锁，完成后置 started=1
- 核1: 自旋等待 started，然后输出启动信息
- 课后实验: 并行加法，对比无锁/有锁版本的 sum 结果

## 踩坑记录

### 坑1: S-mode 下读 mhartid 崩溃
- 现象: 编译通过，QEMU 无输出直接 terminate
- 原因: spinlock.c 中 `r_mhartid()` 读 M-mode CSR，mret 后已是 S-mode，触发非法指令异常
- 解决: 全部替换为 `mycpuid()`（读 tp 寄存器）

### 坑2: mret 后 S-mode 立即崩溃
- 现象: M-mode 中直接调用 main 正常，mret 后无输出
- 原因: RISC-V PMP 寄存器复位后默认拒绝 S/U 模式的所有内存访问
- 解决: start.c 的 mret 前配置 PMP:
  - `w_pmpaddr0(0x3fffffffffffffull)` — 覆盖整个物理地址空间
  - `w_pmpcfg0(0xf)` — TOR 模式，允许 R/W/X

## 测试结果

### 基础启动
```
cpu 0 is booting!
cpu 1 is booting!
```

### 无锁并行加法
```
cpu 0 report: sum = 1006262
cpu 1 report: sum = 1071655  ← 远小于 2000000，数据竞争
```

### 有锁并行加法（粗粒度，锁在循环外）
```
cpu 0 report: sum = 1998243
cpu 1 report: sum = 2000000  ← 结果正确
```

### 锁粒度讨论
- **粗粒度锁**（锁在 for 循环外）: 整个加法过程一次 acquire/release，锁开销最小
- **细粒度锁**（锁在 for 循环内）: sum++ 每次都要 acquire/release，2000000 次自旋开销极大

## 构建与运行

```bash
make build   # 编译
make run     # QEMU 运行 (Ctrl+A, X 退出)
make clean   # 清理
make debug   # 调试模式 (配合 gdb-multiarch)
```

## 环境

- OS: WSL2 + Ubuntu 22.04
- 编译器: riscv64-linux-gnu-gcc
- 模拟器: qemu-system-riscv64
- 参考: xv6-riscv-2020 (util 分支)
