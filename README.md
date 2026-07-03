# DraARL RFBox-1 Firmware

**DraARL-RFBox-1** 是 DraARL 系列第一代 RFBox 终端固件。当前代码面向带 RF 的手持/台式实验硬件：ESP32-S3 N16R8 主控、ST7789 屏幕、EC11 旋钮、SA818 UHF 射频模块、ES8388 音频编解码器，并通过 Wi-Fi 接入 DraARL 服务端。

固件重点覆盖四件事：

1. 本地射频终端控制：频率、亚音、静噪、带宽、功率、RF Guard。
2. 本地 UI 与离线部署入口：LVGL 页面、旋钮/按键交互、BLE GATT 配置。
3. 网络语音链路：DraARL UDP 鉴权、心跳、Opus 16 kHz 语音收发、网络到 RF 桥接。
4. 设备运维：NVS 参数持久化、服务器配置同步、时间同步、OTA 检查与升级。

当前固件版本定义在 [`src/config.h`](src/config.h)：`FIRMWARE_VERSION = 0.0.3`，`DEVICE_MODEL = 1`。

## 授权协议

本项目采用 [PolyForm Noncommercial License 1.0.0](LICENSE)。

- 允许个人学习、研究、实验和非商业二次开发。
- 禁止未经授权的商业使用、商用收费、商业集成或商业分发。
- 二次开发、修改版或再分发版本必须保留 `LICENSE` 中的 `Required Notice:` 行，并明确标注原仓库地址：`https://github.com/Daofengql/DraARL-RFBox-1`。
- 由于包含非商业限制，本项目更准确地说是 source-available / non-commercial source license，不是 OSI 定义下的开放源代码许可。

## 当前能力

- 320x240 LVGL 图形界面，包含启动页、主页面、设置页、RF 高级页和信息页。
- KEY0 长按开机，支持设置页开启自启动。
- EC11 旋钮负责页面导航、数值调整、确认和编辑状态切换。
- SA818 UHF 模块初始化与运行时参数下发。
- 支持 TX/RX 频率、TX/RX 亚音、静噪、宽/窄带、功率高低切换。
- 支持 RF Guard：单次发射时长限制、滑动窗口占空保护和网络语音过载保护。
- ES8388 + I2S 音频链路，Opus 16 kHz 单声道编码/解码。
- DraARL UDP 链路：HTTP 预检查、动态码绑定、UDP 鉴权、心跳、配置同步、语音收发。
- 服务端时间同步，界面按中国标准时间 `UTC+8` 显示。
- Wi-Fi STA 连接，支持 DHCP 或静态 IP。
- BLE GATT 配置入口，带 6 位动态认证码。
- 仓库附带 Web Bluetooth 配置页：[`web/ble-config.html`](web/ble-config.html)。
- OTA 检查与升级：定时检查、待更新状态缓存、MD5/SHA256 校验、更新按钮触发安装。
- PlatformIO 构建，并提供固件打包脚本：[`tools/package_firmware.py`](tools/package_firmware.py)。

## 硬件目标

当前工程默认面向如下硬件组合：

| 类别 | 当前实现 |
| --- | --- |
| 主控 | ESP32-S3，16MB Flash，8MB PSRAM |
| 显示 | ST7789，SPI，320x240 |
| 输入 | KEY0 电源/功能键，EC11 旋钮编码器 |
| 射频 | SA818 UHF 模块 |
| 音频 | ES8388 编解码器 + I2S |
| 网络 | Wi-Fi STA |
| 配置 | BLE GATT + Web Bluetooth |
| 存储 | NVS 参数区，SPIFFS 文件区 |

## 引脚定义

引脚集中定义在 [`src/config.h`](src/config.h)。

| 模块 | 信号 | GPIO |
| --- | --- | --- |
| TFT SPI | SCLK | 4 |
| TFT SPI | MOSI | 5 |
| TFT | RST | 6 |
| TFT | DC | 7 |
| TFT | CS | 15 |
| 背光 | PWM | 16 |
| 按键 | KEY0 | 17 |
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
| 预留网络串口 | TX | 48 |
| 预留网络串口 | RX | 45 |
| SA818 | TXD | 1 |
| SA818 | RXD | 2 |
| SA818 | EN | 42 |
| SA818 | SQL | 41 |
| SA818 | PTT | 40 |
| SA818 | PW | 39 |

