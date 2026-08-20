const config = require('./config')
const mockData = require('./mockData')

const normalizeBaseUrl = url => (url || '').replace(/\/$/, '')

const nowSeconds = () => Math.floor(Date.now() / 1000)

const CLOUD_SPOT_CACHE_KEY = 'cloudSpotStateCache'

const requiredCloudFields = () => {
  const cloud = config.huaweiCloud
  return [
    cloud.iamEndpoint,
    cloud.iotdaEndpoint,
    cloud.projectId,
    cloud.domainName,
    cloud.iamUserName,
    cloud.iamPassword,
    cloud.deviceId
  ]
}

const hasHuaweiCloudConfig = () => requiredCloudFields().every(value => !!value)

const getConfigWarnings = () => {
  const cloud = config.huaweiCloud
  const warnings = []

  if (cloud.projectId && cloud.projectId.length < 30) {
    warnings.push('projectId 可能填成了 IoTDA 产品ID，请填写“我的凭证”里的华南-广州项目ID')
  }

  if (cloud.iotdaEndpoint && cloud.iotdaEndpoint.indexOf('iotda-device') >= 0) {
    warnings.push('iotdaEndpoint 不能填设备接入地址，请填写应用接入 HTTPS(443) 地址')
  }

  if (cloud.iotdaEndpoint && cloud.iotdaEndpoint.indexOf('https://') !== 0) {
    warnings.push('iotdaEndpoint 在代码里必须以 https:// 开头')
  }

  if (cloud.iamEndpoint && cloud.iamEndpoint.indexOf('https://') !== 0) {
    warnings.push('iamEndpoint 在代码里必须以 https:// 开头')
  }

  return warnings
}

const isUsingMock = () => {
  if (config.useMock) {
    return true
  }

  return config.fallbackToMockWhenCloudIncomplete && !hasHuaweiCloudConfig()
}

const getConnectionText = () => {
  if (isUsingMock()) {
    return '本地模拟'
  }

  return '华为云在线'
}

const getHeaderValue = (headers, name) => {
  if (!headers) {
    return ''
  }

  const target = name.toLowerCase()
  const key = Object.keys(headers).find(item => item.toLowerCase() === target)
  return key ? headers[key] : ''
}

const requestRaw = options => new Promise((resolve, reject) => {
  wx.request({
    url: options.url,
    method: options.method || 'GET',
    data: options.data || {},
    timeout: config.huaweiCloud.requestTimeout,
    header: options.header || {},
    success: res => {
      if (res.statusCode >= 200 && res.statusCode < 300) {
        resolve(res)
        return
      }

      const cloudMessage = res.data && (res.data.error_msg || res.data.message)
        ? (res.data.error_msg || res.data.message)
        : '请检查配置、权限和设备状态'
      const errorCode = res.data && res.data.error_code ? `${res.data.error_code}: ` : ''
      const message = `${options.label || '云平台请求'}失败，HTTP ${res.statusCode}，${errorCode}${cloudMessage}`
      reject(new Error(message))
    },
    fail: error => {
      reject(new Error(`${options.label || '云平台请求'}失败：${error.errMsg || error.message || '网络请求失败'}`))
    }
  })
})

const getTokenCacheKey = () => {
  const cloud = config.huaweiCloud
  return `${cloud.tokenCacheKey}_${cloud.projectId}_${cloud.iamUserName}`
}

const getCachedToken = () => {
  const cached = wx.getStorageSync(getTokenCacheKey())
  if (!cached || !cached.token || !cached.expiresAt) {
    return ''
  }

  const expiresAt = new Date(cached.expiresAt).getTime()
  if (expiresAt - Date.now() <= config.huaweiCloud.tokenExpireBuffer) {
    return ''
  }

  return cached.token
}

const saveToken = (token, expiresAt) => {
  wx.setStorageSync(getTokenCacheKey(), {
    token,
    expiresAt
  })
}

