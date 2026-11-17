# 测试文档

> 最后更新：2026-11-17。适用于当前 `main` 分支。
>
> 本文档只描述**可自动执行、不需要硬件、不需要联网、不需要安装第三方库**的测试与检查。
> 硬件在环（HIL）与现场验证不在本文档范围内，边界见第 12 节。

## 1. 目的与取舍

这个项目的测试不是为覆盖率数字服务的，只为一件事：**让"看起来对"变成"能证明对"。**
以下三类问题在集成和现场阶段代价最高，而它们都能在提交前被抓到：

- **跨语言不一致**：Python 侧和 C 侧的帧格式、CRC 多项式、长度算式各写一遍，改一边忘另一边，
  结果固件把自己发出的每一帧都判成坏帧。
- **改代码忘改文档**：README 里的引脚、分区地址、用例数过期，比没有文档更糟。
- **能编译但会卡死**：`while (寄存器标志);` 没有超时、ISR 里直接 `printf`、擦 Flash 忘了解锁，
  编译器一个警告都不会给。

明确**不追求**的东西也写在前面：不做覆盖率统计、不追求通用 C 静态分析的全部规则、不替代上板测试。
理由是这些都要额外依赖或额外时间，而收益远不如把上面三类问题做实。

## 2. 测试环境与约束

| 项 | 要求 / 实测 |
|---|---|
| Python | 3.9 及以上；开发机实测 3.13.12，CI 覆盖 3.9、3.12、3.13 |
| 第三方库 | **无**。只用 `unittest` / `re` / `os` / `json` / `threading` 等标准库 |
| 串口 / MQTT / OpenCV | 由 `raspberrypi/tests/support.py` 打桩，不需要真设备 |
| 硬件 | 不需要（无板卡、无摄像头、无传感器） |
| 网络 | 不需要 |
| 操作系统 | Windows 与 Linux 均可；CI 两个平台各跑一遍完整套件 |
| 编译器 | 自动化测试不需要；固件编译另由人工用 Keil 5 / IAR 8051 验证 |

这条约束是刻意的：任何人 `git clone` 之后不装任何东西就能复现全部结果。

## 3. 快速开始

```bash
python tools/run_tests.py            # 四个步骤全跑，约 2 秒
python tools/run_tests.py --list     # 只列步骤，不执行
```

单独执行某一层：

```bash
python -m unittest discover -s raspberrypi/tests -t raspberrypi   # 树莓派逻辑 123 项
python -m unittest discover -s tests -t tests                     # 固件静态/一致性 74 项
python tools/fw_lint.py                                           # 静态检查（全工程）
python tools/fw_lint.py --list-rules                              # 打印规则清单
python tools/fw_lint.py --json --output fw_lint.json              # 机器可读结果
python tools/check_secrets.py                                     # 密钥/凭据扫描
```

退出码约定：`0` 全部通过；`1` 有用例失败或静态发现；`2` 路径参数错误。

## 4. 测试分层

| 层次 | 位置 | 条数 | 外部依赖 | 解决什么问题 |
|---|---|---|---|---|
| 树莓派纯逻辑 | `raspberrypi/tests/` | 123 | 标准库（串口/MQTT/OpenCV 打桩） | 帧编解码、ACK/重传/去重、过闸会话 TTL、发布线程与队列背压、车牌格式与模糊匹配、配置与密钥读取 |
| 固件静态/一致性 | `tests/` | 74 | 标准库 | 静态规则自测、Flash 分区与 RAM/缓冲预算、协议黄金样本、文档漂移、CI 配置 |
| 固件静态检查 | `tools/fw_lint.py` | 5 条规则 | 无 | ISR 阻塞调用、无超时轮询、无界字符串、共享变量 `volatile`、Flash 解锁 |
| 密钥扫描 | `tools/check_secrets.py` | — | 无 | 明文密码、私钥、内网地址 |

