# 智能停车场管理系统项目说明文档

> 最后更新：2026-08-21。本文档覆盖 CC2530 车位节点、STM32 网关、树莓派网关（车牌识别 + 华为云 MQTT）三部分；微信小程序与网页端不在本文档范围内。

## 1. 项目概述

本项目是基于物联网技术的智能停车场管理系统，采用 **ZigBee + 树莓派 + 华为云 MQTT** 的分层架构，实现车位实时检测、车辆进出管理、车牌识别与云端数据上报。

- **CC2530 车位节点**：ZigBee 无线网络 + HC-SR04 超声波测距，周期上报车位占用状态
- **STM32F103 网关**：串口汇聚、红外触发、RFID 刷卡验证、道闸控制、OLED 显示；不直接联网
- **树莓派 Zero WH**：车牌拍照识别 + 华为云 MQTT 上报与命令中转
- **华为云 IoTDA**：设备影子、属性上报、命令下发，供应用层（小程序等）使用

### 1.1 系统架构

```
┌──────────────────────────────────────────────┐
│                华为云 IoTDA                   │
│   设备影子 / 属性上报 / 命令下发 (MQTTS 8883) │
└──────────────────────┬───────────────────────┘
                       │ MQTT over TLS
                       ▼
┌──────────────────────────────────────────────┐
│           树莓派 Zero WH 网关                 │
│   plate_gateway.py: MQTT + 串口 + 拍照识别    │
└──────────────────────┬───────────────────────┘
                       │ USART2 115200 文本协议
                       ▼
┌──────────────────────────────────────────────┐
│            STM32F103C8T6 网关                 │
│   RFID(PA4-7) 红外(PA11) 舵机(PA1) OLED(I2C) │
│   USART3(PB10/11) <- CC2530 协调器           │
└──────────────────────┬───────────────────────┘
                       │ ZigBee 无线网络
                       ▼
┌──────────────────────────────────────────────┐
│        CC2530 车位节点（HC-SR04）            │
│   周期上报 PXX:XXXcm 距离帧                   │
└──────────────────────────────────────────────┘
```

### 1.2 目录结构

| 目录 | 职责 |
|---|---|
| `CC2530_ParkingNode/` | Z-Stack 工程（IAR），车位节点与协调器同一工程，跳线选择角色 |
| `STM32_Gateway/` | Keil 工程，网关外设主控与串口汇聚 |
| `raspberrypi/` | 树莓派网关：MQTT 云端 + 串口 + 车牌识别 |
| `WechatProject/` | 微信小程序（本文档不展开） |

## 2. 数据流

### 2.1 车位检测数据流

```
HC-SR04 测距 -> CC2530 节点（每 100ms + 随机抖动）
  -> ZigBee P2P 帧 PXX:XXXcm -> 协调器 UART0 原样转发
  -> STM32 USART3 中断解析（距离 < 20cm 判占用）
  -> spot_occupied[] 变化 -> SPOT:<id>:<0|1> + SPOTS:<n>
  -> 树莓派 -> 华为云 Spot_state 属性上报
```

### 2.2 车辆进出数据流

```
红外遮挡（PA11 EXTI11）-> 状态机进入 WAITING_RFID
  -> MFRC522 读卡 -> Flash 白名单验证
  -> 有效且有车位：抬杆（BARRIER:OPEN）-> 3s 后落杆（BARRIER:CLOSE）
  -> 无效卡 / 车位已满：提示后回 IDLE
红外触发同时发送 CAPTURE -> 树莓派拍照识别 -> PLATE 回传 OLED 显示
```

### 2.3 车牌识别数据流

```
CAPTURE（STM32 红外触发，或本地 echo capture > /tmp/plate_cmd）
  -> rpicam-still 拍照（2592x1944）
  -> HSV 蓝牌定位 -> minAreaRect 透视校正 -> CLAHE -> Tesseract OCR
  -> 6 位窗口后处理 -> plate_db.json 模糊匹配
  -> 上报 PlateRecognition.plate_number
  -> PLATE:<车牌> 回 STM32 -> OLED 第 4 行中文显示
```

## 3. 硬件平台

### 3.1 CC2530 车位检测节点

| 硬件组件 | 型号 | 功能描述 |
|---------|------|---------|
| MCU | TI CC2530F256 | ZigBee SoC，8051 内核 + 2.4GHz 无线收发器 |
| 超声波传感器 | HC-SR04 | TRIG=P0.4、ECHO=P0.5，距离测量判车位占用 |
| 晶振 | 32MHz | 系统时钟 |
| 天线 | PCB 天线 | ZigBee 无线通信 |

