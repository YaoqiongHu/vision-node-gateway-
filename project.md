# 项目计划书：智能视觉节点与 CAN 网关

> **项目代号**：vision-node-gateway
> **负责人**：Hus
> **文档性质**：实施计划（按任务依赖顺序推进，不绑定日历）


---

## 1. 项目摘要

### 1.1 一句话

> 用 STM32F407 + OV7670 做**实时视觉节点**（图像采集 → 二值化 → 目标定位），用树莓派 5 做**CAN 网关与诊断中心**（内核驱动收数据 → 网关转发 → UDS 诊断下发参数），双节点通过 CAN 总线与 UART 链路互联。

### 1.2 为什么是这个项目

你手上的硬件（F407、OV7670、Pi 5）刚好能拼出一个**真实的两级嵌入式系统**，而不是单板 demo：

| 维度 | 单板传感器项目 | 本项目 |
|---|---|---|
| 系统结构 | 单节点 | 视觉节点 + 网关，双 ECU、双总线 |
| 数据规模 | 几十字节/次 | 图像（KB 级）+ 元数据（字节级），必须做带宽与协议设计 |
| 实时性 | 可回避 | 不可回避（帧率、帧周期抖动、端到端延迟） |
| 协议栈 | 基本没有 | 自定义帧协议（CRC）+ CAN + UDS |
| 面试可追问面 | 2 层 | 5 层（图像/驱动/协议/网关/诊断） |



### 1.3 关键指标（验收用）

| 指标 | 目标值 |
|---|---|
| 采集帧率（QQVGA RGB565，DCW 硬件 2:1） | ≥ 15 fps（目标 30 fps） |
| 单帧处理耗时（灰度+二值化+质心，160×120 = 19,200 px） | ≤ 20 ms（实测目标 ≤ 5 ms） |
| 帧周期抖动（上限） | ≤ 2 ms（p99） |
| 上传帧率（QQVGA 灰度） | ≥ 5 fps @ 2 Mbps |
| CAN 状态帧周期 | 100 ms ± 5 ms |
| CAN 总线负载（500 kbps） | ≤ 30 % |
| Pi5 驱动→用户态延迟 | p99 ≤ 1 ms |
| 连续运行 | 1 h 无丢帧、无 oops、无内存增长 |

---

## 2. 系统架构

### 2.1 分层视图