## 软件结构

```text
.
|-- platformio.ini          # PlatformIO 工程配置
|-- partitions.csv          # 16MB Flash 分区表
|-- boards                  # ESP32-S3 N16R8 自定义板配置
|-- src
|   |-- main.cpp            # 固件入口、显示/LVGL/背光初始化、OTA 按钮事件
|   |-- config.h            # 固件版本、设备型号、引脚定义
|   |-- drivers             # 显示、EC11、I2C、I2S、ES8388、SA818、UART 驱动
|   |-- logic               # 应用状态机、配置、联网、语音链路、OTA
|   `-- ui                  # SquareLine Studio 导出的 LVGL 代码和资源
|-- lvglProj                # SquareLine Studio 工程
|-- web
|   `-- ble-config.html     # 浏览器端 BLE 配置工具
`-- tools
    `-- package_firmware.py # 固件产物打包脚本
```

### 启动流程

主入口在 [`src/main.cpp`](src/main.cpp)，应用状态机在 [`src/logic/app_logic.cpp`](src/logic/app_logic.cpp)。

1. 背光保持关闭，初始化串口并输出固件版本、内存信息。
2. 初始化 PWM 背光、显示驱动和 LVGL。
3. 分配 LVGL 双缓冲并加载 SquareLine UI。
4. 注册设置页 OTA 更新按钮事件。
5. 初始化应用逻辑，进入等待开机状态。
6. KEY0 长按 1 秒或启用自启动后进入启动页。
7. 依次初始化 SA818、SPIFFS、联网管理器、网络音频链路和 OTA 状态。
8. 加载主界面，开始 Wi-Fi、BLE、DraARL 链路、时间、输入和 UI 刷新循环。

### 主要模块

| 模块 | 职责 |
| --- | --- |
| [`src/logic/app_logic.cpp`](src/logic/app_logic.cpp) | 开机状态机、启动页进度、KEY0、时钟刷新、OTA 定时检查、输入门控 |
| [`src/logic/edit_controller.cpp`](src/logic/edit_controller.cpp) | 主界面/设置页/RF 页/信息页交互，射频参数编辑和持久化 |
| [`src/logic/connectivity_manager.cpp`](src/logic/connectivity_manager.cpp) | Wi-Fi 状态机、BLE GATT、动态码认证、BLE RPC 配置 |
| [`src/logic/net_audio_link.cpp`](src/logic/net_audio_link.cpp) | DraARL HTTP/UDP 状态机、绑定、鉴权、心跳、配置同步、语音桥接、RF Guard |
| [`src/logic/device_config.cpp`](src/logic/device_config.cpp) | Wi-Fi、Radio、Server、OTA、本地 UI 配置的默认值、校验和 NVS 读写 |
| [`src/logic/ota_update.cpp`](src/logic/ota_update.cpp) | 固件版本检查、下载、哈希校验、写入 OTA 分区 |
| [`src/drivers/sa818_driver.cpp`](src/drivers/sa818_driver.cpp) | SA818 AT 指令封装、频率/亚音/静噪/带宽/功率/PTT 控制 |

## UI 与输入

当前 UI 页面：

- `StartUP`：启动日志与进度条。
- `main`：时间、Wi-Fi/BLE/服务器/同步状态、频率和射频状态。
- `SettingPAGE`：亮度、BLE、自启动、更新、高级 RF、关于。
- `RFPAGE`：功率与 RF Guard 参数。
- `InfoPage`：设备与运行状态信息。

输入规则：

- `KEY0` 长按：等待开机时启动设备；主界面中打开电源菜单。
- `KEY0` 短按：主界面中切换/退出当前编辑或页面状态。
- `EC11 旋转`：移动焦点、选择频率位、调整数值。
- `EC11 按压`：确认、进入页面、切换编辑状态。

发射或网络到 RF 桥接期间，应用会暂停本地输入轮询，避免 PTT 活动时误触发 UI 操作。

## 射频功能

默认运行在 SA818 UHF 模式，频率范围限制为：

- 最小：`400.0000 MHz`
- 最大：`470.0000 MHz`

默认射频参数：