### 3.2 STM32 网关

| 硬件组件 | 型号 | 引脚/接口 | 功能 |
|---------|------|---------|------|
| MCU | STM32F103C8T6 | - | Cortex-M3，72MHz，64KB Flash |
| RFID 读卡器 | MFRC522 | 软件 SPI：SDA=PA4 SCK=PA5 MOSI=PA7 MISO=PA6 RST=PB1 | 车辆身份验证 |
| 红外传感器 | 红外对射 | PA11 / EXTI11 | 车辆到达检测 |
| 舵机 | SG90 | TIM2_CH2 PWM，PA1 | 道闸抬杆/落杆 |
| OLED 显示屏 | 0.96 寸 SSD1306 | 软件 I2C | 4 行显示，含 16x16 中文 |
| 串口 1（调试） | USART1 | PA9/PA10，115200 | 日志输出 + 白名单命令 |
| 串口 2（树莓派） | USART2 | PA2/PA3，115200 | 与树莓派通信 |
| 串口 3（协调器） | USART3 | PB10/PB11，115200 | 接收 CC2530 车位帧 |
| Flash 白名单 | 内置 | 0x08007000 | RFID 白名单持久化（最多 20 张） |
| WiFi 模块 | ESP8266 | - | 已停用（WIFI_ENABLE=0，联网移至树莓派） |

### 3.3 树莓派 Zero WH

| 项目 | 值 |
|---|---|
| 型号 | Zero WH（ARMv6，512MB） |
| 摄像头 | CSI（rpicam-still 拍照，2592x1944） |
| 串口 | /dev/serial0 @ 115200（接 STM32 USART2，需共地） |
| 网络 | WiFi 连接华为云（MQTTS 8883） |
| 软件 | Python3 + pyserial + paho-mqtt + OpenCV + pytesseract/Tesseract(chi_sim) |

## 4. 软件架构

### 4.1 CC2530_ParkingNode（IAR EW8051）

关键文件：`Source/ParkingNode.c`、`Source/ParkingNode.h`、`Source/OSAL_ParkingNode.c`

核心功能：
- 角色选择：跳线（`readCoordinatorJumper`）决定协调器 / 路由 / 终端角色，同一工程烧录
- 节点测距：HC-SR04 触发 20us 脉冲、回波计时，距离(cm) = 计时(us) / 58；上报周期 100ms + 0~255ms 随机抖动
- 数据帧：`PXX:XXXcm`（9 字节）正常数据；距离 >= 1000cm 时 `PXX:err`（7 字节）
- 上报：P2P 簇（Cluster 4）单播到协调器（0x0000）；协调器收到后经 UART0 原样转发并追加 `\r\n` 给 STM32 USART3
- 配置（`ParkingNode.h`）：端点 `PARKINGNODE_ENDPOINT=20`、Profile `0x0F08`、车位号 `PARKINGNODE_SPOT_NUM`（每个节点改一处）

### 4.2 STM32_Gateway（Keil MDK）

主要模块：
| 文件 | 职责 |
|---|---|
| `User/main.c` | 主循环：状态机、OLED 刷新、pi_link 处理、车位变化上报 |
| `User/state_machine.c` | 车辆进出状态机、剩余车位管理 |
| `Hardware/pi_link.c` | USART2 与树莓派通信（文本行协议） |
| `Hardware/Serial.c` | USART3 接收 CC2530 帧并解析车位状态 |
| `Hardware/USART.c` | USART1 调试串口 + `addcard/delcard/listcards` 白名单命令 |
| `Hardware/OLED.c` | OLED 软 I2C 驱动 + 16x16 中文字库（31 省份简称） |
| `Hardware/MFRC522.c` / `Infrared.c` / `Servo.c` / `flash_storage.c` | 外设驱动 |
| 遗留模块 | `esp8266.c`、`mqtt_report.c` 在 `WIFI_ENABLE=0` 时休眠；`cc2530_parser.c` 为无人调用的死代码（二进制帧解析），Keil 工程仍编译 |

