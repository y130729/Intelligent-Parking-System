const http = require('http')
const fs = require('fs')
const path = require('path')

if (typeof fetch !== 'function') {
  console.error('需要 Node.js 18 或更高版本。请先执行 node -v 检查版本。')
  process.exit(1)
}

const miniProgramConfig = require('../utils/config')

const HOST = '127.0.0.1'
const PORT = Number(process.env.PORT || 3001)
const ROOT = __dirname

const MIME_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.svg': 'image/svg+xml; charset=utf-8',
  '.ico': 'image/x-icon'
}

const tokenCache = new Map()

const defaultCloud = miniProgramConfig.huaweiCloud || {}

const server = http.createServer(async (req, res) => {
  setCommonHeaders(res)

  if (req.method === 'OPTIONS') {
    res.writeHead(204)
    res.end()
    return
  }

  try {
    if (req.url.startsWith('/api/')) {
      await handleApi(req, res)
      return
    }

    await serveStatic(req, res)
  } catch (error) {
    writeJson(res, error.statusCode || 500, {
      message: error.message || '本地代理服务异常'
    })
  }
})

server.listen(PORT, HOST, () => {
  console.log(`智慧停车本地代理已启动：http://${HOST}:${PORT}`)
  console.log('保持这个窗口不要关闭，然后用浏览器打开上面的地址。')
})

function setCommonHeaders(res) {
  res.setHeader('Access-Control-Allow-Origin', '*')
  res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS')
  res.setHeader('Access-Control-Allow-Headers', 'content-type')
}

async function handleApi(req, res) {
  if (req.method === 'GET' && req.url === '/api/health') {
    writeJson(res, 200, {
      ok: true,
      hasDefaultConfig: hasRequiredCloudFields(defaultCloud)
    })
    return
  }

  if (req.method !== 'POST') {
    throw httpError(405, '接口只支持 POST')
  }

  const body = await readJsonBody(req)
  const cloud = mergeCloudConfig(body.config)
  validateCloudConfig(cloud)

  if (req.url === '/api/shadow') {
    const data = await requestIoTDA(
      cloud,
      `/v5/iot/${cloud.projectId}/devices/${encodeURIComponent(cloud.deviceId)}/shadow`,
      { method: 'GET' }
    )
    writeJson(res, 200, data)
    return
  }

  if (req.url === '/api/commands/open-barrier') {
    const command = cloud.command || {}
    const data = await requestIoTDA(
      cloud,
      `/v5/iot/${cloud.projectId}/devices/${encodeURIComponent(cloud.deviceId)}/commands`,
      {
        method: 'POST',
        body: {
          service_id: command.serviceId,
          command_name: command.commandName,
          paras: command.paras || {}
        }
      }
    )
    writeJson(res, 200, data)
    return
  }

  throw httpError(404, '接口不存在')
}

async function serveStatic(req, res) {
  const pathname = decodeURIComponent(new URL(req.url, `http://${HOST}:${PORT}`).pathname)
  const requested = pathname === '/' ? '/index.html' : pathname
  const filePath = path.resolve(ROOT, `.${requested}`)

  if (!filePath.startsWith(ROOT)) {
    throw httpError(403, '禁止访问该路径')
  }

  const stat = await fs.promises.stat(filePath).catch(() => null)
  if (!stat || !stat.isFile()) {
    throw httpError(404, '文件不存在')
  }

  const ext = path.extname(filePath).toLowerCase()
  res.writeHead(200, {
    'content-type': MIME_TYPES[ext] || 'application/octet-stream'
  })
  fs.createReadStream(filePath).pipe(res)
}

function mergeCloudConfig(clientConfig = {}) {
  const cloud = {
    ...defaultCloud,
    shadow: { ...(defaultCloud.shadow || {}) },
    command: {
      ...(defaultCloud.command || {}),
      paras: { ...((defaultCloud.command && defaultCloud.command.paras) || {}) }
    }
  }

  copyNonEmpty(clientConfig, cloud, [
    'iamEndpoint',
    'iotdaEndpoint',
    'projectId',
    'domainName',
    'iamUserName',
    'iamPassword',
    'deviceId',
    'instanceId',
    'requestTimeout'
  ])

  if (clientConfig.shadow) {
    copyNonEmpty(clientConfig.shadow, cloud.shadow, [
      'serviceId',
      'spotIdProperty',
      'spotStateProperty',
      'cardIdProperty',
      'plateNoProperty',
      'spotsProperty',
      'totalProperty',
      'remainingProperty',
      'updatedAtProperty'
    ])
  }

  if (clientConfig.command) {
    copyNonEmpty(clientConfig.command, cloud.command, [
      'serviceId',
      'commandName'
    ])
    if (clientConfig.command.paras) {
      cloud.command.paras = {
        ...cloud.command.paras,
        ...clientConfig.command.paras
      }
    }
  }

  return cloud
}

function copyNonEmpty(source, target, keys) {
  keys.forEach(key => {
    const value = source[key]
    if (value !== undefined && value !== null && value !== '') {
      target[key] = value
    }
  })
}

