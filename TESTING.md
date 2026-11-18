# 测试文档

> 适用范围：当前 `main` 分支。最后更新：2026-11-18。
>
> 本文档描述仓库内可自动执行的测试与检查项，范围限定为不依赖目标硬件、不依赖外部网络、
> 不引入第三方 Python 库的部分。硬件在环（HIL）测试与现场验证不在本文档范围内，见第 12 节。

## 1. 测试目标

测试针对以下三类缺陷建立拦截，这三类缺陷在集成阶段和现场阶段均难以定位：

| 缺陷类别 | 典型表现 | 拦截手段 |
|---|---|---|
| 跨语言实现不一致 | C 与 Python 各维护一份帧格式、CRC 参数与长度算式，修改一侧后另一侧失效，固件将自身发出的帧判为非法帧 | 常量与算式对表、逐字节样本、双语言往返与模糊测试 |
| 文档与实现不一致 | 引脚分配、分区地址、超时参数、用例数量随代码变化而过期，读者按文档配置后无法工作 | 从源码解析常量，与文档逐项比对 |
| 编译通过但运行异常 | 寄存器轮询无超时、ISR 内调用阻塞函数、擦写 Flash 前未解锁、ISR 与主循环共享变量缺少 `volatile` | 静态规则检查 |

测试不包含以下内容，相关边界见第 12 节：覆盖率统计、通用 C 语言静态分析的完整规则集、硬件在环验证。

## 2. 测试环境与约束

| 项 | 要求 / 实测 |
|---|---|
| Python | 3.9 及以上；开发环境实测 3.13.12，CI 覆盖 3.9、3.12、3.13 |
| 第三方库 | 无。仅使用 `unittest`、`re`、`os`、`json`、`threading` 等标准库 |
| 串口 / MQTT / OpenCV | 由 `raspberrypi/tests/support.py` 提供桩实现，无需真实设备 |
| 硬件 | 不需要（无开发板、无摄像头、无传感器） |
| 网络 | 不需要 |
| 操作系统 | Windows 与 Linux；CI 在两个平台分别执行完整测试集 |
| 编译器 | 自动化测试不需要；固件编译结果由 Keil 5 与 IAR 8051 单独验证 |

上述约束保证仓库克隆后无需安装任何依赖即可复现全部结果。

## 3. 执行方式

```bash
python tools/run_tests.py            # 依次执行全部四个步骤，约 2 秒
python tools/run_tests.py --list     # 仅列出步骤，不执行
```

单独执行某一层：

```bash
python -m unittest discover -s raspberrypi/tests -t raspberrypi   # 树莓派逻辑 123 项
python -m unittest discover -s tests -t tests                     # 固件静态/一致性 74 项
python tools/fw_lint.py                                           # 静态检查（全工程）
python tools/fw_lint.py --list-rules                              # 输出规则清单
python tools/fw_lint.py --json --output fw_lint.json              # 输出机器可读结果
python tools/check_secrets.py                                     # 凭据扫描
```

| 退出码 | 含义 |
|---|---|
| `0` | 全部通过 |
| `1` | 存在失败用例或静态检查发现 |
| `2` | 路径参数无效 |

## 4. 测试分层与规模

| 层次 | 位置 | 条数 | 外部依赖 | 覆盖内容 |
|---|---|---|---|---|
| 树莓派逻辑 | `raspberrypi/tests/` | 123 | 标准库（串口、MQTT、OpenCV 为桩） | 帧编解码、ACK/重传/去重、过闸会话 TTL、发布线程与队列背压、车牌格式与模糊匹配、配置与凭据读取 |
| 固件静态/一致性 | `tests/` | 74 | 标准库 | 静态规则自测、Flash 分区与 RAM 预算、协议样本、文档一致性、CI 配置 |
| 固件静态检查 | `tools/fw_lint.py` | 5 条规则 | 无 | ISR 阻塞调用、无超时轮询、无界字符串、共享变量 `volatile`、Flash 解锁 |
| 凭据扫描 | `tools/check_secrets.py` | — | 无 | 明文密码、私钥、内网地址 |

