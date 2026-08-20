from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED
import html


OUT = Path("智能停车系统课程设计报告.docx")


def esc(text):
    return html.escape(text, quote=False)


def p(text="", style=None, align=None, bold=False):
    ppr = []
    if style:
        ppr.append(f'<w:pStyle w:val="{style}"/>')
    if align:
        ppr.append(f'<w:jc w:val="{align}"/>')
    ppr_xml = f"<w:pPr>{''.join(ppr)}</w:pPr>" if ppr else ""
    rpr = "<w:rPr><w:b/></w:rPr>" if bold else ""
    return f"<w:p>{ppr_xml}<w:r>{rpr}<w:t>{esc(text)}</w:t></w:r></w:p>"


def code_block(text):
    lines = text.splitlines()
    xml = []
    for line in lines:
        xml.append(
            '<w:p><w:pPr><w:pStyle w:val="Code"/></w:pPr>'
            f'<w:r><w:t xml:space="preserve">{esc(line)}</w:t></w:r></w:p>'
        )
    return "".join(xml)


def heading(text, level):
    return p(text, f"Heading{level}")


def page_break():
    return '<w:p><w:r><w:br w:type="page"/></w:r></w:p>'


def bullet(text):
    return (
        '<w:p><w:pPr><w:pStyle w:val="ListParagraph"/></w:pPr>'
        f'<w:r><w:t>{esc("• " + text)}</w:t></w:r></w:p>'
    )


def table(rows):
    cells = []
    for row in rows:
        cells.append("<w:tr>")
        for cell in row:
            cells.append(
                '<w:tc><w:tcPr><w:tcW w:w="3000" w:type="dxa"/></w:tcPr>'
                f'{p(cell)}</w:tc>'
            )
        cells.append("</w:tr>")
    return (
        '<w:tbl><w:tblPr><w:tblStyle w:val="TableGrid"/>'
        '<w:tblW w:w="0" w:type="auto"/></w:tblPr>'
        + "".join(cells)
        + "</w:tbl>"
    )


content = []
content.append(p("课程设计报告", "Title", "center"))
content.append(p("题目：基于微信小程序与华为云 IoTDA 的智慧停车系统", align="center", bold=True))
content.append(p("项目内容：微信小程序端、Web 本地控制端、华为云配置与接口联调", align="center"))
content.append(p(" "))
content.append(p("姓名：__________", align="center"))
content.append(p("班级：__________", align="center"))
content.append(p("学号：__________", align="center"))
content.append(p("指导教师：__________", align="center"))
content.append(p("完成日期：2026 年 6 月", align="center"))
content.append(page_break())

content.append(heading("目录", 1))
toc = [
    "1. 简介",
    "2. 功能说明",
    "3. 系统方案设计",
    "   3.1 微信小程序端设计",
    "   3.2 Web 端设计",
    "   3.3 华为云配置方案",
    "   3.4 数据交互与安全设计",
    "4. 功能实现",
    "   4.1 系统流程图",
    "   4.2 系统功能测试",
    "5. 总结",
    "6. 改进与不足",
]
for item in toc:
    content.append(p(item))
content.append(page_break())

content.append(heading("1. 简介", 1))
content.append(p("本课程设计完成了一个面向校园停车场景的智慧停车系统，系统围绕“车位状态可视化、车辆进出记录、用户车辆绑定、远程道闸控制、云端设备数据同步”等需求展开。项目采用微信小程序作为用户侧入口，同时提供 Web 本地控制端作为管理和调试入口，并通过华为云 IAM 与 IoTDA 设备接入服务实现设备影子读取和命令下发。"))
content.append(p("在实际实现中，小程序端负责展示停车场基础信息、剩余车位、车位占用状态、停车记录和个人车辆信息；Web 端负责展示全场车位地图、操作日志、停车记录以及云端连接配置；华为云部分负责提供设备身份、设备影子数据和命令通道。系统既支持本地模拟数据，便于演示与调试，也支持切换到华为云在线模式读取真实设备上报数据。"))
content.append(p("本报告重点说明本人在该文件项目中完成的微信小程序端、Web 端以及华为云配置相关工作。"))

