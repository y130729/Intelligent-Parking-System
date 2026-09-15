# 智能停车场管理系统项目说明文档

> 最后更新：2026-08-21。本文档覆盖 CC2530 车位节点、STM32 网关、树莓派网关（车牌识别 + 华为云 MQTT）三部分；微信小程序与网页端不在本文档范围内。

## 1. 项目概述

本项目是基于物联网技术的智能停车场管理系统，采用 **ZigBee + 树莓派 + 华为云 MQTT** 的分层架构，实现车位实时检测、车辆进出管理、车牌识别与云端数据上报。

- **CC2530 车位节点**：ZigBee 无线网络 + HC-SR04 超声波测距，周期上报车位占用状态
- **STM32F103 网关**：串口汇聚、红外触发、RFID 刷卡验证、道闸控制、OLED 显示；与树莓派配合实现"车牌或刷卡任一凭证开闸"；不直接联网
- **树莓派 Zero WH**：车牌拍照识别 + 车辆档案匹配自动开闸 + 华为云 MQTT 上报与命令中转
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
红外遮挡（PA11 EXTI11）-> 状态机进入 WAITING_RFID，同时发送 CAPTURE
  -> 树莓派开启"单次过闸会话"（PassageSession，带 TTL，超时自动失效），
     以下两个凭证并行等待：
     ① 车牌：拍照识别 -> vehicles.json 档案命中 -> 树莓派发 OPEN:plate 开闸
     ② 刷卡：MFRC522 读卡 -> STM32 Flash 白名单验证通过 -> 直接抬杆
  -> 任一凭证先命中即开闸一次（会话去重；凭证来源 card/plate/remote 随
     BARRIER:OPEN:<reason> 上报，写入通行记录，远程开闸另记操作人）
  -> 3s 后落杆（BARRIER:CLOSE，会话结束）；无效卡 / 未登记车牌 / 车位已满：提示后回 IDLE
  -> PLATE:<车牌> 回传 OLED 第 4 行显示
离线兜底：树莓派/网络不可用时，STM32 本地刷卡白名单仍可开闸
```

### 2.3 车牌识别数据流

```
CAPTURE（STM32 红外触发，或本地 echo capture > /tmp/plate_cmd）
  -> 开启过闸会话（PassageSession，默认 TTL 60s）
  -> rpicam-still 抓拍（分辨率/曝光时间在 cloud_config.json 的 camera 段配置，
     默认 --timeout 200ms —— 原来固定等 3 秒曝光稳定，但车不会在闸前静止等 3 秒）
  -> HSV 蓝牌定位 -> minAreaRect 透视校正 -> CLAHE -> Tesseract OCR
  -> 车牌结构规范化（plate_format：长度/字符集合合法性 + I/O 修复，
     不针对某一块车牌猜字）-> vehicles.json 模糊匹配（相似度 >= 0.80 取档案内完整车牌）
  -> 上报 PlateRecognition.plate_number
  -> 命中档案且会话未开闸：自动发 OPEN:plate 开闸（记录 barrier_open_by_plate）；未命中记录 plate_unknown
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
| Flash 故障记录 | 内置 | 0x0800FC00 | 异常复位现场（最后一页，Keil 下载不覆盖） |

Flash 分区（STM32F103C8T6 共 64KB，1KB/页）：

| 地址范围 | 用途 |
|---|---|
| `0x08000000` - `0x08006FFF` | 应用程序（代码**不得越过** 0x08007000，否则白名单擦写会擦掉代码） |
| `0x08007000` - `0x080073FF` | 刷卡白名单（`flash_storage.c`） |
| `0x0800FC00` - `0x0800FFFF` | 异常现场记录（`fault_log.c`，位于 IROM1 之外，固件升级也不会被擦） |

> IROM1 = `0x08000000 + 0x7000`（28KB）。这个值有三个作用：应用超限时**链接器直接报错**，而不是让代码悄悄压到白名单扇区；Keil 的下载范围不含 `0x08007000`，所以**烧写固件不会擦掉已登记的卡白名单**；故障记录页也落在下载范围之外，被保留下来。
>
> 当前占用：Code 21496 + RO 3832 + RW 80 = 25408 字节（约 24.8KB / 28KB，88.6%）。接近上限时先删功能，不要放大 IROM1。

### 3.3 树莓派 Zero WH