> 用例总数：197 项（树莓派 123 + 固件 74）

## 5. 固件侧用例清单（`tests/`，74 项）

### 5.1 `test_fw_lint.py` — 21 项

校验静态检查器自身的行为。每条规则须同时具备触发用例（缺陷代码必须被报告）与不触发用例（合法代码不得被报告）。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `RuleSelfTest` | 17 | 五条规则的正向与反向用例；注释与字符串字面量不参与匹配；豁免须注明理由且作用域为同一行或下一行 |
| `RealTreeTest` | 4 | 真实工程扫描结果为 0 项；每条规则均有文档；扫描范围覆盖本项目源码（不少于 20 个文件，不含厂商库）；目标路径存在性检查 |

### 5.2 `test_fw_docs_sync.py` — 16 项

文档与源码的语义一致性检查。比对对象为数值与标识符（引脚、波特率、地址、超时、容量、版本、安全开关），不比对表述文字。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `BaudRateTest` | 2 | 三路 UART 的实际波特率与 `Readme.md` 一致（115200） |
| `PinoutTest` | 2 | `Readme.md` 引脚表与代码中的宏、GPIO 初始化一致（USART1 使用 `DEBUG_USART_TX_GPIO_PIN` 宏） |
| `ConstantDriftTest` | 5 | 超时参数、看门狗参数、节点上报周期、白名单容量、协议版本号 |
| `ZigbeeConfigTest` | 4 | `SECURE=1`、`TC_DEVICE_MAX == TOTAL_SPOTS + 1`、默认密钥非 ZigBeeAlliance09 公开值、`ZG_SECURE_DYNAMIC=0` |
| `TestCountTest` | 2 | `Readme.md` 与本文档声明的用例数量等于实际数量 |
| `CodeSizeTest` | 1 | 文档中的 `Code + RO + RW` 加法自洽，且不超过应用区容量 |

### 5.3 `test_fw_memory_map.py` — 16 项

Flash 分区、RAM 预算与车位模型的结构性约束。上述数值在编译阶段不产生任何告警。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `FlashPartitionTest` | 7 | 各槽页对齐、相邻、位于 Flash 范围内；应用区止于白名单之前；Keil `IROM` 与 `FLASH_APP_LIMIT_ADDR` 一致；故障记录页在下载范围之外；白名单容量不超过 1 页 |
| `RamBudgetTest` | 4 | IRAM 为 20KB；行缓冲小于环形缓冲；DMA 环形缓冲不小于两页擦除窗口（921 字节） |
| `SpotModelTest` | 3 | 车位数与最大 ID 一致；dirty 位图每车位一位；节点编号在协议可表达范围内 |
| `ReadmePartitionTest` | 2 | `Readme.md` 分区表地址与应用区大小与工程配置一致 |

### 5.4 `test_fw_protocol_vectors.py` — 16 项

协议样本与跨语言一致性检查，详见第 8 节。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `GoldenVectorTest` | 7 | 样本清单与代码常量一致；`Readme.md` 声明的样本数一致；编码结果逐字节等于样本；解码往返；`LEN` 为 UTF-8 字节数；帧长为 9 + payload；最坏帧不超出行预算 |
| `RobustnessTest` | 4 | 逐位翻转须被 CRC 拒绝；2000 条随机 payload 往返；3000 条随机字节不抛异常且不判为合法帧；截断帧须拒绝 |
| `FirmwareSourceTest` | 5 | 从 `link_frame.c/h` 读取常量与 Python 模型对表；起始符 `#` 与 `*`；CRC 多项式 `0x07`；帧长算式须计入起始符（`6 + len + 3`）；`LINK_FRAME_MAX_LINE` 覆盖全部开销 |