| 参数 | 默认值 |
| --- | --- |
| TX 频率 | `439.5000 MHz` |
| RX 频率 | `438.5000 MHz` |
| TX 亚音 | `CTCSS 88.5` |
| RX 亚音 | `CTCSS 88.5` |
| 静噪 | `4` |
| 带宽 | 窄带 |
| 功率 | 高 |
| RF Guard | 开启 |
| 单次 TX 限制 | `30 s` |
| 统计窗口 | `300 s` |
| 窗口内最大 TX | `60 s` |

RF 功率通过 `SA818_PW` 控制：

- 高功率：`PW` 高阻态。
- 低功率：`PW` 拉低。

射频参数变更后会立即应用到运行时状态，写入 NVS，并在网络链路可用时安排配置同步。

## 配置与 NVS

配置结构定义在 [`src/logic/device_config.h`](src/logic/device_config.h)，保存在 NVS 命名空间 `device_cfg`。

| 分类 | 键名 | 说明 |
| --- | --- | --- |
| Wi-Fi | `wifi_ssid` | SSID |
| Wi-Fi | `wifi_pwd` | 密码 |
| Wi-Fi | `wifi_dhcp` | DHCP 开关 |
| Wi-Fi | `wifi_ip` | 静态 IP |
| Wi-Fi | `wifi_gw` | 网关 |
| Wi-Fi | `wifi_mask` | 子网掩码 |
| Wi-Fi | `wifi_dns1` | DNS1 |
| Wi-Fi | `wifi_dns2` | DNS2 |
| Radio | `radio_tx` | TX 频率，单位 MHz x10000 |
| Radio | `radio_rx` | RX 频率，单位 MHz x10000 |
| Radio | `radio_txt` | TX 亚音类型 |
| Radio | `radio_txi` | TX 亚音索引 |
| Radio | `radio_rxt` | RX 亚音类型 |
| Radio | `radio_rxi` | RX 亚音索引 |
| Radio | `radio_sql` | 静噪等级 |
| Radio | `radio_bw` | 宽带开关 |
| Radio | `radio_pwr` | 高功率开关 |
| Radio | `radio_rfg_en` | RF Guard 开关 |
| Radio | `radio_rfg_stx` | RF Guard 单次 TX 秒数 |
| Radio | `radio_rfg_win` | RF Guard 统计窗口秒数 |
| Radio | `radio_rfg_max` | RF Guard 窗口内最大 TX 秒数 |
| Server | `svr_call` | 呼号 |
| Server | `svr_ssid` | 节点 SSID |
| Server | `svr_dmr` | DMR ID |
| Server | `svr_udp_h` | UDP 主机 |
| Server | `svr_udp_p` | UDP 端口 |
| Server | `svr_http` | HTTP API 基地址 |
| Server | `svr_acc` | 账号 |
| Server | `svr_pwd` | 设备准入密码 |
| OTA | `ota_auto` | 自动检查更新 |
| OTA | `ota_last` | 上次检查时间 |
| OTA | `ota_ver` | 待更新版本 |
| OTA | `ota_pend` | 是否存在待更新 |
| OTA | `ota_url` | 固件下载地址 |
| OTA | `ota_hash` | 固件哈希 |
| OTA | `ota_alg` | 哈希算法 |
| OTA | `ota_size` | 固件大小 |
| UI | `ui_bl` | 背光亮度 PWM |
| UI | `ui_ast` | 自启动开关 |

默认服务器参数：

- UDP Host：`ptt.4l2.cn`
- UDP Port：`60050`
- HTTP API Base URL：`https://ptt.4l2.cn`

背光亮度保存为 PWM 值，范围 `20` 到 `255`。

## DraARL 网络链路

网络链路由 [`src/logic/net_audio_link.cpp`](src/logic/net_audio_link.cpp) 实现。状态机大致为：

```text
WAIT_WIFI -> PRECHECK -> REQUEST_CODE / CONFIRM_BIND -> RESOLVE_SERVER -> UDP_AUTH_SEND -> UDP_AUTH_WAIT -> RUNNING
```

链路能力：

- 若本地没有账号或设备准入密码，会请求动态绑定码并在屏幕上显示。
- 通过 HTTP 接口完成预检查、请求绑定码、确认绑定。
- DNS 解析服务器地址后建立 UDP 会话。
- 使用 DraARL 包头发送心跳完成 UDP 鉴权。
- 运行态定时发送心跳，并接收配置、时间和语音包。
- 主界面用云端图标表示服务器可用状态，用同步图标表示配置同步动作。

