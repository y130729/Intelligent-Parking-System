# STM32 <-> Raspberry Pi 串口协议

- 串口：STM32 USART2 (PA2/PA3) <-> 树莓派 /dev/serial0，波特率 115200，8N1
- 帧格式：ASCII 文本行，以 `\r\n`（或 `\n`）结尾，单行不超过 64 字节

## STM32 -> 树莓派（事件上报）

| 消息 | 含义 | 树莓派动作（华为云上报） |
|---|---|---|
| `SPOTS:<n>` | 剩余车位数 | Spot_state.remaining_spots |
| `SPOT:<id>:<0/1>` | 车位 id(1起) 状态，1=占用 | Spot_state.Parking_id + Parking_state(occupied/empty) |
| `CARD:<8位HEX>` | 刷卡卡号(4字节) | Spot_state.card_id |
| `BARRIER:OPEN` | 道闸打开（含远程命令应答） | ParkingBarrier.barrier_state=up |
| `BARRIER:CLOSE` | 道闸关闭（含远程命令应答） | ParkingBarrier.barrier_state=down |
| `CAPTURE` | 车辆到达（红外触发），请求拍照识别 | 拍照->OCR->车牌库匹配->上报 PlateRecognition.plate_number |

## 树莓派 -> STM32（下行控制/结果）

| 消息 | 含义 | STM32 动作 |
|---|---|---|
| `PLATE:<车牌>` | 识别结果（UTF-8/GB2312 中文+ASCII） | OLED 第 4 行显示 Plate:<车牌> |
| `PLATE:NONE` | 未识别到车牌 | OLED 第 4 行恢复占位 |
| `OPEN` | 云端 open_barrier 命令 | 抬杆，回发 BARRIER:OPEN |
| `CLOSE` | 云端 close_barrier 命令 | 落杆，回发 BARRIER:CLOSE |

## 云端命令

- 产品模型命令（服务 ParkingBarrier）：open_barrier / close_barrier / query_status
- 树莓派收到命令后通过串口转发 OPEN/CLOSE 给 STM32，等待 BARRIER:OPEN/CLOSE 应答（5s 超时）后向云端回执 result=success/failed
- query_status 由树莓派根据已缓存状态直接应答（remaining_spots/plate_number/barrier_state）

## 触发方式

- STM32 红外（EXTI11）触发 -> 状态机发 CAPTURE 给树莓派 -> 拍照识别 -> 上报 + 回传 PLATE 显示
- 备用本地触发（无 STM32 调试）：`echo capture > /tmp/plate_cmd`