### 5.5 `test_ci_config.py` — 5 项

CI 配置文件的结构不变量。检查不解析 YAML 语法，仅校验易失效的结构项。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `CiFileTest` | 5 | 无制表符与 CRLF；触发条件包含 push 与 pull_request；矩阵覆盖 ubuntu 与 windows、Python 3.9/3.12/3.13 且设置 `fail-fast: false`；所有 `run:` 为纯 `python` 命令；`Readme.md` 徽章指向存在的 workflow 文件 |

## 6. 树莓派侧用例清单（`raspberrypi/tests/`，123 项）

### 6.1 `test_link_frame.py` — 23 项

帧编解码实现（`stm32_protocol`、`link_frame`）的纯函数测试。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `Crc8Test` | 4 | 标准校验向量；空输入为 0；接受 `str` 输入；单比特翻转必然改变 CRC |
| `FrameEncodeTest` | 4 | 字段布局；序号掩码为 1 字节；拒绝空与超长 payload；`LEN` 按 UTF-8 字节计算 |
| `FrameRoundTripTest` | 3 | ASCII 载荷；中文载荷；`bytes` 输入往返 |
| `FrameRejectTest` | 6 | 坏 CRC；截断帧；尾部多余字节；未知版本；缺少 `*`；非帧文本按旧协议处理 |
| `FrameFuzzTest` | 3 | 随机字节不抛异常；随机 `#` 开头输入不抛异常；超长 `reason` 被拒绝 |
| `FirmwareParityTest` | 3 | 与 `link_frame.h` 常量对表；`reason` 枚举顺序一致；`SENSOR_FAULT` 载荷可解析 |

### 6.2 `test_gateway_hardening.py` — 31 项

串口读取、帧收发与配置读取的健壮性测试。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `UartLineSplitTest` | 6 | 半行不作为命令；单次读取多行；裸 `\n` 分行；超长行丢弃且不崩溃；边界长度保留 |
| `SerialWriteTest` | 5 | 单次写出一行帧；序号递增并回绕；超长 payload 拒绝；串口不存在时不中止进程；多线程写入不交错 |
| `FrameIngressTest` | 7 | 收到帧后回 ACK；重复序号仅执行一次；STM32 重启后去重表复位；坏 CRC 丢弃；ACK 帧不再回 ACK；旧文本协议可用；传感器故障单独上报 |
| `SecretConfigTest` | 5 | 环境变量优先；凭据文件可用；缺少凭据时启动失败并输出原因；权限过宽时告警；配置文件不含密码 |
| `CaPathTest` | 3 | 相对 CA 路径按代码目录解析；CA 缺失时拒绝启动；CA 存在时原样返回 |
| `CommandQueueTest` | 5 | MQTT 回调仅入队；非命令 topic 忽略；无效 JSON 忽略；队列满时不抛异常；`query_status` 使用本地状态应答 |

### 6.3 `test_gateway_flow.py` — 14 项

网关处理流程的行为约束，使用桩对象，不连接云端。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `GatewayFlowTest` | 14 | 串口线程不进行网络调用（以会抛异常的桩客户端验证）；网络变慢时串口线程不受影响；发布线程可排空队列；队列满时丢弃最旧项；`CAPTURE` 开启过闸会话；会话过期后下一张卡正常处理；`CLOSE` 结束会话；首个凭证胜出；刷卡开闸记录 `card`；远程开闸记录操作人；旧固件无 `reason` 时的兼容行为；未知行忽略；`SPOT` 词形接受；`PLATE` 命令携带原因 |

### 6.4 `test_passage.py` — 13 项