> 用例总数：197 项（树莓派 123 + 固件 74）

## 5. 固件侧用例清单（`tests/`，74 项）

### 5.1 `test_fw_lint.py` — 21 项

规则检查器自己的测试。**先证明它抓得到，再相信它说"干净"。**

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `RuleSelfTest` | 17 | 五条规则各有"喂坏代码必须响"与"合法写法必须不响"两类用例；注释/字符串不误报；豁免必须写理由且只作用于下一行 |
| `RealTreeTest` | 4 | 对真实工程断言 0 findings；每条规则都有文档；扫描器确实覆盖到了本项目源码（≥20 个文件且不含厂商库）；目标路径存在 |

### 5.2 `test_fw_docs_sync.py` — 16 项

文档与代码的语义一致性。只比对**数字和事实**（引脚、波特率、地址、超时、容量、版本、安全开关），
不比对措辞——改一句话不该红，改一个数字必须红。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `BaudRateTest` | 2 | 三路 UART 实际波特率与 README 一致（115200） |
| `PinoutTest` | 2 | README 引脚表与代码里的宏/GPIO 初始化一致（USART1 用 `DEBUG_USART_TX_GPIO_PIN` 宏） |
| `ConstantDriftTest` | 5 | 超时、看门狗、节点上报周期、白名单容量、帧版本号 |
| `ZigbeeConfigTest` | 4 | `SECURE=1`、`TC_DEVICE_MAX == TOTAL_SPOTS+1`、默认密钥不是公开的 ZigBeeAlliance09、`ZG_SECURE_DYNAMIC=0` |
| `TestCountTest` | 2 | README 与本文档声明的用例数必须等于实际数字 |
| `CodeSizeTest` | 1 | "Code x + RO y + RW z = w" 加法自洽，且装得进应用区 |

### 5.3 `test_fw_memory_map.py` — 16 项

"改一个数字就废板子"的地方。这些量在编译期不会报错，只会在下载或运行时出事。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `FlashPartitionTest` | 7 | 白名单/故障日志各槽页对齐、相邻、在 Flash 范围内；应用区止于白名单之前；Keil `IROM` 与 `FLASH_APP_LIMIT_ADDR` 一致；故障页在下载范围外；白名单装得进 1KB 页 |
| `RamBudgetTest` | 4 | IRAM 20KB；行缓冲 < 环形缓冲；DMA 环形缓冲 ≥ 2× 两页擦除窗口（921 字节） |
| `SpotModelTest` | 3 | 车位数与最大 ID 一致；dirty 位图每车位一位；节点编号在协议可表达范围内 |
| `ReadmePartitionTest` | 2 | README 分区表地址与应用区大小与工程一致 |

### 5.4 `test_fw_protocol_vectors.py` — 16 项

协议对表（详见第 8 节）。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `GoldenVectorTest` | 7 | 清单与代码常量一致；README 声明的样本数一致；编码逐字节等于样本；解码往返；`LEN` 是 UTF-8 字节数不是字符数；帧长 = 9 + payload；最坏帧不超行预算 |
| `RobustnessTest` | 4 | 逐位翻转必被 CRC 拒；2000 条随机 payload 往返；3000 条随机字节不抛异常且不误判为合法帧；截断帧必拒 |
| `FirmwareSourceTest` | 5 | 从 `link_frame.c/h` 读常量与 Python 模型对表；起始符 `#`/`*`；CRC 多项式 `0x07`；帧长算式必须含开头 `#`（`6 + len + 3`）；`LINK_FRAME_MAX_LINE` 覆盖开销 |

### 5.5 `test_ci_config.py` — 5 项

CI 配置本身的不变量（不解析 YAML，只盯最易失效的几处）。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `CiFileTest` | 5 | 无制表符/CRLF；触发条件含 push 与 pull_request；矩阵含 ubuntu+windows、覆盖 py3.9/3.12/3.13 且 `fail-fast: false`；所有 `run:` 都是纯 `python` 命令（不写 shell 专有语法）；README 徽章指向真实存在的 workflow 文件 |