| 项目 | 值 |
|---|---|
| 型号 | Zero WH（ARMv6，512MB） |
| 摄像头 | CSI（rpicam-still 拍照，2592x1944） |
| 串口 | /dev/serial0 @ 115200（接 STM32 USART2，需共地） |
| 网络 | WiFi 连接华为云（MQTTS 8883） |
| 软件 | Python3 + pyserial + paho-mqtt + OpenCV + pytesseract/Tesseract(chi_sim) |
| 运行方式 | systemd 服务（崩溃自动重启 / 开机自启 / 日志进 journald），见 `deploy/` |

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
| `User/main.c` | 主循环：状态机、OLED 刷新、链路处理、车位变化上报、喂狗 |
| `User/state_machine.c` | 车辆进出状态机、剩余车位管理 |
| `User/link.c` | 应用层链路协议：把业务动作拼成 SPOTS/SPOT/CARD/CAPTURE/BARRIER 报文 |
| `Hardware/pi_link.c` | USART2 与树莓派通信（文本行协议） |
| `Hardware/Serial.c` | USART3 接收 CC2530 帧并解析车位状态 |
| `Hardware/USART.c` | USART1 调试串口 + `addcard/delcard/listcards` 白名单命令 |
| `Hardware/OLED.c` | OLED 软 I2C 驱动 + 16x16 中文字库（31 省份简称） |
| `Hardware/MFRC522.c` / `Infrared.c` / `Servo.c` / `flash_storage.c` | 外设驱动 |
| `Hardware/watchdog.c` | 独立看门狗（IWDG，4s）：程序卡死时自动复位 |
| `System/fault_log.c` | 异常现场记录：写 Flash 保留页后复位，开机打印并清除 |

分层：`state_machine`（应用）-> `link`（协议）-> `pi_link`（UART 驱动）。
业务代码不再直接拼协议字符串，也不再出现"WiFi / 串口"两套传输的编译开关分支
（原 `esp8266.c`、`mqtt_report.c`、`cc2530_parser.c` 已删除）。

可靠性设计：
- **看门狗**：主循环每轮喂狗；`delay_ms` 在长延时期间按 500ms 间隔喂狗。
  喂狗条件依赖 SysTick 走时，若时基本身停摆则不会喂狗，照样复位。
- **异常复位不留谜团**：HardFault/MemManage/BusFault/UsageFault 统一记录
  CFSR/HFSR/BFAR/MMFAR 到 0x0800FC00 后复位，下次开机在调试串口打印现场。
- **有超时的等待**：MFRC522 软复位等待加了计数上限，芯片虚焊不再把主循环卡死。

串口总览：
| 串口 | 引脚 | 波特率 | 对端 | 用途 |
|---|---|---|---|---|
| USART1 | PA9/PA10 | 115200 | 调试终端 | 日志 + 白名单增删查命令 |
| USART2 | PA2/PA3 | 115200 | 树莓派 /dev/serial0 | 上行 SPOTS/SPOT/CARD/BARRIER:OPEN[:reason]/BARRIER:CLOSE/CAPTURE/LINK:OK，下行 PLATE/OPEN[:reason]/CLOSE[:reason] |
| USART3 | PB10/PB11 | 115200 | CC2530 协调器 | 接收 `PXX:XXXcm` 车位帧 |

状态机（`state_machine.c`）：
| 状态 | 描述 | 触发/退出 |
|---|---|---|
| `STATE_IDLE` | 空闲 | 红外遮挡 -> WAITING_RFID |
| `STATE_WAITING_RFID` | 等待刷卡/车牌 | 读卡白名单通过且有车位 -> BARRIER_UP；树莓派车牌命中发 OPEN:plate 同样开闸；红外离开或验证失败回 IDLE |
| `STATE_BARRIER_UP` | 抬杆 | 3s 后自动 -> BARRIER_DOWN |
| `STATE_BARRIER_DOWN` | 落杆 | 1s 后 -> IDLE |

RFID 验证流程（离线兜底）：红外触发 -> 等待刷卡 -> MFRC522_Request/Anticoll 取卡号 -> `FlashStorage_IsCardValid` 查白名单（Flash 0x08007000，默认卡 7506D506）-> 有剩余车位则抬杆并回发 `BARRIER:OPEN:card`，无效卡/车位已满提示 3s 后回 IDLE；车牌识别为并行第二凭证，树莓派侧过闸会话内任一凭证先命中即开闸一次（去重）

OLED 显示：第 1 行标题、第 2 行 `Spots: x/20`、第 3 行状态、第 4 行 `Plate:<车牌>`（16x16 中文，方向已修复）