const getToken = forceRefresh => {
  if (isUsingMock()) {
    return Promise.resolve('mock-token')
  }

  if (!forceRefresh) {
    const cached = getCachedToken()
    if (cached) {
      return Promise.resolve(cached)
    }
  }

  const cloud = config.huaweiCloud
  const url = `${normalizeBaseUrl(cloud.iamEndpoint)}/v3/auth/tokens?nocatalog=true`
  const data = {
    auth: {
      identity: {
        methods: ['password'],
        password: {
          user: {
            domain: {
              name: cloud.domainName
            },
            name: cloud.iamUserName,
            password: cloud.iamPassword
          }
        }
      },
      scope: {
        project: {
          id: cloud.projectId
        }
      }
    }
  }

  return requestRaw({
    url,
    label: 'IAM Token',
    method: 'POST',
    data,
    header: {
      'content-type': 'application/json;charset=utf8'
    }
  }).then(res => {
    const token = getHeaderValue(res.header, 'X-Subject-Token')
    if (!token) {
      throw new Error('IAM Token 获取失败')
    }

    const expiresAt = res.data && res.data.token && res.data.token.expires_at
      ? res.data.token.expires_at
      : new Date(Date.now() + 23 * 60 * 60 * 1000).toISOString()
    saveToken(token, expiresAt)
    return token
  })
}

const requestIoTDA = (path, options) => getToken().then(token => {
  const cloud = config.huaweiCloud
  const header = Object.assign({
    'content-type': 'application/json',
    'X-Auth-Token': token
  }, options && options.header ? options.header : {})

  if (cloud.instanceId) {
    header['Instance-Id'] = cloud.instanceId
  }

  return requestRaw({
    url: `${normalizeBaseUrl(cloud.iotdaEndpoint)}${path}`,
    label: 'IoTDA 设备影子/命令接口',
    method: options && options.method ? options.method : 'GET',
    data: options && options.data ? options.data : {},
    header
  }).then(res => res.data)
}).catch(error => {
  if (/token/i.test(error.message || '')) {
    return getToken(true).then(token => {
      const cloud = config.huaweiCloud
      const header = {
        'content-type': 'application/json',
        'X-Auth-Token': token
      }
      if (cloud.instanceId) {
        header['Instance-Id'] = cloud.instanceId
      }

      return requestRaw({
        url: `${normalizeBaseUrl(cloud.iotdaEndpoint)}${path}`,
        label: 'IoTDA 设备影子/命令接口',
        method: options && options.method ? options.method : 'GET',
        data: options && options.data ? options.data : {},
        header
      }).then(res => res.data)
    })
  }

  throw error
})

const parseJsonMaybe = value => {
  if (typeof value !== 'string') {
    return value
  }

  try {
    return JSON.parse(value)
  } catch (error) {
    return value
  }
}

const normalizeStatus = value => {
  const status = parseJsonMaybe(value)
  const raw = typeof status === 'object' && status
    ? (status.status || status.value || status.occupied)
    : status
  const key = String(raw).toLowerCase()
  return config.huaweiCloud.shadow.statusMap[key] || 'empty'
}

const normalizeSpotId = (id, index) => {
  if (id) {
    return String(id).replace('_', '-').toUpperCase()
  }

  const zone = String.fromCharCode(65 + Math.floor(index / 10))
  const number = (index % 10) + 1
  return `${zone}-${number < 10 ? `0${number}` : number}`
}

const getPropertyValue = (properties, names) => {
  const source = properties || {}
  const keys = Array.isArray(names) ? names : [names]
  const matchedKey = keys.find(key => Object.prototype.hasOwnProperty.call(source, key))
  return matchedKey ? source[matchedKey] : undefined
}

const parseTimeSeconds = value => {
  if (!value) {
    return 0
  }

  if (typeof value === 'number') {
    return value > 1000000000000 ? Math.floor(value / 1000) : value
  }

  const parsed = Date.parse(value)
  return Number.isNaN(parsed) ? 0 : Math.floor(parsed / 1000)
}

const buildDefaultCloudSpots = () => {
  const total = config.parking.totalSpots || 50
  const spots = []
  for (let index = 0; index < total; index += 1) {
    spots.push({
      id: normalizeSpotId('', index),
      status: 'empty',
      updated_at: nowSeconds()
    })
  }
  return spots
}

const getCloudSpotCache = () => {
  const cached = wx.getStorageSync(CLOUD_SPOT_CACHE_KEY)
  if (cached && Array.isArray(cached.spots) && cached.spots.length) {
    return cached.spots
  }

  return buildDefaultCloudSpots()
}

const saveCloudSpotCache = spots => {
  wx.setStorageSync(CLOUD_SPOT_CACHE_KEY, {
    spots,
    updated_at: nowSeconds()
  })
}

