# shared/proto/ — 协议单源

一个协议，三份实现，一份黄金向量：

| 文件 | 谁用 |
|---|---|
| `frame.h` | 固件（`node/proto/`）与 Pi5 内核驱动（`driver/f407_link.c`）共用同一份结构体与常量 |
| `frame.py` | `tools/` 与 `tests/` 里的脚本 |
| `golden_vectors.txt` | 逐字节比对的测试向量 |

规矩：

- `frame.h` 只依赖 `<stdint.h>`，不带任何项目头文件。
- 帧头常量（`0xAA 0x55`、`FRAME_VER`、`FRAME_TYPE_*`、`FRAME_MAX_PAYLOAD`）只在这里定义一次，别处引用。
- 改协议时三份实现 + 黄金向量在同一个 commit 里改，否则 CI 红。