开机流程：`LINK:OK` 握手 -> 补发初始 `SPOTS:20` -> 等待串口数据

### 4.3 raspberrypi（树莓派网关）

文件清单：
| 文件 | 职责 |
|---|---|
| `plate_gateway.py` | 主程序：MQTT 云端 + 串口 + 拍照识别集成 |
| `plate_recognition.py` | 车牌定位与 OCR |
| `plate_matcher.py` | 车牌档案模糊匹配（加权编辑距离，数据源 vehicles.json） |
| `plate_format.py` | 中国车牌结构规则：长度/字符集合法性、I/O 修复、候选窗口打分 |
| `passage.py` | 单次过闸会话（带 TTL）与开闸凭证判定（card/plate/remote） |
| `stm32_protocol.py` | STM32 行协议解析与构造（含 OPEN/CLOSE 的 reason 字段） |
| `plate_config.py` | 配置加载与 rpicam-still 命令构造（网关与调试工具共用） |
| `plate_camera.py` | 手动拍照 -> 识别工具 |
| `vehicles.json` | 车辆档案：车牌 + 关联卡号 + 车主（车卡融合数据源，含示例 3 车） |
| `cloud_config.json` | 华为云 MQTT 连接、串口、拍照参数、会话 TTL 等配置 |
| `requirements.txt` | pip 依赖（paho-mqtt / pyserial；OpenCV 建议 apt 安装） |
| `huawei_iotda_product_model.json` | 云端产品模型（已导入） |
| `test/` | 测试图：test.jpg->T234J5、test2.jpg->EM456U、test3.jpg->K4Z567 |
| `tests/` | 自动化测试（69 项，纯逻辑，无需相机/网络/硬件） |
| `deploy/` | systemd 服务文件与安装脚本 |
| `archive/` | 历史备份（旧 pi_cloud.py 等） |

`plate_gateway.py` 功能：
- **线程模型**：`watch_serial` 只做"读串口 + 解析 + 入队"，`publish_worker` 是唯一向 MQTT 发布属性的线程。
  这样云端网络抖动不会阻塞本地串口链路（此前 `wait_for_publish(5)` 放在串口线程里，
  网络一慢串口就停止读取、缓冲溢出丢行）
- 车卡融合：`CAPTURE` 开启单次过闸会话，车牌命中 `vehicles.json` 且会话未开闸时自动发 `OPEN:plate`；
  `CARD` 按档案记录 card_hit/card_unknown；`BARRIER:OPEN` 标记会话已开闸避免重复开闸
- 会话超时：会话 TTL 默认 60s（`passage_ttl_s`），车走掉后下一辆车不会被记成上一次会话的凭证
- 开闸审计：按 `BARRIER:OPEN:<reason>` 记录 credential（card/plate/remote）、operator、card、plate、owner；
  老固件不带 reason 时用"最近刷卡/最近远程命令"兜底
- 事件记录：凭证命中/未命中、开闸/仅流水写入 `events.jsonl`（barrier_open / barrier_open_no_session / plate_unknown 等，运行时文件不入库）
- FIFO 线程 `watch_fifo`：监听 `/tmp/plate_cmd`，支持无 STM32 的本地调试触发
- 拍照互斥：`CAPTURE_STATE` 锁保证同一时刻只有一次拍照+OCR，忙时挂起补拍一次；
  `rpicam-still` 失败重试 2 次、单次超时 15s（原来重试 3 次 x 90s，最坏一次触发能占住近 5 分钟）
- 云端命令：`open_barrier` / `close_barrier`（转发 `OPEN:remote`/`CLOSE:remote` 给 STM32，5s 超时等待 `BARRIER:` 应答后回执 success/failed）、`query_status`（按缓存直接应答）、`capture_photo`
- 断线自动重连 MQTT；串口异常关闭后 5s 重试

测试：`python -m unittest discover -s raspberrypi/tests -t raspberrypi`
（覆盖车牌结构规则、车牌库匹配、通行会话 TTL、行协议解析、拍照参数构造、
以及"串口线程绝不调用网络"的网关流程回归）

车牌识别（`plate_recognition.py`）流程：
HSV 蓝色掩码（H 90-135）-> 形态学开闭 -> 候选轮廓（宽高比 2.0~4.0、目标 3.15、最小面积与最小宽度按图像尺寸取比例，换分辨率不用改阈值）-> minAreaRect 透视校正 -> CLAHE 增强 -> Tesseract LSTM OCR（psm 8->13->7，白名单 A-Z0-9）-> 校正图 OCR 为空时回退识别蓝色包围盒 -> `plate_format.post_process` 按车牌结构（6 位燃油 / 7 位新能源、I/O 修复）选窗口

