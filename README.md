# STM32H743 串口屏 whmi-wri 烧录桥接（固定 115200）

本工程用于透明桥接：

- PC 串口工具/升级工具 <-> `USART1` <-> STM32H743 <-> `USART6` <-> 串口屏
- 三端波特率固定 `115200`，不做任何动态切换

## 1. CubeMX 配置要求

请在 CubeMX 中确认以下配置（本工程已按此生成）：

- `USART1`：Asynchronous，`115200 8N1`，TX/RX 使能，中断使能
- `USART6`：Asynchronous，`115200 8N1`，TX/RX 使能，中断使能
- DMA：至少启用 `USART1_TX`、`USART6_TX`（Normal 模式）
- NVIC：启用 `USART1_IRQn`、`USART6_IRQn`、`DMA1_Stream0_IRQn`、`DMA1_Stream1_IRQn`
- 勾选 Keep User Code（只在 `USER CODE BEGIN/END` 中修改）

## 2. 工程结构

- `Common/Inc/ring_buffer.h`、`Common/Src/ring_buffer.c`：通用环形缓冲
- `App/Inc/bridge.h`、`App/Src/bridge.c`：串口桥接模块
- `Core/Src/main.c`：在用户区调用 `Bridge_Init()`、`Bridge_Process()`

## 3. 转发架构（透明 + 吞吐）

- 两个独立环形缓冲（每方向 `16KB`）：
  - `USART1 -> USART6`
  - `USART6 -> USART1`
- RX：`HAL_UARTEx_ReceiveToIdle_DMA` 接收，按 DMA 片段原样入队
- TX：`HAL_UART_Transmit_DMA` 从队列线性段直接发送，发送完成后出队
- 不做协议解析/转义/校验，不插入任何结束符；`0x05` 与其他字节等价透明转发

## 4. 编译（CLion + CMake + arm-none-eabi-gcc）

确保工具链可用（例如 `arm-none-eabi-gcc --version` 可执行），然后在工程根目录执行：

```bash
cmake --preset Debug
cmake --build --preset Debug
```

## 5. 功能验证步骤

### 5.1 透明转发与 0x05 ACK 节奏

1. PC 端工具连接到 MCU 的 `USART1`（115200）
2. 串口屏连接到 MCU 的 `USART6`（115200）
3. PC 发送：
   - `whmi-wri <filesize>,115200,res0`
   - 紧跟 `FF FF FF`
4. 观察 PC 接收：屏应返回单字节 `0x05`（不带任何附加字节）
5. 数据阶段每发完 4096 字节，屏回 `0x05`，PC 侧应按节奏收到

### 5.2 4096 分包验证

使用升级工具或自写脚本，固定 4096 字节分包发送固件：

- 每包后等待一个 `0x05`
- 若出现多余字节、丢字节、ACK 节奏错位，说明链路不透明

### 5.3 大文件压测（>=1MB）

建议连续烧录或循环发送 `>=1MB` 数据，至少执行 10 轮：

- 观察是否存在卡死、超时、NACK、文件损坏
- 统计总发送字节与接收确认节奏是否一致

## 6. 配套脚本

仓库附带两个 Python 脚本，用于通过 MCU 桥接烧录淘晶驰（TJC）串口屏 `.tft` 固件。两者均依赖 `pyserial`：

```bash
pip install pyserial
```

### 6.1 `scripts/flash_via_mcu.py`（命令行，推荐自动化/回归测试）

无 GUI，所有输出为机器可解析的键值对，适合 CI 与批量压测。流程：自动联机 → 发送 `whmi-wri <size>,115200,0` → 4096 字节分包 → 每包等 `0x05` ACK。

```bash
python scripts/flash_via_mcu.py \
  --port /dev/tty.usbserial-XXXX \
  --file test/demo.tft
```

可选参数：

- `--chunk-size`：数据阶段分包大小，默认 `4096`（与屏协议一致，请勿随意改）
- `--ack-timeout-s`：单包 ACK 超时，默认 `5.0` 秒
- `--boot-wait-s`：发送 `whmi-wri` 后等待首个 `0x05` 的时间，默认 `0.35` 秒

退出码：`0` 成功；`1` 联机失败；`2` 未收到首个 `0x05`；`3` 数据阶段 ACK 超时；`10/11` 参数/文件错误。

典型压测用法（配合 `test/big.tft` 做大文件回归）：

```bash
for i in $(seq 1 10); do
  python scripts/flash_via_mcu.py --port /dev/tty.usbserial-XXXX --file test/big.tft || break
done
```

### 6.2 `test/tftdownloader.py`（Tkinter GUI，手动验证用）

图形化的下载工具，用于手工点按验证：

```bash
python test/tftdownloader.py
```

使用步骤：选择 MCU 所在串口 → 选择下载波特率（默认 `921600`）→ 选择 `.tft` 文件 → 点击“开始下载”。窗口会显示实时速度、进度和日志。

> 注意：GUI 的“下载波特率”会触发屏端切换波特率。若要严格对应本工程“三端固定 115200”的策略，请把下载波特率改为 `115200`；使用更高波特率时，需确认 MCU 固件与上位机在 `whmi-wri` 之后也同步切换，否则会 ACK 超时。

## 7. 注意事项

- 禁止在 `USART1/USART6` 路径上使用 `printf` 或任何日志输出
- 如需升级为 ACK 超时保护，可在 `App/bridge` 内增加“可选状态机”，但仍必须保证数据阶段完全透明

## 8. 调试状态（仅 SWD 观察）

为避免串口污染，桥接状态只保存在内存中，不输出到 `USART1/USART6`：

- 通过 `Bridge_GetStats()` 可读取累计统计
- `fault_code != BRIDGE_FAULT_NONE` 代表桥接进入故障态（例如环形缓冲溢出）
- 故障态下会停止继续桥接，避免“静默丢字节但流程继续”的隐蔽错误