content.append(heading("2. 功能说明", 1))
content.append(p("系统主要功能如下："))
for item in [
    "车位状态展示：在首页或 Web 控制台显示停车场名称、地址、总车位数、剩余车位数、占用率，并以网格形式展示每个车位的空闲或占用状态。",
    "车位筛选：支持按全部、空闲、占用三种状态筛选车位，便于用户快速查看可用车位。",
    "停车记录管理：小程序记录页显示入场、离场、停车时长、费用状态等信息；Web 端也提供停车记录列表用于管理端查看。",
    "车辆绑定：小程序“我的”页面支持填写车牌号、卡号和微信用户信息，并将车辆与当前用户进行绑定。",
    "远程开闸：控制页面和 Web 端均提供远程开闸按钮，在线模式下通过华为云 IoTDA 命令接口向设备下发 open_barrier 指令。",
    "操作日志：记录远程开闸等操作的执行结果、操作人员和时间，方便后续追踪。",
    "华为云联调：通过 IAM 获取 Token，通过 IoTDA 读取设备影子，通过命令接口下发控制命令。",
    "本地模拟模式：当配置不完整或需要课堂演示时，可使用 mock 数据完成车位状态、车辆记录和开闸响应的模拟。",
]:
    content.append(bullet(item))

content.append(heading("3. 系统方案设计", 1))
content.append(heading("3.1 微信小程序端设计", 2))
content.append(p("微信小程序端采用原生小程序页面结构，主要页面包括车位首页、停车记录、我的车辆信息以及日志页面。页面路径在 app.json 中注册，底部 tabBar 提供“车位、记录、我的”三个主要入口。"))
content.append(p("首页 pages/index 通过 utils/cloudApi.js 获取停车场概览数据，将云端设备影子或本地模拟数据统一转换为 overview 对象。页面根据车位状态计算空闲数、占用数和占用率，并通过 filterTabs 实现车位筛选。为了适配设备上报格式不统一的问题，cloudApi 中对 spots 数组、键值映射、扁平字段以及单个车位上报等多种格式进行了兼容处理。"))
content.append(p("记录页 pages/records 用于展示停车记录，并根据绑定车辆的车牌号或卡号过滤相关记录。我的页面 pages/mine 负责保存用户资料、车辆车牌、卡号和通知开关等信息。当设备影子中出现匹配的 card_id 或 plate_no 时，小程序可根据车辆状态判断入场或离场，并写入对应停车记录。"))
content.append(p("控制功能由 pages/control 实现，用户点击远程开闸后调用 cloudApi.openBarrier。若系统处于模拟模式，则返回本地模拟结果；若处于在线模式，则通过华为云 IoTDA 命令接口下发道闸控制命令，并将结果写入操作日志。"))

content.append(heading("3.2 Web 端设计", 2))
content.append(p("Web 端位于 web 目录，包含 index.html、styles.css、app.js 和 server.js。index.html 提供本地控制台页面，app.js 负责页面交互、状态渲染、配置保存、车位数据解析和日志记录，server.js 作为本地 Node 代理服务。"))
content.append(p("Web 控制台的主要区域包括顶部状态栏、停车场概览卡片、车位地图、远程控制面板、操作日志和停车记录。管理人员可以在浏览器中查看车位实时状态，也可以打开配置窗口填写 IAM Endpoint、IoTDA HTTPS 地址、Project ID、账号名、IAM 用户名、设备 ID 等参数，并通过“测试连接”验证是否能读取设备影子。"))
content.append(p("由于浏览器直接访问华为云接口可能受到跨域限制，Web 端通过本地 Node 代理转发请求。server.js 提供 /api/shadow 和 /api/commands/open-barrier 两个接口，分别用于读取设备影子和下发开闸命令。代理服务还缓存 IAM Token，减少重复认证请求。"))

content.append(heading("3.3 华为云配置方案", 2))
content.append(p("华为云部分采用 IAM + IoTDA 的方式完成云端访问。系统配置集中在 utils/config.js 与 Web 端配置面板中，主要配置项包括区域、IAM Endpoint、IoTDA 应用侧 HTTPS 接入地址、Project ID、账号名、IAM 用户名、IAM 密码、Device ID、Instance ID、设备影子服务 ID、车位字段名和命令字段名。"))
content.append(p("本项目使用 cn-south-1 区域，对应 IAM 地址为 https://iam.cn-south-1.myhuaweicloud.com。IoTDA 侧使用应用侧 HTTPS 接入地址访问设备影子和命令接口。报告中不记录具体 IAM 密码和完整项目凭据，实际部署时应由配置文件或配置界面填写，并做好权限隔离。"))
content.append(p("设备影子设计中，系统默认关注 Spot_state 服务，读取 Parking_id、Parking_state、card_id、plate_no 等属性。Parking_id 表示发生变化的车位编号，Parking_state 表示车位状态，card_id 和 plate_no 用于识别车辆或门禁卡。若设备一次性上报 spots 数组，系统也能够解析每个车位的状态。"))
content.append(p("命令下发设计中，道闸控制使用 ParkingBarrier 服务，命令名为 open_barrier，参数中 value 为 open。小程序或 Web 端触发开闸后，系统先获取 IAM Token，再向 IoTDA 命令接口发送 POST 请求，最终由云端转发给设备执行。"))

