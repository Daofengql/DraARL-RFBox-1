# DraARL-ESP32-RF

基于 **ESP32-S3** 的 DraARL 网络射频终端固件，集成了本地 RF 收发、Wi-Fi 联网、BLE 配置、UDP 语音链路、LVGL 图形界面以及设备参数云端同步能力。

项目当前面向一套带显示屏、旋钮、SA818 射频模块和 ES8388 音频编解码器的硬件平台，适合作为网络链路台、手咪终端或实验型数字射频节点的基础固件。

## 功能概览

- 320x240 LVGL 图形界面，包含启动页、主页面、设置页、RF 页面、信息页
- 支持 SA818 UHF 射频模块参数配置
- 支持本地修改发射频率、接收频率、亚音、静噪、带宽、功率
- 设置页可进入“高级射频设置”页面，交互逻辑与设置页保持一致
- RF 功率开关支持高/低状态切换，并持久化保存到本地 NVS
- 支持 ES8388 + I2S 音频采集与播放
- 支持 Opus 16 kHz 语音编码/解码
- 支持 DraARLv1 协议 UDP 鉴权、心跳、语音收发
- 支持服务器配置查询、配置下发、动态配置更新、时间同步
- 本地参数修改后可自动上报服务器，仅在发生变更时触发同步
- 支持 Wi-Fi 配网、BLE GATT 配置、Web Bluetooth 配置页面
- 支持背光亮度保存、设备参数 NVS 持久化
- 主界面具备 Wi-Fi、BLE、服务器状态、配置同步状态图标反馈

## 项目定位

这套固件主要完成三件事：

1. 管理本地硬件资源，包括屏幕、旋钮、按键、背光、音频和 SA818。
2. 提供本地 UI 与 BLE 配置入口，让设备在没有串口控制台的情况下也能完成部署。
3. 接入 DraARL 服务端，实现鉴权、配置同步、时间同步与语音互联。

## 硬件组成

项目当前代码默认面向如下模块组合：

- 主控：ESP32-S3
- 显示：SPI 接口 ST7789，分辨率 320x240
- 编码器：EC11 旋钮编码器
- 音频编解码器：ES8388
- 射频模块：SA818
- 状态灯：WS2812
- 网络：Wi-Fi
- 本地配置：BLE GATT + Web Bluetooth 页面

## 引脚定义

引脚定义来自 [`src/config.h`](src/config.h)。

| 模块 | 信号 | 引脚 |
| --- | --- | --- |
| TFT SPI | SCLK | 4 |
| TFT SPI | MOSI | 5 |
| TFT | DC | 7 |
| TFT | CS | 15 |
| TFT | RST | 6 |
| 背光 | BACKLIGHT | 16 |
| 电源键/按键 | KEY0 | 17 |
| I2C | SDA | 18 |
| I2C | SCL | 8 |
| I2S | MCLK | 46 |
| I2S | BCLK | 9 |
| I2S | WS/LRCK | 10 |
| I2S | DOUT | 11 |
| I2S | DIN | 12 |
| EC11 | CLK | 13 |
| EC11 | DT | 14 |
| EC11 | SW | 21 |
| WS2812 | DATA | 47 |
| 预留网络串口 | TX | 48 |
| 预留网络串口 | RX | 45 |
| SA818 | TXD | 1 |
| SA818 | RXD | 2 |
| SA818 | EN | 42 |
| SA818 | SQL | 41 |
| SA818 | PTT | 40 |
| SA818 | PW | 39 |

## 软件架构

### 启动流程

固件主入口位于 [`src/main.cpp`](src/main.cpp)。

启动顺序如下：

1. 初始化背光 PWM
2. 初始化显示驱动
3. 初始化 LVGL
4. 创建双缓冲绘图缓存
5. 调用 `ui_init()` 加载界面对象
6. 调用 `app_logic_init()` 启动应用逻辑
7. 在 `loop()` 中轮询应用逻辑与 LVGL 定时处理

### 主要模块

#### `src/logic/app_logic.*`

- 管理整机应用状态机
- 控制开机等待、启动页、进入主界面流程
- 负责顶部时间显示、设置页时间显示和本地时钟刷新
- 接收服务器时间包并更新系统时间

#### `src/logic/edit_controller.*`

- 管理主页面、设置页、RF 页面、信息页的编辑和导航逻辑
- 处理旋钮/按键交互
- 管理 SA818 本地配置状态
- 将本地修改后的射频参数写入 NVS
- 本地参数发生变更后触发云端配置同步

#### `src/logic/connectivity_manager.*`

- 管理 Wi-Fi 连接
- 管理 BLE 广播、认证、RPC 通道
- 对接 Web Bluetooth 页面
- 提供 Wi-Fi 扫描、Wi-Fi 设置、射频参数设置、服务器参数设置等 BLE RPC