串口总览：
| 串口 | 引脚 | 波特率 | 对端 | 用途 |
|---|---|---|---|---|
| USART1 | PA9/PA10 | 115200 | 调试终端 | 日志 + 白名单增删查命令 |
| USART2 | PA2/PA3 | 115200 | 树莓派 /dev/serial0 | 上行 SPOTS/SPOT/CARD/BARRIER/CAPTURE/LINK:OK，下行 PLATE/OPEN/CLOSE |
| USART3 | PB10/PB11 | 115200 | CC2530 协调器 | 接收 `PXX:XXXcm` 车位帧 |

状态机（`state_machine.c`）：
| 状态 | 描述 | 触发/退出 |
|---|---|---|
| `STATE_IDLE` | 空闲 | 红外遮挡 -> WAITING_RFID |
| `STATE_WAITING_RFID` | 等待刷卡 | 读卡验证通过且有车位 -> BARRIER_UP；红外离开或验证失败回 IDLE |
| `STATE_BARRIER_UP` | 抬杆 | 3s 后自动 -> BARRIER_DOWN |
| `STATE_BARRIER_DOWN` | 落杆 | 1s 后 -> IDLE |

RFID 验证流程：红外触发 -> 等待刷卡 -> MFRC522_Request/Anticoll 取卡号 -> `FlashStorage_IsCardValid` 查白名单（Flash 0x08007000，默认卡 7506D506）-> 有剩余车位则抬杆并回发 `BARRIER:OPEN`，无效卡/车位已满提示 3s 后回 IDLE

OLED 显示：第 1 行标题、第 2 行 `Spots: x/20`、第 3 行状态、第 4 行 `Plate:<车牌>`（16x16 中文，方向已修复）

开机流程：`LINK:OK` 握手 -> 补发初始 `SPOTS:20` -> 等待串口数据

### 4.3 raspberrypi（树莓派网关）

文件清单：
| 文件 | 职责 |
|---|---|
| `plate_gateway.py` | 主程序：MQTT 云端 + 串口 + 拍照识别集成 |
| `plate_recognition.py` | 车牌定位与 OCR |
| `plate_matcher.py` | 车牌库模糊匹配（加权编辑距离） |
| `plate_camera.py` | 手动拍照 -> 识别工具 |
| `plate_db.json` | 车牌库：冀T234J5 / 鄂EM456U / 湘K4Z567 |
| `cloud_config.json` | 华为云 MQTT 连接与串口配置 |
| `huawei_iotda_product_model.json` | 云端产品模型（已导入） |
| `test/` | 测试图：test.jpg->T234J5、test2.jpg->EM456U、test3.jpg->K4Z567 |
| `archive/` | 历史备份（旧 pi_cloud.py 等） |

`plate_gateway.py` 功能：
- 串口线程 `watch_serial`：解析 STM32 上行消息并按协议映射为华为云属性上报
- FIFO 线程 `watch_fifo`：监听 `/tmp/plate_cmd`，支持无 STM32 的本地调试触发
- 拍照互斥：`CAPTURE_STATE` 锁保证同一时刻只有一次拍照+OCR，忙时挂起补拍一次；`rpicam-still` 失败重试 3 次
- 云端命令：`open_barrier` / `close_barrier`（转发 OPEN/CLOSE 给 STM32，5s 超时等待 `BARRIER:` 应答后回执 success/failed）、`query_status`（按缓存直接应答）、`capture_photo`
- 断线自动重连 MQTT；串口异常关闭后 5s 重试

车牌识别（`plate_recognition.py`）流程：
HSV 蓝色掩码（H 90-135）-> 形态学开闭 -> 候选轮廓（宽高比 2.0~4.0、目标 3.15、最小面积 5000）-> minAreaRect 透视校正 -> CLAHE 增强 -> Tesseract LSTM OCR（psm 8->13->7，白名单 A-Z0-9）-> 校正图 OCR 为空时回退识别蓝色包围盒 -> 6 位滑动窗口按车牌特征打分（保护城市字母位），修正 T/7/1 与 Z/7 误读

车牌库匹配（`plate_matcher.py`）：对 OCR 文本做 5 位起滑动窗口 + 加权编辑距离，配合易混字符表（如 T<->7/1、0<->O、8<->B），相似度 >= 0.80 时采用库内完整车牌，消除单字符误读

## 5. 通信协议

### 5.1 ZigBee（CC2530 节点 -> 协调器 -> STM32）

- 节点 -> 协调器：ZigBee P2P 簇（Cluster 4），Payload 为 ASCII 文本帧：
  - `PXX:XXXcm`：车位号 XX（01-20）+ 距离 XXX cm（9 字节）
  - `PXX:err`：距离超限 >= 1000cm（7 字节）