当前实现的 UDP 类型：

| 类型 | 含义 |
| --- | --- |
| `2` | Heartbeat |
| `3` | Config |
| `5` | Opus 16 kHz |
| `6` | Server Voice |

配置包支持：

- `CONFIG_CMD_QUERY`
- `CONFIG_CMD_APPLY`
- `CONFIG_CMD_TIME`

支持同步的配置项包括 RX/TX 频率、CTCSS、亚音模式和值、静噪、功率、带宽、RF Guard 参数和时间戳。

## 音频链路

音频初始化顺序：

1. I2C。
2. ES8388。
3. I2S。
4. Opus 编码器/解码器。

当前网络语音参数：

| 参数 | 值 |
| --- | --- |
| 采样率 | `16 kHz` |
| 通道 | 单声道 |
| Opus 码率 | `24 kbps` |
| Opus 复杂度 | `1` |
| 单帧 | `60 ms` |
| 每包合并 | `2` 帧 |

网络下行语音可触发本地 RF PTT 并桥接到 SA818；本地音频可编码为 Opus 后通过 DraARL UDP 发送。

## BLE 配置

BLE 设备名格式为：

```text
DraARL-XXXXXX
```

其中 `XXXXXX` 来自 ESP32 eFuse MAC 的低 24 位。

GATT UUID：

| 用途 | UUID |
| --- | --- |
| Service | `6d22f67d-7287-4f4e-8548-b362f9b1f001` |
| Status | `6d22f67d-7287-4f4e-8548-b362f9b1f002` |
| Auth | `6d22f67d-7287-4f4e-8548-b362f9b1f003` |
| RPC TX | `6d22f67d-7287-4f4e-8548-b362f9b1f004` |
| RPC RX | `6d22f67d-7287-4f4e-8548-b362f9b1f005` |

RPC 命令：

- `get_status`
- `get_config`
- `scan_wifi`
- `set_wifi`
- `set_radio`
- `set_server`
- `disconnect`

除 `get_status` 外，RPC 需要先写入屏幕显示的 6 位动态码完成认证。

Web Bluetooth 配置页位于 [`web/ble-config.html`](web/ble-config.html)。建议使用 Chromium 内核浏览器，并在 `https` 或 `localhost` 安全上下文中打开。

## OTA

OTA 逻辑由 [`src/logic/ota_update.cpp`](src/logic/ota_update.cpp) 实现。

检查地址格式：

```text
{http_api_base_url}/api/public/firmware/latest?dev_model={DEVICE_MODEL}&current_version={FIRMWARE_VERSION}
```

行为：

- 主界面进入后，Wi-Fi 就绪时进行首次检查。
- 后续按 1 小时间隔检查。
- 服务端返回新版本后，将版本、下载地址、哈希、文件大小保存到 NVS。
- UI 显示更新提示，设置页更新按钮变为可用。
- 点击更新按钮后下载固件，支持 MD5 或 SHA256 校验，通过后写入 OTA 分区并重启。
- 重启后如果当前版本已经不低于待更新版本，会清理 pending 状态。

## 编译与烧录

项目使用 **PlatformIO + Arduino Framework**。

仓库地址：

```bash
git clone git@github.com:Daofengql/DraARL-RFBox-1.git
cd DraARL-RFBox-1
```

环境名：`esp32s3-n16r8`

主要配置见 [`platformio.ini`](platformio.ini)：

- Platform：`espressif32 @ ^6.0.1`
- Board：`esp32-s3-devkitc-1-n16r8`
- Framework：`arduino`
- Flash：`16MB`
- Flash mode：`qio`
- CPU：`240MHz`
- PSRAM：启用
- Monitor：`115200`

依赖库：

- `LovyanGFX`
- `ArduinoJson`
- `esp32_opus`
- `ESP32RotaryEncoder`
- `NimBLE-Arduino`

### 构建

```bash
pio run -e esp32s3-n16r8
```

### 烧录

```bash
pio run -e esp32s3-n16r8 -t upload
```

### 串口监视

```bash
pio device monitor -b 115200
```

