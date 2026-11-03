# 开发进度

> 按日期倒序记录已完成的工作、验证证据和遗留问题。
> 计划与验收标准以 [`project.md`](project.md) 为准。

---

## 2026-11-03（星期二）

S2 打通两条硬件通路：**XCLK 送时钟**（S2①）和 **SCCB 认出 OV7670**（S2②）。CAN 那条留到下一轮。

### 1. S2① XCLK：PA8 输出 8 MHz

| 文件 | 内容 |
|---|---|
| `node/bsp/bsp_xclk.c` / `.h`（115 行） | `bsp_xclk_init()` / `bsp_xclk_hz()` / `bsp_xclk_source_name()` |
| `node/bsp/board.c` / `.h` | 新增 `board_gpio_enable_clock()`（幂等，按 GPIOA..H 分支） |
| `node/config/board_config.h` | `BOARD_XCLK_*`：端口/引脚/AF、源、分频 |
| `node/app/main.c` | `print_xclk()`：打印源和频率 + 三个寄存器快照 |

- **MCO1 编码**（核过 RM0090）：`RCC->CFGR` 的 `MCO1[1:0]`（bit22:21）= `00` HSI / `01` LSE / **`10` HSE** / `11` PLL；`MCO1PRE[2:0]`（bit26:24）= `0xx` 不分频 / `100`÷2 / `101`÷3 / `110`÷4 / `111`÷5。
- 为了让 `board_config.h` 的宏和寄存器位**一个整数一一对应**，`BOARD_XCLK_SOURCE` 直接用寄存器编码（`2` = HSE），`BOARD_XCLK_DIV` 用分频值（`1` = 不分频）。
- 实测：`RCC->CFGR = 0x0040940A`、`GPIOA->MODER = 0xA82A0000`，与写代码前预测的两个值**完全一致**；逻辑分析仪确认 PA8 = 8 MHz。
- 走的一个弯路：用户先测到"不是 8 MHz"，最后查明是**逻辑分析仪采样率设太低**（24 MS/s 采 8 MHz 只有 3 点/周期）——不是代码问题。以后量时钟先确认采样率 ≥ 10 倍频率。

### 2. S2② SCCB：读出 PID=0x76 / VER=0x73

| 文件 | 内容 |
|---|---|
| `node/bsp/bsp_i2c_soft.c` / `.h` | 软件 I²C：`init/probe/write/read/scan`，100 kHz（半周期 5 µs，走 DWT 延时） |
| `node/drv/ov7670/ov7670.c` / `.h` | 只管 OV7670 的寄存器协议：`write_reg/read_reg/chip_id/present` |
| `node/app/main.c` | `sccb_selftest()`：先扫总线（有没有器件），再读 PID/VER（时序对不对） |

- **SCCB ≠ 标准 I²C**，四个差别：① 第 9 位是"不关心位"，从机**不拉低**；② **不支持重复起始**，读寄存器必须"写地址 + STOP + START + 读"两次传输；③ 没有时钟延展；④ 最大 400 kHz。代码里 `ov7670_read_reg()` 就是按这个写的。
- SCL/SDA 用 **BSRR/BRR 原子置位**而不是读改写 ODR；引脚用**开漏输出 + 内部上拉兜底**（模块自带 4.7 kΩ，内部 40 kΩ 并联也不影响）。
- 读回 `PID = 0x76`、`VER = 0x73`，与手册一致。
- 排障顺序（这次真的用上了）：**先读 GPIO 状态寄存器**（`PUPDR`/`IDR`）确认 MCU 侧没问题，再怀疑接线。实测 `GPIOB->IDR = 0x8FD8`（两条线空闲都是高）、`MODER = 0x00500280`、`PUPDR = 0x00500100`，全部正常 → 问题必然在模块侧。
- 根因：**SCL 和 SDA 接反了**。对调后立刻正常。

### 3. 引脚分配与冲突（已写进 `project.md`）

| 信号 | 引脚 | 排针 | 备注 |
|---|---|---|---|
| XCLK | PA8（MCO1） | J3-33 | AF0 |
| SCL / SDA | PB10 / PB11 | J2-54 / J2-53 | 软件 I²C，MPU6050 以后共用 |
| PCLK | PA6 | J2-29 | DCMI 唯一映射 |
| HREF | PA4 | J2-27 | |
| VSYNC | PB7 | J3-3 | |
| D0–D7 | PC6/PC7/PC8/PC9/PC11/PD3/PB8/PB9 | — | 8 根全是 AF13 唯一映射 |
| PWDN / RESET | PE0 / PE1 | J2-2 / J2-1 | 不接也行，但留着能软件复位 |
| **CAN_RX / TX** | **PD0 / PD1** | J3-22 / J3-21 | **让位**：PB8/PB9 被 DCMI_D6/D7 占了 |