- 协调器 -> STM32：UART0 原样转发文本帧并追加 `\r\n`（STM32 USART3，115200 8N1）

### 5.2 STM32 <-> 树莓派串口协议（USART2）

串口：STM32 USART2 (PA2/PA3) <-> 树莓派 /dev/serial0，波特率 115200，8N1

帧格式：ASCII 文本行，以 `\r\n`（或 `\n`）结尾，单行不超过 64 字节

**STM32 -> 树莓派（事件上报）**

| 消息 | 含义 | 树莓派动作（华为云上报） |
|---|---|---|
| `SPOTS:<n>` | 剩余车位数 | Spot_state.remaining_spots |
| `SPOT:<id>:<0/1>` | 车位 id(1起) 状态，1=占用 | Spot_state.Parking_id + Parking_state(occupied/empty) |
| `CARD:<8位HEX>` | 刷卡卡号(4字节) | Spot_state.card_id |
| `BARRIER:OPEN` | 道闸打开（含远程命令应答） | ParkingBarrier.barrier_state=up |
| `BARRIER:CLOSE` | 道闸关闭（含远程命令应答） | ParkingBarrier.barrier_state=down |
| `CAPTURE` | 车辆到达（红外触发），请求拍照识别 | 拍照->OCR->车牌库匹配->上报 PlateRecognition.plate_number |

**树莓派 -> STM32（下行控制/结果）**

| 消息 | 含义 | STM32 动作 |
|---|---|---|
| `PLATE:<车牌>` | 识别结果（UTF-8 中文+ASCII） | OLED 第 4 行显示 Plate:<车牌> |
| `PLATE:NONE` | 未识别到车牌 | OLED 第 4 行恢复占位 |
| `OPEN` | 云端 open_barrier 命令 | 抬杆，回发 BARRIER:OPEN |
| `CLOSE` | 云端 close_barrier 命令 | 落杆，回发 BARRIER:CLOSE |

**云端命令**

- 产品模型命令（服务 ParkingBarrier）：open_barrier / close_barrier / query_status
- 树莓派收到命令后通过串口转发 OPEN/CLOSE 给 STM32，等待 BARRIER:OPEN/CLOSE 应答（5s 超时）后向云端回执 result=success/failed
- query_status 由树莓派根据已缓存状态直接应答（remaining_spots/plate_number/barrier_state）

**触发方式**

- STM32 红外（EXTI11）触发 -> 状态机发 CAPTURE 给树莓派 -> 拍照识别 -> 上报 + 回传 PLATE 显示
- 备用本地触发（无 STM32 调试）：`echo capture > /tmp/plate_cmd`

### 5.3 华为云 IoTDA 接入（树莓派）

连接配置（`cloud_config.json`）：
| 参数 | 值 |
|---|---|
| Broker | `53b674367f.st1.iotda-device.cn-south-1.myhuaweicloud.com:8883`（MQTTS） |
| 设备 ID | `6a85594cc00ccb6d4b701321_raspberrypi-zeroWH` |
| CA 证书 | `/etc/ssl/certs/ca-certificates.crt` |
| 上报 Topic | `$oc/devices/{device_id}/sys/properties/report` |
| 命令 Topic | `$oc/devices/{device_id}/sys/commands/#` |

产品模型（`huawei_iotda_product_model.json`，已导入云端）：
| 服务 | 属性 | 命令 |
|---|---|---|
| Spot_state | Parking_id(string)、Parking_state(empty/occupied)、remaining_spots(0-20)、card_id(string) | - |
| ParkingBarrier | barrier_state(up/down/opening/closing) | open_barrier / close_barrier / query_status |
| PlateRecognition | plate_number(string) | - |

注意：`Parking_id` + `Parking_state` 为单值属性，多车位上报会互相覆盖影子；小程序用本地缓存增量更新兜底（需改产品模型时另行讨论）。

## 6. 项目配置参数

### 6.1 STM32 配置（`project_config.h`）

```c
#define WIFI_ENABLE           0        // 0=离线模式（云端由树莓派处理），1=STM32 直连 ESP8266
#define TOTAL_SPOTS           20       // 总车位数
#define MAX_SPOT_ID           20       // 最大车位 ID（spot_occupied 数组长度）
#define PARKING_THRESHOLD_CM  20       // 车位占用阈值(cm)，距离小于该值判占用
#define MAX_WHITELIST_CARDS   20       // RFID 白名单最大张数
#define FLASH_SECTOR_ADDR     0x08007000 // 白名单存储扇区
```