### 打包固件

```bash
python tools/package_firmware.py --env esp32s3-n16r8 --version 0.0.3
```

打包脚本会收集 bootloader、分区表、固件和可用的 `boot_app0.bin`，生成 `flash_manifest.json`、`flash_args.txt`、`SHA256SUMS.txt` 和 zip 包。

## 分区表

分区表位于 [`partitions.csv`](partitions.csv)。

| 分区 | 类型 | Offset | Size | 说明 |
| --- | --- | --- | --- | --- |
| `nvs` | data/nvs | `0x9000` | `0x5000` | NVS 参数 |
| `otadata` | data/ota | `0xe000` | `0x2000` | OTA 元数据 |
| `app0` | app/ota_0 | `0x10000` | `0x300000` | OTA 固件槽 0 |
| `app1` | app/ota_1 | `0x310000` | `0x300000` | OTA 固件槽 1 |
| `coredump` | data/coredump | `0x610000` | `0x10000` | 崩溃转储 |
| `spiffs` | data/spiffs | `0x620000` | `0x9E0000` | SPIFFS 文件区 |

## 首次使用流程

1. 编译并烧录固件。
2. 上电后长按 `KEY0` 进入启动流程，或提前在 NVS 中启用自启动。
3. 等待主界面加载。
4. 若没有 Wi-Fi 配置或连续连接失败，设备会自动开启 BLE 配置弹窗。
5. 用支持 Web Bluetooth 的浏览器打开 [`web/ble-config.html`](web/ble-config.html)。
6. 连接 `DraARL-XXXXXX` 设备并输入屏幕上的 6 位动态码。
7. 写入 Wi-Fi、射频和服务器配置。
8. 等待 Wi-Fi 连接、HTTP 预检查、绑定或 UDP 鉴权完成。
9. 观察主界面的 Wi-Fi、BLE、服务器和配置同步图标。

## 常见排查

### 无法进入主界面

- 确认 `KEY0` 硬件连接和上拉状态。
- 检查背光、屏幕 SPI 和供电。
- 打开串口监视器查看启动进度日志。

### SA818 初始化失败

- 检查 SA818 供电和 EN/PTT/PW/SQL 接线。
- 确认 TXD/RXD 与 ESP32 连接方向正确。
- 查看串口中的 `[SA818]` AT 指令日志。

### Wi-Fi 连接失败

- 检查 SSID、密码、路由器频段。
- 如果使用静态 IP，确认 IP、网关、掩码、DNS。
- 连续失败后可通过 BLE 配置页重新写入网络参数。

### BLE 配置页无法连接

- 确认设备端 BLE 已开启或处于自动配网页。
- 确认浏览器支持 Web Bluetooth。
- 确认页面运行在 `https` 或 `localhost`。
- 重新开启 BLE 后动态码会变化，需要输入最新 6 位码。

### 服务器图标保持告警

- 确认 Wi-Fi 已连接。
- 检查 HTTP API 地址、UDP 主机、UDP 端口。
- 检查账号、设备准入密码、节点 SSID、DMR ID 和呼号。
- 若设备未绑定，按屏幕动态码在 Web 控制台完成绑定。
- 结合串口中的 `[NET]`、`[OTA]`、`[TIME]` 日志定位阶段。

### 时间不显示或显示占位符

- 时间依赖服务端下发 Unix 毫秒时间戳。
- 未完成网络时间同步前，界面会显示 `--:--`。
- 当前时区固定为 `CST-8`，即 `UTC+8`。

## 开发注意

- `src/ui` 是 SquareLine Studio 导出的代码，重新导出后需要检查对象名是否变化。
- 逻辑层依赖多个固定 UI 对象名，例如 `ui_time`、`ui_serverstat`、`ui_configsync`、`ui_updateBT`。
- 网络、BLE 和 Wi-Fi 回调不会直接操作 LVGL，而是设置事件标志后由主循环消费。
- 配置变更路径需要同时考虑运行时状态、NVS、UI 刷新和云端同步。
- OTA 与其他 HTTP 请求通过 `http_request_gate` 做互斥，避免并发请求互相打断。
- 当前 README 描述的是本仓库现有实现，硬件原理图、服务端协议文档和量产流程若独立维护，应在发布时同步补齐链接。