## 6. 树莓派侧用例清单（`raspberrypi/tests/`，123 项）

### 6.1 `test_link_frame.py` — 23 项

帧编解码器（`stm32_protocol`/`link_frame`）的纯函数测试。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `Crc8Test` | 4 | 标准校验向量、空串为 0、接受 `str`、单比特翻转必然改变 CRC |
| `FrameEncodeTest` | 4 | 字段布局、序号掩码为 1 字节、拒绝空/超长 payload、`LEN` 按 UTF-8 字节算 |
| `FrameRoundTripTest` | 3 | ASCII、中文载荷、`bytes` 输入往返 |
| `FrameRejectTest` | 6 | 坏 CRC、截断、尾部多余字节、未知版本、缺 `*`、非帧文本按旧协议处理 |
| `FrameFuzzTest` | 3 | 随机字节不抛异常、随机 `#` 开头不抛异常、超长 `reason` 被拒而不是溢出 |
| `FirmwareParityTest` | 3 | 与 `link_frame.h` 常量对表、`reason` 枚举顺序一致、`SENSOR_FAULT` 载荷能解析 |

### 6.2 `test_gateway_hardening.py` — 31 项

串口/帧/配置的健壮性。对应的是"现场踩过或必然会踩"的坑。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `UartLineSplitTest` | 6 | 半行不算命令；一次读到多行；裸 `\n` 也分行；超长行丢弃且不崩；边界长度保留 |
| `SerialWriteTest` | 5 | 一次写一行帧；序号递增并回绕；超长 payload 拒绝；没有串口不算致命；多线程写不会交错 |
| `FrameIngressTest` | 7 | 收到帧回 ACK；重复序号只执行一次；STM32 重启后去重表复位；坏 CRC 丢弃；ACK 帧不再回 ACK；旧文本协议仍可用；传感器故障单独上报而不是当空位 |
| `SecretConfigTest` | 5 | 环境变量优先；密钥文件可用；缺密钥时**启动失败并给出提示**；权限过宽告警；配置文件本身不含密码 |
| `CaPathTest` | 3 | 相对 CA 路径按代码目录解析；CA 缺失拒绝启动（不再降级 `CERT_NONE`）；存在时原样返回 |
| `CommandQueueTest` | 5 | MQTT 回调只入队；非命令 topic 忽略；坏 JSON 忽略；队列满不抛异常；`query_status` 用本地状态直接应答 |

### 6.3 `test_gateway_flow.py` — 14 项

端到端的行为约束（仍是纯逻辑，不连云端）。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `GatewayFlowTest` | 14 | **串口线程绝不直接发网络**（用会抛异常的假客户端验证）；网络慢时串口线程仍快；发布线程能排空队列；队列满丢最旧；`CAPTURE` 开启过闸会话；会话过期后下一张卡正常；`CLOSE` 结束会话；首个凭证胜出；刷卡开闸记 `card`；远程开闸记操作人；旧固件无 `reason` 时的兼容；未知行忽略；`SPOT` 词形接受；`PLATE` 命令带上原因 |

### 6.4 `test_passage.py` — 13 项

过闸会话模型（假时钟驱动，不 `sleep`）。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestPassageSession` | 8 | 未开始时不活跃；开始后活跃；首个凭证胜出；TTL 到期失效；过期后下一张卡开新会话；`finish` 结束；重启复位状态；记录操作人 |
| `TestResolveCredential` | 5 | 显式 `reason` 优先；回落到最近刷卡；回落到最近远程命令；都没有时记 `unknown`；未知 `reason` 字符串被忽略 |

### 6.5 `test_plate_format.py` — 14 项

车牌格式与 OCR 后处理（这是"过拟合到某一张测试图"最容易发生的地方）。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestNormalizeAndRepair` | 2 | 去分隔符/统一大小写；剔除非法字母 |
| `TestValidity` | 4 | 接受结构合法的车牌；拒绝结构非法；新能源标记识别；打分偏向结构合法者 |
| `TestPostProcess` | 8 | 干净输入原样通过；修复非法字母；去掉前导噪声；新能源号保留全长度；**不再强行把 7 改成 Z**；不为省份位凭空造字母；替换表可选开启；从噪声串里挑最佳窗口 |

