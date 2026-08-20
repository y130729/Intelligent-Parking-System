# 智慧停车微信小程序

这是按 `小程序开发.docx` 实现的华为云智慧停车小程序端，当前采用“微信小程序直接调用华为云 IAM + IoTDA 应用侧 API”的课设方案。

## 运行方式

用微信开发者工具导入：

```text
D:\WechatProject
```

项目默认 `useMock: true`，会使用本地模拟数据，方便先演示页面。

## 华南-广州配置

配置文件在：

```text
utils/config.js
```

需要填写：

```js
huaweiCloud: {
```

填好华为云参数后，再把 `useMock` 改成 `false`：

```js
useMock: false,
huaweiCloud: {
  region: 'cn-south-1',
  iamEndpoint: 'https://iam.cn-south-1.myhuaweicloud.com',
  iotdaEndpoint: 'https://你的IoTDA应用侧HTTPS接入地址',
  projectId: '你的华南-广州项目ID',
  domainName: '你的华为云账号名',
  iamUserName: '你的IAM用户名',
  iamPassword: '你的IAM用户密码',
  deviceId: '你的设备ID',
  instanceId: ''
}
```

`iotdaEndpoint` 不建议猜，进入华为云 IoTDA 控制台，在实例的“总览 / 接入信息”里复制“应用侧 HTTPS 接入地址”。如果你的实例要求 `Instance-Id`，也把实例 ID 填到 `instanceId`。

## 微信后台合法域名

在微信公众平台小程序后台配置 `request合法域名`：

```text
https://iam.cn-south-1.myhuaweicloud.com
https://你的IoTDA应用侧HTTPS接入域名
```

开发阶段可以在微信开发者工具里临时勾选“不校验合法域名、TLS版本以及HTTPS证书”，真机和上线前必须配置合法域名。

## 设备影子格式

首页通过设备影子读取车位状态，调用接口：

```text
GET /v5/iot/{project_id}/devices/{device_id}/shadow
```

默认读取 `ParkingLot` 服务下的 `reported.properties.spots`：

```json
{
  "spots": [
    { "id": "A-01", "status": "occupied", "updated_at": 1717801234 },
    { "id": "A-02", "status": "empty", "updated_at": 1717801200 }
  ],
  "total_spots": 50,
  "remaining_spots": 32
}
```

如果你的产品模型字段名不同，改 `utils/config.js` 里的：

```js
shadow: {
  serviceId: 'ParkingLot',
  spotsProperty: 'spots',
  totalProperty: 'total_spots',
  remainingProperty: 'remaining_spots'
}
```

状态值支持：

```text
empty / idle / free / available / 0 / false  -> 空闲
occupied / busy / used / 1 / true            -> 占用
```

## 远程开闸命令

控制页点击“远程开闸”时调用：

```text
POST /v5/iot/{project_id}/devices/{device_id}/commands
```

默认请求体：

```json
{
  "service_id": "ParkingBarrier",
  "command_name": "open_barrier",
  "paras": {
    "value": "open"
  }
}
```

如果你的产品模型命令不同，改：

```js
command: {
  serviceId: 'ParkingBarrier',
  commandName: 'open_barrier',
  paras: {
    value: 'open'
  }
}
```

## 安全提醒

这种直连方案适合课设演示，但会把 IAM 用户名和密码放进小程序前端，正式项目不建议这样做。

课设建议：

- 新建一个专用 IAM 用户。
- 只授予 IoTDA 查询设备影子和下发命令所需权限。
- 不要使用主账号密码。
- 课设结束后立刻改密码或禁用这个 IAM 用户。