- 两个连带结论：`DCMI_D6=PB8`、`DCMI_D7=PB9` 在 F407 上**只有 AF13 这一个映射**，所以 CAN1 必须从 PB8/PB9 改到 PD0/PD1（`CAN1` 有重映射）；`PC9` 不能当 MCO2 用（被 `DCMI_D3` 占了），所以 XCLK 只能走 PA8。

### 4. 串口输出全部改成英文

判据：`grep -rnP '(printf|puts)\s*\(\s*"[^"]*[\x{4e00}-\x{9fff}]'` 命中为空。共改 5 句（`main.c` 3 句 + `app_tasks.c` 2 句）。**代码注释仍然保留中文** —— 注释不占串口带宽，读代码时还要用。

### 5. 接线总表 `docs/wiring.md`

新增了全项目的接线文档（[`docs/wiring.md`](docs/wiring.md)），把之前散在 `project.md` §3.2 里的接线细节收拢成一份"照着插线就能跑"的清单：

- **F407 引脚分配总表**：DCMI 11 根 / SCCB 2 根 / CAN 2 根 / UART 2 根 / SWD 2 根 + 剩余可用引脚。
- **完整冲突清单**：DCMI_D6/D7 逼 CAN1 改 PD0/PD1、DCMI_D3 逼 XCLK 只能走 PA8、OV7670 的 PWDN 占掉 PE0 所以 MPU6050 的 INT 不接、SD 卡占 PC8–PC12 和 DCMI 重叠。
- **每个器件逐根线**：OV7670（18 根，含模块丝印 `SIOC`/`SIOD` 对照）、MPU6050（5 根）、TJA1050（8 脚全列）、UART→Pi5、MCP2515→Pi5（含 40-pin 编号）。
- **供电与电平**：三条铁律（只共地不共电源、3.3V 器件不许接 5V、Pi5 的 GPIO 只接 3.3V）；TJA1050 为什么可以直连（PD0 是 FT 引脚）、MCP2515 5V 版为什么必须加电平转换。
- **CAN 总线**：终端电阻 60 Ω、共地、双绞。
- **上电顺序 + 分阶段自检表 + 已知的坑 7 条**。

同时把 `project.md` 里两处过时的内容改掉了：UART 从最早写的 PA2/PA3 改成实际在用的 **PA9/PA10**；MPU6050 从"独立 I2C2"改成**与 OV7670 共用 SCCB 总线**（地址 0x21 和 0x68 不冲突，且 DCMI 已经占满引脚）。

### 6. 当前状态与遗留

- 构建：**FLASH 14,864 B (1.42%) / RAM 5,544 B (4.23%) / CCM 29,184 B (44.53%)，0 警告**。
- 今天踩过一个坑：`config/board_config.h` 的 SCCB 段被 VSCode 的旧缓冲区覆盖过，编译报 `undeclared`。**改配置头文件前先确认编辑器里没有未保存的旧版本**。
- **未提交**：`project.md` 的引脚表 + 今天全部 S2 代码。按计划攒到 S2 全做完再一起提交。
- 下一步 **S2③：CAN**。建议先在 F407 上用 **CAN1 loopback 模式**自测（芯片内部把 TX 回送给 RX，不需要收发器也不需要 Pi5），把"位定时算错"和"总线/接线问题"分开；通了再接 TJA1050 + Pi5 的 MCP2515，用 `candump` 看 0x180。

---

## 2026-11-02（星期一）

给 VSCode 配好**一键编译 / 烧写 / 调试**的闭环，并补齐外设寄存器视图。

### 1. Cortex-Debug 扩展

- 装的是 **marus25.cortex-debug 1.12.1**，目录 `~/.vscode/extensions/marus25.cortex-debug-1.12.1`。
- 走了一个弯路：`code --install-extension marus25.cortex-debug` 卡死（市场接口超时），直接从 marketplace 下 VSIX 又被 `code` 报 `code: 'Extract'` —— 原因是拿到的文件是 **gzip 套了一层 zip**（`file` 显示 `gzip compressed data`）。`gunzip -c x.vsix > fixed.vsix` 解一层后再装就成功了。
- `armToolchainPath` 必须显式写官方工具链路径：apt 那个 `gcc-arm-none-eabi` 包**不含 gdb**（历史原因，前面已卸）。

### 2. `.vscode/tasks.json`