const createSpotsFromSingleProperties = properties => {
  const shadowConfig = config.huaweiCloud.shadow
  const spotId = getPropertyValue(properties, [
    shadowConfig.spotIdProperty,
    'Parking_id',
    'parking_id',
    'spot_id'
  ])
  const spotState = getPropertyValue(properties, [
    shadowConfig.spotStateProperty,
    'Parking_state',
    'parking_state',
    'spot_state'
  ])

  if (!spotId || spotState === undefined || spotState === null || spotState === '') {
    return []
  }

  const normalizedId = normalizeSpotId(spotId)
  const nextStatus = normalizeStatus(spotState)
  let matched = false
  const spots = getCloudSpotCache().map(spot => {
    if (normalizeSpotId(spot.id) !== normalizedId) {
      return spot
    }

    matched = true
    return Object.assign({}, spot, {
      id: normalizedId,
      status: nextStatus,
      updated_at: nowSeconds()
    })
  })

  if (!matched) {
    spots.push({
      id: normalizedId,
      status: nextStatus,
      updated_at: nowSeconds()
    })
  }

  saveCloudSpotCache(spots)
  return spots
}

const getMatchedShadow = shadowBody => {
  const shadowList = shadowBody && shadowBody.shadow ? shadowBody.shadow : []
  if (!shadowList.length) {
    return null
  }

  const serviceId = config.huaweiCloud.shadow.serviceId
  return shadowList.find(item => item.service_id === serviceId) || shadowList[0]
}

const getShadowProperties = shadowBody => {
  const matched = getMatchedShadow(shadowBody)
  return matched && matched.reported && matched.reported.properties
    ? matched.reported.properties
    : {}
}

const getShadowReportedEventTime = shadowBody => {
  const matched = getMatchedShadow(shadowBody)
  const reported = matched && matched.reported ? matched.reported : {}
  return parseTimeSeconds(reported.event_time || matched && matched.event_time)
}

const getShadowReportedEventMark = shadowBody => {
  const matched = getMatchedShadow(shadowBody)
  const reported = matched && matched.reported ? matched.reported : {}
  const eventTime = reported.event_time || matched && matched.event_time
  return eventTime ? String(eventTime) : ''
}

const createSpotsFromArray = value => {
  const list = parseJsonMaybe(value)
  if (!Array.isArray(list)) {
    return []
  }

  return list.map((item, index) => {
    if (typeof item === 'string' || typeof item === 'number' || typeof item === 'boolean') {
      return {
        id: normalizeSpotId('', index),
        status: normalizeStatus(item),
        updated_at: nowSeconds()
      }
    }

    return {
      id: normalizeSpotId(item.id || item.spot_id || item.name, index),
      status: normalizeStatus(item.status || item.value || item.occupied),
      updated_at: item.updated_at || item.timestamp || nowSeconds()
    }
  })
}

const createSpotsFromMap = map => {
  const parsed = parseJsonMaybe(map)
  if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object') {
    return []
  }

  return Object.keys(parsed).map((key, index) => {
    const value = parsed[key]
    const detail = parseJsonMaybe(value)
    return {
      id: normalizeSpotId(key, index),
      status: normalizeStatus(detail && typeof detail === 'object' ? (detail.status || detail.value || detail.occupied) : detail),
      updated_at: detail && detail.updated_at ? detail.updated_at : nowSeconds()
    }
  })
}

const createSpotsFromFlatProperties = properties => {
  const keys = Object.keys(properties || {}).filter(key => /^[A-Za-z][-_]?\d{1,2}$/.test(key))
  return keys.map((key, index) => ({
    id: normalizeSpotId(key, index),
    status: normalizeStatus(properties[key]),
    updated_at: nowSeconds()
  }))
}

const buildOverviewFromShadow = shadowBody => {
  const properties = getShadowProperties(shadowBody)
  const shadowConfig = config.huaweiCloud.shadow
  const rawSpots = properties[shadowConfig.spotsProperty] || properties.spot_status || properties.spotStatus
  const shadowEventTime = getShadowReportedEventTime(shadowBody)
  const shadowEventMark = getShadowReportedEventMark(shadowBody)

  let spots = createSpotsFromSingleProperties(properties)
  if (!spots.length) {
    spots = createSpotsFromArray(rawSpots)
  }
  if (!spots.length) {
    spots = createSpotsFromMap(rawSpots)
  }
  if (!spots.length) {
    spots = createSpotsFromFlatProperties(properties)
  }
  if (!spots.length) {
    spots = mockData.getParkingOverview().spots
  }

  const reportedTotal = Number(properties[shadowConfig.totalProperty])
  const total = Math.max(reportedTotal || config.parking.totalSpots || 0, spots.length)
  const remaining = Number(properties[shadowConfig.remainingProperty]) ||
    spots.filter(spot => spot.status === 'empty').length

  return {
    parking_name: config.parking.name,
    address: config.parking.address,
    fee_rule: config.parking.feeRule,
    total_spots: total,
    remaining_spots: remaining,
    spots,
    updated_at: Number(properties[shadowConfig.updatedAtProperty]) || shadowEventTime || nowSeconds(),
    last_spot_id: getPropertyValue(properties, [shadowConfig.spotIdProperty, 'Parking_id', 'parking_id', 'spot_id'])
      ? normalizeSpotId(getPropertyValue(properties, [shadowConfig.spotIdProperty, 'Parking_id', 'parking_id', 'spot_id']))
      : '',
    last_spot_state: getPropertyValue(properties, [shadowConfig.spotStateProperty, 'Parking_state', 'parking_state', 'spot_state']) || '',
    last_card_id: getPropertyValue(properties, [shadowConfig.cardIdProperty, 'card_id', 'Card_id', 'cardId']) || '',
    last_plate_no: getPropertyValue(properties, [shadowConfig.plateNoProperty, 'plate_no', 'Plate_no', 'plateNo', 'license_plate', 'car_no']) || '',
    last_shadow_event_time: shadowEventMark,
    raw_shadow: shadowBody
  }
}