过闸会话模型测试，使用可控时钟，不依赖真实时间。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestPassageSession` | 8 | 未开始时不活跃；开始后活跃；首个凭证胜出；TTL 到期失效；过期后下一张卡开启新会话；`finish` 结束会话；重启后状态复位；记录操作人 |
| `TestResolveCredential` | 5 | 显式 `reason` 优先；回落至最近刷卡；回落至最近远程命令；均无时记为 `unknown`；未知 `reason` 字符串忽略 |

### 6.5 `test_plate_format.py` — 14 项

车牌格式校验与 OCR 后处理测试。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestNormalizeAndRepair` | 2 | 去除分隔符并统一大小写；剔除非法字母 |
| `TestValidity` | 4 | 接受结构合法的车牌；拒绝结构非法的输入；新能源标记识别；评分偏向结构合法者 |
| `TestPostProcess` | 8 | 合法输入原样通过；修复非法字母；去除前导噪声；新能源号保留完整长度；不对 `7` 做 `Z` 替换；不为省份位生成字母；替换表默认关闭；从噪声串中选出最佳窗口 |

### 6.6 `test_plate_matcher.py` — 5 项

车辆档案匹配测试。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestMatcher` | 5 | 精确匹配；单字符 OCR 误差仍可匹配；未登记车牌不匹配；原始 OCR 文本走完整流水线；未登记时拒绝自动开闸 |

### 6.7 `test_plate_config.py` — 8 项

拍照参数与命令行构造测试。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestCameraSettings` | 3 | 配置缺少 `camera` 段时使用默认值；配置可覆盖默认值；默认值不被修改 |
| `TestCameraCommand` | 5 | 生成短超时命令行；默认不添加 `--immediate`；`--immediate` 与测光可由配置开启；额外参数透传；空测光参数被省略 |

### 6.8 `test_stm32_protocol.py` — 15 项

STM32 上行文本解析与下行命令构造测试。

| 类 | 条数 | 覆盖内容 |
|---|---|---|
| `TestParseLine` | 12 | `SPOTS`/`SPOT` 的 0/1 与词形；畸形输入；卡号转为大写；`BARRIER` 带与不带 `reason`；未知 `reason` 清空；`LINK`/`CAPTURE`；空白行；未知行 |
| `TestBuilders` | 3 | `OPEN`/`CLOSE` 携带原因；`PLATE` 构造；构造结果可被解析还原 |

## 7. 静态检查规则（FW001–FW005）

`tools/fw_lint.py` 实现 5 条规则。规则集针对本项目已确认的故障模式，不作为通用 C 语言检查器使用。

| 规则 | 检查内容 | 对应故障模式 |
|---|---|---|
| `FW001` | ISR 中调用 `USART_Printf`、`delay_ms`、`malloc` 等阻塞或不可重入函数 | 红外 EXTI 中断内打印日志，115200 波特率下阻塞数毫秒，中断期间串口接收被压制 |
| `FW002` | `while (寄存器标志);` 形式的轮询缺少超时 | `while (Read_MFRC522(CommandReg) & 0x10);` 在器件掉线后导致主循环永久阻塞 |
| `FW003` | `sprintf`、`strcpy`、`strcat`、`gets`、`vsprintf` | 远程下发的 `reason` 经 `sprintf` 写入 32 字节缓冲，存在栈溢出风险 |
| `FW004` | ISR 写入的全局变量未声明 `volatile` | `vehicle_arrive_flag` 未加 `volatile`，开启优化后主循环无法观察到置位 |
| `FW005` | 擦写 Flash 的函数缺少 `FLASH_Unlock()` | STM32F1 系列复位后 Flash 处于锁定状态，写入静默失败，表现为新增卡不生效 |

误报控制措施：

1. 分析前将注释与字符串字面量替换为等长空白，行号保持不变，注释中出现的函数名不参与匹配。
2. 引擎内置豁免：`USART_FLAG_TXE`、`USART_FLAG_TC` 由硬件在 1 个字符时间内清除，对其轮询属正常用法；
   自增、自减条件与 `do { } while ();` 尾部不参与判定。
3. 局部豁免须注明理由，作用域限定为同一行或下一行：

   ```c
   /* lint:disable FW005 由调用者统一解锁 */
   void Flash_Write_Slot(...)  { ... }
   ```