function validateCloudConfig(cloud) {
  const required = ['iamEndpoint', 'iotdaEndpoint', 'projectId', 'domainName', 'iamUserName', 'iamPassword', 'deviceId']
  const missing = required.filter(key => !cloud[key])
  if (missing.length) {
    throw httpError(400, `缺少华为云配置：${missing.join(', ')}`)
  }

  if (!cloud.iamEndpoint.startsWith('https://') || !cloud.iotdaEndpoint.startsWith('https://')) {
    throw httpError(400, 'Endpoint 必须以 https:// 开头')
  }

  if (!cloud.command || !cloud.command.serviceId || !cloud.command.commandName) {
    throw httpError(400, '缺少命令配置：serviceId 或 commandName')
  }
}

function hasRequiredCloudFields(cloud) {
  return ['iamEndpoint', 'iotdaEndpoint', 'projectId', 'domainName', 'iamUserName', 'iamPassword', 'deviceId']
    .every(key => !!cloud[key])
}

async function getIamToken(cloud, forceRefresh = false) {
  const cacheKey = `${cloud.projectId}:${cloud.domainName}:${cloud.iamUserName}`
  const cached = tokenCache.get(cacheKey)
  if (!forceRefresh && cached && new Date(cached.expiresAt).getTime() - Date.now() > 30 * 60 * 1000) {
    return cached.token
  }

  const response = await fetch(`${trimSlash(cloud.iamEndpoint)}/v3/auth/tokens?nocatalog=true`, {
    method: 'POST',
    headers: { 'content-type': 'application/json;charset=utf8' },
    body: JSON.stringify({
      auth: {
        identity: {
          methods: ['password'],
          password: {
            user: {
              domain: { name: cloud.domainName },
              name: cloud.iamUserName,
              password: cloud.iamPassword
            }
          }
        },
        scope: {
          project: { id: cloud.projectId }
        }
      }
    }),
    signal: AbortSignal.timeout(Number(cloud.requestTimeout || 25000))
  })

  const data = await readJsonResponse(response)
  if (!response.ok) {
    throw cloudError('IAM Token', response.status, data)
  }

  const token = response.headers.get('x-subject-token')
  if (!token) {
    throw httpError(502, 'IAM Token 获取失败：响应头缺少 X-Subject-Token')
  }

  const expiresAt = data.token && data.token.expires_at
    ? data.token.expires_at
    : new Date(Date.now() + 23 * 60 * 60 * 1000).toISOString()
  tokenCache.set(cacheKey, { token, expiresAt })
  return token
}

async function requestIoTDA(cloud, apiPath, options = {}, retried = false) {
  const token = await getIamToken(cloud, retried)
  const headers = {
    'content-type': 'application/json',
    'X-Auth-Token': token
  }

  if (cloud.instanceId) {
    headers['Instance-Id'] = cloud.instanceId
  }

  const response = await fetch(`${trimSlash(cloud.iotdaEndpoint)}${apiPath}`, {
    method: options.method || 'GET',
    headers,
    body: options.body ? JSON.stringify(options.body) : undefined,
    signal: AbortSignal.timeout(Number(cloud.requestTimeout || 25000))
  })
  const data = await readJsonResponse(response)

  if (response.status === 401 && !retried) {
    return requestIoTDA(cloud, apiPath, options, true)
  }

  if (!response.ok) {
    throw cloudError('IoTDA 设备影子/命令接口', response.status, data)
  }

  if (data && data.error_code) {
    throw cloudError('IoTDA 设备影子/命令接口', response.status, data)
  }

  return data
}

async function readJsonBody(req) {
  const chunks = []
  let size = 0

  for await (const chunk of req) {
    size += chunk.length
    if (size > 1024 * 1024) {
      throw httpError(413, '请求内容过大')
    }
    chunks.push(chunk)
  }

  if (!chunks.length) return {}
  try {
    return JSON.parse(Buffer.concat(chunks).toString('utf8'))
  } catch (error) {
    throw httpError(400, '请求 JSON 格式不正确')
  }
}

async function readJsonResponse(response) {
  const text = await response.text()
  if (!text) return {}
  try {
    return JSON.parse(text)
  } catch (error) {
    return { message: text }
  }
}

function cloudError(label, status, data) {
  const code = data && data.error_code ? `${data.error_code}: ` : ''
  const message = data && (data.error_msg || data.message) ? (data.error_msg || data.message) : '请检查配置、权限和设备状态'
  return httpError(502, `${label}失败，HTTP ${status}，${code}${message}`)
}

function httpError(statusCode, message) {
  const error = new Error(message)
  error.statusCode = statusCode
  return error
}

function writeJson(res, statusCode, data) {
  res.writeHead(statusCode, {
    'content-type': 'application/json; charset=utf-8'
  })
  res.end(JSON.stringify(data))
}

function trimSlash(value) {
  return String(value || '').replace(/\/$/, '')
}
