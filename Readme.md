# 智能停车场管理系统项目说明文档

[![CI](https://github.com/y130729/Intelligent-Parking-System/actions/workflows/ci.yml/badge.svg)](https://github.com/y130729/Intelligent-Parking-System/actions/workflows/ci.yml)

> 所有自动化检查都不需要硬件：`python tools/run_tests.py`（详见第 10 节）。

> 最后更新：2026-11-17。本文档覆盖 CC2530 车位节点、STM32 网关、树莓派网关（车牌识别 + 华为云 MQTT）三部分；微信小程序与网页端不在本文档范围内。

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
                       │ USART2 115200 帧协议（#VER SEQ LEN payload *CRC + ACK）
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
| `tests/` | 固件静态/一致性测试 + 协议黄金样本（纯 Python，无硬件） |
| `tools/` | 静态检查、密钥扫描、一键测试脚本 |
| `.github/workflows/` | CI：Linux + Windows × Python 3.9/3.12 |

## 2. 数据流

### 2.1 车位检测数据流

```
HC-SR04 测距 -> CC2530 节点（每 100ms + 随机抖动）
  -> ZigBee P2P 帧 PXX:XXXcm -> 协调器 UART0 原样转发
  -> STM32 USART3 中断解析（距离 < 20cm 判占用）
  -> spot_occupied[] 变化 -> 标记 dirty 位图 -> 主循环逐条 SPOT:<id>:<0|1> + SPOTS:<n>
  -> 树莓派 -> 华为云 Spot_state 属性上报
回波超时 / 无回波 -> 节点上报 PXX:FAULT（不再退化成"距离超限=空位"）
  -> STM32 -> SPOT:<id>:FAULT -> 树莓派上报 Parking_state=fault
     （坏传感器绝不能被当成"空位"，否则云端会把车位派给下一辆车）
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
  -> 落杆前先查防砸红外（Barrier_CanClose），确认杆下无人再落杆
     （BARRIER:CLOSE，会话结束）；无效卡 / 未登记车牌 / 车位已满：提示后回 IDLE
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
| Flash 白名单 | 内置 | 0x0800F000 / 0x0800F400 | RFID 白名单持久化（A/B 双槽 + CRC，最多 20 张） |
| Flash 故障记录 | 内置 | 0x0800FC00 | 异常复位现场（最后一页，Keil 下载不覆盖） |

Flash 分区（STM32F103C8T6 共 64KB，1KB/页）：

| 地址范围 | 用途 |
|---|---|
| `0x08000000` - `0x0800EFFF` | 应用程序（代码**不得越过** 0x0800F000，否则白名单擦写会擦掉代码） |
| `0x0800F000` - `0x0800F3FF` | 刷卡白名单 A 槽（`flash_storage.c`） |
| `0x0800F400` - `0x0800F7FF` | 刷卡白名单 B 槽（A/B 交替写，写完才切换"当前生效槽"） |
| `0x0800FC00` - `0x0800FFFF` | 异常现场记录（`fault_log.c`，位于 IROM1 之外，固件升级也不会被擦） |

> IROM1 = `0x08000000 + 0xF000`（60KB）。作用：应用超限时**链接器直接报错**，而不是让代码悄悄压到白名单扇区；Keil 的下载范围不含 `0x0800F000` 之后，所以**烧写固件不会擦掉已登记的卡白名单**；故障记录页也落在下载范围之外，被保留下来。
>
> 当前占用：Code 26280 + RO 3968 + RW 136 = 30384 字节（约 29.7KB / 60KB）；RAM 为 RW 136 + ZI 6336 = 6472 字节（约 6.3KB / 20KB），其中 2KB 是 USART3 的 DMA 接收环形缓冲、512 字节是 USART2 的收发环形缓冲。加入帧协议、发送队列、非阻塞状态机后代码增长，白名单因此从 `0x08007000` 挪到更靠后的位置，应用区仍有 30KB 余量。
>
> 分区迁移：老固件把白名单放在 `0x08007000`。新固件启动时如果新槽为空、老地址数据合理，会**一次性迁移**过去并打印日志——不迁移等于"升级完车主的卡全没了"，旧地址随后不再使用。

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
- **有界等待**：回波高/低电平都不用 `while(ECHO_PIN == x);` 死等，而是带计数的循环
  （`ULTRA_RISE_TIMEOUT_ITERS` / `ULTRA_ECHO_TIMEOUT_ITERS`）。传感器线松脱、车顶把
  回波打飞时，节点只是这一次测距失败，OSAL 任务照常运行——原来那一行会让整个
  ZigBee 节点永久卡死、彻底失联，而且现场看起来只是"这个车位不更新了"
- **采样与滤波**：单次测距连采 5 个点取中值（丢弃极大值/极小值），至少 3 个有效
  点才算一次成功；占用判定加 2cm 迟滞、连续 3 次一致才翻转状态
  （`ULTRASONIC_SAMPLES` / `MIN_VALID` / `HYST_CM` / `CONFIRM`）。
  超声波受温度、车身形状、地面反射影响很大，单点跳变会让云端看到"车位反复占用/空闲"
- 数据帧：`PXX:XXXcm`（9 字节）正常数据；距离 >= 1000cm 时 `PXX:err`（7 字节）；
  测距失败/回波超时时上报 `PXX:FAULT`（**同样是 9 字节定长**）
- **故障不能伪装成空位**：`FAULT` 是独立报文，不会被解析成"距离超限 = 空闲"。
  原来的错误帧错在长度（按 9 字节发实际只有 7 字节），STM32 的 `sscanf("%hucm")`
  解析失败后**静默丢弃**，故障就这么消失了
- **看门狗**：入网成功后启动 CC2530 片内看门狗（`WDCTL`，约 1s），每个上报周期喂狗；
  任务卡死 1 秒内自动复位重新入网
- 上报：P2P 簇（Cluster 4）单播到协调器（0x0000）；协调器收到后经 UART0 原样转发并追加 `\r\n` 给 STM32 USART3
- **APS 确认 + 有限重传**：P2P 上报带 `AF_ACK_REQUEST`，协调器的 APS 实体回 APS ACK，
  发送端收到 `AF_DATA_CONFIRM_CMD` 才算端到端送达。原来只有 MAC 层 ACK（只能证明
  "隔壁节点收到了"），协调器队列满或路由丢包时发送端毫不知情，车位状态就这么静默错
  一次。失败时补发 1 次（`PARKINGNODE_APS_MAX_RETRIES`）：MAC 层本来就有 3 次重试，
  APS 再叠加多次重传在 20 节点 / 4~5Hz 的负载下会把信道占用推高一倍；而周期上报
  自带"下一秒再报"的自愈能力，需要保证的只是偶发丢包和一次性的 `FAULT` 帧。
  EndDevice 是轮询型（`POLL_RATE=1000`），APS ACK 要等节点下一次 poll 才拿到，
  所以"确认失败"通常滞后 1s，这里不做"多久没 ACK 就重发"的定时假设
- **空口加密**：`-DSECURE=1`（`f8wConfig.cfg`），协调器作为信任中心用 `DEFAULT_KEY`
  作 TC Link Key 加密分发一把随机 Network Key；节点入网后用 Network Key 加解密所有
  数据帧。原来 `SECURE=0`，一台 CC2530 + 抓包软件就能看到全部车位数据和入网握手。
  `ZG_SECURE_DYNAMIC=0` 把安全级别固定在编译期（PRO_STANDARD），运行期无法被改成
  "无安全"。`ZDSECMGR_TC_DEVICE_MAX=21`（1 协调器 + 20 节点）——这个值默认是 1，
  不改的话从第 2 个节点起就入不了网
- 配置（`ParkingNode.h`）：端点 `PARKINGNODE_ENDPOINT=20`、Profile `0x0F08`、车位号 `PARKINGNODE_SPOT_NUM`（每个节点改一处）
- 说明：回波计时目前仍用 `Delay_10us()` 轮询而非 Timer1 输入捕获。用定时器捕获
  更准，但 CC2530 的 Timer1 与 MAC 层定时器存在资源冲突，手上没有第二块节点板
  做回归，因此先保证"不会卡死 + 滤波"，把捕获留作后续（见第 9 节）

编译结果（IAR EW8051，三个配置均 0 error；7 个 warning 全在厂商 `hal_lcd-ssd1331.c`）：

| 配置 | CODE | XDATA | IDATA | DATA | CONST |
|---|---|---|---|---|---|
| CoordinatorEB | 152 833 B | 7 630 B / 7 935 B | 192 B | 32 B | 918 B |
| RouterEB | 152 669 B | 7 634 B / 7 935 B | 192 B | 32 B | 930 B |
| EndDeviceEB | 133 675 B | 5 635 B / 7 935 B | 192 B | 32 B | 874 B |

> 开 `SECURE=1` 的代价：协调器/路由 CODE 从约 125KB 涨到约 153KB（+28KB，安全服务
> 提供者 + 信任中心），终端 +8.7KB。XDATA 可用区间是 `0x0001-0x1EFF`（7935 字节，
> 后 256 字节留给 IDATA 覆盖），协调器还剩约 300 字节；XDATA 里的 768 字节是栈
> （`XSTACK`）。再往上加功能要先看这个余量，不要以为 CC2530 有 256KB Flash 就随便加。

### 4.2 STM32_Gateway（Keil MDK）

主要模块：
| 文件 | 职责 |
|---|---|
| `User/main.c` | 主循环：状态机、OLED 刷新、链路处理、车位变化上报、喂狗 |
| `User/state_machine.c` | 车辆进出状态机、剩余车位管理 |
| `User/link.c` | 应用层链路协议：把业务动作拼成 SPOTS/SPOT/CARD/CAPTURE/BARRIER 报文 |
| `Hardware/pi_link.c` | USART2 传输层：帧编解码、ACK、超时重传、按 SEQ 去重、超长行丢弃后重新同步 |
| `Hardware/link_frame.c` | 帧格式与 CRC-8/ATM（纯函数，不碰硬件，可在 PC 上单测） |
| `Hardware/uart_tx.c` | 发送环形队列 + TXE 中断，printf/报文不再阻塞主循环 |
| `Hardware/Serial.c` | USART3 接收 CC2530 帧：DMA1_CH3 循环搬进 2KB 环形缓冲（不占 CPU，擦 Flash 时也不丢字节）；车位变化记 dirty 位图，支持 `PXX:FAULT` |
| `Hardware/barrier.c` | 落杆前安全联锁（防砸红外），限位/电流检测的编译开关 |
| `System/dwt.c` | DWT 周期计数器，用来实测 ISR 耗时 |
| `Hardware/USART.c` | USART1 调试串口 + `addcard/delcard/listcards` 白名单命令 |
| `Hardware/OLED.c` | OLED 软 I2C 驱动 + 16x16 中文字库（31 省份简称） |
| `Hardware/MFRC522.c` / `Infrared.c` / `Servo.c` / `flash_storage.c` | 外设驱动 |
| `Hardware/watchdog.c` | 独立看门狗（IWDG，4s）：程序卡死时自动复位 |
| `System/fault_log.c` | 异常现场记录：写 Flash 保留页后复位，开机打印并清除 |

分层：`state_machine`（应用）-> `link`（业务报文）-> `pi_link`（帧/ACK/重传）-> `uart_tx`（发送队列）-> 硬件。
业务代码不再直接拼协议字符串，也不再出现"WiFi / 串口"两套传输的编译开关分支
（原 `esp8266.c`、`mqtt_report.c`、`cc2530_parser.c` 已删除）。

可靠性设计：
- **看门狗**：主循环每轮喂狗；`delay_ms` 在长延时期间按 500ms 间隔喂狗。
  喂狗条件依赖 SysTick 走时，若时基本身停摆则不会喂狗，照样复位。
- **异常复位不留谜团**：HardFault/MemManage/BusFault/UsageFault 统一记录
  CFSR/HFSR/BFAR/MMFAR 到 0x0800FC00 后复位，下次开机在调试串口打印现场。
- **有超时的等待**：MFRC522 软复位等待加了计数上限，芯片虚焊不再把主循环卡死；
  所有寄存器轮询循环都带超时计数，没有 `while(flag);` 这种写法。
- **中断只做最少的事**：红外 EXTI 里只做"读电平 + 置 volatile 标志 + 记 DWT 起点"，
  打印、状态机、串口发送全部回到主循环。原来在 ISR 里 `USART_Printf`，115200 下
  几十字节要阻塞好几毫秒，而且 1KB 静态缓冲不可重入。
- **ISR 耗时是量出来的**：`ReportIsrTiming()` 打印最坏周期数，
  `cycles / 72` 即微秒数，可以直接回答"你的中断有多快"。
- **发送不阻塞**：`uart_tx` 环形队列 + TXE 中断；队列满时丢新字节并累加
  `dropped` 计数（`UartTx_Pending()` 可查队列深度），不会在主循环里等一个字节发完。
  （不用 DMA 是因为报文只有十几字节、发送频率极低，DMA 的配置与出错恢复成本
  反而更高；这个取舍写在 `uart_tx.c` 的注释里。）
- **车位事件不丢**：20 个节点的变化用 32 位 `spot_dirty_mask` 位图记录，主循环
  每轮按位排空。原来只有一个 `pending_spot_id/status` 槽，同一轮有两个节点变化时
  前一个直接被覆盖——多节点项目里这是必然踩的。
- **白名单掉电安全**：A/B 双槽 + 记录头（magic/version/seq/len/crc32/commit），
  先写非活动槽、回读校验通过、最后写 commit 标志。写到一半掉电，旧槽仍然有效。
- **没有默认卡后门**：不再在空库时自动写入一张固定的示例卡。空库就是空库，必须由产测
  工装或管理员显式 `addcard`——内置默认卡等于一把公开的钥匙。
- **Flash 写之前必须解锁**：F1 复位后 `FLASH_CR` 是锁定的，`FLASH_ErasePage()` /
  `FLASH_ProgramWord()` 会**直接返回错误而不写**。`flash_storage.c` 的写入与 `EraseAll`
  现在都成对调用 `FLASH_Unlock()` / `FLASH_Lock()`，并在解锁后清 `EOP/PGERR/WRPRTERR`
  ——只要挂着一个残留错误位，之后每一次操作都会立刻失败（表现为"第一次加卡失败之后
  就再也写不进去"）。没解锁时不会被静默写坏：回读校验会报
  `verify failed, whitelist NOT committed` 并保留旧槽。
- **commit 字必须留成擦除态**：F1 的 Flash 只能把 1 写成 0。原来的代码先写
  `commit = 0`、最后再写 `WL_COMMIT`，第二次编程必然 PGERR，commit 永远不成立
  （现象：每次加卡都提示 `commit failed`，重启后卡就没了）。现在头部里的 commit
  先保持 `0xFFFFFFFF`，只在最后一步写成 `WL_COMMIT`。
- **擦 Flash 不再丢车位帧**：F1 擦一页时 CPU 无法从 Flash 取指，整个停 20~40ms；
  两页（`EraseAll`）最坏 80ms，115200 下就是 920 字节。原来 64 字节的 RX 环形缓冲
  5.5ms 就满，"加一张卡"必然丢一帧上报。现在 USART3 接收改成 DMA 循环搬运到 2KB
  环形缓冲：DMA 不取指、不依赖 CPU，擦除期间照常收字节；2KB 在满速下是 178ms，
  覆盖两页擦除最坏值仍有 2 倍余量。接收侧仍是轮询 `tail`（不用 DMA 中断），
  所以中断被擦除拖住也不会影响收数。USART2（树莓派）仍走 RXNE 中断，那条链路的
  环形缓冲同时从 256 放宽到 512 字节（= 44ms），因为树莓派侧的节奏是"一次一条命令、
  等 ACK"，不需要 DMA；真要连续灌数据，这条链路也得换成 DMA。

串口总览：
| 串口 | 引脚 | 波特率 | 对端 | 用途 |
|---|---|---|---|---|
| USART1 | PA9/PA10 | 115200 | 调试终端 | 日志 + 白名单增删查命令 |
| USART2 | PA2/PA3 | 115200 | 树莓派 /dev/serial0 | 帧协议：上行 SPOTS/SPOT[:FAULT]/CARD/BARRIER:OPEN[:reason]/BARRIER:CLOSE/CAPTURE/LINK:OK/ACK，下行 PLATE/OPEN[:reason]/CLOSE[:reason]/ACK |
| USART3 | PB10/PB11 | 115200 | CC2530 协调器 | 接收 `PXX:XXXcm` 车位帧；DMA1_CH3 循环接收 + 2KB 环形缓冲 |

状态机（`state_machine.c`）：

| 状态 | 描述 | 触发/退出 |
|---|---|---|
| `STATE_IDLE` | 空闲 | 红外遮挡 -> WAITING_RFID |
| `STATE_WAITING_RFID` | 等待刷卡/车牌 | 白名单卡命中 或 收到 OPEN:plate -> OPENING；超时 `WAIT_CARD_TIMEOUT_MS`(30s) 无凭证 -> IDLE |
| `STATE_OPENING` | 舵机上行（`BARRIER_MOVE_MS` 1500ms） | 行程结束 -> OPEN |
| `STATE_OPEN` | 开闸保持 | 保持 `BARRIER_UP_DELAY_MS`(3s) -> CLOSING |
| `STATE_CLOSING` | 舵机下行 | 行程结束 -> IDLE |
| `STATE_ERROR` | 错误提示（非阻塞） | `ERROR_DISPLAY_DELAY_MS` 到时 -> IDLE |
| `STATE_FAULT` | 安全故障 | 保持开闸，等障碍解除/人工干预 |

时序是"截止时间驱动"：每个状态只记一个 `deadline`，主循环用
`(int32_t)(GetTick() - deadline) >= 0` 判断到期（自带回绕处理），函数立即返回。
原来的写法是 `state_timer += 10` 假设主循环严格 10ms 一轮，但
`Servo_Up()/Servo_Down()` 各自 `delay_ms(1500)`、错误提示 `delay_ms(3000)`，
"3 秒"实际可能是 6 秒以上，抬落杆时序整体是漂的，而且串口在延时期间无人读取。
现在舵机只"设角度"，等待交给状态机。

RFID 验证流程（离线兜底）：红外触发 -> 等待刷卡 -> MFRC522_Request/Anticoll 取卡号 -> `FlashStorage_IsCardValid` 查白名单（Flash A/B 槽）-> 有剩余车位则抬杆并回发 `BARRIER:OPEN:card`，无效卡/车位已满提示 3s 后回 IDLE；车牌识别为并行第二凭证，树莓派侧过闸会话内任一凭证先命中即开闸一次（去重）

道闸安全逻辑（`barrier.c`）：
- `Barrier_CanClose()`：落杆前先查杆下防砸红外，有人/有车则**不落杆**，停在 `STATE_FAULT`
- `BARRIER_HAS_LIMIT_SWITCHES` / `BARRIER_HAS_CURRENT_SENSE`：限位开关与电机电流
  检测的编译开关，当前原型机（SG90 舵机）没有这两路信号，宏置 0；接真实道闸时
  打开即可，安全逻辑已经预留
- `Barrier_EmergencyRelease()`：断电/故障时直接关 PWM，让人可以手动抬杆
- 原则：**故障默认保持开闸或停止**，绝不"出错就落杆"。真实停车场里这是人身安全
  事故，原型阶段就要把策略定下来，而不是等换执行机构时再补

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
| `stm32_protocol.py` | STM32 行协议解析与构造（含 reason 字段）+ 帧编解码（CRC-8/ATM、SEQ、ACK），与固件 `link_frame.c` 一一对应 |
| `plate_config.py` | 配置加载与 rpicam-still 命令构造（网关与调试工具共用） |
| `plate_camera.py` | 手动拍照 -> 识别工具 |
| `vehicles.json` | 车辆档案：车牌 + 关联卡号 + 车主（车卡融合数据源，含示例 3 车） |
| `cloud_config.json` | 华为云 MQTT 连接、串口、拍照参数、会话 TTL 等配置（**不含密码**） |
| `cloud_secret.json` | 设备密钥（`chmod 600`，已在 `.gitignore`，模板见 `cloud_secret.example.json`） |
| `requirements.txt` | pip 依赖（paho-mqtt / pyserial；OpenCV 建议 apt 安装） |
| `huawei_iotda_product_model.json` | 云端产品模型（已导入） |
| `test/` | 测试图：test.jpg->T234J5、test2.jpg->EM456U、test3.jpg->K4Z567 |
| `tests/` | 自动化测试（123 项，纯逻辑，无需相机/网络/硬件；含帧编解码 / fuzz / 半行 / 并发写 / 密钥加载） |
| `deploy/` | systemd 服务文件与安装脚本 |
| `archive/` | 历史备份（旧 pi_cloud.py 等） |

`plate_gateway.py` 功能：
- **线程模型**：`watch_serial` 只做"读串口 + 解析 + 入队"，`publish_worker` 是唯一向 MQTT 发布属性的线程。
  这样云端网络抖动不会阻塞本地串口链路（此前 `wait_for_publish(5)` 放在串口线程里，
  网络一慢串口就停止读取、缓冲溢出丢行）
- **云命令不堵 MQTT 线程**：`on_message` 只把命令放进 `CMD_Q`，由单独的
  `command_worker` 线程串行执行。原来在回调里等串口 ACK（最长 5s），
  paho 的网络线程被挂住，keepalive、重连、后续命令全部排队
- **串口写加锁**：多个线程都会发（命令线程发 OPEN/CLOSE、拍照线程发 PLATE），
  pyserial 的 `write` 不是原子的，两帧交错就是"半帧 + 半帧"。所有写都过
  `SERIAL_LOCK`，并整帧一次写出 + `flush`
- **半行不解析**：`readline()` 超时会返回没有 `\n` 的半行，直接解析会把
  `BARRIER:OP` 当成一条未知命令、真正的 `BARRIER:OPEN` 永远拼不回来。
  现在攒到换行才切（`split_uart_lines`），超过 512 字节仍无换行则整段丢弃重新同步
- **帧层去重**：收到重复 SEQ 只补 ACK、不再执行；坏 CRC 直接丢并计数（`RX_STATS`）。
  老固件发的裸文本仍然兼容（`FRAME_LEGACY` 分支）
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
- **密钥外置**：密码优先级 `环境变量 PARKING_MQTT_PASSWORD` > `cloud_secret.json`（会检查
  文件权限是否宽于 0600 并告警），两者都没有就**直接报错退出**。配置文件里只保留
  "连哪里"，"凭什么连"放在版本库之外
- **TLS 缺 CA 就拒绝启动**：CA 证书不存在时抛 `RuntimeError`，绝不退化成
  `CERT_NONE`——那等于"加密但不验身份"，中间人可以冒充 broker 直接拿走设备密钥
- 断线自动重连 MQTT；串口异常关闭后 5s 重试

测试：`python -m unittest discover -s raspberrypi/tests -t raspberrypi`（123 项，无需相机/网络/硬件）
（覆盖车牌结构规则、车牌库匹配、通行会话 TTL、行协议解析、拍照参数构造、
"串口线程绝不调用网络"的网关流程回归，以及帧编解码 / 随机字节 fuzz /
半行拼接 / 并发写不交错 / 重复 SEQ 不重复执行 / 密钥缺失报错 / CA 缺失拒启动）

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
  - `PXX:FAULT`：测距失败（回波超时/无回波），9 字节定长；STM32 转成
    `SPOT:<id>:FAULT`，与"空位"明确区分
- 节点侧限制：单次测距有界等待（不会卡死），5 点中值滤波 + 2cm 迟滞 + 连续 3 次确认
- 协调器 -> STM32：UART0 原样转发文本帧并追加 `\r\n`（STM32 USART3，115200 8N1）
- **安全性（NWK/APS 层）**：`-DSECURE=1`，协调器作信任中心用 TC Link Key（`-DDEFAULT_KEY`）
  加密分发随机 Network Key，节点入网后用 Network Key 加解密全部数据帧；
  `-DZG_SECURE_DYNAMIC=0` 固定安全级别，`-DZDSECMGR_TC_DEVICE_MAX=21` 容纳 1+20 个设备。
  代价是每帧增加安全头 + 4 字节 MIC，且换密钥后所有设备必须一起重编、重新入网
- **端到端确认**：车位上报带 `AF_ACK_REQUEST`，用 `AF_DATA_CONFIRM_CMD` 统计成败，失败补发 1 次；
  广播（Periodic）和组播（Flash）不走 APS ACK——广播本来就没人回 ACK
- **两段链路的分工**：ZigBee 段负责"节点 -> 协调器"的无线传输（加密 + APS 确认），
  UART 段负责"协调器 -> STM32"（文本帧，无 CRC，靠 STM32 侧的行格式校验）；
  再往后的 STM32 <-> 树莓派段才有完整帧头/CRC/SEQ/ACK（见 5.2）

### 5.2 STM32 <-> 树莓派串口协议（USART2）

串口：STM32 USART2 (PA2/PA3) <-> 树莓派 /dev/serial0，波特率 115200，8N1

**帧格式**（`link_frame.c` / `stm32_protocol.py`，两端逐位一致）

```
'#' VER SEQ(2) LEN(2) PAYLOAD '*' CRC(2)
 1   1    2      2     LEN      1     2      = 9 + LEN 字节

例： #1 05 09 OPEN:card * <CRC>
```

| 字段 | 说明 |
|---|---|
| `#` | 帧起始 |
| `VER` | 协议版本，当前 `1`；版本不符直接判非法（避免只升级一端时互发"能收但读不懂"的帧） |
| `SEQ` | 0~255 循环序号，用于重传去重 |
| `LEN` | payload 字节数（UTF-8 计字节，上限 63） |
| `CRC` | CRC-8/ATM（多项式 0x07，初值 0x00），覆盖 VER..PAYLOAD |

为什么是"文本外壳 + CRC"而不是纯二进制 + COBS：payload 只有十几字节、115200
波特率，纯二进制省不下几个字节；而排障时 `cat /dev/serial0` 就能直接看清
`OPEN:card` 这种语义。真到 460800 以上或 payload 上百字节，再换二进制才有意义。

**链路机制**

| 机制 | 实现 |
|---|---|
| 完整性 | CRC-8；坏帧丢弃并计数（固件 `PiLinkStats`、树莓派 `RX_STATS`） |
| 重传 | 发出后 300ms 未收到 `ACK:<SEQ>` 则重发，最多重发 2 次；仍失败则丢弃并计数 |
| 去重 | 接收端按 SEQ 判断，重复帧只补 ACK、不重复执行（"重传"不等于"重做一次动作"） |
| 兼容 | 不带 `#` 的行按老固件裸文本处理（`FRAME_LEGACY`），升级可以只做一头 |
| 超长行 | payload > 63 / 行 > 512 字节：丢弃并重新同步，绝不越界 |
| 边界 | `LinkFrame_Encode` 先判输出缓冲长度再写；解析先判字段长度再取值 |

**STM32 -> 树莓派（事件上报）**

| 消息 | 含义 | 树莓派动作（华为云上报） |
|---|---|---|
| `SPOTS:<n>` | 剩余车位数 | Spot_state.remaining_spots |
| `SPOT:<id>:<0/1>` | 车位 id(1起) 状态，1=占用 | Spot_state.Parking_id + Parking_state(occupied/empty) |
| `SPOT:<id>:FAULT` | 车位传感器故障 | Parking_state=fault + events.jsonl 记 sensor_fault（**不报成 empty**） |
| `CARD:<8位HEX>` | 刷卡卡号(4字节) | Spot_state.card_id + vehicles.json 档案匹配记录（card_hit/card_unknown） |
| `BARRIER:OPEN[:<reason>]` | 道闸打开（含远程命令应答）；reason=card/plate/remote | ParkingBarrier.barrier_state=up |
| `BARRIER:CLOSE` | 道闸关闭（含远程命令应答）；**不带 reason** | ParkingBarrier.barrier_state=down |
| `CAPTURE` | 车辆到达（红外触发），启动过闸会话并请求拍照识别 | 拍照->OCR->vehicles.json 匹配->命中自动 OPEN + 上报 PlateRecognition.plate_number |
| `ACK:<HEX>` | 对下行帧的确认 | 收到后停止该 SEQ 的重传 |

**树莓派 -> STM32（下行控制/结果）**

| 消息 | 含义 | STM32 动作 |
|---|---|---|
| `PLATE:<车牌>` | 识别结果（UTF-8 中文+ASCII） | OLED 第 4 行显示 Plate:<车牌> |
| `PLATE:NONE` | 未识别到车牌 | OLED 第 4 行恢复占位 |
| `OPEN[:<reason>]` | 云端 open_barrier 命令或车牌档案命中 | 抬杆，回发 BARRIER:OPEN:<reason> |
| `CLOSE[:<reason>]` | 云端 close_barrier 命令或会话结束 | 落杆，回发 BARRIER:CLOSE |
| `ACK:<HEX>` | 对上行帧的确认 | 停止重传 |

**`:reason` 字段**

`reason` 表示这次开闸是"凭什么开的"。它是一个**白名单枚举**，不是自由文本：
固件侧 `LinkReason`（CARD/PLATE/REMOTE）与树莓派侧 `passage.CREDENTIAL_*`
取值必须一致，未知取值一律拒收。

- 为什么必须是枚举：老实现把串口收到的 reason 直接 `sprintf` 进 32 字节缓冲，
  而协议允许 63 字节 payload——发一条 `OPEN:` 接长串就能写穿栈。现在枚举 +
  `snprintf`，越界在类型层面就不可能发生
- 树莓派下发时带上原因，STM32 抬杆/落杆时原样回报，树莓派据此判定本次过闸的凭证并写入审计事件（`barrier_open` 带 credential/operator，未命中会话时写 `barrier_open_no_session`）
- 老固件不带原因（形如裸 `BARRIER:OPEN`），树莓派用"最近 2s 内是否刷过卡 / 最近 5s 内是否下过远程命令"兜底判断；都判断不出则记 `unknown`
- 关闸方向**故意不对称**：下行 `CLOSE:<reason>` 会带原因（树莓派一定知道是谁下的命令），但 STM32 回发的 `BARRIER:CLOSE` 不带——凭证判定只发生在"抬杆"这一刻，关闸只用来结束会话

**云端命令**

- 产品模型命令（服务 ParkingBarrier）：open_barrier / close_barrier / query_status
- 树莓派在 `command_worker` 线程里把 OPEN/CLOSE 转发给 STM32，等待 BARRIER:OPEN/CLOSE 应答（5s 超时）后向云端回执 result=success/failed
- query_status 由树莓派根据已缓存状态直接应答（remaining_spots/plate_number/barrier_state）

**触发方式**

- STM32 红外（EXTI11）触发 -> 状态机发 CAPTURE 给树莓派 -> 拍照识别 -> 命中档案自动 OPEN -> 上报 + 回传 PLATE 显示
- 备用本地触发（无 STM32 调试）：`echo capture > /tmp/plate_cmd`

### 5.3 华为云 IoTDA 接入（树莓派）

连接配置（`cloud_config.json`）：
| 参数 | 值 |
|---|---|
| Broker | `<实例ID>.st1.iotda-device.cn-south-1.myhuaweicloud.com:8883`（MQTTS，按实际实例填写） |
| 设备 ID | `<产品ID>_<设备名>`（按实际设备填写） |
| CA 证书 | `/etc/ssl/certs/ca-certificates.crt`（缺失即拒绝启动，不降级） |
| 设备密码 | **不在配置文件里**：`PARKING_MQTT_PASSWORD` 环境变量或 `cloud_secret.json`（0600） |
| 上报 Topic | `$oc/devices/{device_id}/sys/properties/report` |
| 命令 Topic | `$oc/devices/{device_id}/sys/commands/#` |

产品模型（`huawei_iotda_product_model.json`，已导入云端）：
| 服务 | 属性 | 命令 |
|---|---|---|
| Spot_state | Parking_id(string)、Parking_state(empty/occupied/**fault**)、remaining_spots(0-20)、card_id(string) | - |
| ParkingBarrier | barrier_state(up/down/opening/closing) | open_barrier / close_barrier / query_status |
| PlateRecognition | plate_number(string) | - |

注意：`Parking_id` + `Parking_state` 为单值属性，多车位上报会互相覆盖影子；小程序用本地缓存增量更新兜底（需改产品模型时另行讨论）。

注意：`Parking_state` 现在会多出一个 `fault` 取值（传感器故障）。云端产品模型若把该属性
定义成枚举，需要把 `fault` 加进去，否则平台侧会拒收或截断这条上报。

属性上报在树莓派侧走独立发布线程（`queue.Queue` + `publish_worker`）：串口线程只负责"解析 + 入队"，不再等待云端 `wait_for_publish`。队列满时丢弃最旧的一条并计数，保证本地链路不会被云端链路拖死（云端协议本身未变）。

## 6. 项目配置参数

### 6.1 STM32 配置（`User/project_config.h`）

```c
#define SYS_CLOCK_FREQ        72000000UL // 系统时钟（与 system_stm32f10x.c 一致）
#define TOTAL_SPOTS           20       // 总车位数
#define MAX_SPOT_ID           20       // 最大车位 ID（spot_occupied 数组长度）
#define MAX_CARD_ID_LENGTH    4        // 卡号字节数
#define PARKING_THRESHOLD_CM  20       // 车位占用阈值(cm)，距离小于该值判占用
#define FLASH_APP_LIMIT_ADDR  0x0800F000u // 应用程序上限（文档性宏，见下）
```

Flash 分区（64KB，1KB/页）：改分区表需同步 `fault_log.h` 与 Keil 的 IROM1 设置

| 地址范围 | 用途 |
|---|---|
| `0x08000000` - `0x0800EFFF` | 应用程序（不得越过 `0x0800F000`，IROM1 已设为此范围） |
| `0x0800F000` - `0x0800F3FF` | RFID 刷卡白名单 A 槽（`Hardware/flash_storage.c`） |
| `0x0800F400` - `0x0800F7FF` | RFID 刷卡白名单 B 槽（A/B 交替，掉电安全） |
| `0x0800FC00` - `0x0800FFFF` | 故障记录（`System/fault_log.c`，下载范围之外，升级固件不会擦除） |

白名单容量与存储地址在 `Hardware/flash_storage.h`：`MAX_WHITELIST_CARDS`、
`FLASH_SLOT_A_ADDR` / `FLASH_SLOT_B_ADDR`（`FLASH_SECTOR_ADDR` 保留为 A 槽别名）、
`CARD_ID_LENGTH`。

> 掉电安全：`FlashStorage_WriteWhitelist()` 先擦**非活动槽**，逐字写入后**回读校验**，
> 通过才写 commit 标志并切换活动槽。写一半掉电，另一个槽仍是完整的旧记录。
> 记录头含 magic/version/seq/len/crc32，读取时校验不过就当空库处理（而不是把
> 半写的垃圾当成卡号白名单）。

> 原先的 `WIFI_ENABLE` 开关与华为云明文凭据已从固件中删除：ESP8266/MQTT 实现整体移除，云端连接统一由树莓派承担（理由见 4.2 分层说明）。

### 6.2 时间配置

```c
/* User/project_config.h */
#define BARRIER_UP_DELAY_MS    3000   // 开闸保持时间
#define BARRIER_MOVE_MS        1500   // 舵机从一端走到另一端的时间
#define ERROR_DISPLAY_DELAY_MS 3000   // 错误提示显示时长
#define WAIT_CARD_TIMEOUT_MS  30000   // 车辆到位后等刷卡的最长时间（超时回 IDLE）

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

#define PARKINGNODE_ULTRASONIC_SAMPLES   5    // 单次测量采样点数（取中值）
#define PARKINGNODE_ULTRASONIC_MIN_VALID 3    // 至少几个点有效才算一次成功
#define PARKINGNODE_ULTRASONIC_MAX_CM    400  // 有效量程上限(cm)，超出判无效
#define PARKINGNODE_ULTRASONIC_HYST_CM   2    // 状态翻转迟滞(cm)
#define PARKINGNODE_ULTRASONIC_CONFIRM   3    // 连续几次一致才翻转占用状态

#define ULTRA_RISE_TIMEOUT_ITERS 3000  // 等回波拉高的计数上限（有界等待）
#define ULTRA_ECHO_TIMEOUT_ITERS 4000  // 等回波结束的计数上限

#define PARKINGNODE_APS_MAX_RETRIES    1   // APS 确认失败后的补发次数（MAC 层已重试 3 次）
#define PARKINGNODE_P2P_REPORT_MAX_LEN 9   // 上报帧最大长度（PXX:XXXcm / PXX:FAULT）
```

ZigBee 安全与确认参数在 `Projects/zstack/Tools/CC2530DB/f8wConfig.cfg`：

```
-DSECURE=1                      // 空口加密（原来为 0，明文）
-DZG_SECURE_DYNAMIC=0            // 安全级别编译期固定为 PRO_STANDARD
-DZDSECMGR_TC_DEVICE_MAX=21      // 信任中心 TC Link Key 条目：1 协调器 + 20 节点
-DDEFAULT_KEY="{...}"           // TC Link Key，仓库里是示例值，部署前必须替换
-DAPSC_ACK_WAIT_DURATION_POLLED=3000  // 轮询型终端的 APS ACK 等待（ms）
-DAPSC_MAX_FRAME_RETRIES=3       // APS 层帧重试次数
-DNWK_MAX_DATA_RETRIES=2         // NWK 层数据重试次数
```

## 7. 编译与部署

### 7.1 CC2530 节点（IAR）

1. 打开 `ParkingNode.eww` 工作空间，选择 ParkingNode 工程
2. 目标板类型 CC2530EB，编译生成 `.hex`
3. TI Flash Programmer 下载；每块节点板修改 `PARKINGNODE_SPOT_NUM`，跳线决定协调器/节点角色
4. 命令行构建（CI/脚本）：
   `IarBuild.exe ParkingNode.ewp -build EndDeviceEB`（另有 `CoordinatorEB`/`RouterEB`），
   退出码 0 = 成功；`-clean <cfg>` 可强制全量重编
5. 当前构建状态：三个配置均 **0 error**；剩余 7 条 warning 全部来自厂商 SDK 的
   `hal_lcd-ssd1331.c`（未使用变量 / 指针符号性），与本项目代码无关。
   本项目源文件为 CRLF（IAR 的 `Pa050 non-native end of line` 就是 LF 触发的）
6. 资源占用（EndDeviceEB，含 Z-Stack 协议栈）：CODE 125 020 B、XDATA 5 415 B、
   IDATA 192 B、DATA 32 B、CONST 870 B；CC2530F256 为 256KB Flash / 8KB SRAM

### 7.2 STM32 网关（Keil）

1. 打开 `Project.uvprojx`，Target 1（STM32F103C8），编译生成 `.axf`
2. ST-Link/J-Link 下载。注意 IROM1 只覆盖 `0x08000000`-`0x0800EFFF`，**下载固件不会擦除白名单扇区与故障记录页**
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

# 设备密钥：二选一，不要写进 cloud_config.json（该文件进版本库）
#   a) 环境变量（写进 unit 的 Environment=，注意 unit 文件本身也要 chmod 600）
#   b) 独立文件
sudo sh -c 'printf "{\"password\": \"%s\"}\n" "<设备密钥>" > /opt/parking/raspberrypi/cloud_secret.json'
sudo chmod 600 /opt/parking/raspberrypi/cloud_secret.json
sudo chown pi:pi /opt/parking/raspberrypi/cloud_secret.json
```

> 密钥缺失时网关**直接启动失败**并说明原因（不是带着空密码连云端）。
> 这一条是刻意的：静默失败会让人以为"设备在线只是没数据"。

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
#    pscp -pw <密码> raspberrypi\*.py <user>@<IP>:/home/<user>/parking_gateway/
#    （cloud_config.json / vehicles.json / cloud_secret.json 是设备本地配置，
#      仓库里的是模板，不要回传覆盖）
#    pscp -pw <密码> -r raspberrypi\tests raspberrypi\deploy <user>@<IP>:/home/<user>/parking_gateway/
#    （也不要覆盖 cloud_secret.json —— 密钥只存在设备上，从不经版本库流转）
#    （不要覆盖 vehicles.json —— 那是设备上的实际车辆档案）

# 3) 清掉 Windows 带过去的 .pyc，避免路径/mtime 不一致
rm -rf ~/parking_gateway/__pycache__ ~/parking_gateway/tests/__pycache__

# 4) 装服务：把 deploy/plate-gateway.service 里的路径/用户替换成实际值后写入 /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl restart plate-gateway
```

树莓派侧的实际路径与 repo 里的默认值不同，sed 替换的是：`User/Group=<user>`、`WorkingDirectory=ExecStart` 用 `/home/<user>/parking_gateway`、venv 解释器用 `/home/<user>/parking_gateway/.venv/bin/python3`、`ReadWritePaths=/home/<user>/parking_gateway /tmp`。

部署自检（不依赖相机）：

```bash
cd ~/parking_gateway && .venv/bin/python3 -m unittest discover -s tests -t .   # 123 项
sudo -n ls -l /proc/$(pgrep -f plate_gateway.py)/fd | grep ttyS0               # 串口已打开
ss -tnp | grep 8883                                                            # MQTTS 已连上
journalctl -u plate-gateway -n 20 -o cat
```

**提交前自检**

```bash
python tools/check_secrets.py            # 扫全仓库：明文密码 / 私钥 / AccessKey
python tools/check_secrets.py --staged   # 只看暂存区，可挂成 pre-commit 钩子
python -m unittest discover -s raspberrypi/tests -t raspberrypi   # 123 项
```

**开发时前台运行**（调试用，不装服务）

```bash
cd raspberrypi && source ../.venv/bin/activate && python3 plate_gateway.py
python -m unittest discover -s tests -t .   # 单元测试（123 项，无需相机/串口/网络）
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
| 链路完整性 | 串口帧带 CRC-8 + 序号 + ACK + 重传，坏帧丢弃、重复帧不重做；裸文本仍兼容 |
| 空口安全 | ZigBee 段 `SECURE=1`：信任中心分发随机 Network Key，全帧加密；TC Link Key 可轮换 |
| 端到端确认 | 车位上报带 APS ACK，失败补发 1 次；MAC 层已有 3 次重试，不重复叠加 |
| 抗干扰 | 超声波 5 点中值 + 2cm 迟滞 + 3 次确认；回波超时上报 `FAULT` 而不是"空位" |
| 掉电安全 | 白名单 A/B 双槽 + CRC + commit 标志；写一半掉电不会丢整库，也不再内置默认卡 |
| 安全联锁 | 落杆前查防砸红外，故障态保持开闸；限位/电流检测接口已预留 |
| 可观测 | ISR 耗时用 DWT 周期计数器实测；串口收发/丢帧/重传均有计数器 |
| 凭据管理 | 密码走环境变量或 0600 独立文件，CA 缺失拒绝启动，配置文件里只有"连哪里" |

## 9. 注意事项

1. **ZigBee 网络**：首次部署协调器先启动，节点自动入网。现在开了 `SECURE=1`，**协调器与全部节点必须用同一份 `f8wConfig.cfg` 编译**（同一把 TC Link Key）；只重编一部分节点会出现"入网失败"或"入网成功但数据不通"
2. **共地**：STM32 / 树莓派 / CC2530 必须共地，否则串口数据不可靠（此前刷卡不上报、云命令无响应的根因）
3. **串口电平**：USART3 与协调器、USART2 与树莓派均需电平匹配
4. **源码编码**：STM32 源码为 UTF-8，修改注意保持编码，避免中文注释乱码
5. **服务托管**：网关已由 systemd 托管，日志进 journald（`journalctl -u plate-gateway`），崩溃自动重启，开机自启；只有本地触发文件 `/tmp/plate_cmd` 仍放在 tmpfs（重启即清空，属预期）
6. **识别取景**：抓拍无真实车牌多为取景/时机问题，非算法故障；识别逻辑按用户要求暂不改
7. **影子覆盖**：多车位 `Parking_id`/`Parking_state` 单值属性互相覆盖，需要时再改产品模型（每车位一属性或 json 全量快照）
8. **车辆档案**：`vehicles.json` 为车卡统一档案（plate + cards），STM32 Flash 白名单为离线刷卡兜底，新增车辆/卡片时两处需同步
9. **看门狗副作用**：`WATCHDOG_TIMEOUT_MS`（4s）必须大于最长的正常阻塞（错误提示 3s），后续若加更长的阻塞流程需同步调大，否则会被 IWDG 复位
10. **凭据**：密码已从 `cloud_config.json` 移除，改为 `PARKING_MQTT_PASSWORD` 环境变量或
    `cloud_secret.json`（0600，已 gitignore）；CA 证书缺失时直接拒绝启动，不再退化为
    `CERT_NONE`。**注意：本仓库的历史提交里出现过明文密码**，如果仓库要公开，请在华为云
    轮换设备密钥，并用 `git filter-repo` 清理历史——删掉文件不等于删掉历史。
11. **协议要成对升级**：帧协议 VER=1，固件与树莓派必须同步更新。只升一头时，新端会拒收
    旧版本帧（旧端发裸文本时新端仍兼容），排障第一步先确认两端的协议版本。
12. **白名单分区迁移**：老固件白名单在 `0x08007000`，新固件首次启动会一次性迁移到 A 槽。
    迁移后若回滚到老固件，老固件会看到空库（老地址不再更新）——回滚前先备份白名单。
13. **传感器故障上报**：`SPOT:<id>:FAULT` → 云端 `Parking_state=fault`，需要产品模型
    把 `fault` 加进枚举，否则平台侧会丢弃这条属性。
14. **执行机构是原型件**：SG90 舵机没有限位开关、没有电机电流检测、没有防砸冗余。
    `barrier.c` 已把安全策略和接口留好（`BARRIER_HAS_LIMIT_SWITCHES` /
    `BARRIER_HAS_CURRENT_SENSE`），换真实道闸时必须补齐这几路信号再上线。
15. **ZigBee 密钥轮换**：`f8wConfig.cfg` 里的 `-DDEFAULT_KEY` 是示例值（保证 clone 下来能编能跑），
    现场部署必须换成自己的随机 16 字节，生成方法写在配置文件注释里。换钥匙等于换网络：
    协调器和所有节点一起重编、重新入网；首次升级建议整片擦除一次（Z-Stack 的 NV 页不在
    下载范围内，旧的安全表/密钥会残留），开过 `NV_RESTORE` 的设备要先清 NV，否则拿旧
    Network Key 发的帧会被信任中心直接丢弃。改完用 `tools/check_secrets.py` 自查一遍，
    并且不要把真实密钥提交进公开仓库。
16. **加卡不再打断车位采集**：白名单写入要擦 Flash（CPU 停 20~80ms），USART3 已改成 DMA
    接收 + 2KB 环形缓冲，擦除期间字节照常进 SRAM。如果以后把缓冲改回中断方式，这个坑会
    立刻回来——"加一张卡丢一帧车位"这种问题很难在现场复现。

## 10. 测试与质量保障

设计原则：**不需要硬件、不需要编译器、不需要 `pip install`**。评审者 `git clone` 之后
一条命令即可复现全部检查：

```bash
python tools/run_tests.py          # 4 个步骤，约 2 秒
python tools/run_tests.py --list   # 只看步骤
```

| 层次 | 位置 | 规模 | 外部依赖 | 覆盖什么 |
|---|---|---|---|---|
| 树莓派纯逻辑 | `raspberrypi/tests/` | 123 项 | 标准库（串口/MQTT/OpenCV 用桩） | 帧编解码、ACK/重传/去重、过闸会话 TTL、发布线程与队列背压、车牌格式与模糊匹配 |
| 固件静态/一致性 | `tests/` | 73 项 | 标准库 | 静态规则自测、Flash 分区、RAM/缓冲预算、协议黄金样本、文档漂移、CI 配置 |
| 固件静态检查 | `tools/fw_lint.py` | 5 条规则 | 无 | ISR 阻塞调用、无超时轮询、无界字符串、共享变量 volatile、Flash 解锁 |
| 密钥扫描 | `tools/check_secrets.py` | — | 无 | 明文密码、私钥、内网地址 |

CI 在 Linux + Windows × Python 3.9/3.12 四个组合上跑同一套命令（`.github/workflows/ci.yml`）。

### 10.1 为什么不引入 cppcheck / clang-tidy / pytest

| 方案 | 优点 | 为什么没选 |
|---|---|---|
| cppcheck / clang-tidy | 规则多、业界认可 | 要装工具链；Windows 上还要配 include 路径。评审者跑不起来就等于没有 |
| pytest | 插件生态好 | 多一个依赖，而 `unittest` 已经够用（两个套件合计 196 项） |
| PC 端编译 C 做单测 | 能真正执行固件逻辑 | 本机没有 host 编译器；且 `link_frame.c` 是纯函数，真要做得先引 cmake，收益不如先把静态规则做实 |

取舍很明确：**可复现性优先于规则数量**。宁可只有 5 条规则，也要保证任何人 clone 下来
两秒内跑完、结果是可信的。

### 10.2 固件静态检查的五条规则

这不是通用 C 检查器，而是"本项目踩过的坑"的自动化版本。每条规则都对应一次真实的
事故，报错信息里写着后果：

| 规则 | 检查什么 | 对应的事故 |
|---|---|---|
| `FW001` | ISR 里调用 `printf` / `delay_ms` / `malloc` | 红外 EXTI 里直接打印，115200 下阻塞数毫秒，期间串口接收被压住 |
| `FW002` | `while (寄存器/标志);` 没有超时 | `while (Read_MFRC522(CommandReg) & 0x10);`：RC522 虚焊后主循环永久卡死 |
| `FW003` | `sprintf` / `strcpy` / `strcat` / `gets` | 远程 `reason` 直接 `sprintf` 进 32 字节缓冲，可栈溢出 |
| `FW004` | ISR 里写的全局变量没有 `volatile` | `vehicle_arrive_flag` 没加 `volatile`，优化后主循环永远看不到置位 |
| `FW005` | 擦/写 Flash 的函数没有 `FLASH_Unlock()` | F1 复位后 Flash 锁定，白名单**静默写不进去**，表现为"每次加卡都失败" |

规则引擎自己也带测试（`tests/test_fw_lint.py`）：先喂**故意写错的代码片段**确认规则
真的会响，再对真实工程断言 0 findings。只报"全绿"而不证明"抓得到"的检查器没有意义。

豁免写法（必须写理由，且只作用于下一行）：

```c
/* lint:disable FW005 由调用者统一解锁 */
```

### 10.3 协议黄金样本（跨语言对表）

`tests/vectors.json` 保存了 6 条帧的**逐字节期望值**，由 `tools/gen_protocol_vectors.py`
生成。它同时是给 C 侧和 Python 侧的同一份"合同"：

```
#10509OPEN:card*91      seq=0x05  payload="OPEN:card"
```

测试除了比对字节，还会：随机生成 2000 条 payload 往返、3000 条随机字节确认不抛异常、
对每条样本逐位翻转确认**必定**被 CRC 拒绝；并从 `link_frame.c` 里核对帧长算式必须是
`6 + len + 3`——这条算式曾经少算开头的 `#`，导致固件把自己发出的每一帧都判成坏帧。

### 10.4 结构性检查（改一个数字就废板子的地方）

| 检查 | 为什么值得单独测 |
|---|---|
| Flash 分区对齐/不重叠/在 IROM1 之外 | 白名单原来在 `0x08007000`，把应用区掐死在 28KB；压到白名单页上等于每次烧固件都擦卡 |
| Keil `IROM` 与 `FLASH_APP_LIMIT_ADDR` 一致 | 两边不一致时链接器不会报错，下载时才擦到数据 |
| 白名单容量装得下 1KB 页 | 21 张卡 + 24 字节头就跨页了，擦除会擦掉隔壁分区 |
| DMA 环形缓冲 ≥ 2× 擦除窗口字节数 | 擦一页 CPU 停 20~40ms，缓冲不够就丢车位帧 |
| 行缓冲 < 环形缓冲 | 否则环再大也会被行截断 |
| 位图宽度 ≥ 车位数 | 20 个车位要 20 bit，且必须 `volatile uint32_t` |

### 10.5 文档漂移检查

README 里的波特率、引脚、分区地址、超时参数、白名单容量、协议版本、ZigBee 安全开关，
都会和源码逐项比对；还校验"当前占用：Code x + RO y + RW z = w"的加法自洽且装得进
应用区。改代码忘改文档会直接红——对外文档过期比没有文档更糟。

### 10.6 明确不覆盖的范围

写清楚边界，比堆覆盖率数字更专业：

- **没有 PC 端编译执行固件逻辑**：`link_frame.c` 是纯函数，等有 host 编译器时可以把它
  直接编进 CI，与 `vectors.json` 对拍；目前只用"常量 + 算式 + 样本"做静态对表。
- **没有 HIL（硬件在环）**：舵机、RC522、超声波的时序只能在现场验；测试覆盖的是
  "逻辑和约定"，不是"电气特性"。
- **没有覆盖率统计**：不引入 `coverage.py`（又一个依赖），靠用例清单和失败信息说话。
- **微信小程序与网页端**不在本次范围。