content.append(heading("3.4 数据交互与安全设计", 2))
content.append(p("小程序端的核心数据访问封装在 utils/cloudApi.js 中，Web 端的云访问封装在 web/app.js 与 web/server.js 中。系统通过统一的 normalizeStatus、normalizeSpotId、buildOverviewFromShadow 等方法，将云端原始数据转换为页面可直接使用的数据结构。"))
content.append(p("在安全设计上，当前方案适合课程设计和课堂演示，但小程序前端直接保存 IAM 用户名和密码存在风险。正式项目中应将 IAM 凭据放在后端服务器或云函数中，由后端生成临时访问凭证或提供受限业务接口；同时应创建最小权限 IAM 用户，只授予读取设备影子和下发指定命令所需的权限。"))

content.append(heading("4. 功能实现", 1))
content.append(heading("4.1 系统流程图", 2))
content.append(p("系统整体业务流程如下："))
content.append(code_block("""
用户打开小程序或 Web 控制台
        ↓
读取本地配置并判断运行模式
        ↓
┌─────────────── 是 ───────────────┐
│          使用本地模拟数据          │
└─────────────── 否 ───────────────┘
        ↓
向华为云 IAM 申请 X-Subject-Token
        ↓
携带 X-Auth-Token 请求 IoTDA 设备影子
        ↓
解析设备影子中的车位、卡号、车牌和时间信息
        ↓
刷新车位地图、剩余车位、占用率、停车记录
        ↓
用户点击远程开闸
        ↓
调用 IoTDA 命令接口下发 open_barrier
        ↓
记录执行结果并更新操作日志
""".strip()))
content.append(p("小程序端流程重点是“页面加载 - 调用 cloudApi - 解析 overview - 渲染页面 - 用户交互 - 写入本地记录”；Web 端流程重点是“读取配置 - 调用本地代理 - 代理访问华为云 - 返回结果 - 浏览器渲染控制台”。"))

content.append(heading("4.2 系统功能测试", 2))
content.append(p("为验证系统功能，设计如下测试项目："))
content.append(table([
    ["测试项目", "测试方法", "预期结果"],
    ["小程序首页展示", "打开小程序车位首页，执行刷新操作", "显示停车场名称、总车位、剩余车位、占用率和车位网格"],
    ["车位筛选", "点击全部、空闲、占用筛选按钮", "车位列表按状态正确过滤，统计数据保持一致"],
    ["停车记录", "进入记录页面，查看已绑定车辆记录", "展示入场时间、离场时间、停车时长和费用状态"],
    ["车辆绑定", "在我的页面填写车牌号和卡号并保存", "提示绑定成功，本地保存车辆信息"],
    ["远程开闸", "在控制页或 Web 端点击远程开闸", "模拟模式返回成功；在线模式向 IoTDA 下发 open_barrier 命令"],
    ["Web 控制台", "运行 node web/server.js 后访问本地地址", "展示车位地图、指标、日志、记录和配置窗口"],
    ["华为云连接", "填写 IAM 与 IoTDA 配置后测试连接", "成功获取 Token 并读取设备影子，页面提示同步成功"],
]))
content.append(p("测试结果表明，系统在本地模拟模式下可以完成完整页面演示；在华为云配置完整且设备在线时，可以通过 IoTDA 读取设备影子并下发控制命令。系统对设备影子字段做了多格式兼容处理，因此在设备上报单个车位变化或批量车位列表时均能进行解析。"))

content.append(heading("5. 总结", 1))
content.append(p("本课程设计围绕智慧停车场景，完成了微信小程序端、Web 本地控制端和华为云 IoTDA 联调方案。小程序端实现了用户侧常用功能，包括车位查看、记录查询、车辆绑定和远程开闸；Web 端实现了管理侧控制台，便于调试云端连接、查看全场车位和执行开闸操作；华为云部分通过 IAM Token、设备影子和设备命令接口实现了前端系统与物联网设备之间的数据交互。"))
content.append(p("通过本项目，我进一步理解了微信小程序页面结构、状态管理、云 API 封装、设备影子数据解析和 IoT 命令下发流程，也掌握了在课程设计中将本地模拟、前端展示和云端联调结合起来的方法。整个系统能够较完整地体现智慧停车系统从数据采集、状态展示到远程控制的基本闭环。"))