#### `src/logic/net_audio_link.*`

- 管理 DraARL 网络链路状态机
- 处理 HTTP 预检查、动态码申请、绑定确认、UDP 鉴权
- 处理心跳包、配置包、时间包、语音包
- 处理服务器配置查询、服务器配置动态下发、本地配置主动上报
- 管理主页面云端状态图标与配置同步图标

#### `src/logic/device_config.*`

- 封装本地配置结构
- 提供默认配置
- 提供 NVS 读写接口
- 管理 Wi-Fi、射频、服务器、背光等配置持久化

#### `src/drivers/*`

- 显示、编码器、I2C、I2S、ES8388、SA818、UART、WS2812 驱动实现

#### `src/ui/*`

- SquareLine Studio 生成的 LVGL 页面与资源文件

### 页面结构

当前 UI 主要包含以下页面：

- `StartUP`：启动日志和进度条
- `main`：主界面，显示时间、网络状态、服务器状态、同步状态和主要频率信息
- `SettingPAGE`：设置页，包含亮度、BLE、高级射频设置、关于等入口
- `RFPAGE`：高级射频设置页，用于功率等扩展射频参数调整
- `InfoPage`：设备/状态信息页

### 输入方式

- `KEY0`：长按触发开机流程
- `EC11 旋钮旋转`：切换焦点或调整值
- `EC11 按压`：确认/进入/切换编辑状态

## 射频功能说明

### 支持的本地射频参数

- 发射频率 `TX`
- 接收频率 `RX`
- 发射亚音
- 接收亚音
- 静噪等级
- 发射带宽
- 发射功率

### 功率切换逻辑

RF 页面提供功率切换按钮：

- 打开时：`PW` 引脚进入高阻态，界面显示“高”
- 关闭时：`PW` 引脚拉低，界面显示“低”

同时该功率状态会：

- 立即作用到 SA818 运行时状态
- 保存到本地 NVS
- 在本地参数发生变更后上报服务器

### 频率范围

当前代码默认限制在 UHF 范围：

- 最小值：`400.0000 MHz`
- 最大值：`470.0000 MHz`

SA818 初始化默认为 `SA818_UHF`。

## 配置与持久化

### 配置结构

设备配置由以下三大部分组成：

- `WiFiConfig`
- `RadioConfig`
- `ServerConfig`

定义见 [`src/logic/device_config.h`](src/logic/device_config.h)。

### 默认服务器参数

当前默认值如下：

- UDP Host：`ptt.4l2.cn`
- UDP Port：`60050`
- HTTP API Base URL：`https://ptt.4l2.cn`

### NVS 命名空间

所有参数保存在 NVS 命名空间：

- `device_cfg`

### NVS 键说明

| 分类 | 键名 | 说明 |
| --- | --- | --- |
| Wi-Fi | `wifi_ssid` | Wi-Fi SSID |
| Wi-Fi | `wifi_pwd` | Wi-Fi 密码 |
| Wi-Fi | `wifi_dhcp` | DHCP 开关 |
| Wi-Fi | `wifi_ip` | 静态 IP |
| Wi-Fi | `wifi_gw` | 网关 |
| Wi-Fi | `wifi_mask` | 子网掩码 |
| Wi-Fi | `wifi_dns1` | DNS1 |
| Wi-Fi | `wifi_dns2` | DNS2 |
| 射频 | `radio_tx` | 发射频率，单位 `x10000 Hz` |
| 射频 | `radio_rx` | 接收频率，单位 `x10000 Hz` |
| 射频 | `radio_txt` | 发射亚音类型 |
| 射频 | `radio_txi` | 发射亚音索引 |
| 射频 | `radio_rxt` | 接收亚音类型 |
| 射频 | `radio_rxi` | 接收亚音索引 |
| 射频 | `radio_sql` | 静噪等级 |
| 射频 | `radio_bw` | 带宽，`false=窄带` |
| 射频 | `radio_pwr` | 功率高低状态 |
| 服务器 | `svr_call` | 呼号 |
| 服务器 | `svr_ssid` | 节点 SSID |
| 服务器 | `svr_dmr` | DMR ID |
| 服务器 | `svr_udp_h` | UDP 主机地址 |
| 服务器 | `svr_udp_p` | UDP 端口 |
| 服务器 | `svr_http` | HTTP API 基地址 |
| 服务器 | `svr_acc` | 账号 |
| 服务器 | `svr_pwd` | 设备准入密码 |
| UI | `ui_bl` | 背光亮度 PWM |

### 背光范围

背光亮度限制：

- 最小值：`20`
- 最大值：`255`

## 云端连接与协议

### 使用的协议

设备接入 DraARLv1 协议。

