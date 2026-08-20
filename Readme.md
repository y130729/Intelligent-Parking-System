# 智能停车场管理系统项目说明文档

## 1. 项目概述

本项目是一个基于物联网技术的智能停车场管理系统，采用 **ZigBee + WiFi** 的混合无线通信架构，实现了车位实时检测、车辆进出管理、云端数据上报等功能。

### 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     华为云IoT平台                                │
│  (MQTT协议接收数据、远程控制、数据存储与分析)                      │
└─────────────────────────┬───────────────────────────────────────┘
                          │ WiFi (ESP8266)
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                    STM32F103 网关                               │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐          │
│  │ RFID读卡器│ │ 红外传感器│ │   舵机   │ │  OLED显示 │          │
│  │ MFRC522  │ │ 车辆检测  │ │ 道闸控制 │ │  车位信息 │          │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘          │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │              CC2530 ZigBee 协调器                        │  │
│  │           (USART3接收车位节点数据)                        │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────┬───────────────────────────────────────┘
                          │ ZigBee无线网络
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│              CC2530 车位检测节点 (多个)                         │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  HC-SR04超声波传感器 → 距离测量 → 车位占用状态判断        │   │
│  │  ZigBee无线传输 → 将数据发送至网关                        │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. 硬件平台

### 2.1 CC2530 车位检测节点

| 硬件组件 | 型号 | 功能描述 |
|---------|------|---------|
| MCU | TI CC2530F256 | ZigBee SoC，集成8051内核和2.4GHz无线收发器 |
| 超声波传感器 | HC-SR04 | 距离测量，检测车位是否被占用 |
| 晶振 | 32MHz | 系统时钟 |
| 天线 | PCB天线 | ZigBee无线通信 |

### 2.2 STM32 网关

| 硬件组件 | 型号 | 功能描述 |
|---------|------|---------|
| MCU | STM32F103C8T6 | ARM Cortex-M3，72MHz主频，64KB Flash |
| RFID模块 | MFRC522 | 车辆身份识别，支持Mifare卡 |
| 红外传感器 | 红外对射传感器 | 检测车辆到达事件 |
| 舵机 | SG90 | 道闸控制 |
| WiFi模块 | ESP8266 | 连接华为云IoT平台 |
| OLED显示屏 | 0.96寸 | 显示剩余车位数量 |
| ZigBee协调器 | CC2530 | 接收车位节点数据 |

---

## 3. 软件架构

### 3.1 CC2530_ParkingNode 工程

#### 3.1.1 目录结构

```
CC2530_ParkingNode/
├── Components/           # Z-Stack协议栈组件
│   ├── hal/              # 硬件抽象层
│   │   ├── common/       # 通用HAL驱动
│   │   ├── include/      # HAL头文件
│   │   └── target/       # 目标板特定驱动(CC2530EB/USB/ZNP)
│   ├── mac/              # MAC层实现
│   ├── mt/               # Monitor Test协议
│   ├── osal/             # OSAL操作系统抽象层
│   ├── services/         # 系统服务
│   └── stack/            # Z-Stack协议栈
└── Projects/
    └── zstack/
        └── cc2530_ParkingNode/
            └── ParkingNode/
                └── Source/
                    ├── ParkingNode.c        # 车位节点主应用
                    ├── OSAL_ParkingNode.c   # OSAL任务初始化
                    └── ParkingNode.h        # 应用头文件
```

#### 3.1.2 核心功能实现

**超声波距离测量** (`ParkingNode.c:489-555`)

```c
void ParkingNode_SendDistanceData( void )
{
    // 触发超声波传感器 - 20us高电平脉冲
    TRIG_PIN = 1;
    Delay_20us();
    TRIG_PIN = 0;
    
    // 等待Echo引脚变高
    while(ECHO_PIN == 0);
    
    // 测量回波脉冲宽度
    while(ECHO_PIN == 1) {
        Delay_10us();
        us_count++;
    }
    
    // 计算距离：距离(cm) = 时间(us) / 58
    distance_data = us_count * 10 / 58;
}
```

**数据发送格式**

| 格式 | 长度 | 说明 |
|------|------|------|
| `PXX:XXXcm` | 9字节 | 正常数据，P=Parking，XX=车位号，XXX=距离(cm) |
| `PXX:err` | 7字节 | 错误数据，距离超过1000cm |

**ZigBee网络配置**

- **设备类型**: Router/End Device
- **目标地址**: 协调器(0x0000)
- **上报周期**: 100ms + 随机抖动
- **端口号**: `PARKINGNODE_ENDPOINT`

#### 3.1.3 OSAL任务调度

