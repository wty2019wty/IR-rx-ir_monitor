# ESP32-S3 红外遥控信号监视器与录制回放器

基于 ESP32-S3-N16R8（16MB Flash / 8MB PSRAM）与 ESP-IDF v6.0.2。
通过 VS1838B 接收红外遥控信号，使用 RMT 以 2us 分辨率采集原始波形，
在 SSD1315（128x64，兼容 SSD1306 指令集）OLED 上实时显示。

## 功能概述

1. **RAW MONITOR** — 原始信号监视+录制（暂停时自动开始录制，按上键保存）
2. **NEC MONITOR** — NEC解码监视+录制（暂停时自动开始录制，按上键保存）
3. **PLAYBACK** — 回放列表，按OK连续回放3次
4. **STORAGE MGR** — 存储管理（查看空间、删除全部/单个）

## 硬件连接

| 功能 | GPIO | 说明 |
|------|------|------|
| IR 接收 | GPIO4 | VS1838B OUT（解调后基带信号，空闲为高电平） |
| IR 发射 | GPIO5 | 可选载波，需外接三极管驱动红外发光二极管 |
| OLED SDA | GPIO8 | I2C 数据（SSD1315，默认地址 0x3C） |
| OLED SCL | GPIO9 | I2C 时钟（400kHz） |
| 按键-上 | GPIO39 | 菜单上移 |
| 按键-下 | GPIO38 | 菜单下移 |
| 按键-确定 | GPIO1 | 确认 / 进入 / 暂停切换 |
| 按键-返回 | GPIO2 | 返回上级 |

按键接 GND，内部上拉使能，按下为低电平。OLED 建议外接 4.7k 上拉。

> ⚠️ 重要：N16R8 模块启用 8MB 八线 PSRAM 时，GPIO33-37 被 PSRAM 占用
> （GPIO36=SPIIO7、GPIO37=SPIDQS），**不能**用作普通 GPIO，否则会损坏 PSRAM 访问
> 导致看门狗复位。GPIO26-32 为 Flash 引脚同样不可用。请避开这些引脚。
> 引脚可在 `idf.py menuconfig` → "IR Monitor Configuration" 中修改。

## 操作说明

### 主菜单
- 上 / 下：移动高亮光标
- OK：进入选中的模式
- BACK：返回上级（在主菜单无作用）

### RAW/NEC 监视模式
- 实时显示红外信号特征或NEC解码结果
- **OK**：暂停/恢复监视
- **暂停时按上键**：保存暂停前最后接收到的信号到存储
- **BACK**：返回主菜单

操作流程：
1. 进入监视模式，开始接收红外信号
2. 对准遥控器按下要录制的按键
3. 看到信号显示后，按OK暂停
4. 按上键保存该信号（显示"SAVED!"）
5. 按OK恢复监视，继续录制其他信号
6. 按BACK返回主菜单

### 回放列表
- 上 / 下：选择录制
- **OK**：连续回放3次
- BACK：返回主菜单

### 存储管理
- 上 / 下：选择操作
- OK：执行（DELETE ALL 或 DELETE ONE）
- DELETE ALL 会显示确认界面，再按OK确认
- DELETE ONE 显示列表，选择后按OK删除
- BACK：返回主菜单

## 工程结构

```
IR-rx-ir_monitor/
├── CMakeLists.txt
├── sdkconfig.defaults        # esp32s3 / 16MB flash / 8MB 八线 PSRAM
├── partitions.csv            # 自定义分区表（含 LittleFS 存储分区）
├── components/
│   └── esp_littlefs/         # LittleFS 文件系统组件（第三方）
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild     # 引脚、OLED 地址、载波频率等可配置项
│   ├── app_main.c
│   ├── app_ir.c              # RMT 采集 + 原始特征分析 + NEC 解码 + 录制回放
│   ├── app_oled.c            # SSD1315 I2C 驱动（帧缓冲）
│   ├── app_ui.c              # 菜单 / 按键 / 界面状态机
│   ├── font5x7.c             # 5x7 ASCII 字体
│   └── include/
│       ├── app_ir.h
│       ├── app_oled.h
│       ├── app_ui.h
│       └── font5x7.h
```

## 编译与烧录

在 PowerShell 中：

```powershell
& 'D:\esp\v6.0.2\esp-idf\export.ps1'
cd IR-rx-ir_monitor
idf.py build
idf.py -p COMx flash monitor
```

> **首次烧录前**：需将 `esp_littlefs` 组件放入 `components/` 目录（见工程结构）。
> 首次烧录时 LittleFS 分区会自动格式化。

如需修改引脚或 OLED 参数：

```powershell
idf.py menuconfig
# 进入 "IR Monitor Configuration"
```

## 实现说明

- 启动流程：OLED init → RMT init → 后台异步挂载 LittleFS → UI 立即可用。
  存储就绪后自动开始 IR 接收。主菜单、RAW、NEC 监控无需等待存储初始化。
- RMT RX 使用 500kHz 分辨率（1 tick = 2us），DMA 缓冲 256 个 symbol，
  空闲 50ms 判定一帧结束。
- RMT TX 同样使用 500kHz 分辨率，支持 38kHz 载波调制（可通过 menuconfig 修改频率和占空比）。
- NEC 解码采用"扫描 9ms 引导码"的方式，不依赖信号极性，容错范围较官方示例更宽，
  支持 8 位/16 位地址 NEC 与重复码。
- 原始模式中"引导脉冲/间隔"为第一、二段电平宽度；最小/最大脉冲不含 9ms 引导码。
- OLED 驱动为纯软件帧缓冲（1KB），整屏刷新约 20ms（400kHz I2C）。
- I2C 使用同步模式并在启动时自动探测 OLED 地址（先 0x3C 后 0x3D），
  若探测失败会在串口日志中明确提示，便于排查接线/供电/地址问题。
- 界面文本为 ASCII（128x64 无法容纳中文 5x7 字号）。
- 录制功能使用 LittleFS 文件系统存储，支持掉电安全，比 SPIFFS 挂载更快。
  每个录制保存为独立的二进制文件（`ir_xxx.bin`）。
- 录制文件格式：文件头（64字节）+ RMT 符号数据（每个符号4字节）。
- 支持最多 50 个录制，每个录制最多 256 个 RMT 符号。
- 回放功能通过独立的 FreeRTOS 任务执行，支持连续回放3次。
- 使用自定义分区表，为 LittleFS 分配约 14MB 存储空间（可存储大量录制）。
- RMT ISR 使用简单标志变量代替 FreeRTOS 队列，避免 DMA 中断兼容性问题。