协议详细说明见：

- [`Protocol.md`](Protocol.md)

### 当前实现的核心报文

- `TypeHeartbeat (2)`：心跳
- `TypeConfig (3)`：配置同步
- `TypeOpus16K (5)`：Opus 16 kHz 语音
- `TypeServerVoice (6)`：服务器互联语音

### 配置同步支持项

当前设备实现的 `TypeConfig` TLV 配置项包括：

- `rx_freq`
- `tx_freq`
- `rx_ctcss`
- `tx_ctcss`
- `sql_level`
- `power_level`
- `tx_bandwidth`
- `timestamp`

### 配置同步行为

当前固件已经支持以下同步路径：

1. 服务器发送配置查询，设备返回当前本地配置。
2. 服务器发送配置下发，设备即时更新本地运行参数。
3. 服务器动态修改参数后，设备可实时接收并应用。
4. 本地用户修改参数后，设备会保存到 NVS，并按需上报服务器。
5. 仅在配置实际变化时才触发上报，避免无意义重复同步。
6. 服务器可发送时间同步包，设备更新本地系统时钟和界面显示。

### 主界面服务器状态图标

主界面有两个与云端相关的图标：

- `serverstat`
  - 未连通或链路未完成：显示 `cloud_alert`
  - UDP 鉴权成功并进入运行态：显示 `cloud_done`
- `configsync`
  - 配置同步开始时显示 `cloud_sync`
  - 配置同步完成后延迟约 1 秒隐藏

## 时间同步

时间由 [`src/logic/app_logic.cpp`](src/logic/app_logic.cpp) 统一管理。

当前行为如下：

- 使用服务器下发的 Unix 毫秒时间戳校时
- 本地时区固定为 `CST-8`，即中国标准时间 `UTC+8`
- 顶部 Header 时间每 1 秒刷新一次
- 设置页时间单独使用右对齐标签显示
- 若尚未完成时间同步，则显示占位内容 `--:--` / `--:--:--`

## 音频链路

### 编码参数

当前网络语音链路使用：

- 采样率：`16 kHz`
- 通道数：`1`
- 编码器：`Opus`
- 默认码率：`24 kbps`
- 单帧长度：`60 ms`
- 每次发送合并 `2` 帧

### 音频通道

- ES8388 负责音频编解码
- I2S 负责数据传输
- 网络下行语音可桥接到本地 RF 发射
- 本地音频可编码后上传至服务器

## BLE 配置能力

### BLE 服务

设备通过 NimBLE 暴露 GATT 服务，用于：

- 广播设备
- 显示动态认证码
- RPC 配置交互
- Wi-Fi 扫描
- Wi-Fi / Radio / Server 参数读取与写入

### Web Bluetooth 配置页

仓库附带浏览器端 BLE 配置页面：

- [`web/ble-config.html`](web/ble-config.html)

页面能力包括：

- 通过 Web Bluetooth 连接设备
- 输入设备屏幕上的动态码完成认证
- 扫描周围 Wi-Fi
- 写入 Wi-Fi 参数
- 写入 SA818 参数
- 写入 DraARL 服务器参数

建议使用支持 Web Bluetooth 的 Chromium 内核浏览器，并在 `https` 或 `localhost` 环境下打开。

## 编译环境

项目使用 **PlatformIO + Arduino Framework**。

### 已配置的板级环境

见 [`platformio.ini`](platformio.ini)：

- 环境名：`esp32s3-n16r8`
- 平台：`espressif32 @ ^6.0.1`
- 开发板：`esp32-s3-devkitc-1`
- 框架：`arduino`
- Flash：`16MB`
- CPU：`240MHz`
- Flash 模式：`qio`
- PSRAM：已启用

### 依赖库

- `LovyanGFX`
- `ArduinoJson`
- `esp32_opus`
- `FastLED`
- `ESP32RotaryEncoder`
- `NimBLE-Arduino`

## 分区表

分区表位于 [`partitions.csv`](partitions.csv)。

| 分区 | 类型 | 大小 | 说明 |
| --- | --- | --- | --- |
| `nvs` | data | `0x5000` | NVS 参数区 |
| `otadata` | data | `0x2000` | OTA 元数据 |
| `app0` | app | `0x300000` | OTA 固件槽位 0 |
| `app1` | app | `0x300000` | OTA 固件槽位 1 |
| `coredump` | data | `0x10000` | 崩溃转储 |
| `spiffs` | data | `0x9E0000` | SPIFFS 文件区 |

## 编译与烧录

### 1. 安装依赖

建议安装：

- VS Code
- PlatformIO IDE 插件

也可以直接使用命令行版 PlatformIO。

### 2. 克隆仓库