| 任务 | 命令 | 说明 |
|---|---|---|
| `build` | `make` | `Ctrl+Shift+B` 默认任务，`problemMatcher` 用 `$gcc` + `fileLocation` 指向 `node/`，点错误能跳到源码 |
| `flash` | `openocd … program vision-node.elf verify reset exit` | `dependsOn: build`，改完代码一键烧 |
| `clean` / `test` / `check-layers` | 对应 make 目标 | |

### 3. `.vscode/launch.json`

两个配置，都基于 `type: cortex-debug` + `servertype: openocd`：

- **调试（ST-Link + OpenOCD）**：`preLaunchTask: build` → 烧写 → 停在 `main()`
- **烧写并调试（不重新构建）**：省掉编译那步

关键字段：`openOCDLaunchCommands: ["adapter speed 1800"]`（ST-Link V2 实测最快 1800 kHz）、`device: STM32F407ZG`、`rtos: "FreeRTOS"`（调试侧栏能看到任务列表和栈水位）、`svdFile`。

### 4. 外设寄存器视图（SVD）

- 文件：[`node/arch/cmsis/STM32F407.svd`](node/arch/cmsis/STM32F407.svd)，2,164,866 字节，`schemaVersion="1.1"`，**91 个外设**（含 `RCC`/`USART1`/`DCMI`/`GPIOA`/`CAN1`/`FLASH`/`DMA1/2`），不含 `DWT`/`SysTick` 这类内核外设。
- 来源：`cmsis-svd/cmsis-svd-data` 仓库（老地址 `posborne/cmsis-svd` 已废弃，文件迁走了），溯源与 sha256 记在 `node/arch/cmsis/README.md`。
- 作用：调试时 `RCC->CFGR = 0x940A` 会展开成 `SW=PLL / HPRE=/1 / PPRE1=/4 / PPRE2=/2`，S3 调 DCMI 时 `CAPTURE`/`VSYNC`/`FNE` 这些位直接显示成勾选状态。

### 5. 验证：F5 链路实测

用 openocd + gdb 手工复现了 Cortex-Debug 会做的事：

```
target extended-remote :50021 → monitor reset halt → load → tbreak main → continue
→ Temporary breakpoint 1, main () at app/main.c:111
→ pc = 0x80005dc, sp = 0x20020000
→ SystemCoreClock = 0xA037A00 = 168000000  ✓
```

烧写、复位、断点、符号解析全部正常。`node/vision-node.elf` 被 `node/.gitignore` 忽略，`build/` 同理。

---

## 2026-10-22（星期四）

今天从"零环境"做到 **S1 完成**：工具链落地 → 工程骨架 → 启动/链接/时钟 → 裸机点灯串口（S1a）→ FreeRTOS 双任务（S1b）。

### 1. 工具链与工程骨架

- **CMSIS 头落地**：`node/arch/cmsis/` 放入 8 个芯片描述头（`core_cm4.h`、`cmsis_gcc.h`、`cmsis_compiler.h`、`cmsis_version.h`、`mpu_armv7.h`、`stm32f4xx.h`、`stm32f407xx.h`、`system_stm32f4xx.h`）。
  来源：STM32CubeF4 `Drivers/CMSIS/Include/`（Core，实体文件）+ `cmsis_device_f4/Include/`（设备头）。版本与 CubeMX 生成的工程天然对齐，溯源、commit、sha256、许可全部记在 `node/arch/cmsis/README.md`。
- **工具链**：ARM GNU Toolchain **14.3.rel1** 解压到 `~/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi`（gcc 14.3.1 / gdb 15.2）。
  卸掉了 apt 的 `gcc-arm-none-eabi 14.2` —— Ubuntu 那个包**不含 `arm-none-eabi-gdb`**（Debian 因许可移除），Cortex-Debug 没法用；而且两套工具链并存时 `make` 会选中错的那个。
- **Makefile**：分层构建（`arch bsp drv os lib proto app`），每层只拿到"自己 + 下层"的 `-I`，跨层 include 在编译期就编不过；`make check-layers` 用 grep 守两条红线（`drv/bsp/arch` 不许有 RTOS 头，`lib/proto` 不许有外设头）；`make test` 在 PC 上跑单测。工具链路径自动探测 `~/opt`，不依赖 PATH。
- **VSCode**：`.vscode/c_cpp_properties.json` 指向真实 `arm-none-eabi-gcc` + 11 条 `includePath` + `defines: STM32F407xx, HSE_VALUE`；`.vscode/settings.json` 把 `*.h` 关联为 **c**。
  之前满屏红错的根因：**cpptools 在编译器还不存在时静默降级到 `/usr/bin/gcc`**（配置写于 12:48:49，工具链解压完是 12:52:00，差 3 分钟），拿 x86 的 gcc 去解析 ARM 的 CMSIS 头。