> 早期版本在 `post_process` 里硬编码了针对测试车牌 `冀T234J5` 的替换规则
> （首位 1/7/8 改成 T、序号中间 7 改成 Z）。那套规则换一辆车就会失效，甚至把
> 合法车牌改坏，现已删除：首位到底属于哪个字母属于"消歧"，交由车辆库匹配决定。

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
| `CARD:<8位HEX>` | 刷卡卡号(4字节) | Spot_state.card_id + vehicles.json 档案匹配记录（card_hit/card_unknown） |
| `BARRIER:OPEN[:<reason>]` | 道闸打开（含远程命令应答）；reason=card/plate/remote | ParkingBarrier.barrier_state=up |
| `BARRIER:CLOSE` | 道闸关闭（含远程命令应答）；**不带 reason** | ParkingBarrier.barrier_state=down |
| `CAPTURE` | 车辆到达（红外触发），启动过闸会话并请求拍照识别 | 拍照->OCR->vehicles.json 匹配->命中自动 OPEN + 上报 PlateRecognition.plate_number |

**树莓派 -> STM32（下行控制/结果）**

| 消息 | 含义 | STM32 动作 |
|---|---|---|
| `PLATE:<车牌>` | 识别结果（UTF-8 中文+ASCII） | OLED 第 4 行显示 Plate:<车牌> |
| `PLATE:NONE` | 未识别到车牌 | OLED 第 4 行恢复占位 |
| `OPEN[:<reason>]` | 云端 open_barrier 命令或车牌档案命中 | 抬杆，回发 BARRIER:OPEN:<reason> |
| `CLOSE[:<reason>]` | 云端 close_barrier 命令或会话结束 | 落杆，回发 BARRIER:CLOSE |

**`:reason` 字段**

`reason` 表示这次开闸是"凭什么开的"，取值 `card`（RFID 白名单）/ `plate`（车牌档案命中）/ `remote`（云端或网页端远程命令）。

- 树莓派下发时带上原因，STM32 抬杆/落杆时原样回报，树莓派据此判定本次过闸的凭证并写入审计事件（`barrier_open` 带 credential/operator，未命中会话时写 `barrier_open_no_session`）
- 老固件不带原因（形如裸 `BARRIER:OPEN`），树莓派用"最近 2s 内是否刷过卡 / 最近 5s 内是否下过远程命令"兜底判断；都判断不出则记 `unknown`
- 关闸方向**故意不对称**：下行 `CLOSE:<reason>` 会带原因（树莓派一定知道是谁下的命令），但 STM32 回发的 `BARRIER:CLOSE` 不带——因为凭证判定只发生在"抬杆"这一刻，关闸只用来结束会话，多带一个字段没有用处

**云端命令**

- 产品模型命令（服务 ParkingBarrier）：open_barrier / close_barrier / query_status
- 树莓派收到命令后通过串口转发 OPEN/CLOSE 给 STM32，等待 BARRIER:OPEN/CLOSE 应答（5s 超时）后向云端回执 result=success/failed
- query_status 由树莓派根据已缓存状态直接应答（remaining_spots/plate_number/barrier_state）

**触发方式**

- STM32 红外（EXTI11）触发 -> 状态机发 CAPTURE 给树莓派 -> 拍照识别 -> 命中档案自动 OPEN -> 上报 + 回传 PLATE 显示
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

属性上报在树莓派侧走独立发布线程（`queue.Queue` + `publish_worker`）：串口线程只负责"解析 + 入队"，不再等待云端 `wait_for_publish`。队列满时丢弃最旧的一条并计数，保证本地链路不会被云端链路拖死（云端协议本身未变）。

## 6. 项目配置参数

### 6.1 STM32 配置（`User/project_config.h`）

```c
#define SYS_CLOCK_FREQ        72000000UL // 系统时钟（与 system_stm32f10x.c 一致）
#define TOTAL_SPOTS           20       // 总车位数
#define MAX_SPOT_ID           20       // 最大车位 ID（spot_occupied 数组长度）
#define MAX_CARD_ID_LENGTH    4        // 卡号字节数
#define PARKING_THRESHOLD_CM  20       // 车位占用阈值(cm)，距离小于该值判占用
#define FLASH_APP_LIMIT_ADDR  0x08007000u // 应用程序上限（文档性宏，见下）
```