### 6.6 `test_plate_matcher.py` — 5 项

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestMatcher` | 5 | 精确匹配；单字符 OCR 误差仍能匹配；未登记车牌不匹配；从原始 OCR 文本走完整流水线；未登记时拒绝自动开闸 |

### 6.7 `test_plate_config.py` — 8 项

拍照参数（`--timeout` 这类"从拍静物继承来"的参数在这里被锁住）。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestCameraSettings` | 3 | 配置缺 `camera` 段时用默认值；配置可覆盖默认值；默认值不被修改 |
| `TestCameraCommand` | 5 | 生成短超时命令行；默认不加 `--immediate`；`--immediate` 与测光可从配置开启；额外参数透传；空测光参数被省略 |

### 6.8 `test_stm32_protocol.py` — 15 项

STM32 上行文本的解析与下行命令构造。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestParseLine` | 12 | `SPOTS`/`SPOT` 的 0/1 与词形、畸形输入、卡号大写化、`BARRIER` 带与不带 `reason`、未知 `reason` 清空、`LINK`/`CAPTURE`、空白行、未知行 |
| `TestBuilders` | 3 | `OPEN`/`CLOSE` 带原因；`PLATE` 构造；构造后可被解析回来 |

## 7. 静态检查规则（FW001–FW005）

这不是通用 C 检查器，而是**本项目踩过的坑的自动化版本**。每条规则的报错信息里都写着后果。

| 规则 | 检查什么 | 对应的故障模式 |
|---|---|---|
| `FW001` | ISR 里调用 `USART_Printf` / `delay_ms` / `malloc` 等阻塞或不可重入函数 | 红外 EXTI 里直接打印，115200 下阻塞数毫秒，期间串口接收被压住 |
| `FW002` | `while (寄存器/标志);` 没有超时 | `while (Read_MFRC522(CommandReg) & 0x10);`：RC522 虚焊后主循环永久卡死 |
| `FW003` | `sprintf` / `strcpy` / `strcat` / `gets` / `vsprintf` | 远程 `reason` 直接 `sprintf` 进 32 字节缓冲，可栈溢出 |
| `FW004` | ISR 里写的全局变量没有 `volatile` | `vehicle_arrive_flag` 没加 `volatile`，优化后主循环永远看不到置位 |
| `FW005` | 擦/写 Flash 的函数没有 `FLASH_Unlock()` | F1 复位后 Flash 锁定，白名单**静默写不进去**，表现为"每次加卡都失败" |

工程上做了三件事来压误报，否则规则没人愿意开：

1. **注释与字符串先被抹掉**（保持长度，行号不变），所以注释里写 `sprintf` 不会报。
2. **明确的豁免白名单**：`USART_FLAG_TXE/TC` 由硬件在 1 个字符时间内自清，轮询它们是对的；
   自增/自减条件、`do {} while();` 的尾部也不报。
3. **局部豁免必须写理由**，且只作用于同一行或下一行：

   ```c
   /* lint:disable FW005 由调用者统一解锁 */
   void Flash_Write_Slot(...)  { ... }
   ```

## 8. 协议黄金样本（跨语言对表）

`tests/vectors.json` 保存 6 条帧的**逐字节期望值**，由 `tools/gen_protocol_vectors.py` 生成，
不手写（手写样本会跟着实现一起错）。它同时是 C 侧和 Python 侧的同一份"合同"：同一组字节，
两边都必须认。

| 样本 | seq | len | 帧（ASCII 形式） |
|---|---|---|---|
| `open_card` | 5 | 9 | `#10509OPEN:card*91` |
| `open_plate` | 6 | 10 | `#1060AOPEN:plate*68` |
| `close_remote` | 7 | 12 | `#1070CCLOSE:remote*30` |
| `plate_chinese` | 42 | 15 | `#12A0FPLATE:<冀>A00001*E2`（中文占 3 字节） |
| `spot_fault` | 255 | 12 | `#1FF0CSPOT:7:FAULT*C0` |
| `link_ok` | 0 | 7 | `#10007LINK:OK*49` |

