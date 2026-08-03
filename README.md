# ESP32-S3 红外遥控信号监视器

基于 ESP32-S3-N16R8（16MB Flash / 8MB PSRAM）与 ESP-IDF v6.0.2。
通过 VS1838B 接收红外遥控信号，使用 RMT 以 1us 分辨率采集原始波形，
在 SSD1315（128x64，兼容 SSD1306 指令集）OLED 上实时显示。

两种监视模式：

1. **RAW SIGNAL FEATURES**（原始信号特征）— 显示跳变沿数量、总时长、引导脉冲/间隔、
   脉冲数量、最小/最大数据脉冲、最后间隔，以及协议类型提示。
2. **NEC DECODE**（NEC 解码）— 显示地址、命令、32 位原始码、校验状态，并识别重复码
   （按键长按）与扩展 16 位地址 NEC。

## 硬件连接

| 功能 | GPIO | 说明 |
|------|------|------|
| IR 接收 | GPIO4 | VS1838B OUT（解调后基带信号，空闲为高电平） |
| OLED SDA | GPIO8 | I2C 数据（SSD1315，默认地址 0x3C） |
| OLED SCL | GPIO9 | I2C 时钟（400kHz） |
| 按键-上 | GPIO39 | 菜单上移 |
| 按键-下 | GPIO38 | 菜单下移 |
| 按键-确定 | GPIO37 | 确认 / 进入 / 暂停切换 |
| 按键-返回 | GPIO36 | 返回上级 |

按键接 GND，内部上拉使能，按下为低电平。OLED 建议外接 4.7k 上拉。

## 操作说明

开机进入主菜单：

- 上 / 下：移动高亮光标
- OK：进入选中的模式
- 在监视界面按 OK：冻结/恢复画面（PAUSE / LIVE）
- BACK：返回主菜单

## 工程结构

```
IR-rx/
├── CMakeLists.txt
├── sdkconfig.defaults        # esp32s3 / 16MB flash / 8MB 八线 PSRAM
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild     # 引脚、OLED 地址等可配置项
│   ├── app_main.c
│   ├── app_ir.c / app_ir.h   # RMT 采集 + 原始特征分析 + NEC 解码
│   ├── app_oled.c / app_oled.h # SSD1315 I2C 驱动（帧缓冲）
│   ├── app_ui.c / app_ui.h   # 菜单 / 按键 / 界面状态机
│   ├── font5x7.c / font5x7.h # 5x7 ASCII 字体
│   └── include/
```

## 编译与烧录

在 PowerShell 中：

```powershell
& 'D:\esp\v6.0.2\esp-idf\export.ps1'
cd G:\esp32s3\IR-rx
idf.py build
idf.py -p COMx flash monitor
```

如需修改引脚或 OLED 参数：

```powershell
idf.py menuconfig
# 进入 "IR Monitor Configuration"
```

## 实现说明

- RMT RX 使用 1MHz 分辨率（1 tick = 1us），DMA 缓冲 512 个 symbol，
  空闲 50ms 判定一帧结束，可覆盖 NEC 及大部分长帧空调遥控器。
- NEC 解码采用"扫描 9ms 引导码"的方式，不依赖信号极性，容错范围较官方示例更宽，
  支持 8 位/16 位地址 NEC 与重复码。
- 原始模式中"引导脉冲/间隔"为第一、二段电平宽度；最小/最大脉冲不含 9ms 引导码。
- OLED 驱动为纯软件帧缓冲（1KB），整屏刷新约 20ms（400kHz I2C）。
- 界面文本为 ASCII（128x64 无法容纳中文 5x7 字号）。