Flash 分区（64KB，1KB/页）：改分区表需同步 `fault_log.h` 与 Keil 的 IROM1 设置

| 地址范围 | 用途 |
|---|---|
| `0x08000000` - `0x08006FFF` | 应用程序（不得越过 `0x08007000`，IROM1 已设为此范围） |
| `0x08007000` - `0x080073FF` | RFID 刷卡白名单（`Hardware/flash_storage.c`） |
| `0x0800FC00` - `0x0800FFFF` | 故障记录（`System/fault_log.c`，下载范围之外，升级固件不会擦除） |

白名单容量与存储地址在 `Hardware/flash_storage.h`：`MAX_WHITELIST_CARDS`、`FLASH_SECTOR_ADDR`、`CARD_ID_LENGTH`。

> 原先的 `WIFI_ENABLE` 开关与华为云明文凭据已从固件中删除：ESP8266/MQTT 实现整体移除，云端连接统一由树莓派承担（理由见 4.2 分层说明）。

### 6.2 时间配置

```c
/* User/project_config.h */
#define BARRIER_UP_DELAY_MS   3000     // 道闸抬起延时
#define BARRIER_DOWN_DELAY_MS 1000     // 道闸落下延时
#define ERROR_DISPLAY_DELAY_MS 3000    // 错误提示显示时长

/* Hardware/watchdog.h */
#define WATCHDOG_TIMEOUT_MS        4000u  // IWDG 溢出时间（须大于最长正常阻塞）
#define WATCHDOG_FEED_INTERVAL_MS  500u   // 喂狗间隔（含 delay_ms 长延时期间）

/* Hardware/MFRC522.c */
#define RC522_RESET_TIMEOUT   1000u    // RC522 软复位等待上限（超时返回 MI_ERR，不再死等）
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
2. ST-Link/J-Link 下载。注意 IROM1 只覆盖 `0x08000000`-`0x08006FFF`，**下载固件不会擦除白名单扇区与故障记录页**
3. 无人值守（CI/脚本）构建：`UV4.exe -b Project.uvprojx -j0 -o build.log`，退出码 0=无警告、1=有警告、2=有错误
4. `UV4.exe` 被占用时可用命令行语法检查：
   `arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -DSTM32F10X_MD -DUSE_STDPERIPH_DRIVER -I Start -I Library -I User -I Hardware -I System -fsyntax-only <file>`

### 7.3 树莓派网关

**首次安装（systemd 托管）**

```bash
# 依赖：OpenCV 在 Pi Zero 上用 apt 装（pip 编译不动）
sudo apt install -y python3-venv python3-opencv tesseract-ocr tesseract-ocr-chi-sim

# 代码放到 /opt/parking（内含 raspberrypi/ 与 cloud_config.json）
sudo mkdir -p /opt/parking && sudo cp -r raspberrypi /opt/parking/