const normalizeOverview = overview => {
  const spots = overview.spots || []
  const total = overview.total_spots || spots.length
  const remaining = spots.filter(spot => spot.status === 'empty').length

  return Object.assign({}, overview, {
    total_spots: total,
    remaining_spots: remaining
  })
}

const login = () => Promise.resolve({
  token: isUsingMock() ? 'mock-token' : '',
  role: 'admin'
})

const getDeviceShadow = () => {
  if (isUsingMock()) {
    return Promise.resolve(null)
  }

  const cloud = config.huaweiCloud
  const path = `/v5/iot/${cloud.projectId}/devices/${cloud.deviceId}/shadow`
  return requestIoTDA(path, { method: 'GET' })
}

const getParkingOverview = () => {
  if (isUsingMock()) {
    return Promise.resolve(mockData.getParkingOverview()).then(normalizeOverview)
  }

  const warnings = getConfigWarnings()
  if (warnings.length) {
    return Promise.reject(new Error(warnings[0]))
  }

  return getDeviceShadow().then(buildOverviewFromShadow).then(normalizeOverview)
}

const updateLocalSpotStatus = update => Promise.resolve(mockData.updateSpotStatus(update)).then(normalizeOverview)

const openBarrier = operator => {
  if (isUsingMock()) {
    return Promise.resolve({
      command: 'open_barrier',
      result: 'success',
      timestamp: nowSeconds(),
      message: '道闸已开启',
      operator
    })
  }

  const cloud = config.huaweiCloud
  const command = cloud.command
  const path = `/v5/iot/${cloud.projectId}/devices/${cloud.deviceId}/commands`
  const data = {
    service_id: command.serviceId,
    command_name: command.commandName,
    paras: command.paras
  }

  return requestIoTDA(path, {
    method: 'POST',
    data
  }).then(res => {
    const commandResult = res && res.response && res.response.paras
      ? res.response.paras.result
      : 'success'
    const ok = !res.error_code && String(commandResult).toLowerCase() !== 'failed'

    return {
      command_id: res.command_id,
      command: command.commandName,
      result: ok ? 'success' : 'failed',
      timestamp: nowSeconds(),
      message: ok ? '道闸已开启' : (res.error_msg || '设备未返回成功状态'),
      raw_response: res,
      operator
    }
  })
}

const getOperationLogs = () => Promise.resolve(mockData.getOperationLogs())

const addOperationLog = log => Promise.resolve(mockData.addOperationLog(log))

const getParkingRecords = () => Promise.resolve(mockData.getParkingRecords())

const addEntryRecord = record => Promise.resolve(mockData.addEntryRecord(record))

const addExitRecord = record => Promise.resolve(mockData.addExitRecord(record))

const getVehicle = () => Promise.resolve(mockData.getVehicle())

const saveVehicle = vehicle => Promise.resolve(mockData.saveVehicle(vehicle))

const submitFeedback = feedback => Promise.resolve(mockData.submitFeedback(feedback))

module.exports = {
  login,
  getToken,
  getDeviceShadow,
  getParkingOverview,
  updateLocalSpotStatus,
  openBarrier,
  getOperationLogs,
  addOperationLog,
  getParkingRecords,
  addEntryRecord,
  addExitRecord,
  getVehicle,
  saveVehicle,
  submitFeedback,
  isUsingMock,
  hasHuaweiCloudConfig,
  getConnectionText,
  getConfigWarnings
}