## 8. 协议样本与跨语言一致性

`tests/vectors.json` 保存 6 条帧的逐字节期望值，由 `tools/gen_protocol_vectors.py` 生成，不接受手工编辑。
该清单同时作为 C 侧与 Python 侧的接口契约。

| 样本 | seq | len | 帧（ASCII 形式） |
|---|---|---|---|
| `open_card` | 5 | 9 | `#10509OPEN:card*91` |
| `open_plate` | 6 | 10 | `#1060AOPEN:plate*68` |
| `close_remote` | 7 | 12 | `#1070CCLOSE:remote*30` |
| `plate_chinese` | 42 | 15 | `#12A0FPLATE:<冀>A00001*E2`（中文占 3 字节） |
| `spot_fault` | 255 | 12 | `#1FF0CSPOT:7:FAULT*C0` |
| `link_ok` | 0 | 7 | `#10007LINK:OK*49` |

帧格式：`#` + `VER`(1 hex) + `SEQ`(2 hex) + `LEN`(2 hex) + `PAYLOAD` + `*` + `CRC8`(2 hex)。
CRC 采用 CRC-8/ATM（多项式 `0x07`，初值 `0x00`），覆盖范围 `VER..PAYLOAD`。

除逐字节比对外，测试还覆盖：随机往返（2000 条 payload）、随机输入（3000 条字节序列，要求不抛异常且不判为合法帧）、
逐位翻转（每条样本的每一位翻转后须被 CRC 拒绝）。帧长算式由源码核对，须为 `6 + len + 3`（含起始符）。

## 9. 结构与文档一致性检查项

| 检查项 | 不满足时的后果 |
|---|---|
| Flash 各槽页对齐、不重叠、位于应用区之外 | 白名单区域与代码段重叠，烧写固件时擦除全部卡记录 |
| Keil `IROM` 与 `FLASH_APP_LIMIT_ADDR` 一致 | 链接阶段无告警，下载时覆盖数据区 |
| 白名单容量不超过 1 页 | 跨页写入时擦除相邻分区 |
| DMA 环形缓冲不小于 2 倍单页擦除窗口 | 擦除期间 CPU 停顿 20–40 ms，缓冲不足导致车位帧丢失 |
| 行缓冲小于环形缓冲 | 缓冲区容量被行长度限制 |
| 位图宽度不小于车位数 | 超出位宽的编号无法表示 |
| `Readme.md` 的引脚、波特率、地址、超时、容量、版本与源码一致 | 现场按文档接线或配置时失效 |
| `Readme.md` 与本文档的用例数量等于实际数量 | 对外公布的测试规模不可信 |
| ZigBee 未使用公开默认密钥 | 任意设备可加入网络 |
| CI 矩阵覆盖双平台、`run:` 步骤不含 shell 专有语法 | 某个平台上静默失效或无法执行 |

## 10. 缺陷记录

以下缺陷由上述检查发现，均已完成修复并纳入回归测试。

| 编号 | 发现方式 | 问题 | 影响 | 修复 |
|---|---|---|---|---|
| D-01 | `fw_lint` FW003 | `Serial.c` 中 `Serial_Printf` 使用 `vsprintf` 写入 `char String[100]` | 格式化结果超长时写穿栈缓冲 | 改为 `vsnprintf(String, sizeof(String), ...)` |
| D-02 | `fw_lint` FW003 | `pi_link.c` 中两处 ACK 使用 `sprintf` 写入 `char ack[8]` | 同上 | 改为 `snprintf(ack, sizeof(ack), ...)` |
| D-03 | 强制全量重建 | `Infrared.c` 将 `uint8_t` 赋值给枚举类型 `BitAction`（`#188-D`） | 类型语义不明确，更换编译器或开启高等级优化后行为可能变化 | 改用 `uint8_t` 接收 |
| D-04 | 强制全量重建 | `Infrared.c` 文件末尾缺少换行（`#1-D`） | 部分工具链在拼接文件时出错 | 补充换行 |
| D-05 | 强制全量重建 | `OLED.c` 文件末尾缺少换行（`#1-D`） | 同上 | 补充换行 |