```bash
git clone git@github.com:Daofengql/DraARL-ESP32-RFv1.git
cd DraARL-ESP32-RFv1
```

如果你的本地目录名不同，不影响编译。

### 3. 编译

```bash
pio run -e esp32s3-n16r8
```

### 4. 烧录

```bash
pio run -e esp32s3-n16r8 -t upload
```

### 5. 串口监视

```bash
pio device monitor -b 115200
```

### 6. 清理构建产物

```bash
pio run -t clean
```

## 首次使用建议流程

1. 烧录固件并上电。
2. 长按 `KEY0` 进入启动流程。
3. 等待设备进入主界面。
4. 在设置页开启 BLE。
5. 使用手机或电脑打开 `web/ble-config.html`。
6. 通过 BLE 连接设备，输入屏幕显示的动态认证码。
7. 配置 Wi-Fi、射频参数和服务器参数。
8. 保存参数后等待设备联网。
9. 观察主界面 Wi-Fi、BLE、服务器和配置同步图标状态。

## 目录结构

```text
.
|-- platformio.ini          # PlatformIO 工程配置
|-- partitions.csv          # ESP32 分区表
|-- Protocol.md             # DraARLv1 协议说明
|-- src
|   |-- main.cpp            # 固件入口
|   |-- config.h            # 引脚定义
|   |-- drivers             # 硬件驱动
|   |-- logic               # 应用逻辑、配置、联网、同步
|   `-- ui                  # LVGL / SquareLine 生成界面
|-- lvglProj                # SquareLine 工程目录
`-- web
    `-- ble-config.html     # 浏览器 BLE 配置工具
```

## 开发说明

### 关于 UI 工程

- `src/ui` 为生成后的 LVGL 代码
- `lvglProj` 为 SquareLine 项目源文件
- 若重新导出界面，请注意同步检查对象名是否变化
- 逻辑层大量依赖固定对象名，例如 `ui_time`、`ui_serverstat`、`ui_configsync`

### 关于配置同步

- 本地射频修改会驱动运行时参数、生效到硬件、写入 NVS，并安排云端同步
- 服务器动态下发配置时，也会更新本地状态与 UI
- 若服务端请求当前配置，设备会主动回传完整射频配置

### 关于时间显示

- 设备本地并未独立实现 RTC 芯片逻辑
- 时间以服务端下发时间戳为准
- 若未联网或服务端未下发时间，界面保持占位符显示

### 关于浏览器配置页

- 页面依赖浏览器 Web Bluetooth 能力
- 页面样式依赖公网 CDN 资源
- 若离线使用，需要自行准备前端依赖并在安全上下文中部署

## 常见排查思路

### 设备不开机或界面无变化

- 检查 `KEY0` 是否工作正常
- 检查背光引脚和屏幕 SPI 连接
- 打开串口监视器查看启动日志

### Wi-Fi 无法连接

- 检查 SSID 和密码是否正确
- 若使用静态 IP，确认 IP、网关、掩码、DNS 填写正确
- 检查路由器频段和信号强度

### BLE 页面连不上设备

- 确认设备 BLE 已开启
- 确认浏览器支持 Web Bluetooth
- 确认当前页面运行在 `https` 或 `localhost`
- 确认输入了设备屏幕显示的动态码

### 服务器图标一直是告警状态

- 检查 Wi-Fi 是否已联网
- 检查服务器地址、端口、账号、准入密码是否正确
- 检查设备 `SSID` / `DMR ID` / `Callsign` 是否符合服务端要求
- 结合串口输出查看鉴权和 UDP 状态机日志

### 时间显示不正确

- 确认服务器发送的是 Unix 毫秒时间戳
- 确认服务端时间本身正确
- 当前设备按 `UTC+8` 显示本地时间

## 适合继续扩展的方向

- OTA 升级流程
- 更多 RF 参数或信道管理
- 更完善的状态页和诊断页
- 本地日志持久化
- 离线配置导入导出
- 更完整的服务器控制指令支持

## 相关文件

- [`src/main.cpp`](src/main.cpp)
- [`src/config.h`](src/config.h)
- [`src/logic/app_logic.cpp`](src/logic/app_logic.cpp)
- [`src/logic/edit_controller.cpp`](src/logic/edit_controller.cpp)
- [`src/logic/connectivity_manager.cpp`](src/logic/connectivity_manager.cpp)
- [`src/logic/net_audio_link.cpp`](src/logic/net_audio_link.cpp)
- [`src/logic/device_config.cpp`](src/logic/device_config.cpp)
- [`Protocol.md`](Protocol.md)

## 说明

本 README 主要描述当前仓库代码已经实现的功能与工程结构。后续如果协议、UI 资源、硬件原理图或服务端接口发生变化，建议同步更新本文档，避免界面命名、配置字段和协议细节脱节。
