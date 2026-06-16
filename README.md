# Lab-3: 中断异常初步

## 实验目标

为 OS 内核引入中断和异常的识别处理能力，实现串口输入中断（回显+编辑）和时钟中断（滴答）。

## 中断 vs 异常

| | 中断 | 异常 |
|------|------|------|
| 恢复后 | 执行下一条指令 (PC+4) | 重执行当前指令 (PC) |
| 性质 | 异步、意料之内 | 同步、通常是 bug |
| 例 | 串口输入、时钟滴答 | 非法指令、page fault |

RISC-V 用 **trap** 统称二者，通过 `scause` 寄存器最高位区分（1=中断，0=异常）。

## 整体 trap 处理流程

### 串口中断（外设中断）
```
UART 硬件信号 → PLIC → CPU trap →
kernel_vector (trap.S, 保存32个寄存器) →
trap_kernel_handler → external_interrupt_handler →
plic_claim → uart_intr → plic_complete →
kernel_vector (恢复寄存器, sret 返回)
```

### 时钟中断
```
MTIME == MTIMECMP → M-mode trap →
timer_vector (trap.S, 更新MTIMECMP, 触发S-mode软件中断) →
mret 返回 S-mode → 软件中断触发 kernel_vector →
trap_kernel_handler → timer_interrupt_handler →
timer_update (ticks++) → w_sip 清除软件中断 →
sret 返回正常执行流
```

## 实验内容

### 1. start.c — M-mode 时钟初始化

在 mret 前增加 `timer_init()` 调用。该函数（骨架已提供）：
- 设置 `MTIMECMP = MTIME + INTERVAL`（首次触发时间）
- 配置 mscratch 数组，与 trap.S 的 timer_vector 协作
- 设置 M-mode trap 入口 `w_mtvec(timer_vector)`
- 开启 M-mode 中断总开关(`MSTATUS_MIE`)和时钟分开关(`MIE_MTIE`)

### 2. trap/timer.c — 系统时钟（S-mode）

三个函数：

| 函数 | 逻辑 |
|------|------|
| timer_create | 清零 ticks，初始化自旋锁 |
| timer_update | 加锁 → ticks++ → 解锁 |
| timer_get_ticks | 加锁 → 读 ticks → 解锁后返回 |

`timer_init`（M-mode 部分）已由骨架提供，不需要修改。时钟中断到达后，M-mode 的 `timer_vector` 更新 MTIMECMP 并制造 S-mode 软件中断，S-mode 的 `timer_interrupt_handler` 调用 `timer_update` 递增 ticks。

### 3. trap/trap_kernel.c — trap 分发逻辑

**trap_kernel_handler**：读 sepc/sstatus/scause/stval，根据 scause 最高位分派：
- 中断类：case 1(S-mode软件中断)、5(S-mode时钟中断) → `timer_interrupt_handler`；case 9(S-mode外设中断) → `external_interrupt_handler`
- 异常类：目前只打 panic（lab-3 不要求处理异常）

**external_interrupt_handler**：`plic_claim()` 取中断号，若是 `UART_IRQ` 则调 `uart_intr()`，最后 `plic_complete()` 通知 PLIC。

**timer_interrupt_handler**：仅 CPU-0 执行 `timer_update()`，然后 `w_sip(r_sip() & ~2)` 清除 S-mode 软件中断 pending 位。

### 4. lib/uart.c — 中断驱动的字符输入

`uart_intr` 在原有回显基础上增加了编辑功能：
- `\r`（回车）→ 输出 `\n`
- `127`(Delete) 或 `\b`(Backspace) → 输出 `\b \b`（左移+空格覆盖+左移）
- 其他字符 → 原样回显

## 测试结果

### 时钟滴答测试（test-1, test-2）

两个核每隔 10 ticks 输出一次 "di da"：

```
cpu 0 is booting!
cpu 0:di da
cpu 1 is booting!
cpu 1:di da
cpu 0:di da
cpu 1:di da
cpu 0:di da
cpu 1:di da
```

修改 `INTERVAL`（默认 1000000 ≈ 0.1 秒）可调节滴答频率：
- 减半 → 加倍输出速度
- 加倍 → 减半输出速度

### UART 输入测试（test-3）

启动后直接敲键盘即可测试：
- 普通字符 → 回显在屏幕上
- Backspace → 删除光标前一个字符
- Enter → 换行

```
cpu 0 is booting!
cpu 1 is booting!
^_^
```

## 代码变更

| 文件 | 内容 | 类型 |
|------|------|------|
| boot/start.c | 新增 `timer_init()` 调用 | 手写 |
| trap/timer.c | timer_create / timer_update / timer_get_ticks | 手写 |
| trap/trap_kernel.c | 填充 switch-case + external_interrupt_handler | 手写 |
| lib/uart.c | uart_intr 增加 Backspace/换行 | 手写 |
| main.c | 完整的初始化序列 + 时钟滴答输出 | 手写 |
| trap/trap.S | kernel_vector + timer_vector | 骨架 |
| trap/plic.c | PLIC 初始化/claim/complete | 骨架 |
| arch/type.h | MIE_MTIE 等中断使能宏 | 骨架更新 |

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
