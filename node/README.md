# node/ — STM32F407 视觉节点固件

## 分层

依赖方向（箭头 = 上层可以 include 下层，下层永远不知道上层存在）：

```
app  →  proto / lib  →  drv  →  bsp  →  arch
             ↑
             os（旁路：被 app 使用；drv 通过回调把事件交出去）
```

## arch/：三个文件都已落地

| 文件 | 干什么 | 写错的现象 |
|---|---|---|
| `startup_stm32f407xx.s` | 98 项向量表（16 内核 + 82 外部中断，IRQ79 保留）；复位时拷 `.data`、拷 `.ccmram`、清 `.bss`，再 `SystemInit` → `__libc_init_array` → `main` | 表项少一条或顺序错，中断跳进隔壁 handler，现象是"某个外设一开就 HardFault" |
| `stm32f407.ld` | FLASH 1M @0x0800_0000、RAM 128K @0x2000_0000、CCM 64K @0x1000_0000；段：`.isr_vector` / `.text` / `.data` / `.bss` / `.ccmram` / `._user_heap_stack` | 栈顶不在 RAM 末尾 → 一进中断就踩到 `.bss` |
| `system_stm32f4xx.c` | HSE 8 MHz → PLL(M=8,N=336,P=2,Q=7) → 168 MHz，APB1 /4 = 42 MHz、APB2 /2 = 84 MHz，Flash 5 WS，开 FPU；HSE 起振失败退 HSI 16 MHz | 忘记先提 Flash 等待周期 → 取到乱码指令；APB1 超 42 MHz → 偶发死机 |

链接脚本里带了几条 `ASSERT`，写错就在**链接期**炸，不用等上板：向量表必须是 98×4 字节且落在 `0x0800_0000`、栈顶必须等于 RAM 末尾、段符号顺序必须递增。实测把断言改成 50 项后链接立刻失败（`向量表必须是 98 项 × 4 字节`）。

两条容易踩的细节：

- **SRAM1(112K) 和 SRAM2(16K) 物理连续、都 DMA 可达**，所以链接脚本合成一个 128K 的 `RAM` 区，不拆 `.sram1/.sram2`——拆开只会多一个"变量落错区"的出错机会。真正要单独对待的只有 CCM。
- **`.ccmram` 带 `AT> FLASH`**：谁在 CCM 里放带初值的变量，初值会被启动代码拷过去，而不会静默变成垃圾值。代价是 CCM 里的未初始化缓冲也会占同样大小的 Flash（1 MB 里无所谓）。


| 目录 | 回答什么问题 | 换什么才动它 |
|---|---|---|
| `arch/` | 这颗 MCU 怎么启动、内存怎么摆 | 换 MCU |
| `bsp/` | 这颗料接哪根管脚、哪个 DMA、多少时钟（直接写寄存器） | **换板子** |
| `drv/` | 这个器件怎么用（寄存器序列、状态机） | **换芯片** |
| `os/` | 任务/队列/通知的唯一封装 | 换 RTOS |
| `lib/` | 纯算法（灰度、二值化、质心、CRC、环形缓冲） | 几乎不动 |
| `proto/` | 0xAA55 帧协议、0x180/0x181 报文布局 | 协议改版 |
| `app/` | 业务编排：采集→处理→上报，参数管理 | 需求变化 |
| `config/` | 引脚、波特率、NVIC 优先级、默认阈值、栈深 | 调参 |

`bsp` 与 `drv` 分开是最值钱的一刀：换板子只动 `bsp`，换器件只动 `drv`。

## 技术选型：CMSIS + 裸寄存器

**不用 ST HAL，也不用 LL**，工程里没有任何第三方 `.c` 文件。

- F4 的 LL 封装里**没有 CAN 和 DCMI**（`stm32f4xx_hal_driver/Inc/` 只有 adc/bus/cortex/crc/dac/dma/dma2d/exti/fmc/fmpi2c/fsmc/gpio/i2c/iwdg/lptim/pwr/rcc/rng/rtc/sdmmc/spi/system/tim/usart/usb/utils/wwdg 这 26 个），所以"全 LL"本来就不成立，混着写更糟。
- HAL 的 `HAL_Init()` 会占用 SysTick 并让 `HAL_Delay()` 轮询 `uwTick`，而 FreeRTOS 也拿 SysTick 当心跳、还压到最低优先级——`HAL_Delay()` 在调度器启动后会漂移甚至挂死。裸寄存器版本没有这个问题：延时只有 `vTaskDelay` 和 DWT。
- DCMI 只需要 `DCMI_CR` / `DCMI_IER` / `DCMI_SR` 三个寄存器，DMA 是 `DMA2_Stream1/7` 的十几个字段。用 HAL 包装反而看不清写了什么。

