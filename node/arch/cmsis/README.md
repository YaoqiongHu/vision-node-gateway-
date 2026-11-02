# arch/cmsis/ — 芯片描述头文件

这里放 **8 个头文件**，不放任何 `.c`。它们只声明"`GPIOA` 指向 `0x4002_0000`、这一位叫 `MODER`"，本身不是库。

| 文件 | 来源仓库 | 仓库内路径 |
|---|---|---|
| `core_cm4.h` | `STMicroelectronics/STM32CubeF4` | `Drivers/CMSIS/Include/` |
| `cmsis_gcc.h` | 同上 | 同上（`core_cm4.h` 会 include 它） |
| `cmsis_compiler.h` | 同上 | 同上 |
| `cmsis_version.h` | 同上 | 同上 |
| `mpu_armv7.h` | 同上 | 同上 |
| `stm32f4xx.h` | `STMicroelectronics/cmsis_device_f4` | `Include/` |
| `stm32f407xx.h` | 同上 | 同上 |
| `system_stm32f4xx.h` | 同上 | 同上 |

`cmsis_armclang.h` / `cmsis_iccarm.h` 是给别的编译器用的，不用拿。

## 来源与版本

**为什么从 STM32CubeF4 取 Core、而不是从 ARM 的 CMSIS 仓库取**：CubeF4 把 CMSIS Core 直接贴在 `Drivers/CMSIS/Include/`（不是 submodule），设备头则是 submodule 指向 `cmsis_device_f4`。两边取的就是 CubeMX 生成工程时用的那一套，版本天然对齐，不会出现"CMSIS 6 的 Core 配 CMSIS 5 时代写的设备头"。

| 项 | 值 |
|---|---|
| Core 仓库 | https://github.com/STMicroelectronics/STM32CubeF4 （branch `master`, commit `60c8abe0ae88`） |
| Device 仓库 | https://github.com/STMicroelectronics/cmsis_device_f4 （branch `master`, commit `9192c7b9df75`） |
| CMSIS Core 版本 | 5.x（`core_cm4.h` 里 `__CM_CMSIS_VERSION_MAIN = 5U`） |
| 取用日期 | 2026-10-22 |
| 取用方式 | `curl -O https://raw.githubusercontent.com/<repo>/master/<path>/<file>` |

**校验和**（`sha256sum` 记录在此，将来谁动了这几个文件一眼能看出）：

```
b51963d271c1571ca3463654c76aa7ea50c4eac5d0a2710df10414ac64d9e0f8  cmsis_compiler.h
43bfd1fe69fbbc2ca70aaf7d43cc3e20f16e0c7f66e1bdef9b51af6dc14b2617  cmsis_gcc.h
184c19fd3ee73632edf35a0b4d49cd48be75fbf49e6ccb19d9db05fa83bea4b3  cmsis_version.h
f5b63d52dd1557b15ca414cb59c264f595a7b5669db2a5878dffe22f4caedc8c  core_cm4.h
29206b52ee02290ed6f5a5415ebd4187de802cf176d9b3cb844390d8e5571372  mpu_armv7.h
ae1ab57d8ab750f64f04fd73476bc2760c761e18e45bde648e0bc350c9a1d9ac  stm32f407xx.h
a19edea6b2f4df2ab3c567782c4d313a52319c4303bd56ad7dd1d0e077014657  stm32f4xx.h
02c067d5a135f540c03215dc834b1e60a97bbd4e55bfd3994b0c82a15ea3012a  system_stm32f4xx.h
```

## 规则

- **平铺在这一个目录里**，Makefile 只给 `-Iarch/cmsis` 一个路径，`#include "stm32f4xx.h"` 就能直接找到。
- **原样引用，一个字节都不要改**。要打补丁就在 `arch/` 里另建文件包一层（比如 `arch/stm32f4xx_conf.h` 之类）。
- 上面 5 个 CMSIS Core 文件是 **Apache-2.0**（文件头有 SPDX 标识）；3 个 ST 设备头写的是"licensed under terms in the LICENSE file in the root directory of this software component"，即 STM32CubeF4 根目录的 `LICENSE.md`。
- `system_stm32f4xx.c` **不放这里**——那是我们自己写的（`arch/system_stm32f4xx.c`），因为时钟树要按 8 MHz 晶振 → 168 MHz 自己配，不用 ST 那份默认的。

## STM32F407.svd

不是头文件，但和上面这批是同一类东西：给**调试器**看的芯片描述，让寄存器按名字显示（见 `.vscode/launch.json` 的 `"svdFile"`）。

| 项 | 值 |
|---|---|
| 来源 | https://raw.githubusercontent.com/cmsis-svd/cmsis-svd-data/main/data/STMicro/STM32F407.svd |
| 说明 | `posborne/cmsis-svd` 已停止更新，数据迁到了 `cmsis-svd/cmsis-svd-data` |
| 取用日期 | 2026-11-02 |
| 大小 / 结构 | 2,164,866 字节，`schemaVersion="1.1"`，91 个外设 |
| 覆盖 | `RCC` `USART1` `DCMI` `GPIOA~GPIOI` `CAN1` `FLASH` `DMA1/2` `TIM1~14` 等 |
| 不含 | `DWT`/`SysTick` 这类内核外设（SVD 只描述芯片外设，内核寄存器由调试器自己处理） |
| 影响 | 只有调试时用；不参与编译，删掉 `make` 照样过 |

```sh
# 校验
sha256sum STM32F407.svd
e2afca4f03f654225886d8508551cb7ac8f3c95ee28f18953aad466ecb56e6d6  STM32F407.svd
```