- **ST-Link udev 规则** `/etc/udev/rules.d/60-stlink.rules` 已装，无需 sudo 即可烧写；PATH 写进 `~/.bashrc` 与 `/etc/profile.d/`。

### 2. `node/arch/` 三个文件（自己写，不抄 CubeMX）

| 文件 | 内容 |
|---|---|
| `startup_stm32f407xx.s` | 98 项向量表（16 内核 + 82 外部中断，IRQ79 保留槽），复位流程：拷 `.data` → 拷 `.ccmram` → 清 `.bss` → 清 `.ccmbss` → `SystemInit` → `__libc_init_array` → `main`；所有 handler 用 `.thumb_set` 做弱别名，FreeRTOS 接管 SVC/PendSV/SysTick 时不用改这个文件 |
| `stm32f407.ld` | FLASH 1M@`0x08000000` / RAM 128K@`0x20000000` / CCM 64K@`0x10000000`；5 条链接期断言（向量表 98×4、`_estack` = RAM 末尾、`.data` 地址单调…） |
| `system_stm32f4xx.c` | 开 FPU（CPACR）+ HSE 8 MHz → PLL M=8/N=336/P=2/Q=7 → **168 MHz**，FLASH 5WS + 预取/缓存，AHB /1、APB1 /4、APB2 /2；HSE 起振失败退回 HSI 16 MHz；`SystemCoreClockUpdate()` 按寄存器重算 |

**踩到的 5 个坑（都写进了代码注释）**：

1. `AHBPrescTable` / `APBPrescTable` **不能写 `static`** —— CMSIS 头里已经是 `extern const`，直接编译报错。
2. **`HSE_VALUE` / `HSI_VALUE` 在 CMSIS 头里根本没有定义**（ST 放在 HAL 的 `stm32f4xx_hal_conf.h`）。不用 HAL 就得自己 `#define` 并加整除守卫。
3. `ENTRY(Reset_Handler)` 需要**全局强符号**：只写 `.type` 不写 `.global` 会变局部符号，ld 只警告然后自己猜入口地址。
4. `.ccmram` 若用 `(NOLOAD)`，**带初值的变量会静默变成垃圾**。改成 `>CCMRAM AT> FLASH` + 启动代码拷贝。
5. `--gc-sections` 会丢掉 `.ccmram` 里的变量（`__attribute__((used))` 只管编译器不管链接器），要用 `KEEP(*(.ccmram*))`。

链接期断言做过反证：把 `98*4` 故意改成 `50*4`，链接立即失败并打印中文断言文案。

### 3. S1a —— 裸机：时钟 + 串口 + 点灯

新增/改动的文件：

| 文件 | 作用 |
|---|---|
| `node/config/board_config.h` | 全部引脚与波特率宏（换板子只改这里） |
| `node/bsp/board.c/.h` | GPIO 时钟、LED、AF/速度/上下拉配置、从寄存器反推实际时钟 |
| `node/bsp/bsp_dwt.c/.h` | DWT 周期计数器（时间基准；**故意不用 SysTick**，它以后要交给 FreeRTOS） |
| `node/bsp/bsp_uart.c/.h` | USART1 115200 8N1 轮询收发 + `_write` 重定向 |
| `node/bsp/syscalls.c` | newlib 桩，连同 `_close/_fstat/_isatty/_lseek/_read` 一起补齐，消掉 5 条链接警告 |
| `node/app/main.c` | 启动横幅 + 10 ms 定节拍主循环 + LED + 每秒打印 |
| `node/arch/stm32f407.ld` + `startup` | 新增 `.ccmbss (NOLOAD)` 段 |

`.ccmbss` 这个改动的理由：CCM 里零初始化的东西（堆、任务栈）如果走 `.ccmram AT> FLASH`，会**连带在 Flash 里存一份等大的零** —— 4 KB 堆白占 4 KB Flash，将来 24 KB 内核堆就是 24 KB。加一个 NOLOAD 段之后 FLASH 从 11632 B 降回 7560 B。

**板子上读回来的验证证据**：