# 建 venv、写 unit、开机自启
sudo /opt/parking/raspberrypi/deploy/install.sh /opt/parking pi
```

装机后由 `plate-gateway.service` 托管：`Restart=always` / `RestartSec=2` / `StartLimitBurst=5`、日志进 journald、`MemoryMax=256M`（Pi Zero 512MB）。

**日常运维**

```bash
journalctl -u plate-gateway -f          # 跟踪日志（不再写 /tmp，重启不丢）
sudo systemctl restart plate-gateway    # 重启
sudo systemctl status plate-gateway     # 状态
echo capture > /tmp/plate_cmd           # 本地触发一次拍照（unit 未启用 PrivateTmp）
```

**原地升级（当前这台树莓派的实际部署方式）**

树莓派上没有外网（`apt`/`pip` 都下不动），所以不重建 venv，直接复用 `~/parking_gateway/.venv`，只替换代码：

```bash
# 1) 备份（出问题可直接拷回）
BK=~/parking_gateway_backup_$(date +%Y%m%d_%H%M); mkdir -p $BK
cp -a ~/parking_gateway/*.py ~/parking_gateway/*.json $BK/

# 2) 本机上传（Windows 侧）
#    pscp -pw <密码> raspberrypi\*.py raspberrypi\cloud_config.json zny@<IP>:/home/zny/parking_gateway/
#    pscp -pw <密码> -r raspberrypi\tests raspberrypi\deploy zny@<IP>:/home/zny/parking_gateway/
#    （不要覆盖 vehicles.json —— 那是设备上的实际车辆档案）

# 3) 清掉 Windows 带过去的 .pyc，避免路径/mtime 不一致
rm -rf ~/parking_gateway/__pycache__ ~/parking_gateway/tests/__pycache__

# 4) 装服务：把 deploy/plate-gateway.service 里的路径/用户替换成实际值后写入 /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl restart plate-gateway
```

树莓派侧的实际路径与 repo 里的默认值不同，sed 替换的是：`User/Group=zny`、`WorkingDirectory=ExecStart` 用 `/home/zny/parking_gateway`、venv 解释器用 `/home/zny/parking_gateway/.venv/bin/python3`、`ReadWritePaths=/home/zny/parking_gateway /tmp`。

部署自检（不依赖相机）：

```bash
cd ~/parking_gateway && .venv/bin/python3 -m unittest discover -s tests -t .   # 69 项
sudo -n ls -l /proc/$(pgrep -f plate_gateway.py)/fd | grep ttyS0               # 串口已打开
ss -tnp | grep 8883                                                            # MQTTS 已连上
journalctl -u plate-gateway -n 20 -o cat
```

**开发时前台运行**（调试用，不装服务）

```bash
cd raspberrypi && source ../.venv/bin/activate && python3 plate_gateway.py
python -m unittest discover -s tests -t .   # 单元测试（69 项，无需相机/串口/网络）
```

SSH 工具：plink/pscp（PuTTY）；批量脚本建议 base64 后 `echo $b64 | base64 -d | bash`

## 8. 系统特性总结

| 特性 | 描述 |
|------|------|
| 无线传感 | ZigBee 车位采集（100ms 周期 + 随机抖动） |
| 实时检测 | 超声波 <20cm 判占用，状态变化即上报 |
| 身份验证 | 车卡融合：车牌识别或 RFID 白名单任一命中开闸（单次过闸去重，离线刷卡兜底） |
| 自动控制 | 状态机抬/落杆，红外联动，远程命令 |
| 车牌识别 | 拍照 OCR + 车牌库模糊纠错 |
| 云端集成 | 树莓派 MQTT 上报与命令下发（STM32 不直连） |
| 中文显示 | OLED 16x16 中文字库（31 省份简称） |
| 离线调试 | /tmp/plate_cmd 本地触发拍照 |
| 可靠性 | IWDG 看门狗（4s 超时自复位）+ 故障现场记录到 Flash 并在开机打印；寄存器等待循环均带超时 |
| 现场运维 | 树莓派服务化（systemd 自启 + 崩溃重启 + journald 日志），串口线程与云端发布解耦 |
| 可审计 | 每次开闸记录凭证来源（card/plate/remote）与会话 TTL，区分刷卡、识别、远程开闸 |

## 9. 注意事项

1. **ZigBee 网络**：首次部署协调器先启动，节点自动入网
2. **共地**：STM32 / 树莓派 / CC2530 必须共地，否则串口数据不可靠（此前刷卡不上报、云命令无响应的根因）
3. **串口电平**：USART3 与协调器、USART2 与树莓派均需电平匹配
4. **源码编码**：STM32 源码为 UTF-8，修改注意保持编码，避免中文注释乱码
5. **服务托管**：网关已由 systemd 托管，日志进 journald（`journalctl -u plate-gateway`），崩溃自动重启，开机自启；只有本地触发文件 `/tmp/plate_cmd` 仍放在 tmpfs（重启即清空，属预期）
6. **识别取景**：抓拍无真实车牌多为取景/时机问题，非算法故障；识别逻辑按用户要求暂不改
7. **影子覆盖**：多车位 `Parking_id`/`Parking_state` 单值属性互相覆盖，需要时再改产品模型（每车位一属性或 json 全量快照）
8. **车辆档案**：`vehicles.json` 为车卡统一档案（plate + cards），STM32 Flash 白名单为离线刷卡兜底，新增车辆/卡片时两处需同步
9. **看门狗副作用**：`WATCHDOG_TIMEOUT_MS`（4s）必须大于最长的正常阻塞（错误提示 3s），后续若加更长的阻塞流程需同步调大，否则会被 IWDG 复位
10. **已知遗留（凭据）**：`raspberrypi/cloud_config.json` 目前以明文保存华为云设备密钥且已入库，`plate_gateway.py` 在找不到 CA 证书时会退化为 `CERT_NONE`。正式启用建议改为环境变量/独立配置文件（加入 `.gitignore`）并在云端轮换密钥，同时把 CA 缺失视为启动失败。