帧格式：`#` + `VER`(1 hex) + `SEQ`(2 hex) + `LEN`(2 hex) + `PAYLOAD` + `*` + `CRC8`(2 hex)，
CRC 为 CRC-8/ATM（poly `0x07`，init `0x00`），覆盖 `VER..PAYLOAD`。

除了逐字节比对，测试还会做三件事：**随机往返**（2000 条 payload）、**随机垃圾输入**（3000 条字节，
要求不抛异常且不误判）、**逐位翻转**（每条样本的每一位翻转后必须被 CRC 拒绝）。
另外直接从 `link_frame.c` 里核对帧长算式必须是 `6 + len + 3`——这条算式曾经少算开头的 `#`，
导致固件把自己发出的每一帧都判成坏帧。

## 9. 一致性与结构检查清单

| 检查项 | 失效后果 |
|---|---|
| Flash 各槽页对齐 / 不重叠 / 在应用区之外 | 白名单压在代码段上，每次烧固件等于擦掉所有卡 |
| Keil `IROM` 与 `FLASH_APP_LIMIT_ADDR` 一致 | 链接器不报错，下载时才擦到数据区 |
| 白名单容量 ≤ 1 页 | 跨页时擦除会连带擦掉隔壁分区 |
| DMA 环形缓冲 ≥ 2× 单页擦除窗口 | 擦一页 CPU 停 20~40ms，缓冲不够就丢车位帧 |
| 行缓冲 < 环形缓冲 | 环再大也会被行长度截断 |
| 位图宽度 ≥ 车位数 | 第 21 个车位无处安放 |
| README 的引脚/波特率/地址/超时/容量/版本与源码一致 | 文档误导现场接线 |
| README 与本文档的用例数等于实际数 | 对外数字不可信 |
| ZigBee 未使用公开默认密钥 | 任何人都能入网 |
| CI 矩阵覆盖双平台双版本、`run:` 步骤无 shell 专有语法 | Windows 上静默失效或直接不跑 |

## 10. 缺陷记录（已修复）

以下 5 项缺陷由自动化检查暴露，已修复并回归：

| 编号 | 发现方式 | 问题 | 影响 | 修复 |
|---|---|---|---|---|
| D-01 | `fw_lint` FW003 | `Serial.c` 的 `Serial_Printf` 用 `vsprintf` 写 `char String[100]` | 格式化结果超长即写穿栈缓冲 | 改 `vsnprintf(String, sizeof(String), ...)` |
| D-02 | `fw_lint` FW003 | `pi_link.c` 两处 ACK 用 `sprintf` 写 `char ack[8]` | 同上，越界写栈 | 改 `snprintf(ack, sizeof(ack), ...)` |
| D-03 | 强制全量重建 | `Infrared.c` 把 `uint8_t` 赋给枚举 `BitAction`（`#188-D`） | 语义含混，换编译器/开高优化可能变行为 | 改用 `uint8_t` 接收 |
| D-04 | 强制全量重建 | `Infrared.c` 文件末尾缺换行（`#1-D`） | 部分工具链拼接到下一文件时出错 | 补换行 |
| D-05 | 强制全量重建 | `OLED.c` 文件末尾缺换行（`#1-D`） | 同上 | 补换行 |

