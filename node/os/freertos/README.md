# FreeRTOS 内核（vendored）

| | |
|---|---|
| 版本 | **V11.3.2**（2026-10-16 发布） |
| 来源 | <https://github.com/FreeRTOS/FreeRTOS-Kernel> tag `V11.3.2` |
| 许可 | MIT（见同目录 `LICENSE.md`） |
| 引入日期 | 2026-10-22 |
| 改动 | **零改动**。升级时整目录替换即可，不要把本地修改混进来 |

## 拷了什么

```
freertos/
├── include/                           # 21 个头，原样
├── tasks.c  list.c  queue.c           # 必需
├── timers.c  event_groups.c  stream_buffer.c
├── portable/GCC/ARM_CM4F/port.c       # Cortex-M4 + FPU 的 GCC 移植层
├── portable/GCC/ARM_CM4F/portmacro.h
└── portable/MemMang/heap_4.c          # 首次适配 + 相邻块合并
```

**没拷**：`croutine.c`（上古协程，已废弃）、其余 `portable/` 下几十个移植层、
`examples/`、CMake 脚本。

## 配置在哪

内核**不含** `FreeRTOSConfig.h` —— 那是应用侧的，放在
`node/config/FreeRTOSConfig.h`（和 `board_config.h` 同一个目录，都归 `config` 层）。
那边负责：tick 频率、堆大小、优先级数量、中断优先级门限、`configASSERT` 行为。

## 为什么用 heap_4 而不是 heap_1/2/3/5

- `heap_1` 只能分配不能释放（本项目够用，但没有释放能力以后要改）。
- `heap_2` 不合并相邻空闲块，长时间运行会碎。
- `heap_3` 包一层 `malloc`，等于把 newlib 的堆和非重入问题引进来。
- `heap_5` 支持多块不连续内存，本项目 CCM 是连续的一块，用不上。
- **`heap_4`：首次适配 + 相邻空闲块合并**，行为可预测，够用且不碎。

堆本体 `ucHeap[24 KB]` 由 `os/os_task.c` 定义，段属性落在 **`.ccmbss`（CCM）** ——
任务栈和内核对象永远不该成为 DMA 目标，放 CCM 从物理上杜绝这类错。

## 静态分配的钩子

`configSUPPORT_STATIC_ALLOCATION = 1`，所以内核要求应用提供空闲任务的内存：

```c
void vApplicationGetIdleTaskMemory(...)   /* 在 os/os_task.c */
```

以后要把业务任务也换成 `xTaskCreateStatic` 时，照同样的模式加
`vApplicationGetTimerTaskMemory`（本项目 `configUSE_TIMERS = 0`，暂时不需要）。