**代价要认**：CAN 位定时、UART 分数波特率这些得自己推式子；排错要靠调试器看外设寄存器和写完后回读打印。用 CubeMX 生成一份 HAL 工程**当参考答案**（看它最后写到哪个位）是合法手段，但代码一行不抄进仓库。

## 四条硬规矩

1. **`drv/` 不出现 FreeRTOS 头文件。** 驱动只返回状态码或注册回调；中断到任务的翻译统一放在 `os/isr_bridge.c`。这样驱动可在 PC 上单测，也能换 RTOS。
2. **`lib/` 与 `proto/` 不出现外设头文件。** `make test` 会用主机 gcc 编译它们，偷偷 include 一个 `stm32f4xx.h` 立刻编译失败——单测就是分层的守卫。
3. **跨层 include 带路径前缀**：`#include "bsp/bsp_uart.h"`、`#include "lib/img/binarize.h"`。`-I` 由 Makefile 按层给，越界编不过。
4. **公共头与私有头分开**：`ov7670.h` 对外，`ov7670_priv.h` 只在本目录用。

## 常用命令

```bash
make                 # 编固件
make size            # flash / RAM 占用
make toolchain       # 打印实际用的 gcc（两套工具链并存时必看）
make test            # PC 上跑 lib/ + proto/ 单测
make check-layers    # 检查是否有人越层 include
make clean
```

## 环境（已在本机实测通过）

| 项 | 版本 |
|---|---|
| 编译器 | Arm GNU Toolchain **14.3.Rel1**，`~/opt/arm-gnu-toolchain-14.3.rel1-x86_64-arm-none-eabi/bin` |
| 调试器 | 官方工具链自带 `arm-none-eabi-gdb` 15.2 |
| 烧写 | OpenOCD 0.12.0（`interface/stlink.cfg` + `target/stm32f4x.cfg`） |
| 主机 gcc | 系统自带，只给 `make test` 用 |

机器上**只留这一套**：apt 的 `gcc-arm-none-eabi`（14.2，且不含 gdb）已 purge 干净。
PATH 挂在两处 —— `~/.bashrc`（交互 shell）与 `/etc/profile.d/arm-gnu-toolchain.sh`（登录 shell / 系统上下文）。非交互脚本里即使 PATH 没有它也不影响 `make`，因为 Makefile 直接探 `~/opt/arm-gnu-toolchain-*`，不靠 PATH。

**为什么选官方而不是 apt**：Ubuntu 的 `gcc-arm-none-eabi`（14.2）确实能编过，但那个包**不含 `arm-none-eabi-gdb`**，Cortex-Debug 没法用，所以已经卸掉了。Makefile 会自动挑 `~/opt/arm-gnu-toolchain-*` 里版本号最大的，找不到才退回 `PATH`，所以 `make` 的结果和 shell 的 `PATH` 怎么排无关。

已核对的四件事：

1. **`arch/cmsis/` 这 8 个头是够的**——`#include "stm32f4xx.h"` 之后访问 `RCC->AHB1ENR`、`GPIOA->MODER`、`DCMI->CR`、`CAN1->MCR`、`__get_PRIMASK()`、`__DSB()` 全部编过，在 `-Wall -Wextra -Wshadow -Wundef -Wdouble-promotion` 下 **0 warning**。不需要 `stm32f4xx_conf.h`，也不需要任何 `.c`。
2. **FPU 真的开着**——`_Static_assert(__FPU_USED == 1)` 通过，`float` 乘法生成 `vmul.f32`/`vadd.f32`，不是软浮点库调用。
3. **链接链完整**——`nano.specs` 与 `nosys.specs` 都在，从 `app/main.c` 到 `vision-node.{elf,hex,bin}` 能一路走通。
4. **`uint32_t` 之类不是头文件缺**——它来自 newlib 的 `<stdint.h>`，只有"没给编译器路径"时编辑器才会红。