> D-03 ~ D-05 值得单独记一笔：**增量编译一直报 "0 Error(s), 0 Warning(s)"**，只有 `-r` 全量重建才暴露。
> 也就是说"编译干净"这个结论本身要先确认是不是真编译过。

## 11. 基线结果（2026-11-17 实测）

可在任意机器复现；数字变了说明基线变了，应同步更新本节与 README。

| 检查项 | 命令 | 结果 | 耗时 |
|---|---|---|---|
| 树莓派逻辑单测 | `python -m unittest discover -s raspberrypi/tests -t raspberrypi` | 123 项通过 | 0.5s |
| 固件静态/一致性 | `python -m unittest discover -s tests -t tests` | 74 项通过 | 0.5s |
| 固件静态检查 | `python tools/fw_lint.py` | 0 findings（5 条规则） | 0.1s |
| 密钥扫描 | `python tools/check_secrets.py` | 通过 | 0.7s |
| 合计 | `python tools/run_tests.py` | **4/4 通过** | 约 2.2s |

固件编译（人工执行，不属自动化测试）：

| 工具链 | 目标 | 结果 |
|---|---|---|
| Keil MDK 5（ARMCC V5.06u4） | `STM32_Gateway/Project.uvprojx` 全量重建 | 0 Error / 0 Warning，`Code=26280 RO=3968 RW=136 ZI=6336` |
| IAR 8051 | `ParkingNode.ewp` 三配置（CoordinatorEB/RouterEB/EndDeviceEB） | 0 error；7 个 warning 来自 Z-Stack 原厂库 |

## 12. 明确不覆盖的范围

写清楚边界比堆覆盖率数字更专业：

- **没有 PC 端编译执行固件 C 逻辑**。`link_frame.c` 是纯函数，等 CI 里装上 host 编译器后可以直接编进来，
  与 `vectors.json` 对拍；目前只用"常量 + 算式 + 样本"做静态对表。
- **没有 HIL**。舵机行程、RC522 读卡距离、HC-SR04 回波时序、OLED 实际显示效果，只能在现场验；
  测试覆盖的是"逻辑与约定"，不是"电气特性"。
- **没有覆盖率统计**。不引入 `coverage.py`，靠用例清单和失败信息说话。
- **没有长稳/压力测试**。ZigBee 空口丢包率、连续加卡时的帧丢失率需要现场仪器。
- **微信小程序与网页端**不在范围内。
- **Linux 与树莓派实机**未在本地验证：本机无 Linux 环境，跨平台正确性依赖 CI 首次运行的结论。

## 13. 维护约定

1. **新增用例后同步数字**：`TestCountTest` 会比对 README 与本文档声明的总数，写错就红。
2. **新增 lint 规则时**：必须同时补 `RuleSelfTest` 的正向（坏代码要响）与负向（好代码不响）用例，
   并在 README「测试与结果」的规则表与本文第 7 节的表格里各加一行。
3. **零依赖原则**：测试与检查脚本只能 import 标准库。要引第三方库，先证明没有替代方案。
4. **提交前必跑** `python tools/run_tests.py`；CI 会跑同一套命令。
5. **规则豁免要留理由**，且尽量缩小作用范围；能改代码就不要加豁免。

## 附录 A：新增一个用例的步骤

1. 固件侧放 `tests/`，树莓派侧放 `raspberrypi/tests/`，文件名与类名沿用现有风格。
2. 需要读源码常量，用 `tests/common.py` 的 `defines()` / `define_int()` / `irom1()` / `iram1()`，
   不要在用例里写死路径或数字。
3. 确认这条用例**失败时信息足够定位**（断言里带上下文，如文件名、样本名、实际值）。
4. 跑 `python tools/run_tests.py`；`TestCountTest` 会提示把 README 与本文档的数字加一。
5. 如果这条用例对应一个真实故障模式，在第 10 节记一行：发现方式、影响、修复。