```c
const pTaskEventHandlerFn tasksArr[] = {
    macEventLoop,           // MAC层任务
    nwk_event_loop,         // 网络层任务
    Hal_ProcessEvent,       // HAL层任务
    MT_ProcessEvent,        // Monitor Test任务
    APS_event_loop,         // APS层任务
    ZDApp_event_loop,       // ZDO应用任务
    ParkingNode_ProcessEvent // 车位节点应用任务
};
```

---

### 3.2 STM32_Gateway 工程

#### 3.2.1 目录结构

```
STM32_Gateway/
├── Hardware/             # 硬件驱动模块
│   ├── MFRC522.c/h       # RFID读卡器驱动
│   ├── Infrared.c/h      # 红外传感器驱动
│   ├── Servo.c/h         # 舵机驱动
│   ├── OLED.c/h          # OLED显示屏驱动
│   ├── esp8266.c/h       # WiFi模块驱动
│   ├── flash_storage.c/h # Flash存储驱动
│   ├── Serial.c/h        # 串口通信
│   └── USART.c/h         # USART配置
├── Library/              # STM32标准库
├── User/                 # 用户应用层
│   ├── main.c            # 主程序入口
│   ├── state_machine.c/h # 状态机管理
│   ├── mqtt_report.c/h   # MQTT数据上报
│   ├── cc2530_parser.c/h # ZigBee数据解析
│   └── project_config.h  # 项目配置
└── System/               # 系统配置
```

#### 3.2.2 核心模块说明

**状态机管理** (`state_machine.c`)

系统实现了车辆进出的状态机控制：

| 状态 | 描述 | 触发条件 |
|------|------|---------|
| `STATE_IDLE` | 空闲状态 | 系统启动完成 |
| `STATE_WAITING_RFID` | 等待刷卡 | 红外传感器检测到车辆到达 |
| `STATE_BARRIER_UP` | 道闸抬起 | 刷卡验证通过且有剩余车位 |
| `STATE_BARRIER_DOWN` | 道闸落下 | 延时3秒后自动关闭 |

**RFID卡片验证流程**

```
车辆到达 → 红外触发 → 等待刷卡 → MFRC522_Request → MFRC522_Anticoll 
→ 获取卡号 → FlashStorage_IsCardValid → 验证通过 → 开闸
```

**MQTT数据上报** (`mqtt_report.c`)

系统通过ESP8266连接华为云IoT平台，上报以下数据：

| 上报类型 | Topic | Payload示例 |
|---------|-------|------------|
| 车位状态 | `$oc/devices/{device_id}/sys/properties/report` | `{"services":[{"service_id":"Spot_state","properties":{"Parking_id":"1","Parking_state":"occupied"}}]}` |
| 剩余车位 | 同上 | `{"services":[{"service_id":"Spot_state","properties":{"remaining_spots":15}}]}` |
| 卡号信息 | 同上 | `{"services":[{"service_id":"Spot_state","properties":{"card_id":"A1B2C3D4"}}]}` |
| 道闸状态 | 同上 | `{"services":[{"service_id":"ParkingBarrier","properties":{"barrier_state":"open"}}]}` |

**CC2530数据解析** (`cc2530_parser.c`)

定义了专用通信协议帧格式：

```
帧格式: [0xAA][spot_id][spot_state][xor_check][0xBB]
- 0xAA: 帧头
- spot_id: 车位号(1-20)
- spot_state: 状态(0=空闲, 1=占用)
- xor_check: 异或校验
- 0xBB: 帧尾
```

#### 3.2.3 主循环流程

```c
while (1) {
    StateMachine_Process();           // 处理车辆进出状态机
    OLED_DisplayRemainingSpots();      // 更新OLED显示
    ESP8266_ProcessWiFiConnection();   // 处理WiFi连接
    ESP8266_ProcessMQTTConnection();   // 处理MQTT连接
    ESP8266_ProcessData();             // 处理WiFi数据
    ProcessUltrasonicData();           // 处理超声波数据
    MQTT_ProcessHeartbeat();           // 发送心跳
    ProcessPendingSpotUpdate();        // 处理延迟的车位状态上报
    ESP8266_ProcessPendingTasks();     // 处理待处理任务
    delay_ms(10);
}
```

---

## 4. 通信协议

### 4.1 ZigBee通信（节点→网关）

**数据帧格式**:

```
Cluster ID: PARKINGNODE_P2P_CLUSTERID
Payload: "PXX:XXXcm" 或 "PXX:err"
- P: 标识字符
- XX: 车位号(01-20)
- XXX: 距离(000-999cm)
```

### 4.2 USART通信（CC2530协调器→STM32）

**帧格式**:

```
[0xAA][spot_id][spot_state][xor_check][0xBB]
- 帧头: 0xAA
- 车位号: 1字节(1-20)
- 状态: 1字节(0=空闲, 1=占用)
- 校验: 异或校验
- 帧尾: 0xBB
```

