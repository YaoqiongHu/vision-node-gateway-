# 硬件：DevEBox STM32F407Z 开发板

> 数据来源：开发板原理图 `stm32f407f.pdf`（Altium，2 页）+ ST-Link 线上实测。
> 换板子时**只需要改 `node/config/board_config.h`**，这份文档跟着改。

## 1. 芯片

| | |
|---|---|
| 型号 | **STM32F407ZGT6**（LQFP144） |
| 内核 | Cortex-M4F @ 168 MHz，带 FPU（单精度） |
| Flash | **1 MB**（`0x1FFF7A22` 读出 `0x0400`，与 `DBGMCU_IDCODE=0x101F6413` 的 DEV_ID `0x413` 一致） |
| SRAM | 112 KB + 16 KB = **128 KB**（`0x2000_0000`，连续，DMA 可达） |
| CCM | **64 KB**（`0x1000_0000`，**DMA 不可达**，放 RTOS 堆/栈） |
| HSE | **8 MHz**（Y2 无源晶振）→ 与 `-DHSE_VALUE=8000000` 对应 |
| LSE | 32.768 kHz（Y1，RTC 用） |

## 2. 本项目用到的引脚

> 每一根线怎么接（含 OV7670、MPU6050、CAN、UART、Pi5 侧）见 **[`wiring.md`](wiring.md)**。本节只记"这块板子上有哪些脚、在哪一排针"。

| 功能 | 引脚 | 排针位置 | 备注 |
|---|---|---|---|
| LED0 | **PF9** | J2-17 | 阳极 3V3，经 510 Ω → **低电平点亮** |
| LED1 | **PF10** | J2-20 | 同上 |
| USART1_TX | **PA9** | J3-32 | 同时引到 J5 ISP 排针第 4 脚 |
| USART1_RX | **PA10** | J3-31 | 同时引到 J5 ISP 排针第 3 脚 |
| SWDIO | PA13 | J3-28 | 板载 4 针 SWD 排针 |
| SWCLK | PA14 | J3-27 | 同上 |
| 3V3 / GND | — | J3-57 / J3-58,60；J2-59,60 / J2-55 | |
| 5V | — | J3-59；J5-1 | 电源引出，**不是信号** |

后续 S3 接 OV7670 时要避开的板载外设：SD 卡（PC8–PC12、PD2）、W25Q16（PB12–PB15）、NRF2401。不插卡时这些线是自由的，但要在 S3 布线上确认清楚。

## 3. 电源与启动

```
MiniUSB 5V ──R5(500mA)──> U1 AMS1117-3.3 ──> 3V3 全板
```

- 板上的 MiniUSB 座 **是 USB Device（PA11/PA12），不是 USB 串口** —— 板上没有 CH340 一类芯片，插电脑不会出现 COM 口。
- **BOOT0**（J2-57）板上经 R10 510 Ω 默认下拉到 GND = 0 → 从主 Flash 启动。
- **BOOT1 复用 PB2**（J2-35 / J2-58）。
- 三种模式：`BOOT0=0` 用户闪存（**我们用这个**）；`BOOT0=1,BOOT1=0` 系统存储区（ISP 串口下载）；`BOOT0=1,BOOT1=1` 内部 RAM。
- 我们用 ST-Link SWD 烧写，**BOOT 跳线一根都不用动**。

## 4. J5「ISP 串口」排针

| 脚 | 信号 |
|---|---|
| 1 | 5V（取电用） |
| 2 | GND |
| 3 | PA10 / RXD1 |
| 4 | PA9 / TXD1 |

接 USB-TTL **只接 2/3/4 三根**，5V 别接（板上已有电源，两路会顶牛）。PA9/PA10 是 FT 引脚，5 V 电平的 TTL 模块也不会烧 MCU。

接线（交叉）：

```
USB-TTL GND ── J5-2 GND
USB-TTL TXD ── J5-3 PA10/RXD1
USB-TTL RXD ── J5-4 PA9/TXD1
```

## 5. 原厂固件备份

烧写本项目固件前已把整片 Flash 读出来存到 `~/backup/f407-factory-firmware-YYYYMMDD.bin`（1 MB）。要恢复：

```sh
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program ~/backup/f407-factory-firmware-20261022.bin 0x08000000 verify reset exit"
```