### 6.2 时间配置

```c
#define RFID_TIMEOUT_MS       5000     // RFID 超时时间
#define BARRIER_UP_DELAY_MS   3000     // 道闸抬起延时
#define BARRIER_DOWN_DELAY_MS 1000     // 道闸落下延时
#define ERROR_DISPLAY_DELAY_MS 3000    // 错误提示显示时长
#define HEARTBEAT_INTERVAL_S  30       // 心跳间隔(秒，仅 WIFI_ENABLE=1 时生效)
```

### 6.3 车位节点配置（`ParkingNode.h`）

```c
#define PARKINGNODE_ENDPOINT           20   // 端点号
#define PARKINGNODE_SPOT_NUM           1    // 当前车位号（每节点修改）
#define PARKINGNODE_SEND_PERIODIC_MSG_TIMEOUT 100 // 上报周期(ms)，叠加随机抖动
```

## 7. 编译与部署

### 7.1 CC2530 节点（IAR）

1. 打开 `ParkingNode.eww` 工作空间，选择 ParkingNode 工程
2. 目标板类型 CC2530EB，编译生成 `.hex`
3. TI Flash Programmer 下载；每块节点板修改 `PARKINGNODE_SPOT_NUM`，跳线决定协调器/节点角色

### 7.2 STM32 网关（Keil）

1. 打开 `Project.uvprojx`，Target 1（STM32F103C8），编译生成 `.axf`
2. ST-Link/J-Link 下载；`UV4.exe` 被占用时可用命令行语法检查：
   `arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -DSTM32F10X_MD -DUSE_STDPERIPH_DRIVER -I Start -I Library -I User -I Hardware -I System -fsyntax-only <file>`

### 7.3 树莓派网关

- 项目目录 `~/parking_gateway`，venv：`~/.venv`（OpenCV 4.10 / pytesseract / paho-mqtt / pyserial / Tesseract 5.5 + chi_sim）
- 启动：`cd ~/parking_gateway && source .venv/bin/activate && setsid nohup python3 plate_gateway.py >/tmp/gateway.log 2>&1 < /dev/null &`（无 setsid 时用 nohup）
- 重启：`pkill -f '[p]late_gateway.py'`
- 查看日志：`tail -n 30 /tmp/gateway.log`
- 本地触发：`echo capture > /tmp/plate_cmd`
- SSH 工具：plink/pscp（PuTTY）；批量脚本建议 base64 后 `echo $b64 | base64 -d | bash`

## 8. 系统特性总结

| 特性 | 描述 |
|------|------|
| 无线传感 | ZigBee 车位采集（100ms 周期 + 随机抖动） |
| 实时检测 | 超声波 <20cm 判占用，状态变化即上报 |
| 身份验证 | RFID 白名单（Flash 持久化，CLI 增删查） |
| 自动控制 | 状态机抬/落杆，红外联动，远程命令 |
| 车牌识别 | 拍照 OCR + 车牌库模糊纠错 |
| 云端集成 | 树莓派 MQTT 上报与命令下发（STM32 不直连） |
| 中文显示 | OLED 16x16 中文字库（31 省份简称） |
| 离线调试 | /tmp/plate_cmd 本地触发拍照 |

## 9. 注意事项

1. **ZigBee 网络**：首次部署协调器先启动，节点自动入网
2. **共地**：STM32 / 树莓派 / CC2530 必须共地，否则串口数据不可靠（此前刷卡不上报、云命令无响应的根因）
3. **串口电平**：USART3 与协调器、USART2 与树莓派均需电平匹配
4. **源码编码**：STM32 源码为 UTF-8，修改注意保持编码，避免中文注释乱码
5. **/tmp 易失**：`/tmp/gateway.log`、`/tmp/plate_cmd` 重启即清空，网关进程不自启
6. **识别取景**：抓拍无真实车牌多为取景/时机问题，非算法故障；识别逻辑按用户要求暂不改
7. **影子覆盖**：多车位 `Parking_id`/`Parking_state` 单值属性互相覆盖，需要时再改产品模型（每车位一属性或 json 全量快照）
8. **遗留死代码**：`cc2530_parser.c`（二进制帧解析）无人调用，可清理或改造为走 pi_link