```
┌──────────────────────────── 视觉节点（STM32F407）────────────────────────────┐
│ 应用层  FreeRTOS 任务：采集/处理/CAN/链路/日志 + 参数管理（阈值、曝光、ROI）  │
│ 算法层  灰度化 → 二值化 → 质心/连通域 → 组帧上传(QQVGA)                        │
│ 驱动层  DCMI + DMA 双缓冲 │ SCCB(I2C) 配置 │ bxCAN │ USART + DMA │ DWT 计时    │
│ 硬件    OV7670 (8bit 并口) │ TJA1050(CAN PHY) │ 24MHz XCLK                     │
└───────────────┬─────────────────────────────────────┬─────────────────────────┘
                │ CAN 500 kbps（状态帧/结果帧/UDS）      │ UART 2 Mbps（图像帧 + 命令帧）
                ▼                                       ▼
┌──────────────────────────── 网关（树莓派 5，Linux 6.x）───────────────────────┐
│ 应用层  gateway（网关进程）│ uds_server │ collector（存图/统计）                │
│ 内核层  serdev 字符设备 /dev/frame0（环形缓冲 + 等待队列 + poll）              │
│         SocketCAN（can0，MCP2515）│ 设备树 overlay                            │
│ 硬件    MCP2515 + TJA1050 │ UART2 (GPIO4/5)                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 数据流

1. **视觉节点**：OV7670（RGB565 + DCW 硬件 2:1）→ DCMI+DMA → 双缓冲（QQVGA 160×120，38,400 B/帧）→ 灰度/二值化 → 质心 → 结果入队；
2. **元数据上总线**：结果/状态打包成 CAN 帧（0x180 状态、0x181 结果）发到网关；
3. **图像上链路**：灰度化后的 QQVGA 灰度帧按自定义帧协议（帧头+长度+序号+CRC16）走 UART 到 Pi5；
4. **网关**：serdev 驱动收帧 → 环形缓冲 → `poll` 通知用户态 → 存图/统计；同时 SocketCAN 收 CAN 帧做聚合；
5. **诊断**：诊断仪发 UDS 请求（0x7E0）→ 网关 UDS server 处理 → 读当前配置（0x22）或写阈值（0x2E）→ 通过 UART 命令帧下发到视觉节点 → 节点应答。

### 2.3 五个关键设计决策

| # | 决策 | 理由 | 面试价值 |
|---|---|---|---|
| 1 | **图像走 UART、状态走 CAN** | CAN 500 kbps 传一帧 QQVGA 需 300+ ms，带宽不够；车载真实架构也是"元数据上 CAN，图像上以太网/LVDS" | 能讲清"为什么不用 CAN 传图像" |
| 2 | **F407 做节点、Pi5 做网关** | F407 + FreeRTOS 用任务优先级保证确定性；Pi5 跑 Linux 承担协议栈与存储 | 能讲"实时任务放 MCU、非实时放 Linux"的分工 |
| 3 | **DCMI + DMA 双缓冲** | 单缓冲会在处理期间丢帧；双缓冲让采集与处理并行 | 能讲 DMA 循环模式、半满/全满中断 |
| 4 | **自定义帧协议而不是裸发** | 需要帧同步、长度、序号、校验、重同步 | 能讲协议设计（同步字、转义、CRC 多项式） |
| 5 | **内核 serdev 驱动而非用户态读串口** | 帧解析、缓冲、并发在上游做；用户态只读结构体 | 能讲 serdev/DT/字符设备/并发 |

---

## 3. 硬件方案

### 3.1 清单

| 类别 | 物料 | 数量 | 说明 |
|---|---|---|---|
| 主控 | STM32F407 开发板（含 ST-Link） | 1 | 已有 |
| 摄像头 | OV7670 模块（8-bit 并口，3.3V，**无 FIFO**） | 1 | 已有 |
| 惯性传感器 | MPU6050 模块（I2C，地址 0x68） | 1 | 已有，可选增强 |
| 网关 | 树莓派 5 8GB | 1 | 已有 |
| CAN 控制器 | MCP2515 模块（SPI 转 CAN，**8 MHz 晶振**） | 1 | 已有 |
| CAN 收发器 | TJA1050 模块（F407 侧） | 1 | 已有 |
| 接插 | 杜邦线 | 1 套 | 已有 |
| 调试 | 8 MHz 逻辑分析仪（8CH） | 1 | 看 PCLK/VSYNC/HREF、SPI、I2C 波形 |
| 总线 | 120 Ω 终端电阻 | 2 | 短线可只在两端各并一个 |


### 3.2 接线

> **完整的接线总表见 [`docs/wiring.md`](docs/wiring.md)** —— 那里有每一根线的排针位置、模块丝印对照、供电与电平规则、CAN 终端电阻、上电自检清单和踩过的坑。本节只保留决策要点。

**A. OV7670 ↔ STM32F407**（走开发板摄像头接口，按板卡原理图核对引脚）

| OV7670 | F407 | 说明 |
|---|---|---|
| VCC | 3.3V | **严禁 5V** |
| GND | GND | — |
| SCL / SDA | PB10 / PB11（软件 I²C） | SCCB 配置通道，7 位地址 0x21（写 0x42 / 读 0x43）；和 MPU6050 共用一条总线 |
| XCLK | **PA8（MCO1，AF0，输出 HSE 8 MHz）** | 模块无晶振，必须外部送时钟；PC9 不能作 MCO2，它被 DCMI_D3 占用 |
| PCLK | PA6（DCMI_PIXCLK） | — |
| HREF | PA4（DCMI_HSYNC） | — |
| VSYNC | PB7（DCMI_VSYNC） | — |
| D0–D7 | PC6 / PC7 / PC8 / PC9 / PC11 / PD3 / PB8 / PB9 | DCMI_D0–D7，AF13 唯一映射，不可改 |
| RESET | PE1 | 低有效，正常运行时拉高 |
| PWDN | PE0 | 拉低才工作 |

> **引脚冲突已解决**：DCMI_D6/D7 固定占 PB8/PB9，所以 **CAN1 不能走 PB8/PB9**，改用 PD0/PD1（见下面的 CAN 表）。

**B. UART 链路（F407 ↔ Pi5）**

| F407 | Pi5 | 说明 |
|---|---|---|
| USART1_TX (PA9) | GPIO15 / RXD (pin 10) | **交叉**：TX→RX |
| USART1_RX (PA10) | GPIO14 / TXD (pin 8) | **交叉**：RX←TX |
| GND | GND (pin 6) | **必须共地** |

用的是 **USART1（PA9/PA10）**，不是最早写的 PA2/PA3 —— 这两个脚在 J3 排针和 J5「ISP 串口」头上都有，
接线最方便，而且 S1 的串口驱动已经跑在这对脚上，不用再改代码。
从 **J3-32 / J3-31** 取线，不要从 J5 取（J5 第 1 脚是 5 V，接错会烧器件）。
两端都是 3.3 V TTL，**不需要电平转换**；2 Mbps 下杜邦线控制在 20 cm 以内。

**C. CAN 总线（F407 bxCAN ↔ Pi5 MCP2515）**

| 节点 | 引脚 | 说明 |
|---|---|---|
| F407 | **CAN_RX (PD0) / CAN_TX (PD1)** → TJA1050（TXD→PD1、RXD→PD0）→ CANH/CANL | 收发器已有，直接接。PB8/PB9 让给 DCMI_D6/D7 |
| Pi5 MCP2515 | SPI0：CS=GPIO8(CE0)、MISO=GPIO9、MOSI=GPIO10、SCLK=GPIO11，INT=GPIO25 | 复用 SPI0 |
| 总线 | CANH–CANH、CANL–CANL；两端各并 120 Ω | 短线可只在两端接 |

**D. 供电与地**：F407 与 Pi5 各自独立供电；**只共 GND，不要把 5V 互连**（避免双电源倒灌）。

**E. MPU6050 ↔ F407（Should，与 OV7670 共用 SCCB 总线）**

| MPU6050 | F407 | 说明 |
|---|---|---|
| VCC | 3.3V | 模块多有 LDO，接 3.3V 最稳 |
| GND | GND | — |
| SCL | PB10 | 与 OV7670 同一条软件 I²C 总线（模块自带 4.7 kΩ 上拉） |
| SDA | PB11 | — |
| AD0 | GND | 地址 = 0x68（接 3.3V 则 0x69） |
| INT | 不接 | S14 走轮询即可；PE0 已分给 OV7670 的 PWDN |

> **为什么不另开一条 I2C2**：PB10/PB11 本来就是 I2C2 的引脚，但本项目用软件 I²C 是为了兼容 SCCB。
> 两者地址不冲突（0x21 vs 0x68），同一时刻只有一个从机被寻址，混在一条总线上不会互相干扰；
> 而 DCMI 已经吃掉 11 个引脚，再开一条 I²C 纯属浪费。真出问题，`bsp_i2c_scan()` 一次就能看出是 0x21 没了还是 0x68 没了。

### 3.3 硬件确认结果（已核对）

| 项 | 结论 | 对应动作 |
|---|---|---|
| OV7670 是否带 AL422B FIFO | **无 FIFO** | 走 DCMI + DMA 路线，步骤 3–4 方案不变 |
| F407 侧 CAN 收发器 | **已有 TJA1050** | TXD→PD1、RXD→PD0、5V 供电；无需再买 |
| MCP2515 模块 | **已有，8 MHz 晶振** | 设备树必须写 `oscillator=8000000`；**先确认模块是 3.3V 还是 5V 版**（5V 版 SPI 要加电平转换，否则会打坏 Pi5 的 GPIO，见 `docs/wiring.md` §7） |
| 杜邦线 | **已有** | — |
| 板卡型号 | **DevEBox STM32F407ZGT6 核心板**（LQFP144，1 MB Flash，128K+64K） | HSE = 8 MHz，已实测 168 MHz 稳定 |
| 摄像头接口引脚 | **已定**（见 3.2 表 A） | D0–D7 / PCLK / VSYNC / HREF / XCLK 全部落到排针 |
| XCLK 来源 | **MCO1 (PA8) 输出 HSE 8 MHz** | 模块无晶振，必须外部送时钟 |

Pi5 侧配置（8 MHz 是硬约束，写成 16M 会让 `can0` 直接起不来）：

```ini
# /boot/firmware/config.txt 末尾
dtoverlay=mcp2515-can0,oscillator=8000000,interrupt=25
```

```bash
sudo reboot
dmesg | grep -i mcp2515                      # 应看到 mcp2515 注册成功
sudo ip link set can0 up type can bitrate 500000
ip -details link show can0                   # 确认 bitrate 500000、state ERROR-ACTIVE
```

> 无 FIFO 的 OV7670 还有个隐性坑：**PCLK 是连续输出的**，DCMI 靠 VSYNC/HREF 判帧。如果 XCLK 太高（24 MHz）而 SRAM 带宽吃紧，会出现"半帧后错位"。调试时先把 XCLK 降到 8 MHz 拿到稳定图，再逐级升。

---

## 4. 软件设计

### 4.1 视觉节点（STM32F407 + FreeRTOS）

**为什么最终用 FreeRTOS（而不是一直裸机）**

1. 项目里有 5 类不同节拍的工作：帧同步（事件驱动）、100 ms 周期 CAN、1 kHz IMU 采样、UART 流式发送、日志——裸机主循环要靠状态机硬编码这些节拍，可读性会迅速崩坏；
2. **确定性可测**：`vTaskDelayUntil` 让 CAN 周期帧的抖动可测量、可优化，这正是"实时性测量"这一步要的素材；
3. 面试关键词：任务划分、优先级设计、队列/通知/stream buffer、`FromISR`、临界区、栈水位——这些是"用过 RTOS 做工程"和"写过 RTOS demo"的区别；
4. **和你的 mini RTOS 项目形成对照**：你手写过 PendSV 切换与就绪表，现在用 FreeRTOS 的 `port.c` 能直接对上——"我实现过，所以我知道它为什么这么写"，这是很强的说服力。

> 代价：多一层间接（栈、堆、优先级）。换来的是一条能写进文档的对照结论——"同一套采集逻辑，裸机轮询 / RTOS 任务化两种组织方式的抖动与可维护性差异"。

**任务划分**

| 任务 | 优先级 | 栈 | 触发/周期 | 职责 |
|---|---|---|---|---|
| `vCanTask` | 5（最高） | 512 B | 100 ms `vTaskDelayUntil` | 状态帧、事件帧、心跳；保证周期抖动最小 |
| `vCamTask` | 4 | 512 B | DCMI 帧中断通知 | 切缓冲、启动下一帧 DMA、投递帧指针到队列 |
| `vProcTask` | 3 | 1 KB | 队列阻塞 | RGB565→灰度/二值化/质心/组帧，结果入结果队列 |
| `vImuTask` | 3（Should） | 512 B | 10 ms | 读 6 轴、校准、互补滤波，产出 0x183 数据 |
| `vLinkTask` | 2 | 768 B | stream buffer 触发 | 组帧、CRC、UART DMA 发送 |
| `vLogTask` | 1（最低） | 512 B | 事件/1 s | 串口日志、统计输出（可被任意任务抢占） |

**优先级设计理由（面试必问）**：CAN 周期帧的确定性最重要 → 最高；相机帧是硬实时（丢帧不可恢复）→ 次之；算法与 IMU 同层，靠时间片轮转；链路可以晚几十毫秒（DMA 缓冲兜着）→ 低；日志纯尽力而为 → 最低。

**同步与通信**

| 场景 | 机制 | 理由 |
|---|---|---|
| 帧中断 → `vCamTask` | `xTaskNotifyFromISR`（任务通知） | 比信号量轻，单消费者场景最快 |
| 帧指针 → `vProcTask` | 队列（长度 2） | 传递指针不做拷贝；满则丢最旧帧并计数 |
| 处理结果 → `vCanTask` | 队列（长度 4） | 解耦算法与发送 |
| UART 字节流 → `vLinkTask` | **stream buffer**（空闲中断写入） | 字节流天然适合 stream buffer，比消息队列省内存 |
| 共享状态（帧计数/配置） | 互斥量 + 结构体快照 | 配置类数据用 mutex，短临界区 |
| 中断内共享计数 | `taskENTER_CRITICAL_FROM_ISR` 或 `atomic` 计数 | 保持极短 |

**中断优先级（最容易翻车的地方）**

- `configMAX_SYSCALL_INTERRUPT_PRIORITY` 必须配成 NVIC 优先级 **5**（`5 << 4`，`configPRIO_BITS=4`）；
- **所有调用 `xxxFromISR()` 的中断，NVIC 优先级数值必须 ≥ 5**（数值越大优先级越低）；配成 0–4 的中断里调 `FromISR` 会触发断言或直接 HardFault；
- DCMI、DMA、USART、CAN 这几个中断统一设为 5–7，`SysTick`/`PendSV` 保持最低（15）；
- 调试顺序：先只开 DCMI 一个中断跑通，再加 CAN，最后加 UART——一次加一个，出问题才能二分。

**内存预算（F407 共 192 KB = 128 KB SRAM + 64 KB CCM；CCM 不能被 DMA 访问）**

| 缓冲 | 大小 | 位置 |
|---|---|---|
| QVGA RGB565 单/双缓冲 | 150 KB / 300 KB | **超预算**，不可行（已否决） |
| **QQVGA RGB565 双缓冲（160×120×2）** | 2 × 38,400 B = 75 KB | SRAM1（DMA 可访问），**采用** |
| QQVGA 灰度 / 上传缓冲 | 19,200 B | SRAM1（UART DMA 发送源，可与二值图共用） |
| QQVGA 1bpp 二值图（ROI/连通域用，可选） | 2,400 B | SRAM1 |
| 协议环形 + 队列 + 统计 | 4,096 B | SRAM1 |
| **SRAM 小计** | **102,496 B / 131,072 B** | 余 27.9 KB |
| FreeRTOS 堆（heap_4） | 24,576 B | **CCM**，`configTOTAL_HEAP_SIZE` |
| 各任务栈合计 | 6,144 B | **CCM**（或堆分配；`configCHECK_FOR_STACK_OVERFLOW=2`） |
| 日志/调试缓冲 | 4,096 B | **CCM** |

> **推荐**：OV7670 走 `COM7=0x14`（QVGA RGB565），再用 **DCW 硬件 2:1 亚抽样**直接输出 QQVGA（160×120，38,400 B/帧），DCMI+DMA 真双缓冲；软件把 R5G6B5 定点加权（`(77R+150G+29B)>>8`）成灰度 19,200 B，处理格式与上传格式是同一块缓冲。
>
> **为什么放弃 QVGA 采集**：QVGA RGB565 单帧就要 153,600 B，**超过 128 KB 的 SRAM 总量**，连单缓冲都放不下；而 QQVGA 是"采集即上传格式"，顺带省掉一次全帧软件降采样（19,200 次抽样）。
>
> **为什么不用 8 bit Bayer**：1 字节/像素只有 Bayer RAW 一条路（手册无灰度模式），而缩放模块在 DSP 通路里、RAW 会绕过 DSP，所以 Bayer 拿不到 QQVGA，只能 QVGA 满帧 76,800 B + 软件 2×2 合并，且 Bayer mosaic 会让同一灰度在 R/G/B 位置取到不同数值，二值化阈值不稳。
>
> **CCM 只能给 CPU 用，不能给 DMA**——堆和栈放 CCM，把 128 KB SRAM 全部留给图像缓冲，这是面试常考点，写进文档。
>
> **图像大缓冲必须静态分配**（`static uint8_t frame_buf[38400];` 落在 `.bss`，SRAM 里 DMA 直接可达，不用加段属性），不要从 FreeRTOS 堆里 `pvPortMalloc(150KB)`——堆碎片 + 大块分配失败是新手常见死法；`configSUPPORT_STATIC_ALLOCATION=1` 打开，任务和队列也尽量静态创建。

**模块划分**

| 模块（`node/` 下路径） | 职责 | 关键点 |
|---|---|---|
| `bsp/bsp_i2c_soft.c` + `drv/ov7670/` | SCCB 位翻转 + OV7670 寄存器配置 | 读 PID(0x0A)=0x76、VER(0x0B)=0x73 确认通信 |
| `bsp/bsp_dcmi.c` | DCMI + DMA 双缓冲、帧中断 | DMA 循环模式，半满/全满回调切换缓冲 |
| `lib/img/*.c` | RGB565→灰度、二值化、质心、组帧 | 定点运算，避免浮点；耗时用 DWT 计时 |
| `bsp/bsp_can.c` + `proto/can_msgs.h` | bxCAN 初始化、过滤器、收发 | 标准帧 11 位 ID；500 kbps；错误计数上报 |
| `drv/mpu6050/` | 读 6 轴 + 零偏校准（**Should**） | 与 OV7670 共用 PB10/PB11 的软件 I²C（地址 0x68）；采样 1 kHz |
| `lib/attitude/` | 互补滤波/一阶低通 → roll/pitch（**Should**） | 定点实现；用 DWT 时间戳做 dt，不依赖 RTOS tick |
| `proto/frame_proto.c` + `bsp/bsp_uart.c` | UART 帧协议、DMA 发送、命令解析 | CRC16-CCITT；帧头+长度+序号 |
| `bsp/bsp_dwt.c` | DWT 周期计数、帧周期统计 | 算 jitter（max/mean） |
| `os/os_task.c` + `app/app_tasks.c` | 任务创建、优先级、栈大小、同步对象 | 静态创建优先；栈水位 ≥30% 余量 |
| `app/main.c` | 时钟/外设初始化 → 建任务 → `vTaskStartScheduler()` | 调度器启动后 `main` 不再返回 |

**单帧耗时预算（160×120 = 19,200 px；33 ms/帧 @30 fps，66 ms/帧 @15 fps）**

| 阶段 | 预算 | 估算依据（168 MHz，定点） |
|---|---|---|
| DCMI 采集（硬件） | 与处理并行（DMA） | 38,400 B/帧，@15 fps 仅 576 KB/s |
| RGB565 → 灰度 | ≤ 2 ms | 19,200 px × ≈8 cycle ≈ 154 kcycle ≈ 0.9 ms |
| 二值化 | ≤ 1 ms | 19,200 px × ≈4 cycle ≈ 0.5 ms |
| 质心/连通域 | ≤ 1 ms | 单遍累加 sum_x/sum_y/count ≈ 0.7 ms |
| 组帧 + UART 发送 | ≤ 1 ms（DMA 后台发送） | 19,200 B @ 2 Mbps ≈ 96 ms 后台时间，CPU 只做 CRC（查表 ≈ 0.4 ms） |
| **合计** | **≤ 5 ms** | 占 33 ms 帧周期的 15 %，留足余量 |

### 4.2 网关驱动（树莓派 5，内核 serdev）

**技术选型**：`serdev` 驱动（挂在 UART 控制器节点下），而非 tty + 用户态解析。

| 要素 | 实现 |
|---|---|
| 设备树 | `uart2` 下加子节点 `f407-link`，`compatible = "hu,f407-link"`；`current-speed = <2000000>` |
| 驱动结构 | `struct serdev_device_driver` + `probe/remove` + `receive_buf` 回调 |
| 数据通路 | `receive_buf`（软中断/工作队列上下文）→ 帧解析状态机 → 环形缓冲 |
| 字符设备 | `/dev/frame0`：`read` 返回一帧结构体，`poll` 等待数据，`ioctl` 取统计 |
| 并发 | 环形缓冲用 `spin_lock_irqsave`；等待队列 + `wait_event_interruptible`；多读者各自游标或共享读指针（二选一，写进设计文档） |
| 错误处理 | CRC 失败计数、缓冲溢出计数、帧重同步（丢弃到下一个同步字） |

> 备选（若 serdev 调试长期没有进展）：改用 SPI 链路（F407 SPI 从机 + Pi5 SPI 主机 + 数据就绪 GPIO + `request_threaded_irq`）。能力覆盖相同，实现风险更高，仅作降级方案。

### 4.3 帧协议（F407 → Pi5，UART）

```
偏移  字段        长度   说明
0     0xAA        1      同步字 1
1     0x55        1      同步字 2
2     ver         1      协议版本 = 0x01
3     type        1      0x01 图像帧 / 0x02 统计帧 / 0x03 命令 / 0x04 应答
4     len         2      小端，payload 长度（≤ 2048）
6     seq         2      小端，帧序号（检测丢帧）
8     payload     N      —
8+N   crc16       2      小端，CRC16-CCITT(0x1021)，覆盖 ver..payload
```

- 图像帧 payload：`w(1) h(1) fmt(1) 像素数据`（QQVGA 灰度 = 160×120 = 19200 B，故 len 用 2 字节）；
- 命令帧 payload：`cmd(1) param(2)`，如 `0x01 阈值`、`0x02 曝光`；
- 重同步：收到非法同步字时逐字节滑动，直到匹配 `0xAA 0x55` 且 CRC 正确。

### 4.4 CAN 帧定义（迷你 DBC）

| CAN ID | 方向 | 周期 | 信号 |
|---|---|---|---|
| 0x180 | 节点 → 网关 | 100 ms | 帧率(×10)、处理耗时 ms、目标数、曝光、丢帧计数 |
| 0x181 | 节点 → 网关 | 事件(≤10 Hz) | 目标中心 X/Y（各 2B）、目标面积（2B）、置信度量（1B） |
| 0x182 | 网关 → 节点 | 100 ms | 网关心跳、期望帧率、运行模式 |
| 0x183 | 节点 → 网关 | 10 ms（**Should**） | roll/pitch（各 2B，0.01°）、角速度模长、温度、DWT 时间戳低位（无磁力计，yaw 不输出） |
| 0x7DF | 诊断仪 → 全网 | 事件 | 功能寻址 UDS |
| 0x7E0 / 0x7E8 | 诊断仪 ↔ 网关 | 事件 | 物理寻址 UDS 请求/响应 |

### 4.5 UDS 诊断（网关侧）

| 服务 | 作用 | 本项目用法 |
|---|---|---|
| 0x10 | 会话控制 | 01 默认 / 03 扩展 |
| 0x22 | 读 DID | 0x0100 帧率、0x0101 曝光、0x0102 阈值、0x0103 目标坐标、0xF186 会话、0xF190 VIN |
| 0x2E | 写 DID | 写 0x0102 阈值 → UART 命令帧下发 → 节点生效（**核心亮点**） |
| 0x27 | 安全访问 | seed/key：扩展会话 + 正确 key 才允许 0x2E |
| 0x3E | 保持连接 | 维持非默认会话 |
| 0x7F | 否定响应 | NRC：0x11/0x12/0x13/0x22/0x31/0x33/0x35/0x78 |

### 4.6 功能范围与优先级（防蔓延）

| 优先级 | 功能 | 说明 |
|---|---|---|
| **Must** | **FreeRTOS 移植与任务化（6 个任务 + 优先级设计）**、DCMI+DMA 采集、灰度/二值化/质心、CAN 状态帧与结果帧、UART 帧协议、serdev 驱动、环形缓冲+poll、网关转发、实时性报告、README | 不做完这些，项目讲不完整 |
| **Should** | UDS（0x10/0x22/0x27/0x3E + 0x2E 下发）、**MPU6050 姿态通道（0x183 + 互补滤波）**、collector（CSV/统计）、kmemleak 验证 | 决定"能讲多深"，主线稳了就补 |
| **Could** | UDS 多帧（ISO-TP FF/CF/FC）、CAN↔UDP 转发、IIO 子系统、FFT 频谱、逻辑分析仪自动化测量 | 有余力再做 |
| **Won't** | 多传感器融合、边缘推理、Web 界面、云端、AUTOSAR 工程实操 | 明确不做，避免分散 |

**MPU6050 的取舍（重要）**：它是**锦上添花，不是主线**。价值在于"多传感器 + 一阶滤波/互补滤波 + 共享时间戳"，能撑起一句面试回答；风险在于多一条 I2C 总线、一套零偏校准、一个额外的 CAN 帧。

因此给它定死三条规则：

1. **主线 Must 全部跑通、且总线/驱动部分没有遗留问题后才做**（顺序上排在步骤 14）；
2. 做就做最小闭环：读 6 轴 → 零偏校准 → 互补滤波出 roll/pitch → 0x183 帧，**不做** DMP、不做卡尔曼、不做磁力计；
3. 一旦中途受阻且无法快速二分定位，立即回滚（代码留在 `feat/mpu6050` 分支，不合并 `main`）。

---

## 5. 产出清单

| 类别 | 交付物 | 位置 | 验收 |
|---|---|---|---|
| 代码 | 视觉节点固件（F407） | `node/` | 可独立烧录运行，串口输出日志 |
| 代码 | Pi5 内核驱动 | `driver/` | `insmod` 出 `/dev/frame0`，`poll` 可用 |
| 代码 | 网关 + UDS 服务 | `gateway/` | 可收发 CAN、可被诊断脚本驱动 |
| 代码 | 测试脚本 | `tests/` | 压测、CRC 校验、UDS 回归 |
| 文档 | 架构设计 | `docs/design.md` | 架构图 + 数据流 + 决策记录 |
| 文档 | 协议规范 | `docs/protocol.md` | 帧格式 + CAN 矩阵（DBC 手工版） |
| 文档 | 诊断规范 | `docs/uds.md` | 服务/DID/NRC 表 |
| 文档 | 性能与实时性报告 | `docs/perf.md` | 指标表 + 测量方法 + 结论 |
| 文档 | 调试与踩坑记录 | `docs/debug-log.md` | 至少 5 个真实问题 |
| 展示 | 2 篇技术博客 | 知乎/CSDN/个人站 | 图像通路 + 驱动/并发 各一篇 |
| 展示 | 3 分钟演示视频 | — | 能看清"采集→CAN→诊断下发"全链路 |
| 展示 | 简历 bullet（军工版/车载版） | 附录 C | 每条带真实数字 |
| 能力 | 44 问答案 | 附录 D | 脱稿讲 30 分钟 |

---

## 6. 实施步骤（按任务顺序，不绑日历）

### 6.1 阶段划分

| 阶段 | 步骤 | 主题 | 阶段完成判据 |
|---|---|---|---|
| P1 | S1–S2 | 骨架与硬件通路 | 双端"hello"、`can0` 起来、SCCB/DCMI 能读到 ID |
| P2 | S3–S5 | 图像采集与处理 | 稳定 ≥15 fps、输出目标坐标、单帧处理 ≤20 ms |
| P3 | S6–S8 | CAN 与上行链路 | 双节点 CAN 互通、Pi5 收到图像并落盘 |
| P4 | S9–S10 | Linux 驱动 | `/dev/frame0` 可读、压测稳定 |
| P5 | S11–S13 | 网关、实时性、诊断 | 网关转发、实时性对比表、UDS 端到端 |
| P6 | S14–S15 | 可选增强与收口 | v1.0 + 文档/博客/视频，能讲 30 分钟 |

### 6.2 步骤明细

| 步骤 | 目标 | 主要任务 | 完成判据 |
|---|---|---|---|
| **S1** | 工程骨架与双端环境 | 交叉编译链与 Makefile、FreeRTOS 移植（先只验证"两个任务点灯 + 串口"）、Pi5 侧 hello 模块 `insmod`/`rmmod`；README、`.gitignore`、分支与提交规范 | 两个任务能跑；模块能装卸；首个 commit |
| **S2** | 三条通路先通，再写逻辑 | MCP2515 overlay（`oscillator=8000000`）+ `ip link` + `candump` 自发自收；TJA1050 接线与电平自检；软件 I²C 读 MPU6050 `WHO_AM_I`（与 OV7670 共用总线）；OV7670 供电、XCLK、PCLK 波形确认 | `can0` ERROR-ACTIVE；读出 0x68；PCLK 有波形 |
| **S3** | 图像第一帧 | GPIO 模拟 SCCB 读 `0x0A`/`0x0B`；下初始化序列；DCMI 单帧抓取；串口打印像素 min/max/mean | 读到 0x76/0x73；统计值合理（非全 0/全 255） |
| **S4** | 连续采集 | DCMI+DMA 循环模式、半满/全满中断、缓冲切换；帧率与丢帧统计；RGB565→灰度定点转换 | QQVGA 稳定 ≥15 fps（目标 30），1 h 丢帧 <1 % |
| **S5** | 出目标坐标 | 可配阈值二值化、质心/面积、ROI 裁剪、DWT 计时、ASCII 可视化 | 串口输出坐标；单帧处理 ≤20 ms |
| **S6** | 把主循环拆成任务 | 6 个任务 + 优先级 + 任务通知/队列/stream buffer；中断优先级阈值配置；栈水位统计 | 任务化后 S4/S5 指标不回退；无栈溢出 |
| **S7** | 元数据上总线 | bxCAN 初始化/过滤器/位定时；100 ms 周期状态帧；错误计数上报 | `candump` 看到 0x180 周期帧，p99 抖动 ≤2 ms |
| **S8** | 图像上链路 | 帧协议编解码 + CRC16 + DMA 发送 + 空闲中断接收；QQVGA 灰度组帧；Pi5 Python 收帧存 PNG | Pi5 上打开 PNG 与节点画面一致 |
| **S9** | 内核收帧 | DT overlay（`uart2` + `f407-link`）、`probe`、`receive_buf`、帧解析、字符设备 `read` | `/dev/frame0` 读到完整帧 |
| **S10** | 并发与稳定性 | 环形缓冲 + 等待队列 + `poll` + `ioctl` 统计；多读者游标设计；`lockdep`/`kmemleak` | 4 线程压测 1 h：0 oops、0 丢帧、无泄漏 |
| **S11** | 打通双总线 | SocketCAN 接收 + 解析 + 聚合转发；接收过滤器；bus-off 自动恢复 | 网关进程稳定运行 + 总线负载表 |
| **S12** | 拿实时性数据 | F407：tick 驱动 vs 硬件定时器触发对比；Pi5：`cyclictest` / `ftrace irqsoff`；端到端延迟 | 实时性对比表 + 抖动源定位结论 |
| **S13** | 诊断闭环 | UDS 0x10/0x22/0x27/0x3E；0x2E 写阈值 → UART 命令 → 节点生效；`python-can` 回归脚本 | 诊断脚本端到端跑通，0x2E 修改真实生效 |
| **S14** | 多传感器（可选） | 软件 I²C 读 6 轴、零偏校准、互补滤波、0x183 帧 | roll/pitch 合理，静止不漂 |
| **S15** | 收口 | 文档（design/protocol/uds/perf/debug-log）、CI + `checkpatch` + 单测、博客 2 篇、3 分钟视频、简历 bullet、44 问 | v1.0 release；脱稿讲 30 分钟 |

> **为什么 S6 才任务化**：S3–S5 阶段采集逻辑先用裸机主循环写，是为了让"OV7670 能不能出图"这个问题尽早、单独地暴露出来；同时调图像链路和 RTOS，出问题时无法二分。S6 再把已经验证过的逻辑搬进任务——**一次只引入一个新变量**。

### 6.3 依赖与降级规则

1. **S2 不通过不进 S3**：总线、时钟、供电问题会伪装成"驱动 bug"，先证明硬件通路是通的；
2. **S4 帧率不达标不进 S5**：处理再快也补不回采集，先把采集做稳；
3. **S10 压测不过不进 S11**：驱动不稳，上层全白做；
4. **S13 之前不碰 S14**：UDS 是硬需求，姿态是加分项；
5. 任一步骤受阻且无法二分定位时，先按 §8 的对应降级方案处理；仍不行就退回到"上一个已通过判据"的步骤重做，不要带病往下堆；
6. 每次降级都写进 `docs/debug-log.md`——**降级过程本身就是面试素材**（"当时为什么放弃 A 选 B"）。

---

## 7. 验收标准

### 7.1 功能

- [ ] 拔掉摄像头，节点不崩（错误路径有提示）；
- [ ] 改 F407 阈值参数，二值化结果随之变化；
- [ ] 拔掉 CAN 线，网关在 1 s 内检测并记录，插回后自动恢复；
- [ ] 诊断仪断电重连后仍能建立会话（会话超时逻辑正确）；
- [ ] `rmmod` 驱动后 `/dev/frame0` 消失，再次 `insmod` 可正常使用。

### 7.2 性能

- [ ] 采集帧率 ≥15 fps，连续 1 h 丢帧率 < 1 %；
- [ ] 单帧处理 ≤20 ms；
- [ ] 帧周期 p99 抖动 ≤2 ms；
- [ ] CAN 负载 ≤30 %，1 h 无错误帧累积；
- [ ] 驱动→用户态延迟 p99 ≤1 ms；
- [ ] 1 h 连续运行无 oops、`kmemleak` 无增长。

### 7.3 质量

- [ ] `-Wall -Wextra -Werror` 零警告（用户态与固件）；
- [ ] 内核代码过 `checkpatch.pl --strict`；
- [ ] `configCHECK_FOR_STACK_OVERFLOW=2` 打开，1 h 运行无栈溢出触发；
- [ ] 所有任务 `uxTaskGetStackHighWaterMark` 余量 ≥30%，并记录到文档；
- [ ] CI 能跑通构建 + 单元测试（CRC、协议解析、CAN 打包）；
- [ ] README 让别人能从零复现；
- [ ] commit 历史体现真实迭代（按步骤推进，不要攒成一次性大提交）；

### 7.4 面试

- [ ] 能画架构图并讲清每个决策的取舍；
- [ ] 能回答附录 D 的 44 问；
- [ ] 能现场改一个 bug（例如把 CRC 校验改错，让你定位）。

---

## 8. 风险与应对

| # | 风险 | 概率 | 影响 | 应对 |
|---|---|---|---|---|
| 1 | 无 FIFO 版 OV7670 首帧抓不到 | 中 | 高 | XCLK 先降到 8 MHz；逻辑分析仪看 PCLK/VSYNC/HREF 有没有波形；先用 QQVGA 抓通再放大 |
| 2 | SCCB 配置不生效（读不到 0x76） | 中 | 高 | 先用 GPIO 模拟 I2C + 逻辑分析仪看波形；确认 0x21 地址与上拉电阻 |
| 3 | 双缓冲内存不够 | **已关闭** | 低 | 改采 QQVGA（160×120，38,400 B/帧）+ DCW 硬件 2:1，真双缓冲共 75 KB，稳在 128 KB SRAM 内 |
| 4 | CCM 被误用给 DMA | 中 | 高 | 链接脚本显式把 DMA 缓冲放 SRAM1/2；文档写明 |
| 5 | UART 2 Mbps 丢字节 | 中 | 中 | 降 1 Mbps 先跑通；加 DMA + 空闲中断；协议层加重同步 |
| 6 | serdev 驱动调试卡住 | 中 | 高 | 若排查多轮仍无进展，切 SPI 链路方案（附录 B 有备选设计） |
| 7 | MCP2515 起不来 | 中 | 中 | 8 MHz 已确认：overlay 必须是 `oscillator=8000000`；INT 接 GPIO25；先 loopback 再双节点 |
| 8 | CAN 双节点不通 | 中 | 中 | 交叉接线、终端电阻、bitrate 一致、F407 先 loopback 自测 |
| 9 | FreeRTOS 中断优先级配错（`FromISR` 在高于 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的中断里调用） | **高** | 高 | 先把 NVIC 分组设成 4 位优先级、syscall 阈值 = 5；所有 `FromISR` 中断统一 5–7；用 `configASSERT` 抓 |
| 10 | 栈溢出（图像处理函数局部数组大） | 中 | 高 | 大数组一律 static 或放帧缓冲；开 `configCHECK_FOR_STACK_OVERFLOW=2`；看高水位 |
| 11 | FreeRTOS 堆不够 / 碎片 | 中 | 中 | heap_4 + 静态创建任务与队列；大缓冲静态分配，绝不 `pvPortMalloc(150KB)` |
| 12 | 早期就追求"功能全"（并行做多件事） | 高 | 高 | 严格按 S1→S15 顺序推进；S10 之前不加 UDS，S13 之前不碰 MPU6050 |

---

## 9. 工程化与质量

| 项 | 规范 |
|---|---|
| 分支 | `main` 保护；功能用 `feat/xxx`，修 bug 用 `fix/xxx` |
| 提交 | `feat: / fix: / docs: / test: / perf:` 前缀，一条 commit 一件事 |
| 代码风格 | 内核侧遵守内核编码风格（`checkpatch`）；固件侧统一 4 空格 + 命名前缀 |
| 测试 | 用户态单元测试（CRC、协议解析、CAN 打包）；集成脚本（压测、UDS 回归） |
| CI | GitHub Actions：交叉编译（`arm-none-eabi-gcc`）+ 主机侧编译 + 单测 + `checkpatch` |
| 文档 | 设计文档写"为什么"，不写"是什么"；每个决策一段"取舍" |
| 版本 | `v0.1`（S5 完成：能采能算）、`v0.5`（S10 完成：驱动可用）、`v1.0`（S15 完成），打 tag 发 release |

---

## 10. 后续扩展方向

1. **视觉节点升级**：换 OV5640（DCMI + JPEG）或加 CSI 摄像头到 Pi5，做双路融合；
2. **总线升级**：CAN FD（MCP2517FD）、车载以太网（SOME/IP 最小实现）；
3. **算法升级**：Sobel 边缘 + 连通域标记 + 简单跟踪（帧间关联）；或 FFT 振动/纹理分析（贴 14 所信号处理）；
4. **诊断升级**：DTC 故障码管理、0x19 读故障、0x31 例程控制、刷写（0x34/0x36/0x37）；
5. **安全**：Secure Boot + 固件签名（复用你 Bootloader 项目）；
6. **可延伸的课题**：*"面向车载/机载的视觉节点与实时网关设计与实现"*——正好覆盖上面 1–4 中的两项。

---

## 附录 A：关键参数速查

**OV7670（SCCB 地址 0x21）**

| 寄存器 | 地址 | 用途 |
|---|---|---|
| PID | 0x0A | 应为 0x76 |
| VER | 0x0B | 应为 0x73 |
| COM7 | 0x12 | 复位/输出格式选择：`QVGA 0x10`、`CIF 0x20`、`QCIF 0x08`、`RGB 0x04`、`YUV 0x00`、`Bayer RAW 0x01` |
| CLKRC | 0x11 | 内部时钟分频 |
| COM3 | 0x0C | 位[3] 缩放使能 / 位[2] DCW 使能 |
| COM14 | 0x3E | 位[4] DCW&PCLK 使能 / 位[3] 手动缩放 / 位[2:0] PCLK 分频 |
| COM15 | 0x40 | 位[5:4]=`01` → RGB565（默认 0xC0 = 全范围 00–FF） |
| SCALING_XSC/YSC | 0x70/0x71 | 水平/竖直缩放系数；位[7] 测试图案 |
| SCALING_DCWCTR | 0x72 | 水平/竖直亚抽样率：`01`=2取1、`10`=4取1、`11`=8取1（默认 0x11，但 DCW 默认关） |
| SCALING_PCLK_DIV | 0x73 | DSP 缩放时钟分频，须与 COM14[2:0] 同值 |
| AGC/AEC | 0x00–0x10 | 自动增益/曝光 |

> **QQVGA 关键配置（本项目采用）**：`{0x12,0x14}` QVGA+RGB565 → `{0x40,0xD0}` RGB565+全范围 → `{0x0C,0x0C}` 缩放+DCW 使能 → `{0x3E,0x19}` DCW&PCLK 使能+手动缩放+PCLK/2 → `{0x72,0x11}` 水平/竖直各 2:1 → `{0x73,0x01}` 缩放时钟/2。
>
> **手册陷阱**：数据手册 p13 的 COM7 格式表把 RGB 和 Bayer RAW 的 `COM7[2]/COM7[0]` 印反了（同一份手册 p18 的 COM15 位[5:4] 明确写"RGB555/565 在 `COM7[2]=1` 且 `COM7[0]=0` 时有效"）。以 **RGB=0x04、Bayer RAW=0x01** 为准，与 Linux mainline `ov7670.c` 一致。面试可讲"我用两处交叉引用 + 驱动源码确认了手册笔误"。
>
> **S3 调试技巧**：`COM7[1]` 与 `SCALING_XSC[7]/YSC[7]` 可开测试图案（八色条/渐变灰条/移位1）。调 DCMI+DMA 时先开彩条，不依赖镜头/光照/曝光就能验证 PCLK/HREF/VSYNC 时序和字节序，通了再关掉抓真实画面——正好应对"无 FIFO 版首帧抓不到"的风险。

**MPU6050（I2C 地址 0x68，AD0 接地）**

| 寄存器 | 地址 | 用途 |
|---|---|---|
| WHO_AM_I | 0x75 | 应为 0x68 |
| PWR_MGMT_1 | 0x6B | 0x80 复位，0x01 用 X 轴陀螺时钟唤醒 |
| SMPLRT_DIV | 0x19 | 采样率分频（1 kHz / (1+div)） |
| CONFIG | 0x1A | DLPF 数字低通（滤波带宽） |
| GYRO_CONFIG | 0x1B | ±250/500/1000/2000 dps |
| ACCEL_CONFIG | 0x1C | ±2/4/8/16 g |
| ACCEL_XOUT_H | 0x3B | 连续 14 字节：加速度 6 + 温度 2 + 陀螺 6 |
| INT_ENABLE | 0x38 | 数据就绪中断（bit0） |

> 读数换算（±2 g / ±250 dps）：`acc_g = raw / 16384.0`，`gyro_dps = raw / 131.0`。零偏校准的做法：静止 2 s 采 500 组求均值，作为 `gyro_bias` 在后续读数里减掉——**不做这一步，积分 10 s 就能漂出几度**，面试问"你的姿态怎么保证精度"时这就是答案之一。

**F407 关键点**

| 项 | 值 |
|---|---|
| SRAM | 112 KB + 16 KB + CCM 64 KB（**CCM 不可 DMA**） |
| DCMI | 8 位并口，DMA 双缓冲，帧/行中断 |
| bxCAN | 2.0B，标准帧 11 位，500 kbps |
| USART | 2 Mbps（8N1），DMA 发送 + 空闲中断接收 |
| 计时 | DWT->CYCCNT / 168 MHz |

**Pi5 关键点**

| 项 | 值 |
|---|---|
| SPI0 | GPIO7–11（CE1/CE0/MISO/MOSI/SCLK），pin 26/24/21/19/23 |
| uart2 | GPIO4/5（pin 7/29），避开 console |
| MCP2515 INT | GPIO25（pin 22） |
| MCP2515 晶振 | 8 MHz → overlay 写 `oscillator=8000000` |
| 内核头 | `/lib/modules/$(uname -r)/build`，包名可能是 `linux-headers-rpi-2712` |

---

## 附录 B：仓库结构

> **分层原则**：上层可以 include 下层，**下层永远不知道上层存在**。目录只是结果，依赖方向才是本质。

```
vision-node-gateway/
├── README.md
├── docs/{design,protocol,uds,perf,debug-log}.md
├── shared/proto/            # ★ 协议单源：frame.h（固件+内核驱动共用）、frame.py、golden_vectors.txt
├── node/                    # STM32F407 固件（S1 建立，见 B.1）
├── driver/                  # Pi5 内核驱动（S9 再建）
├── gateway/                 # 用户态网关（S11 再建）
├── tools/                   # can_decode.py / uds_client.py / show_frame.py
├── tests/                   # test_crc.c / test_protocol.c / stress.sh
└── .github/workflows/build.yml
```

### B.0 技术选型：CMSIS + 裸寄存器

**不用 ST HAL，也不用标准库（SPL），也不用 LL。** 工程里第三方 `.c` 文件数为 **0**，只引 8 个声明性质的芯片描述头（`core_cm4.h`、`stm32f4xx.h`、`stm32f407xx.h` 等，放在 `arch/cmsis/`）。

三条理由：

1. **"全 LL" 在本项目其实不成立。** ST 官方 `stm32f4xx-hal-driver/Inc/` 里 F4 的 LL 头只有 26 个（`adc bus cortex crc dac dma dma2d exti fmc fmpi2c fsmc gpio i2c iwdg lptim pwr rcc rng rtc sdmmc spi system tim usart usb utils wwdg`），**没有 `stm32f4xx_ll_can.h`，也没有 `stm32f4xx_ll_dcmi.h`**——而这两个正是本项目的核心外设。所以"LL 有的用 LL、没有的裸写"必然变成两套风格混用，比统一裸写更难维护。
2. **避开 HAL + FreeRTOS 的经典冲突。** `HAL_Init()` 把 SysTick 配成 1 ms 并让 `HAL_Delay()` 轮询 `uwTick`；FreeRTOS 同样以 SysTick 为心跳、还把它压到最低优先级。调度器启动后 `HAL_Delay()` 会漂移甚至永不返回。裸寄存器版本里延时只有 `vTaskDelay`（任务内）和 DWT 忙等（临界区内）两条确定路径。
3. **DCMI 的寄存器本来就少**：`DCMI_CR` / `DCMI_IER` / `DCMI_SR` 三个，加 `DMA2_Stream1/7` 的十余个字段。HAL 的 `HAL_DCMI_Init()` 反而把"到底写了哪一位"藏起来了。

**代价与对策**：

| 风险 | 对策 |
|---|---|
| CAN 位定时算错（现象是"完全没波形"） | 先把 `tq / BS1 / BS2 / SJW / prescaler` 的式子写在 `docs/design.md` 再写代码；F407 先 loopback 自测（S7 已有此判据） |
| UART 分数波特率 | 2 Mbps @ 168 MHz 恰好整除（`USARTDIV = 5.25`，`BRR = 0x54`）；115200 带小数，用 `BRR` 的 `DIV_Fraction` 算准 |
| 没有库兜底，配错了没提示 | 每段 `bsp_*_init()` 末尾**回读关键寄存器并打印**；调试器开外设视图（Cortex-Debug / SVD） |
| 参考实现缺失 | 用 CubeMX 生成一份 HAL 工程**只当参考答案**（看它最后写到哪个寄存器位），代码不抄进仓库 |

### B.1 固件分层（`node/`）

```
node/
├── arch/                    # 换 MCU 才动（三个文件都已写）
│   ├── startup_stm32f407xx.s   # 98 项向量表 + .data/.ccmram 拷贝 + .bss 清零
│   ├── stm32f407.ld         # FLASH 1M / RAM 128K / CCM 64K；段：.isr_vector/.data/.bss(SRAM)、.ccmram(CCM，DMA 不可达)
│   ├── system_stm32f4xx.c   # 自己写：HSE 8 MHz ×N/P → 168 MHz、APB1 42/APB2 84、开 FPU、HSE 失败退 HSI
│   └── cmsis/               # 8 个芯片描述头（只有 .h，见 B.0）
├── bsp/                     # 回答"这颗料接哪根管脚、哪个 DMA、多少时钟"
│   ├── board.c/.h           # board_init()
│   ├── bsp_uart.c           # 2 Mbps 图像口
│   ├── bsp_can.c            # bxCAN
│   ├── bsp_i2c_soft.c       # SCCB 位翻转
│   ├── bsp_dcmi.c           # DCMI + DMA 双缓冲 + 中断
│   └── bsp_dwt.c            # 周期计数
├── drv/ov7670/              # 回答"这个器件怎么用"
│   ├── ov7670.c/.h          # 写序列、读 PID/VER、切格式
│   └── ov7670_regs.c        # QQVGA 寄存器表 + DCW 表
├── os/                      # 唯一允许出现 FreeRTOS 头的目录
│   ├── freertos/            # FreeRTOSConfig.h + 内核 + port
│   ├── os_task.c/.h         # os_task_create / os_notify_from_isr …
│   ├── os_queue.c/.h
│   └── isr_bridge.c         # 中断 → 任务的唯一桥
├── lib/                     # 零硬件依赖，PC 上可编译
│   ├── img/                 # rgb565_gray.c / binarize.c / centroid.c / roi.c
│   ├── ringbuf/
│   └── crc/                 # crc16_ccitt 查表
├── proto/                   # 0xAA55 帧编解码、0x180/0x181 报文布局
├── app/                     # 业务编排
│   ├── main.c
│   ├── app_tasks.c          # 6 个任务体
│   ├── app_capture.c
│   ├── app_config.c/.h      # UDS 0x2E 写的就是这里
│   └── app_state.c
├── config/                  # board_config.h（引脚/波特率/NVIC 优先级）、app_config.h（默认阈值/栈深）
├── test/                    # host-native 单测
├── scripts/                 # openocd.cfg、烧写、抓帧
└── Makefile
```

`lib/attitude/`、`drv/mpu6050/` 到 S14 再建；Pi5 那三棵树到 S9/S11 再建。

### B.2 依赖方向

| 层 | 可以 include | 绝对不允许 |
|---|---|---|
| `app` | 全部 | — |
| `os` | `arch`、`config` | `drv`、`bsp`、`app` |
| `proto` | `lib`、`shared/proto` | 外设、OS、板级 |
| `lib` | **无项目头**（只有 `<stdint.h>` 这类标准头） | 其余全部 |
| `drv` | `bsp`、`lib`、`config` | **RTOS**、`app` |
| `bsp` | `arch`（含 `arch/cmsis/`）、`config` | RTOS、`app` |
| `arch` | `arch/cmsis/` | 其余全部 |

### B.3 四条硬规矩

1. **`drv/` 不出现 FreeRTOS 头文件**：驱动只返回状态码或注册回调，中断到任务的翻译统一放 `os/isr_bridge.c`。这样驱动可单测、可换 RTOS。
2. **`lib/` 与 `proto/` 不出现外设头文件**：`make test` 用主机 gcc 编译它们，偷偷 include `stm32f4xx.h` 立刻红——**单测就是分层的守卫**。
3. **跨层 include 带路径前缀**：`#include "bsp/bsp_uart.h"`。`-I` 由 Makefile 按层给，越界编不过。
4. **公共头与私有头分开**：`ov7670.h` 对外，`ov7670_priv.h` 只在本目录用。

### B.4 模块搬迁对照

| 原 `src/` 平铺方案 | 新位置 | 理由 |
|---|---|---|
| `sccb.c` | `bsp/bsp_i2c_soft.c` + `drv/ov7670/ov7670.c` | 位翻转是总线，寄存器序列是器件 |
| `camera.c` | `bsp/bsp_dcmi.c` | DCMI/DMA/中断全是外设资源 |
| `imgproc.c` | `lib/img/*.c` | 纯算法，PC 可单测 |
| `can_node.c` | `bsp/bsp_can.c` + `proto/can_msgs.h` | 外设 vs 报文布局 |
| `link.c` | `proto/frame_proto.c` + `bsp/bsp_uart.c` | 协议 vs 串口 |
| `timing.c` | `bsp/bsp_dwt.c` | |
| `mpu6050.c` / `attitude.c` | `drv/mpu6050/` / `lib/attitude/` | S14 |
| `tasks.c` | `app/app_tasks.c` + `os/os_task.c` | 任务编排 ≠ API 封装 |
| `main.c` | `app/main.c` | |

### B.5 不要过度分层

`middleware/`、`services/`、`domain/` 在节点侧没有第二个实现，不建。判据：**一个目录只有 1 个文件且 foreseeable 不会再长，就先别建**；层数 ≤ 6。只因「以后可能换芯片」就加一层抽象，是负债不是资产。

**SPI 链路备选方案**（仅当 serdev 卡住时启用）：F407 SPI 从机（`SPI_NSS` 硬件从机模式）+ 数据就绪 GPIO（F407 拉高 → Pi5 `request_threaded_irq`）+ Pi5 `spi_sync` 读帧。驱动结构与 serdev 版一致，仅替换传输层。

---

## 附录 C：简历 bullet（数字必须自测，`<X>` 为占位）

**军工版（14 所）**

> **智能视觉节点与实时网关（STM32F407 + FreeRTOS + 树莓派 5）** ｜ 个人项目 ｜ C / RTOS / Linux 内核 / 实时性
> - 在 STM32F407 上基于 FreeRTOS 划分 6 个任务（CAN 周期帧最高优先级、采集/处理/链路/日志分级），用任务通知 + stream buffer + 队列实现中断与任务间解耦；实现 OV7670 采集通路（SCCB + DCMI + DMA 双缓冲 + DCW 硬件 2:1 降采样），QQVGA RGB565 稳定 `<X>` fps，连续 1 h 丢帧率 `<Y>`%，用 DWT 测得单帧处理耗时 `<Z>` ms；
> - 实现灰度化 + 可配阈值二值化 + 质心定位，单帧定位 `<A>` ms，通过 500 kbps CAN 周期帧输出目标坐标与状态，总线负载 `<B>`%；
> - 在树莓派 5 上编写 serdev 内核驱动（设备树 + 字符设备 + 环形缓冲 + 等待队列 + poll），多进程并发读取，4 线程压测 `<N>` 小时 0 oops、0 丢帧；
> - 用 `cyclictest`/`ftrace irqsoff` 量化端到端确定性：p99 `<C>` µs，定位 `<D>` 为主要抖动源，输出实时性报告与架构文档。

**车载版**

> **基于 STM32F407（FreeRTOS）+ 树莓派 5 的视觉节点与 CAN 网关** ｜ 个人项目 ｜ C / RTOS / Linux / CAN / UDS
> - 视觉节点：FreeRTOS 6 任务（优先级设计 + 任务通知/队列/stream buffer），OV7670（SCCB 配置）+ DCMI + DMA 双缓冲采集，QQVGA 灰度经自定义帧协议（帧头+长度+序号+CRC16）上传，2 Mbps 下 `<X>` fps；
> - 双节点 CAN 网络（F407 bxCAN ↔ Pi5 MCP2515）：按迷你 DBC 定义 0x180 状态帧与 0x181 事件帧，实现接收过滤与 bus-off 自动恢复；
> - Pi5 端 CAN 网关 + UDS server（ISO 14229：0x10/0x22/0x27/0x2E/0x3E，ISO-TP 单帧），支持通过 0x2E 远程下发二值化阈值到视觉节点并生效；
> - 输出设计文档、诊断规范、性能报告，`python-can` 自动化回归 `<N>` 个用例。

---

## 附录 D：面试 44 问

**图像与实时性（1–10）**
1. OV7670 的 PCLK/VSYNC/HREF 各自作用？一帧的时序是怎样的？
2. DCMI 的 DMA 双缓冲怎么工作？半满/全满中断各做什么？
3. 为什么 DMA 缓冲不能放 CCM RAM？
4. 一帧 QVGA RGB565 多少字节？15 fps 需要多少带宽？
5. 为什么图像不走 CAN？算一下 500 kbps 传一帧要多久。
6. 双缓冲和单缓冲的区别？单缓冲在什么时候丢帧？
7. 灰度化用定点怎么算？为什么避免浮点？
8. 质心定位的原理？目标不连续/多个目标时怎么办？
9. 帧周期 jitter 怎么测？DWT 计数的精度和溢出怎么处理？
10. 如果把帧率提到 30 fps，瓶颈在哪，怎么优化？

**CAN 与诊断（11–20）**
11. CAN 帧结构？标准帧与扩展帧的区别？
12. 仲裁机制为什么不会冲突？优先级怎么体现？
13. 位定时怎么算？采样点为什么重要？
14. 终端电阻为什么是 120 Ω？
15. bus-off 怎么产生？怎么恢复？
16. 过滤器（mask/list）怎么配？和 CPU 负载的关系？
17. UDS 的会话有哪几种？0x10 的作用？
18. 0x27 安全访问的 seed/key 流程？
19. 0x2E 写 DID 为什么必须先过安全访问？
20. ISO-TP 的 SF/FF/CF/FC 分别是什么？NRC 0x78 干什么用？

**Linux 驱动（21–32）**
21. serdev 驱动和 tty 用户态读串口，取舍是什么？
22. `receive_buf` 在什么上下文执行？能睡吗？
23. 设备树怎么匹配到你的驱动？probe 时机？
24. `copy_to_user` 为什么不能睡在原子上下文？返回值怎么处理？
25. 环形缓冲生产者/消费者怎么同步？`spin_lock_irqsave` 保护什么？
26. 等待队列 + `poll` 的完整流程？
27. 多进程同时 `read`，你的游标怎么设计？
28. 帧重同步怎么做？CRC 检测到错误后丢多少字节？
29. `kmemleak`/`lockdep` 各查什么？
30. `rmmod` 时资源释放顺序？
31. 驱动里出现 oops，你怎么定位到行？
32. 如果要支持 4 路这样的链路，架构怎么改？

**工程与综合（33–40）**
33. 为什么用 MCU 采集、Linux 做网关？反过来行不行？
34. 你的帧协议为什么这样设计？和 Modbus/SLIP 比有什么取舍？
35. 项目最大的 bug 是什么？怎么定位的？
36. 连续跑 1 小时怎么验证没有内存泄漏？
37. 别人怎么复现你的项目？README 里哪些是必须的？
38. 如果换成 OV5640（JPEG 输出），你的采集链路要改哪些地方？
39. 你了解 14 所/车载域控做什么吗？说 2–3 个具体点。
40. 这个项目里你最想继续深入的是哪一层？为什么？

**FreeRTOS（41–44）**
41. 你的 6 个任务优先级怎么定的？把日志任务调到最高会发生什么？
42. `xTaskNotifyFromISR` 和信号量、事件组、stream buffer 分别适合什么场景？你为什么这么选？
43. `configMAX_SYSCALL_INTERRUPT_PRIORITY` 是什么？配错了会怎样？你的 NVIC 分组是怎么设的？
44. 你写过 mini RTOS，FreeRTOS 的 `port.c` 和你实现的有哪些异同（PendSV、就绪表、优先级位图、临界区）？

**参考方向（附录 E）**：14 所笔试偏 C/数据结构/OS/信号处理；车载面试偏 CAN/UDS/AUTOSAR 概念。项目覆盖了大部分，剩下的是概念阅读。

---

## 附录 E：概念阅读清单（只求不空白，不深挖）

| 主题 | 读什么 | 目标 |
|---|---|---|
| AUTOSAR Classic | 分层（MCAL/BSW/RTE/ASW）白皮书 | 能画图讲清各层职责 |
| ISO 26262 | ASIL 分级、安全机制概念 | 能说清 ASIL A–D 差异 |
| 车规 MCU | TC3xx（TriCore）/S32K/RH850 概览 | 能说出与 Cortex-M 的差异 |
| 1553B / PCIe / RapidIO | 各自速率、拓扑、典型场景 | 面试被问总线时不空白 |
| 军标 GJB / 国产化 | GJB 概述、天脉/ReWorks/麒麟 | 知道军工软件生态名词 |
| PREEMPT_RT | 原理与代价（吞吐 vs 确定性） | 能解释为什么实时性要付出代价 |

---

> **一句话**：这个项目不是"又一个采集 demo"，而是一条**从像素到总线、从 RTOS 到内核、从数据到诊断**的完整链路。面试官随便挑一层追问，你都能往下讲三层——这就是秋招时你缺的东西。