D-03 至 D-05 仅在 `-r` 全量重建时出现；增量编译的输出为 0 Error / 0 Warning。编译结论应以全量重建结果为准。

## 11. 基线结果

2026-11-18 实测，可在任意机器复现。数值发生变化时应同步更新本节与 `Readme.md` 的「测试与结果」部分。

| 检查项 | 命令 | 结果 | 耗时 |
|---|---|---|---|
| 树莓派逻辑 | `python -m unittest discover -s raspberrypi/tests -t raspberrypi` | 123 项通过 | 0.5s |
| 固件静态/一致性 | `python -m unittest discover -s tests -t tests` | 74 项通过 | 0.5s |
| 固件静态检查 | `python tools/fw_lint.py` | 0 项发现（5 条规则） | 0.1s |
| 凭据扫描 | `python tools/check_secrets.py` | 通过 | 0.7s |
| 合计 | `python tools/run_tests.py` | 4/4 通过 | 约 2.2s |

固件编译验证（人工执行，不属于自动化测试）：

| 工具链 | 目标 | 结果 |
|---|---|---|
| Keil MDK 5（ARMCC V5.06u4） | `STM32_Gateway/Project.uvprojx` 全量重建 | 0 Error / 0 Warning，`Code=26280 RO=3968 RW=136 ZI=6336` |
| IAR 8051 | `ParkingNode.ewp` 三配置（CoordinatorEB / RouterEB / EndDeviceEB） | 0 error；7 条 warning 来自 Z-Stack 厂商库 |

## 12. 未覆盖范围

- **PC 端编译执行固件 C 代码**：`link_frame.c` 为纯函数，待 CI 引入宿主编译器后可与其对接；当前以常量、算式与样本三方对表的方式验证。
- **硬件在环测试**：舵机行程、RC522 读卡距离、HC-SR04 回波时序、OLED 实际显示效果需现场验证；测试覆盖逻辑与协议约定，不覆盖电气特性。
- **覆盖率统计**：未引入 `coverage.py` 等工具，以用例清单与失败信息为准。
- **长稳与压力测试**：ZigBee 空口丢包率、连续加卡时的帧丢失率需现场仪器测量。
- **微信小程序与网页端**：不在本仓库测试范围内。
- **树莓派实机**：跨平台正确性以 CI 在两个平台上的执行结果为准，未在本地树莓派上运行测试套件。

## 13. 维护规范

1. 用例数量变更时，同步更新 `Readme.md` 与本文档的总数；`TestCountTest` 会校验两者的一致性。
2. 新增静态规则时，同时提供触发用例与不触发用例，并在 `Readme.md` 的「测试与结果」规则表与本文第 7 节各增加一行。
3. 测试与检查脚本只允许引用标准库。
4. 提交前执行 `python tools/run_tests.py`；CI 执行同一组命令。
5. 豁免须注明理由并限制作用范围；若能通过修改代码消除，不使用豁免。

## 附录 A：新增用例的步骤

1. 固件侧用例置于 `tests/`，树莓派侧用例置于 `raspberrypi/tests/`，文件与类命名沿用现有风格。
2. 需要读取源码常量时，使用 `tests/common.py` 的 `defines()`、`define_int()`、`irom1()`、`iram1()`，不在用例中硬编码路径或数值。
3. 断言失败信息须包含定位所需上下文（文件名、样本名、实际值）。
4. 执行 `python tools/run_tests.py`；若用例数量变化，`TestCountTest` 会提示同步文档中的总数。
5. 用例对应的缺陷记入第 10 节，包含发现方式、影响与修复方式。