### 烧写（ST-Link）

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
        -c "program build/vision-node.elf verify reset exit"
```

udev 规则已装好：`/etc/udev/rules.d/60-stlink.rules`（VID `0483`、`GROUP="plugdev"`、`MODE="0664"`），当前用户已在 `plugdev` 组，插上就能烧。换机器重建用：

```bash
sudo tee /etc/udev/rules.d/60-stlink.rules >/dev/null <<'EOF'
SUBSYSTEM=="usb", ATTR{idVendor}=="0483", MODE="0664", GROUP="plugdev"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### VSCode

仓库根的 `.vscode/` 三个文件：

- `c_cpp_properties.json`：`compilerPath` 指向 `~/opt/arm-gnu-toolchain-14.3.rel1-…/bin/arm-none-eabi-gcc`，`compilerArgs` 带上 `-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard`（否则 IntelliSense 里没有 `__ARM_ARCH_7EM__` 这类宏，把 CMSIS 当 x86 解析），`defines` = `STM32F407xx` + `HSE_VALUE` + `HSI_VALUE`，`includePath` = 11 个项目目录 + newlib 的 `arm-none-eabi/include` 与 `lib/gcc/…/include`。
- `settings.json`：`*.h` 按 **C** 解析（本工程全是 C，按 C++ 解析会多出一批假错），`*.ld` 按纯文本，`node/build` 不进搜索与监视。

三条经验：

1. **红波浪线九成是"编辑器没找到编译器"，不是头文件坏了。** 看 `输出` 面板的 **C/C++ 配置警告** 频道：如果写着"无法使用 compilerPath … 解析配置，请改用 /usr/bin/gcc"，说明那一刻 `compilerPath` **还不存在**（本项目就踩过：配置写在 12:48，工具链 12:52 才解压完），IntelliSense 会一直拿 `/usr/bin/gcc` 按 x86 解析 ARM 头文件。
2. 改完配置立刻生效不了时：`Ctrl+Shift+P` → **C/C++: Reset IntelliSense Database**，再 → **Developer: Reload Window**。
3. `compilerPath` 里的版本号是**写死**的。换工具链版本时改这一行；`make` 不用改（它按 `~/opt/arm-gnu-toolchain-*` 自动挑最新的）。

#### 调试：Cortex-M4 只有 6 个硬件断点

`.vscode/launch.json` 里的 `runToEntryPoint: "main"` 是靠 gdb **临时插一个硬件断点**实现的。Cortex-M4 的硬件断点单元一共只有 **6 个**，编辑器里留下的断点一多就会占满，表现为：

```
Cannot insert hardware breakpoint 2: Remote failure reply: 0E
→ continue 失败 → 弹窗 "A serious error occurred with gdb, unable to continue or interrupt"
```

本项目踩过：`bsp_i2c_soft.c:219/222` + `main.c:143/145/146/173` 共 6 个断点，加上 `main` 的临时断点正好 7 个，超了 1 个。**保持启用的断点 ≤ 5 个**即可。

另外留了一个不消耗断点的配置「调试（精简，无外设视图）」：它不设 `runToEntryPoint`，启动后停在复位向量，按继续就跑起来。

## 加一个文件

把 `.c` 放进对应目录即可，Makefile 自动收集；头文件用相对仓库根 `node/` 的路径 include。
新增一个**层**才需要改 `Makefile` 的 `LAYERS` 和 `*_incs`。

## 内存约定

- SRAM1+SRAM2 = 128 KiB，DMA 可达 → 图像缓冲只能放这里。
- CCM 64 KiB @ `0x1000_0000`，DMA **不可达** → 只放堆、栈、日志。
- 大缓冲一律 `static`，绝不 `pvPortMalloc`。

## 不要过度分层

`middleware/`、`services/`、`domain/` 这些在节点侧没有第二个实现，先不建。
判据：一个目录只有 1 个文件且 foreseeable 不会再长，就先别建。主干（`arch`→`bsp`→`drv`→`lib/proto`→`app`）**≤ 6 层**，`os` 是旁路不算主干。
**`hal/` 已经删了**——既然只用 CMSIS + 裸寄存器，那一层不存在。