| 寄存器 | 值 | 结论 |
|---|---|---|
| `RCC->CR` | `0x03037383` | HSERDY=1、PLLRDY=1 |
| `RCC->PLLCFGR` | `0x07405408` | M=8 N=336 P=2 Q=7 src=HSE → 168 MHz |
| `RCC->CFGR` | `0x0000940A` | SWS=PLL、AHB/1、APB1/4、APB2/2 |
| `FLASH->ACR` | `0x00000705` | 5 WS + 预取/指令缓存/数据缓存 |
| `USART1->BRR` | `0x2D9` = 729 | 84 MHz / 729 = 115226 baud，误差 +0.02% |
| `GPIOF->MODER` | `0x00140000` | PF9/PF10 通用输出 |
| `GPIOA->MODER` / `AFR[1]` | `0xA8280000` / `0x770` | PA9/PA10 = AF7 |

主循环计时：604 tick × 10 ms = 6040 ms，与 6040 ms 时的 `g_uptime_ms` 完全吻合；10 秒窗口内与墙钟偏差 0.2% 以内。

**用户确认**：LED 闪烁、串口输出的数据都正常。

### 4. S1b —— 串口自环 + FreeRTOS

- **串口自环自检**：发 22 字节再从 PA10 收回来比对。没接跳线判"跳过"，接了判 PASS/FAIL。用来在**没有任何外部设备**的情况下闭环证明收发链路。
- **FreeRTOS V11.3.2 vendored（零改动）** 到 `node/os/freertos/`：`tasks/list/queue/timers/event_groups/stream_buffer` + `portable/GCC/ARM_CM4F` + `heap_4`，版本/来源/许可/为什么选 heap_4 记在同目录 `README.md`。
- `node/config/FreeRTOSConfig.h`：tick 1000 Hz、堆 24 KB、7 个优先级、`configMAX_SYSCALL_INTERRUPT_PRIORITY = 5<<4`、内核优先级 15<<4、栈溢出检查 2 级、断言接到 `os_assert_failed`。
- `node/os/os_task.c/.h` + `os_hooks.h`：全工程**只有这一层允许出现 FreeRTOS 头**。`ucHeap[24 KB]` 与空闲任务栈显式放 `.ccmbss`（CCM）—— 任务栈在物理上就不可能被 DMA 碰到。
- `node/app/app_tasks.c`：两个任务 —— `led`（100 ms 节拍，PF10 每次翻转、PF9 每 5 次翻转）+ `report`（1 s 打印堆剩余/历史最小/栈水位）。

**板子上读回来的验证证据**：

| 检查项 | 值 | 结论 |
|---|---|---|
| 向量表 [11] SVC / [14] PendSV / [15] SysTick | `0x08000B45` / `0x08000B8D` / `0x08000CD9` | 已由 port.c 接管，不再是 Default_Handler |
| `SysTick->LOAD` | `0x2903F` = 168000−1 | 168 MHz / 168000 = **1 kHz** |
| `SCB->SHPR3` | `0xF0F00000` | PendSV 与 SysTick 优先级都是 **15<<4**（最低） |
| `pxCurrentTCB` | `0x200001EC` | 调度器在切任务 |
| `xTickCount` | 10 s 窗口推进 10188 ms | 与墙钟一致 |
| `g_assert_count`/`g_malloc_failed`/`g_stack_overflow` | 0 / 0 / 0 | 无断言、未爆堆、未爆栈 |

**用户确认**：串口输出正常。

### 5. 当前状态

```
FLASH:  13060 B / 1 MB    (1.25%)
RAM:     5536 B / 128 KB  (4.22%)
CCMRAM: 29184 B / 64 KB   (44.53%)   ← 24 KB 内核堆 + 4 KB newlib 堆 + 空闲任务栈
```

- `make` / `make check-layers` / `make test` 全绿，**0 编译警告**。
- 板卡档案写在 [`docs/board.md`](docs/board.md)（引脚表、电源、BOOT、ISP 排针接法）。
- 烧写前已把**原厂固件整片备份**到 `~/backup/f407-factory-firmware-20261022.bin`（1 MB），恢复命令见 `docs/board.md` 第 5 节。

### 6. 遗留与下一步

- **代码尚未 commit**（`git status` 里 S1a/S1b 全是未跟踪文件）。
- 自环 PASS 还没实测：需要一根杜邦线短接 **J3-31 (PA10) ↔ J3-32 (PA9)** 再复位，串口应打印 `[loopback] PASS：22 字节原样收回`。
- **S2（SCCB 读 `PID=0x76` / `VER=0x73`）需要 OV7670 模块到手并接线**，硬件没到之前不推进。
- 建议买一个 **3.3 V 电平的 USB-TTL（CH340C / CP2102，¥5–10）**：让 printf 不占用逻辑分析仪，后面 S8 与树莓派 5 对拍也方便。当前用逻辑分析仪夹 PA9 解码同样能验收。