content.append(heading("6. 改进与不足", 1))
content.append(p("当前系统仍存在以下不足和改进方向："))
for item in [
    "安全性方面，IAM 用户名和密码不应长期保存在前端配置中，后续应改为后端服务、云函数或临时凭证方式访问华为云。",
    "实时性方面，目前在线模式主要通过定时轮询设备影子实现刷新，后续可引入 MQTT、WebSocket 或云端消息推送，降低延迟和请求次数。",
    "数据持久化方面，当前停车记录、车辆绑定和操作日志主要保存在本地或模拟数据中，后续可接入数据库，实现多用户、多设备统一管理。",
    "权限管理方面，远程开闸功能应增加管理员身份认证、操作审批或二次确认，避免误操作。",
    "异常处理方面，可以继续完善设备离线、Token 过期、命令超时、网络失败等场景下的提示和重试机制。",
    "界面体验方面，可进一步优化车位地图布局，增加楼层、区域、车位导航和收费明细展示，使系统更接近实际停车场应用。",
]:
    content.append(bullet(item))


document_xml = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body>
    {''.join(content)}
    <w:sectPr>
      <w:pgSz w:w="11906" w:h="16838"/>
      <w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440" w:header="720" w:footer="720" w:gutter="0"/>
    </w:sectPr>
  </w:body>
</w:document>
'''

styles_xml = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:style w:type="paragraph" w:default="1" w:styleId="Normal">
    <w:name w:val="Normal"/>
    <w:rPr><w:rFonts w:ascii="Times New Roman" w:eastAsia="宋体"/><w:sz w:val="24"/></w:rPr>
    <w:pPr><w:spacing w:line="360" w:lineRule="auto"/></w:pPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Title">
    <w:name w:val="Title"/>
    <w:rPr><w:rFonts w:ascii="Times New Roman" w:eastAsia="黑体"/><w:b/><w:sz w:val="44"/></w:rPr>
    <w:pPr><w:spacing w:after="360"/></w:pPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading1">
    <w:name w:val="heading 1"/>
    <w:basedOn w:val="Normal"/>
    <w:next w:val="Normal"/>
    <w:qFormat/>
    <w:rPr><w:rFonts w:ascii="Times New Roman" w:eastAsia="黑体"/><w:b/><w:sz w:val="32"/></w:rPr>
    <w:pPr><w:spacing w:before="240" w:after="120"/></w:pPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Heading2">
    <w:name w:val="heading 2"/>
    <w:basedOn w:val="Normal"/>
    <w:next w:val="Normal"/>
    <w:qFormat/>
    <w:rPr><w:rFonts w:ascii="Times New Roman" w:eastAsia="黑体"/><w:b/><w:sz w:val="28"/></w:rPr>
    <w:pPr><w:spacing w:before="180" w:after="80"/></w:pPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="ListParagraph">
    <w:name w:val="List Paragraph"/>
    <w:basedOn w:val="Normal"/>
    <w:pPr><w:ind w:left="360"/><w:spacing w:line="360" w:lineRule="auto"/></w:pPr>
  </w:style>
  <w:style w:type="paragraph" w:styleId="Code">
    <w:name w:val="Code"/>
    <w:basedOn w:val="Normal"/>
    <w:rPr><w:rFonts w:ascii="Consolas" w:eastAsia="等线"/><w:sz w:val="20"/></w:rPr>
    <w:pPr><w:spacing w:line="260" w:lineRule="auto"/></w:pPr>
  </w:style>
</w:styles>
'''

content_types = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>
'''

rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>
'''

doc_rels = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>
'''

core = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/" xmlns:dcmitype="http://purl.org/dc/dcmitype/" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <dc:title>智能停车系统课程设计报告</dc:title>
  <dc:creator>Codex</dc:creator>
</cp:coreProperties>
'''

app = '''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties">
  <Application>Codex</Application>
</Properties>
'''

with ZipFile(OUT, "w", ZIP_DEFLATED) as zf:
    zf.writestr("[Content_Types].xml", content_types)
    zf.writestr("_rels/.rels", rels)
    zf.writestr("word/document.xml", document_xml)
    zf.writestr("word/_rels/document.xml.rels", doc_rels)
    zf.writestr("word/styles.xml", styles_xml)
    zf.writestr("docProps/core.xml", core)
    zf.writestr("docProps/app.xml", app)

print(OUT.resolve())