**串口配置**:
- 波特率: 115200
- 数据位: 8位
- 停止位: 1位
- 校验位: 无

### 4.3 MQTT通信（STM32→华为云）

**华为云IoT配置**:

| 参数 | 值 |
|------|-----|
| Broker | `21210fb4d9.st1.iotda-device.cn-south-1.myhuaweicloud.com` |
| Port | 1883 |
| Device ID | `6a02cb70c00ccb6d4b5364bb_HW_MQTT` |
| Client ID | `6a02cb70c00ccb6d4b5364bb_HW_MQTT_0_1_2026061608` |
| 心跳间隔 | 30秒 |

---

## 5. 项目配置参数

### 5.1 停车场配置 (`project_config.h`)

```c
#define TOTAL_SPOTS           20          // 总车位数
#define MAX_SPOT_ID           1           // 最大车位ID
#define PARKING_THRESHOLD_CM  20          // 车位占用阈值(cm)
```

### 5.2 时间配置

```c
#define RFID_TIMEOUT_MS       5000        // RFID超时时间
#define BARRIER_UP_DELAY_MS   3000        // 道闸抬起延时
#define BARRIER_DOWN_DELAY_MS 1000        // 道闸落下延时
#define HEARTBEAT_INTERVAL_S  30          // 心跳间隔(秒)
```

### 5.3 车位节点配置 (`ParkingNode.h`)

```c
#define PARKINGNODE_ENDPOINT          10   // 端点号
#define PARKINGNODE_SPOT_NUM          1    // 当前车位号
#define PARKINGNODE_SEND_PERIODIC_MSG_TIMEOUT 100  // 上报周期(ms)
```

---

## 6. 数据流分析

### 6.1 车位检测数据流

```
HC-SR04传感器 → CC2530 GPIO → 距离计算 → ZigBee发送 → CC2530协调器
                                                                 │
                                                                 ▼
                                                          STM32 USART3
                                                                 │
                                                                 ▼
                                                     cc2530_parser解析
                                                                 │
                                                                 ▼
                                                  更新spot_occupied数组
                                                                 │
                                                                 ▼
                                                    MQTT_ReportSpotStatus
                                                                 │
                                                                 ▼
                                                        华为云IoT平台
```

### 6.2 车辆进出数据流

```
红外传感器触发 → EXTI中断 → StateMachine_SetVehicleArrive
                                                          │
                                                          ▼
                                                    STATE_WAITING_RFID
                                                          │
                                                          ▼
                                                    MFRC522读卡
                                                          │
                                                          ▼
                                                    FlashStorage验证
                                                    ┌────────┴────────┐
                                                    ▼                 ▼
                                               验证通过          验证失败
                                                    │                 │
                                                    ▼                 ▼
                                            检查剩余车位        显示"无效卡片"
                                                    │
                                              ┌─────┴─────┐
                                              ▼           ▼
                                          有车位        无车位
                                              │           │
                                              ▼           ▼
                                       开闸放行      显示"车位已满"
```

---

## 7. 编译与部署

### 7.1 CC2530节点

**开发环境**: IAR Embedded Workbench for 8051

**编译步骤**:
1. 打开 `CC2530_ParkingNode.eww` 工作空间
2. 选择 `ParkingNode` 项目
3. 配置目标板类型(CC2530EB)
4. 编译生成 `.hex` 文件
5. 使用TI Flash Programmer下载到CC2530

### 7.2 STM32网关

**开发环境**: Keil MDK-ARM

**编译步骤**:
1. 打开 `Project.uvprojx` 项目
2. 选择 `Target 1`
3. 配置编译选项(使用STM32F103C8)
4. 编译生成 `.axf` 文件
5. 使用J-Link或ST-Link下载到STM32

---

## 8. 系统特性总结

| 特性 | 描述 |
|------|------|
| **无线通信** | ZigBee实现车位数据采集，WiFi实现云端连接 |
| **实时检测** | 超声波传感器100ms周期检测车位状态 |
| **智能识别** | RFID卡片验证车辆身份 |
| **自动控制** | 道闸自动开关，状态机管理 |
| **云端集成** | 华为云IoT平台数据上报与远程控制 |
| **数据可视化** | OLED实时显示剩余车位 |
| **故障处理** | CRC校验、超时重连、状态恢复 |

---

## 9. 注意事项

1. **ZigBee网络**：首次部署需确保协调器先启动，节点自动加入网络
2. **WiFi连接**：确保ESP8266能连接到配置的AP热点
3. **RFID卡片**：有效卡片需提前写入Flash存储
4. **串口通信**：CC2530协调器与STM32通过USART3连接，需注意电平匹配
5. **电源管理**：节点采用电池供电时需考虑低功耗策略