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

## 6. 注意事项

- 禁止在 `USART1/USART6` 路径上使用 `printf` 或任何日志输出
- 如需升级为 ACK 超时保护，可在 `App/bridge` 内增加“可选状态机”，但仍必须保证数据阶段完全透明

## 7. 调试状态（仅 SWD 观察）

为避免串口污染，桥接状态只保存在内存中，不输出到 `USART1/USART6`：

- 通过 `Bridge_GetStats()` 可读取累计统计
- `fault_code != BRIDGE_FAULT_NONE` 代表桥接进入故障态（例如环形缓冲溢出）
- 故障态下会停止继续桥接，避免“静默丢字节但流程继续”的隐蔽错误
