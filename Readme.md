# 智能停车场管理系统

[![CI](https://github.com/y130729/Intelligent-Parking-System/actions/workflows/ci.yml/badge.svg)](https://github.com/y130729/Intelligent-Parking-System/actions/workflows/ci.yml)

基于 **CC2530 + STM32F103 + 树莓派** 的分层停车场终端：CC2530 节点用超声波检测车位占用，
STM32F103 网关负责 RFID 识别、红外触发、道闸控制与本地显示，树莓派完成车牌识别与云端对接。
车辆凭 **车牌识别或 RFID 刷卡任一凭证** 通过同一道闸，两者并行判定、互不阻塞。

自动化检查全部不需要硬件：`python tools/run_tests.py`（197 项，约 2 秒，见「测试与结果」）。

---

## Demo

### 系统架构

```
┌──────────────────────────────────────────────────┐
│              华为云 IoTDA（MQTTS 8883）            │
│        设备影子 / 属性上报 / 命令下发               │
└───────────────────────┬──────────────────────────┘
                        │  MQTT over TLS
┌───────────────────────▼──────────────────────────┐
│  树莓派 Zero WH                                    │
│  plate_gateway.py                                  │
│   ├─ 串口帧编解码（CRC-8 / SEQ / ACK / 重传）       │
│   ├─ 车牌识别（拍照 → OCR → 车辆档案匹配）          │
│   ├─ 过闸会话与凭证判定（card / plate / remote）    │
│   └─ 发布线程（解析与上报解耦）                     │
└───────────────────────┬──────────────────────────┘
                        │  USART2 115200 8N1
                        │  #VER SEQ LEN payload *CRC
┌───────────────────────▼──────────────────────────┐
│  STM32F103C8T6 网关                                │
│   ├─ USART3 ← CC2530 协调器（车位帧）               │
│   ├─ RFID MFRC522 / 红外对射 / 舵机 / OLED          │
│   ├─ Flash 白名单 A/B 槽 + 故障现场记录             │
│   └─ 事件驱动状态机 + IWDG                         │
└───────────────────────┬──────────────────────────┘
                        │  ZigBee（SECURE=1，全帧加密）
┌───────────────────────▼──────────────────────────┐
│  CC2530 车位节点 × 20（HC-SR04 超声波）            │
│   周期上报 PXX:XXXcm / PXX:FAULT                   │
└──────────────────────────────────────────────────┘
```

### 一次过闸的时序

```
车辆到位 ──红外 EXTI11──► STM32
                          ├─► CAPTURE ──帧──► 树莓派 ─┬─ 拍照 + OCR
                          │                            └─ 匹配车辆档案
车主刷卡 ──► MFRC522 ─────┤                                   │
                          │                        命中 ──► OPEN:plate
                          │◄──────── 帧 ──────────────────────┘
                          ├─► 舵机抬杆 / OLED 显示 / 回发 BARRIER:OPEN:plate
                          └─► 保持结束后落杆，会话关闭
```

两条凭证路径同时打开：刷卡走本地白名单（离线可用），车牌走树莓派识别与档案匹配。
任一命中即开闸，本次会话内不再重复计凭证。

### 实测输出

```console
$ python tools/run_tests.py
树莓派逻辑单测                  PASS  0.5s
固件静态/一致性测试               PASS  0.4s
固件静态检查                   PASS  0.1s
密钥扫描                     PASS  0.6s

4/4 通过
```

### 串口帧示例

```
#10509OPEN:card*91
 │ │  │  │         └─ CRC-8/ATM
 │ │  │  └─────────── PAYLOAD
 │ │  └────────────── LEN = 9 字节
 │ └───────────────── SEQ
 └─────────────────── VER=1
```

---

## Highlights

- **三层处理器，边界清晰**：CC2530 只管测距与入网，STM32 只管外设与时序，树莓派只管识别与云端。
  STM32 不直接联网，网络抖动不会反压串口；实测固件占用 `Code 26280 + RO 3968 + RW 136 = 30384` 字节。
- **自定义串口帧协议**：`#VER SEQ LEN PAYLOAD *CRC8`，带 ACK、重传与去重；C 与 Python 两份实现
  逐位一致，由 6 条黄金样本与随机模糊测试对拍。
- **掉电安全白名单**：A/B 双槽交替写 + CRC32 + 提交标志，写一半掉电仍是完整旧库；
  Flash 分区与链接脚本区间互不重叠，烧写固件不会擦掉已登记的卡（最多 20 张）。
- **实时性有界**：主循环不执行阻塞动作，动作由状态机按截止时间推进；寄存器轮询与回波等待全部带超时，
  IWDG 4 秒兜底，异常复位前把 `CFSR/BFAR` 等现场写入 Flash 并在下次开机打印。
- **可验证**：197 项无硬件自动化测试 + 5 条面向本项目故障模式的静态规则，CI 在 Linux 与 Windows 上
  各跑一遍，2 秒内出结论。

---

## 架构

### 分层与职责

| 层 | 处理器 | 负责 | 明确不做 |
|---|---|---|---|
| 感知层 | CC2530F256 × 20 | 超声波测距、滤波与迟滞、ZigBee 入网上报 | 不保存业务状态，不解析云端语义 |
| 控制层 | STM32F103C8T6 | 串口汇聚、RFID 验卡、红外触发、道闸与 OLED、白名单持久化 | 不直连网络，不做车牌识别 |
| 网关层 | 树莓派 Zero WH | 车牌识别与档案匹配、过闸会话、MQTT 上报与命令中转 | 不直接驱动道闸与传感器 |
| 云端 | 华为云 IoTDA | 设备影子、属性存储、命令下发 | 不参与本地开闸判定 |

### 目录结构

| 路径 | 内容 |
|---|---|
| `CC2530_ParkingNode/` | IAR 工程（Z-Stack），车位节点与协调器共用一套源码，跳线选择角色 |
| `STM32_Gateway/` | Keil 工程：`User/`（状态机、链路、主循环）、`Hardware/`（外设驱动）、`System/`（时基、看门狗、故障记录） |
| `raspberrypi/` | 网关脚本：串口帧、识别、匹配、会话、云端；`deploy/` 为 systemd 安装脚本 |
| `tests/` | 固件侧静态与一致性测试 |
| `tools/` | 静态检查、密钥扫描、协议样本生成、一键测试 |
| `TESTING.md` | 测试文档：用例清单、规则、结果基线 |
| `WechatProject/` | 微信小程序（独立目录） |

### 数据流

1. **车位检测**：HC-SR04 测距 → CC2530 五点中值 + 2cm 迟滞 + 连续 3 次确认 → ZigBee → 协调器 UART
   → STM32 解析 `PXX:XXXcm` → 占用状态变化 → `SPOT:<id>:<0|1>` 与 `SPOTS:<n>` → 树莓派 → 云端。
2. **车辆过闸**：红外触发 → `CAPTURE` → 树莓派拍照识别；同时 RC522 轮询刷卡。任一凭证命中 → `OPEN:<reason>`
   → 抬杆、OLED 显示、`BARRIER:OPEN:<reason>` 回传 → 保持结束后落杆 → 会话关闭。
3. **车牌识别**：`rpicam-still` 抓拍 → Tesseract（`chi_sim`）OCR → 结构校验与纠正 → 档案模糊匹配
   → `PLATE:<车牌>` 回显到 OLED，命中则同时 `OPEN:plate`。

---

## 硬件与软件

### 硬件

| 部件 | 型号 | 说明 |
|---|---|---|
| 车位节点 MCU | TI CC2530F256 | 8051 内核 + 2.4GHz 收发，256KB Flash / 8KB SRAM |
| 测距 | HC-SR04 | TRIG=P0.4，ECHO=P0.5 |
| 网关 MCU | STM32F103C8T6 | Cortex-M3 @72MHz，64KB Flash / 20KB SRAM |
| 读卡 | MFRC522 | 软件 SPI：SDA=PA4、SCK=PA5、MISO=PA6、MOSI=PA7、RST=PB1 |
| 车辆检测 | 红外对射 | PA11 / EXTI11 |
| 执行机构 | SG90 舵机 | TIM2_CH2 PWM @PA1，原型件（见「设计说明 · 安全联锁与执行机构边界」） |
| 显示 | 0.96" SSD1306 | 软件 I2C：SCL=PB8、SDA=PB9，含 16×16 中文字库 |
| 网关主机 | 树莓派 Zero WH | ARMv6 / 512MB，CSI 摄像头（2592×1944） |

### 接线与串口

| 接口 | 引脚 | 波特率 | 对端 |
|---|---|---|---|
| USART1 | PA9/PA10 | 115200 8N1 | 调试串口（日志、白名单命令） |
| USART2 | PA2/PA3 | 115200 8N1 | 树莓派 `/dev/serial0` |
| USART3 | PB10/PB11 | 115200 8N1 | CC2530 协调器 UART0 |

三块板必须**共地**，否则串口偶发丢字节且难以复现。

### Flash 分区（STM32F103C8T6，64KB，1KB/页）

| 地址范围 | 用途 |
|---|---|
| `0x08000000` – `0x0800EFFF` | 应用程序（IROM1 = `0xF000`，即 60KB；越界即链接失败） |
| `0x0800F000` – `0x0800F3FF` | 刷卡白名单 A 槽 |
| `0x0800F400` – `0x0800F7FF` | 刷卡白名单 B 槽（与 A 槽交替写入） |
| `0x0800FC00` – `0x0800FFFF` | 故障现场记录（在下载范围之外，升级固件不擦除） |

白名单容量 20 张（最多 20 张），RST 后的下载范围不含数据页，所以重新烧写固件不会清空已登记的卡。

### 工具链与依赖

| 目标 | 工具 | 说明 |
|---|---|---|
| CC2530 | IAR EW8051 + TI Z-Stack | 工程 `ParkingNode.ewp`，配置 `CoordinatorEB` / `RouterEB` / `EndDeviceEB` |
| STM32 | Keil MDK 5（ARMCC V5.06） | 工程 `Project.uvprojx`，标准外设库 |
| 树莓派 | Python 3.9+ | `pyserial`、`paho-mqtt`、`OpenCV`、`pytesseract` + Tesseract `chi_sim` |
| 运行时 | systemd | `Restart=always`、日志进 journald、`MemoryMax=256M` |

---

## Quick Start

### 1. 车位节点（CC2530）

```bash
# GUI：打开 ParkingNode.eww，选目标板 CC2530EB，编译后用 TI Flash Programmer 下载
# 命令行：
IarBuild.exe "CC2530_ParkingNode/Projects/zstack/cc2530_ParkingNode/ParkingNode/CC2530DB/ParkingNode.ewp" \
    -build EndDeviceEB        # 另有 CoordinatorEB / RouterEB；退出码 0 = 成功
```

每块节点板改 `PARKINGNODE_SPOT_NUM`（车位号），跳线决定协调器/节点角色。协调器与全部节点必须用同一份
`f8wConfig.cfg` 编译，否则入网失败。

### 2. 网关固件（STM32）

```bash
# GUI：打开 Project.uvprojx，Target 1，编译并下载（ST-Link / J-Link）
# 命令行（无界面）：
UV4.exe -b STM32_Gateway/Project.uvprojx -j0 -o build.log   # 0=无警告 1=有警告 2=错误
```

当前构建结果：**0 error / 0 warning**，应用区占用 `Code 26280 + RO 3968 + RW 136 = 30384` 字节（约 29.7KB / 60KB），
RAM 为 `RW 136 + ZI 6336 = 6472` 字节（约 6.3KB / 20KB）。

### 3. 树莓派网关

```bash
# 依赖（Pi Zero 上 OpenCV 用 apt 装，pip 编译不动）
sudo apt install -y python3-venv python3-opencv tesseract-ocr tesseract-ocr-chi-sim

# 部署到 /opt/parking 并注册服务（内部建 venv、写 unit、开机自启）
sudo mkdir -p /opt/parking && sudo cp -r raspberrypi /opt/parking/
sudo /opt/parking/raspberrypi/deploy/install.sh /opt/parking pi

# 设备密钥：环境变量或 0600 独立文件，绝不写进 cloud_config.json
sudo sh -c 'printf "{\"password\": \"%s\"}\n" "<device-secret>" > /opt/parking/raspberrypi/cloud_secret.json'
sudo chmod 600 /opt/parking/raspberrypi/cloud_secret.json
```

> `install.sh` 会新建 venv 并 `pip install`，需要外网。现场机器若无外网，改为复用已有 venv 原地替换：
> 备份 `.py` 与 `.json` → 覆盖脚本与 `tests/` → 清掉 `__pycache__` → `sudo systemctl restart plate-gateway`。
> 设备本地的 `cloud_config.json`、`vehicles.json`、`cloud_secret.json` 不要被仓库模板覆盖。

日常运维：

```bash
sudo systemctl status plate-gateway
journalctl -u plate-gateway -f          # 日志（不写 /tmp）
sudo systemctl restart plate-gateway
echo capture > /tmp/plate_cmd           # 手动触发一次拍照
```

### 4. 自检

```bash
python tools/run_tests.py               # 全部无硬件检查（197 项，约 2 秒）
python tools/check_secrets.py           # 明文密码 / 私钥 / 内网地址扫描
```

---

## 使用说明

### 操作流程

1. 车辆驶到闸前压住红外对射，STM32 发 `CAPTURE`，树莓派开始拍照识别。
2. 车主刷卡（RC522 读 4 字节卡号）或识别命中车辆档案，任一满足即抬杆。
3. OLED 共 4 行：标题、`Spots: n/20` 余量、状态提示（如 `Please Swipe Card`）、`Plate:<车牌>`。
4. 保持 `BARRIER_UP_DELAY_MS` 后自动落杆；落杆前检查防砸红外，故障态保持开闸。

### 本地调试（不依赖云端）

```bash
echo capture > /tmp/plate_cmd              # 触发一次拍照识别
journalctl -u plate-gateway -f            # 观察识别与开闸过程
sudo systemctl stop plate-gateway         # 调试时先停服务，避免与手动进程抢串口
```

### 可调参数

```c
/* User/project_config.h */
#define TOTAL_SPOTS                    20      /* 车位数 */
#define PARKING_THRESHOLD_CM           20      /* 距离小于 20cm 判占用 */
#define BARRIER_UP_DELAY_MS            3000    /* 抬杆保持时间 */
#define BARRIER_MOVE_MS                1500    /* 舵机单程时间 */
#define ERROR_DISPLAY_DELAY_MS         3000    /* 错误提示时长 */
#define WAIT_CARD_TIMEOUT_MS           30000   /* 到位后等凭证的上限，超时回 IDLE */

/* Hardware/watchdog.h */
#define WATCHDOG_TIMEOUT_MS            4000u   /* IWDG 溢出时间 */
#define WATCHDOG_FEED_INTERVAL_MS      500u    /* 喂狗间隔 */

/* CC2530 ParkingNode.h */
#define PARKINGNODE_SEND_PERIODIC_MSG_TIMEOUT  100  /* 上报周期(ms)，叠加随机抖动 */
```

`WATCHDOG_TIMEOUT_MS` 必须大于最长正常阻塞（当前为错误提示 3s）；新增更长的阻塞流程时需同步调整。

### 通信协议

#### ZigBee（节点 → 协调器 → STM32）

| 方向 | 内容 |
|---|---|
| 节点 → 协调器 | `PXX:XXXcm`（车位号 + 距离）、`PXX:err`（超量程）、`PXX:FAULT`（回波超时） |
| 协调器 → STM32 | UART0 原样转发并补 `\r\n`，115200 8N1 |

安全参数在 `f8wConfig.cfg`：`-DSECURE=1`（NWK/APS 全帧加密）、`-DZG_SECURE_DYNAMIC=0`、
`-DZDSECMGR_TC_DEVICE_MAX=21`（1 个协调器 + 20 个节点）。车位上报带 `AF_ACK_REQUEST`，
失败补发 1 次；广播与组播不走 APS 确认。

#### STM32 ↔ 树莓派（USART2）

```
#  VER  SEQ  LEN  PAYLOAD          *  CRC
1   1     2     2   0..63           1   2     帧长 = 9 + LEN
```

| 字段 | 说明 |
|---|---|
| `#` | 帧起始符 |
| `VER` | 协议版本，当前 `VER=1`；版本不符直接丢弃 |
| `SEQ` | 0–255 循环序号，用于去重与重传匹配 |
| `LEN` | payload 字节数（UTF-8 计字节，上限 63） |
| `CRC` | CRC-8/ATM（poly `0x07`、init `0x00`），覆盖 VER..PAYLOAD |

链路机制：发出后 300ms 未收到 `ACK:<SEQ>` 重发，最多 2 次；接收端按 SEQ 去重，重复帧只补 ACK；
不带 `#` 的行按裸文本兼容处理；超长行丢弃并重新同步。

**上行（STM32 → 树莓派）**

| 消息 | 含义 |
|---|---|
| `SPOTS:<n>` | 剩余车位数 |
| `SPOT:<id>:<0\|1>` | 车位状态（1=占用） |
| `SPOT:<id>:FAULT` | 传感器故障，与空位区分 |
| `CARD:<8位HEX>` | 刷卡卡号（4 字节） |
| `CAPTURE` | 车辆到达，请求拍照识别 |
| `BARRIER:OPEN[:<reason>]` | 抬杆完成，`reason` ∈ card / plate / remote |
| `BARRIER:CLOSE` | 落杆完成（不带 reason） |
| `ACK:<HEX>` | 对下行帧的确认 |

**下行（树莓派 → STM32）**

| 消息 | 含义 |
|---|---|
| `PLATE:<车牌>` | 识别结果，显示到 OLED 第 4 行 |
| `PLATE:NONE` | 未识别，恢复占位 |
| `OPEN[:<reason>]` | 抬杆指令 |
| `CLOSE[:<reason>]` | 落杆指令 |
| `ACK:<HEX>` | 对上行帧的确认 |

`reason` 是白名单枚举（固件 `LinkReason` 与树莓派 `CREDENTIAL_*` 取值一一对应），不是自由文本；
未知取值一律拒收。凭证判定只发生在抬杆那一刻，关闸只用于结束会话。

#### 云端（华为云 IoTDA）

| 项 | 值 |
|---|---|
| 连接 | MQTTS 8883，CA 缺失即拒绝启动 |
| 凭据 | `PARKING_MQTT_PASSWORD` 环境变量或 0600 的 `cloud_secret.json` |
| 上报 | `$oc/devices/{device_id}/sys/properties/report` |
| 命令 | `$oc/devices/{device_id}/sys/commands/#`：`open_barrier` / `close_barrier` / `query_status` |

### 已知限制

- **执行机构是原型件**：SG90 没有限位开关与电流检测，落杆依赖固定延时；换真实道闸前需补齐防砸与限位信号。
- **车位属性是单值**：`Parking_id` + `Parking_state` 多车位会互相覆盖影子，需要每车位独立属性或全量快照。
- **传感器故障需要云端枚举**：`Parking_state=fault` 必须在产品模型里声明，否则平台侧拒收。
- **协议成对升级**：`VER=1`，固件与树莓派需同步更新；只升一端时新端会拒收旧版本帧。
- **车辆档案双份**：`vehicles.json` 是车卡统一档案，STM32 白名单是离线兜底，新增车辆需两处同步。
- **识别效果依赖取景**：抓拍时机与构图直接影响 OCR 结果，属现场调参范围。

---

## 设计说明

### 1. 网络边界放在树莓派，而不是 MCU

STM32 只做本地闭环：读卡、触发、驱动、持久化。所有网络行为在树莓派完成，因此网络抖动只影响上报延迟，
不影响一次过闸。反过来，MCU 侧也不需要 TCP/IP 栈与 TLS，省下的 Flash 留给业务与安全分区。

### 2. 协议用"文本外壳 + CRC"，不是纯二进制

payload 只有十几字节、链路 115200，纯二进制省不下几个字节，却让排障变难。
当前方案 `cat /dev/serial0` 就能直接读出 `OPEN:card`；同时用 CRC-8、序号、ACK、重传补上可靠性。
如果以后链路升到 460800 以上或 payload 上百字节，再换二进制分帧（COBS/SLIP）才有意义。

### 3. 白名单按"掉电原子"设计

记录结构含 magic / version / seq / len / CRC32。写入流程：擦非活动槽 → 逐字写入 → 回读校验 → 写提交标志
→ 切换活动槽。任一步掉电，另一个槽仍是完整旧库；读取校验不过就当空库处理，不会把半写数据当成卡号。
固件里不内置任何默认卡。

### 4. 状态机按截止时间推进，不用阻塞延时

`state_timer` 以 10ms 时基累加，状态切换只设置目标状态与截止时间，舵机与提示音不再 `delay_ms` 占用主循环。
主循环保持恒定节拍，串口与红外事件不会因为一次抬杆而延迟上百毫秒。

### 5. 一切等待都有上界

寄存器轮询（RC522 复位、USART 标志）、超声波回波等待、云端确认全部带超时；超时走错误路径而不是死等。
IWDG 4 秒兜底，HardFault 记录 `CFSR/BFAR` 等现场到 Flash 保留页并在下次开机打印，异常可追溯而不只是复位。

### 6. 串口线程只做"解析 + 入队"

`handle_stm32_line` 不进行任何网络调用：事件压入 `queue.Queue`，由独立发布线程消费；队列满时丢最旧一条并计数。
这样云端变慢不会阻塞串口读取，STM32 持续上报也不会撑爆接收缓冲。

### 7. 过闸会话与凭证审计

收到 `CAPTURE` 开启会话（带 TTL），会话内首个凭证胜出，`BARRIER:CLOSE` 或超时结束。
每次开闸记录凭证来源（card / plate / remote）与操作人，区分本地刷卡、车牌识别与远程开闸，便于事后追溯。
远程开闸单独记 `remote`，不与刷卡混淆。

### 8. 车牌识别：结构约束优先，替换表只做兜底

先按中国车牌结构校验（省份简称 + 发牌机关字母 + 序号 5 位，区分燃油与新能源），再做模糊匹配；
常见 OCR 混淆（如 `0/O`、`1/I`）以可配置表兜底，而不是针对某块测试车牌写死替换规则。
匹配允许单字符误差，但未登记车牌不会自动开闸。

### 9. 安全联锁与执行机构边界

`barrier.c` 在落杆前检查防砸红外，故障态保持开闸；限位开关与电机电流检测以编译开关预留接口
（`BARRIER_HAS_LIMIT_SWITCHES` / `BARRIER_HAS_CURRENT_SENSE`）。原型用 SG90 演示动作，
生产道闸必须补齐这几路信号才能上线。

---

## 测试与结果

| 层次 | 位置 | 规模 | 覆盖内容 |
|---|---|---|---|
| 树莓派逻辑 | `raspberrypi/tests/` | 123 项 | 帧编解码、ACK/重传/去重、会话 TTL、队列背压、车牌格式与匹配、配置与密钥读取 |
| 固件静态/一致性 | `tests/` | 74 项 | 静态规则自测、Flash 分区与 RAM 预算、协议黄金样本、文档漂移、CI 配置 |
| 固件静态检查 | `tools/fw_lint.py` | 5 条规则 | ISR 阻塞调用、无超时轮询、无界字符串、共享变量 volatile、Flash 解锁 |
| 密钥扫描 | `tools/check_secrets.py` | — | 明文密码、私钥、内网地址 |

合计 197 项，一条命令约 2 秒跑完；CI 在 Linux 与 Windows × Python 3.9/3.12 四个组合上执行同一套命令。

### 静态规则

规则针对本项目的失败模式，而非通用 C 检查：

| 规则 | 检查 | 典型后果 |
|---|---|---|
| `FW001` | ISR 中调用 `printf` / `delay_ms` / `malloc` | 中断里阻塞数毫秒，串口接收被压住 |
| `FW002` | `while (标志);` 无超时 | 器件掉线后主循环永久卡死 |
| `FW003` | `sprintf` / `strcpy` / `strcat` / `gets` | 远程字段过长时写穿栈缓冲 |
| `FW004` | ISR 写全局变量缺 `volatile` | 开优化后主循环看不到置位 |
| `FW005` | 擦写 Flash 缺 `FLASH_Unlock()` | 写入静默失败，表现为"加卡不生效" |

### 协议黄金样本

`tests/vectors.json` 保存了 6 条帧的逐字节样本，同时作为 C 与 Python 两侧的合同；测试还会做随机往返、
随机字节模糊输入，并逐位翻转确认 CRC 必定拒绝。

### 实测结果

| 项 | 结果 |
|---|---|
| 自动化测试 | 197 项通过，约 2 秒 |
| 静态检查 | 0 findings |
| Keil 全量重建 | 0 error / 0 warning，`Code 26280 + RO 3968 + RW 136 = 30384` 字节 |
| IAR 三配置 | 0 error（7 条 warning 来自 Z-Stack 厂商库） |

### 不覆盖范围

- 没有 PC 端编译执行固件 C 逻辑：`link_frame.c` 的验证方式是常量、算式与样本三方对表。
- 没有硬件在环：舵机行程、读卡距离、回波时序、OLED 效果需现场验证。
- 没有覆盖率统计：以用例清单与失败信息为准，不引第三方覆盖率工具。
- 微信小程序与网页端不在本仓库测试范围内。

完整的用例清单、规则说明与基线数据见 [`TESTING.md`](TESTING.md)。
