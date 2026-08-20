const STORAGE_KEY = 'parkingWebConfig'
const TOKEN_KEY = 'parkingWebIamToken'
const API_BASE_URL = location.protocol === 'file:' ? 'http://127.0.0.1:3001' : ''
const CLOUD_SPOT_CACHE_KEY = 'parkingWebCloudSpotStateCacheV2'
const ACCESS_EVENT_SNAPSHOT_KEY = 'parkingWebAccessEventSnapshotV3'
const RECORDS_KEY = 'parkingWebRecords'

const defaultConfig = {
  useMock: false,
  iamEndpoint: 'https://iam.cn-south-1.myhuaweicloud.com',
  iotdaEndpoint: '',
  projectId: '',
  domainName: '',
  iamUserName: '',
  iamPassword: '',
  deviceId: '',
  instanceId: '',
  requestTimeout: 25000,
  pollInterval: 3000,
  parkingName: '广东工业大学智慧停车场',
  parkingAddress: '广东工业大学大学城校区西门停车区',
  parkingFeeRule: '首小时免费，之后 3 元/小时，24 小时封顶 30 元',
  shadow: {
    serviceId: 'Spot_state',
    spotIdProperty: 'Parking_id',
    spotStateProperty: 'Parking_state',
    cardIdProperty: 'card_id',
    plateNoProperty: 'plate_no',
    spotsProperty: 'spots',
    totalProperty: 'total_spots',
    remainingProperty: 'remaining_spots',
    updatedAtProperty: 'updated_at'
  },
  command: {
    serviceId: 'ParkingBarrier',
    commandName: 'open_barrier',
    paras: {
      value: 'open'
    }
  }
}

const state = {
  config: loadConfig(),
  overview: null,
  spots: [],
  filter: 'all',
  logs: loadLogs(),
  records: loadRecords(),
  pollTimer: null,
  mockTimer: null,
  barrierState: 'closed'
}

const els = {}

document.addEventListener('DOMContentLoaded', () => {
  bindElements()
  bindEvents()
  loadSettingsForm()
  renderRecords()
  start()
})

function bindElements() {
  [
    'connectionText', 'statusPill', 'parkingName', 'parkingAddress', 'noticeText',
    'remainingCount', 'totalCount', 'occupancyRate', 'barrierState', 'spotGrid',
    'filterTabs', 'openBarrierBtn', 'commandState', 'logList', 'recordList',
    'settingsDialog', 'settingsBtn', 'refreshBtn', 'clearLogsBtn', 'saveSettingsBtn',
    'testCloudBtn', 'useMockInput', 'iamEndpointInput', 'iotdaEndpointInput',
    'projectIdInput', 'domainNameInput', 'iamUserNameInput', 'iamPasswordInput',
    'deviceIdInput', 'instanceIdInput'
  ].forEach(id => {
    els[id] = document.getElementById(id)
  })
}

function bindEvents() {
  els.settingsBtn.addEventListener('click', () => els.settingsDialog.showModal())
  els.refreshBtn.addEventListener('click', () => refreshOverview(true))
  els.openBarrierBtn.addEventListener('click', openBarrier)
  els.clearLogsBtn.addEventListener('click', () => {
    state.logs = []
    saveLogs()
    renderLogs()
  })
  els.filterTabs.addEventListener('click', event => {
    const button = event.target.closest('button[data-filter]')
    if (!button) return
    state.filter = button.dataset.filter
    renderFilterTabs()
    renderSpots()
  })
  els.saveSettingsBtn.addEventListener('click', () => {
    state.config = readSettingsForm()
    saveConfig()
    localStorage.removeItem(TOKEN_KEY)
    stopTimers()
    start()
  })
  els.testCloudBtn.addEventListener('click', async () => {
    const previous = state.config
    state.config = readSettingsForm()
    try {
      await getParkingOverview()
      setNotice('连接测试成功，已读取设备影子')
    } catch (error) {
      setNotice(error.message)
      alert(error.message)
    } finally {
      state.config = previous
    }
  })
}

function loadConfig() {
  const saved = localStorage.getItem(STORAGE_KEY)
  if (!saved) return { ...defaultConfig }
  try {
    return mergeConfig(defaultConfig, JSON.parse(saved))
  } catch (error) {
    return { ...defaultConfig }
  }
}

function mergeConfig(base, saved) {
  return {
    ...base,
    ...saved,
    shadow: { ...base.shadow, ...(saved.shadow || {}) },
    command: { ...base.command, ...(saved.command || {}) }
  }
}

function saveConfig() {
  localStorage.setItem(STORAGE_KEY, JSON.stringify(state.config))
}

function loadLogs() {
  try {
    return JSON.parse(localStorage.getItem('parkingWebLogs')) || []
  } catch (error) {
    return []
  }
}

function saveLogs() {
  localStorage.setItem('parkingWebLogs', JSON.stringify(state.logs.slice(0, 30)))
}

function loadRecords() {
  try {
    const records = JSON.parse(localStorage.getItem(RECORDS_KEY))
    if (Array.isArray(records)) return normalizeRecords(records)
      .filter(record => !String(record.id || '').startsWith('rec-demo-'))
  } catch (error) {
    return createRecords()
  }
  return createRecords()
}

function saveRecords() {
  localStorage.setItem(RECORDS_KEY, JSON.stringify(state.records.slice(0, 50)))
}

function normalizeRecords(records) {
  return records.map((record, index) => {
    if (record.entryTime || record.status === 'active' || record.status === 'completed') {
      return {
        ...record,
        cardId: record.cardId || record.card_id || ''
      }
    }

    const isActive = record.status === '停车中'
    return {
      id: record.id || `legacy-${index}`,
      plate: record.plate || record.plate_no || '绑定车辆',
      cardId: record.cardId || record.card_id || '',
      status: isActive ? 'active' : 'completed',
      entryTime: Math.floor(Date.now() / 1000) - (isActive ? 40 * 60 : 2 * 3600),
      exitTime: isActive ? 0 : Math.floor(Date.now() / 1000) - 20 * 60,
      fee: record.fee || 0
    }
  })
}

function loadSettingsForm() {
  const config = state.config
  els.useMockInput.checked = config.useMock
  els.iamEndpointInput.value = config.iamEndpoint
  els.iotdaEndpointInput.value = config.iotdaEndpoint
  els.projectIdInput.value = config.projectId
  els.domainNameInput.value = config.domainName
  els.iamUserNameInput.value = config.iamUserName
  els.iamPasswordInput.value = config.iamPassword
  els.deviceIdInput.value = config.deviceId
  els.instanceIdInput.value = config.instanceId
}

function readSettingsForm() {
  return mergeConfig(defaultConfig, {
    useMock: els.useMockInput.checked,
    iamEndpoint: els.iamEndpointInput.value.trim(),
    iotdaEndpoint: els.iotdaEndpointInput.value.trim(),
    projectId: els.projectIdInput.value.trim(),
    domainName: els.domainNameInput.value.trim(),
    iamUserName: els.iamUserNameInput.value.trim(),
    iamPassword: els.iamPasswordInput.value,
    deviceId: els.deviceIdInput.value.trim(),
    instanceId: els.instanceIdInput.value.trim()
  })
}

function start() {
  setConnectionText()
  refreshOverview(true)
  renderLogs()
  if (!state.config.useMock) {
    state.pollTimer = setInterval(() => refreshOverview(false), state.config.pollInterval)
  }
}

function stopTimers() {
  if (state.pollTimer) clearInterval(state.pollTimer)
  if (state.mockTimer) clearInterval(state.mockTimer)
  state.pollTimer = null
  state.mockTimer = null
}

function setConnectionText() {
  const text = state.config.useMock ? '本地模拟' : '华为云在线（本地代理）'
  els.connectionText.textContent = text
  els.statusPill.textContent = text
}

async function refreshOverview(showNotice) {
  try {
    const overview = await getParkingOverview()
    applyOverview(overview)
    handleAccessEvent(overview)
    if (showNotice) setNotice(state.config.useMock ? '当前是本地模拟，不会读取华为云上报' : getCloudSyncNotice(overview))
  } catch (error) {
    setNotice(error.message)
  }
}

function getCloudSyncNotice(overview) {
  if (overview.last_spot_id && overview.last_spot_state) {
    return `设备影子已同步：${overview.last_spot_id} ${overview.last_spot_state}`
  }
  return '设备影子数据已同步'
}

async function getParkingOverview() {
  if (state.config.useMock) {
    return getMockOverview()
  }
  const shadow = await getDeviceShadow()
  return buildOverviewFromShadow(shadow)
}

function validateCloudConfig() {
  if (state.config.iamEndpoint && !state.config.iamEndpoint.startsWith('https://')) {
    throw new Error('IAM Endpoint 在网页配置中必须以 https:// 开头')
  }
  if (state.config.iotdaEndpoint && !state.config.iotdaEndpoint.startsWith('https://')) {
    throw new Error('Endpoint 在网页配置中必须以 https:// 开头')
  }
}

async function requestProxy(path, label) {
  validateCloudConfig()
  const response = await fetchWithTimeout(`${API_BASE_URL}${path}`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      config: createProxyConfig()
    })
  }, label)
  return response.json()
}

function createProxyConfig() {
  return {
    iamEndpoint: state.config.iamEndpoint,
    iotdaEndpoint: state.config.iotdaEndpoint,
    projectId: state.config.projectId,
    domainName: state.config.domainName,
    iamUserName: state.config.iamUserName,
    iamPassword: state.config.iamPassword,
    deviceId: state.config.deviceId,
    instanceId: state.config.instanceId,
    requestTimeout: state.config.requestTimeout,
    shadow: state.config.shadow,
    command: state.config.command
  }
}

function getPropertyValue(properties, names) {
  const source = properties || {}
  const keys = Array.isArray(names) ? names : [names]
  const matchedKey = keys.find(key => Object.prototype.hasOwnProperty.call(source, key))
  return matchedKey ? source[matchedKey] : undefined
}

function normalizeSpotId(id, index) {
  if (id) return String(id).replace('_', '-').toUpperCase()
  return spotId(index || 0)
}

function normalizeCardId(cardId) {
  return String(cardId || '').trim().toUpperCase()
}

function normalizePlateNo(plateNo) {
  return String(plateNo || '').replace(/\s/g, '').toUpperCase()
}

function normalizeSpotState(value) {
  return String(value || '').trim().toLowerCase()
}

async function getIamToken() {
  const cached = readTokenCache()
  if (cached) return cached

  const url = `${trimSlash(state.config.iamEndpoint)}/v3/auth/tokens?nocatalog=true`
  const response = await fetchWithTimeout(url, {
    method: 'POST',
    headers: { 'content-type': 'application/json;charset=utf8' },
    body: JSON.stringify({
      auth: {
        identity: {
          methods: ['password'],
          password: {
            user: {
              domain: { name: state.config.domainName },
              name: state.config.iamUserName,
              password: state.config.iamPassword
            }
          }
        },
        scope: {
          project: { id: state.config.projectId }
        }
      }
    })
  }, 'IAM Token')

  const token = response.headers.get('X-Subject-Token')
  if (!token) {
    throw new Error('IAM Token 获取失败：浏览器无法读取 X-Subject-Token，可能被 CORS 限制')
  }
  const data = await response.json()
  const expiresAt = data.token && data.token.expires_at ? data.token.expires_at : new Date(Date.now() + 23 * 60 * 60 * 1000).toISOString()
  localStorage.setItem(TOKEN_KEY, JSON.stringify({ token, expiresAt, projectId: state.config.projectId, user: state.config.iamUserName }))
  return token
}

function readTokenCache() {
  try {
    const cached = JSON.parse(localStorage.getItem(TOKEN_KEY))
    if (!cached || cached.projectId !== state.config.projectId || cached.user !== state.config.iamUserName) return ''
    if (new Date(cached.expiresAt).getTime() - Date.now() < 30 * 60 * 1000) return ''
    return cached.token
  } catch (error) {
    return ''
  }
}

async function getDeviceShadow() {
  return requestProxy('/api/shadow', 'IoTDA 设备影子')
}

async function openBarrier() {
  els.commandState.className = 'response-box'
  els.commandState.textContent = state.config.useMock ? '正在模拟开闸...' : '正在通过 IoTDA 下发 open_barrier...'
  try {
    const result = state.config.useMock ? mockOpenBarrier() : await sendOpenBarrierCommand()
    state.barrierState = 'open'
    addLog('远程开闸', result.message || '道闸已开启', 'success')
    els.commandState.className = 'response-box success'
    els.commandState.textContent = result.message || '道闸已开启'
    renderLogs()
    renderBarrier()
  } catch (error) {
    addLog('远程开闸', error.message, 'failed')
    els.commandState.className = 'response-box failed'
    els.commandState.textContent = error.message
    renderLogs()
  }
}

async function sendOpenBarrierCommand() {
  const data = await requestProxy('/api/commands/open-barrier', 'IoTDA 命令下发')
  const commandResult = getCommandResult(data)
  const message = data.error_msg || data.message || ''
  if (
    data.error_code ||
    String(commandResult).toLowerCase() === 'failed' ||
    /timeout|timed out|超时|failed|失败/i.test(message)
  ) {
    throw new Error(data.error_code ? `${data.error_code}: ${message || '命令执行失败'}` : (message || '命令执行失败'))
  }

  return {
    message: String(commandResult).toLowerCase() === 'success' ? '道闸已开启' : '道闸命令已下发',
    data
  }
}

function getCommandResult(data) {
  if (!data) return ''
  if (data.response && data.response.paras) return data.response.paras.result
  if (data.paras) return data.paras.result
  return ''
}

async function fetchWithTimeout(url, options, label) {
  const controller = new AbortController()
  const timer = setTimeout(() => controller.abort(), state.config.requestTimeout)
  try {
    const response = await fetch(url, { ...options, signal: controller.signal })
    if (!response.ok) {
      let message = `${label}失败，HTTP ${response.status}`
      try {
        const data = await response.json()
        message += `，${data.error_code ? `${data.error_code}: ` : ''}${data.error_msg || data.message || '请检查配置'}`
      } catch (error) {
        message += '，请检查配置'
      }
      throw new Error(message)
    }
    return response
  } catch (error) {
    if (error.name === 'AbortError') throw new Error(`${label}超时`)
    if (error.message.includes('Failed to fetch')) {
      throw new Error(`${label}失败：请先运行 node web/server.js，并打开 http://127.0.0.1:3001`)
    }
    throw error
  } finally {
    clearTimeout(timer)
  }
}

function buildOverviewFromShadow(shadowBody) {
  const properties = getShadowProperties(shadowBody)
  const shadowConfig = state.config.shadow
  const rawSpots = properties[shadowConfig.spotsProperty] || properties.spot_status || properties.spotStatus
  let spots = createSpotsFromSingleProperties(properties)
  if (!spots.length) spots = createSpots(rawSpots)
  if (!spots.length) spots = createSpotsFromFlatProperties(properties)
  if (!spots.length) spots = getMockOverview().spots
  const reportedTotal = Number(properties[shadowConfig.totalProperty])
  const total = Math.max(reportedTotal || spots.length, spots.length)
  const lastSpotId = getPropertyValue(properties, [shadowConfig.spotIdProperty, 'Parking_id', 'parking_id', 'spot_id'])
  const lastSpotState = getPropertyValue(properties, [shadowConfig.spotStateProperty, 'Parking_state', 'parking_state', 'spot_state'])
  return {
    parking_name: state.config.parkingName,
    address: state.config.parkingAddress,
    fee_rule: state.config.parkingFeeRule,
    total_spots: total,
    remaining_spots: spots.filter(spot => spot.status === 'empty').length,
    spots,
    last_spot_id: lastSpotId ? normalizeSpotId(lastSpotId) : '',
    last_spot_state: lastSpotState || '',
    last_card_id: getPropertyValue(properties, [shadowConfig.cardIdProperty, 'card_id', 'Card_id', 'cardId']) || '',
    last_plate_no: getPropertyValue(properties, [shadowConfig.plateNoProperty, 'plate_no', 'Plate_no', 'plateNo', 'license_plate', 'car_no']) || '',
    last_shadow_event_time: getShadowEventMark(shadowBody)
  }
}

function getMatchedShadow(shadowBody) {
  const shadow = shadowBody && Array.isArray(shadowBody.shadow) ? shadowBody.shadow : []
  if (!shadow.length) return null
  const matched = shadow.find(item => item.service_id === state.config.shadow.serviceId)
  return matched || shadow[0]
}

function getShadowProperties(shadowBody) {
  const matched = getMatchedShadow(shadowBody)
  return matched && matched.reported && matched.reported.properties ? matched.reported.properties : {}
}

function getShadowEventMark(shadowBody) {
  const matched = getMatchedShadow(shadowBody)
  const reported = matched && matched.reported ? matched.reported : {}
  const eventTime = reported.event_time || (matched && matched.event_time)
  return eventTime ? String(eventTime) : ''
}

function buildDefaultCloudSpots() {
  return createDefaultSpots()
}

function getCloudSpotCache() {
  try {
    const cached = JSON.parse(localStorage.getItem(CLOUD_SPOT_CACHE_KEY))
    if (cached && Array.isArray(cached.spots) && cached.spots.length) return cached.spots
  } catch (error) {
    return buildDefaultCloudSpots()
  }
  return buildDefaultCloudSpots()
}

function saveCloudSpotCache(spots) {
  localStorage.setItem(CLOUD_SPOT_CACHE_KEY, JSON.stringify({
    spots,
    updated_at: Date.now()
  }))
}

function createSpotsFromSingleProperties(properties) {
  const shadowConfig = state.config.shadow
  const spotIdValue = getPropertyValue(properties, [shadowConfig.spotIdProperty, 'Parking_id', 'parking_id', 'spot_id'])
  const spotStateValue = getPropertyValue(properties, [shadowConfig.spotStateProperty, 'Parking_state', 'parking_state', 'spot_state'])
  if (!spotIdValue || spotStateValue === undefined || spotStateValue === null || spotStateValue === '') return []

  const id = normalizeSpotId(spotIdValue)
  const status = normalizeStatus(spotStateValue)
  let matched = false
  const spots = getCloudSpotCache().map(spot => {
    if (normalizeSpotId(spot.id) !== id) return spot
    matched = true
    return { ...spot, id, status, updated_at: Date.now() / 1000 }
  })
  if (!matched) {
    spots.push({ id, status, updated_at: Date.now() / 1000 })
  }
  saveCloudSpotCache(spots)
  return spots
}

function createSpots(value) {
  const parsed = parseMaybe(value)
  if (Array.isArray(parsed)) {
    return parsed.map((item, index) => {
      if (typeof item !== 'object') {
        return { id: spotId(index), status: normalizeStatus(item), updated_at: Date.now() / 1000 }
      }
      return {
        id: String(item.id || item.spot_id || spotId(index)).replace('_', '-').toUpperCase(),
        status: normalizeStatus(item.status || item.value || item.occupied),
        updated_at: item.updated_at || item.timestamp || Date.now() / 1000
      }
    })
  }
  if (parsed && typeof parsed === 'object') {
    return Object.keys(parsed).map((key, index) => ({ id: key.replace('_', '-').toUpperCase(), status: normalizeStatus(parsed[key]), updated_at: Date.now() / 1000 }))
  }
  return []
}

function createSpotsFromFlatProperties(properties) {
  return Object.keys(properties || {})
    .filter(key => /^[A-Za-z][-_]?\d{1,2}$/.test(key))
    .map((key, index) => ({ id: key.replace('_', '-').toUpperCase(), status: normalizeStatus(properties[key]), updated_at: Date.now() / 1000 || index }))
}

function parseMaybe(value) {
  if (typeof value !== 'string') return value
  try {
    return JSON.parse(value)
  } catch (error) {
    return value
  }
}

function normalizeStatus(value) {
  const parsed = parseMaybe(value)
  const raw = parsed && typeof parsed === 'object' ? (parsed.status || parsed.value || parsed.occupied) : parsed
  const key = String(raw).toLowerCase()
  if (['occupied', 'busy', 'used', '1', 'true', '占用', '已占用'].includes(key)) return 'occupied'
  return 'empty'
}

function getMockOverview() {
  if (!state.spots.length) state.spots = createDefaultSpots()
  const total = state.spots.length
  return {
    parking_name: state.config.parkingName,
    address: state.config.parkingAddress,
    fee_rule: state.config.parkingFeeRule,
    total_spots: total,
    remaining_spots: state.spots.filter(spot => spot.status === 'empty').length,
    spots: state.spots
  }
}

function createDefaultSpots() {
  const spots = []
  for (const zone of ['A', 'B', 'C', 'D', 'E']) {
    for (let i = 1; i <= 10; i += 1) {
      const id = `${zone}-${String(i).padStart(2, '0')}`
      spots.push({ id, status: 'empty', updated_at: Date.now() / 1000 })
    }
  }
  spots.push({ id: '1', status: 'empty', updated_at: Date.now() / 1000 })
  return spots
}

function startMockUpdates() {
  state.mockTimer = setInterval(() => {
    if (!state.spots.length) return
    const index = Math.floor(Math.random() * state.spots.length)
    state.spots[index] = {
      ...state.spots[index],
      status: state.spots[index].status === 'empty' ? 'occupied' : 'empty',
      updated_at: Date.now() / 1000
    }
    applyOverview(getMockOverview())
    setNotice(`${state.spots[index].id} 状态已模拟更新`)
  }, 8000)
}

function mockOpenBarrier() {
  return new Promise(resolve => {
    setTimeout(() => resolve({ message: '道闸已开启，本地模拟响应成功' }), 700)
  })
}

function applyOverview(overview) {
  state.overview = overview
  state.spots = overview.spots || []
  els.parkingName.textContent = overview.parking_name || state.config.parkingName
  els.parkingAddress.textContent = overview.address || state.config.parkingAddress
  const total = overview.total_spots || state.spots.length
  const remaining = state.spots.filter(spot => spot.status === 'empty').length
  const occupied = total - remaining
  els.remainingCount.textContent = remaining
  els.totalCount.textContent = total
  els.occupancyRate.textContent = total ? `${Math.round((occupied / total) * 100)}%` : '--'
  renderBarrier()
  renderSpots()
}

function handleAccessEvent(overview) {
  if (state.config.useMock) return

  const cardId = normalizeCardId(overview.last_card_id)
  const plateNo = normalizePlateNo(overview.last_plate_no)
  const hasAccessIdentity = Boolean(cardId || plateNo)
  const currentSnapshot = createAccessSnapshot(overview, cardId, plateNo)
  const previousSnapshot = readAccessSnapshot()

  if (previousSnapshot && previousSnapshot.eventKey === currentSnapshot.eventKey) return

  saveAccessSnapshot(currentSnapshot)
  if (!previousSnapshot && !hasAccessIdentity) return
  if (previousSnapshot && hasSpotChanged(previousSnapshot, currentSnapshot)) return
  if (!hasAccessIdentity) return

  const now = Math.floor(Date.now() / 1000)
  const record = {
    plate: plateNo,
    cardId,
    timestamp: now
  }

  const isInside = Boolean(findActiveRecord(record))
  const action = isInside ? '离场' : '入场'
  if (isInside) {
    addExitRecord(record)
  } else {
    addEntryRecord(record)
  }

  const message = `${getRecordCardLabel(record)} 已${action}`
  setNotice(message)
  addLog(`${action}通知`, message, 'success')
  renderLogs()
  renderRecords()
}

function readAccessSnapshot() {
  try {
    return JSON.parse(localStorage.getItem(ACCESS_EVENT_SNAPSHOT_KEY))
  } catch (error) {
    return null
  }
}

function saveAccessSnapshot(snapshot) {
  localStorage.setItem(ACCESS_EVENT_SNAPSHOT_KEY, JSON.stringify(snapshot))
}

function createAccessSnapshot(overview, cardId, plateNo) {
  const spotId = String(overview.last_spot_id || '').trim().toUpperCase()
  const spotState = normalizeSpotState(overview.last_spot_state)
  const eventKey = overview.last_shadow_event_time ||
    `${cardId || ''}|${plateNo || ''}|${spotId}|${spotState}`

  return { eventKey, cardId, plateNo, spotId, spotState }
}

function hasSpotChanged(previous, current) {
  return previous.spotId !== current.spotId ||
    previous.spotState !== current.spotState
}

function renderBarrier() {
  els.barrierState.textContent = state.barrierState === 'open' ? '已开' : '关闭'
}

function renderSpots() {
  const spots = state.filter === 'all' ? state.spots : state.spots.filter(spot => spot.status === state.filter)
  els.spotGrid.innerHTML = spots.map(spot => `
    <button class="spot ${spot.status}" type="button" title="${spot.id}">
      <strong>${spot.id}</strong>
      <span>${spot.status === 'empty' ? '空闲' : '占用'}</span>
    </button>
  `).join('')
}

function renderFilterTabs() {
  els.filterTabs.querySelectorAll('button').forEach(button => {
    button.classList.toggle('active', button.dataset.filter === state.filter)
  })
}

function addLog(action, message, result) {
  state.logs.unshift({ action, message, result, time: new Date().toLocaleString() })
  state.logs = state.logs.slice(0, 30)
  saveLogs()
}

function renderLogs() {
  if (!state.logs.length) {
    els.logList.innerHTML = '<div class="log-item"><strong>暂无操作</strong><span>点击远程开闸后会记录在这里</span></div>'
    return
  }
  els.logList.innerHTML = state.logs.map(log => `
    <div class="log-item">
      <strong>${log.action} · ${log.result === 'failed' ? '失败' : '成功'}</strong>
      <span>${log.message}</span><br>
      <span>${log.time}</span>
    </div>
  `).join('')
}

function createRecords() {
  return []
}

function findActiveRecord(record) {
  const cardId = normalizeCardId(record.cardId)
  const plate = normalizePlateNo(record.plate)
  return state.records.find(item => item.status === 'active' && (
    (cardId && normalizeCardId(item.cardId) === cardId) ||
    (plate && normalizePlateNo(item.plate) === plate)
  ))
}

function addEntryRecord(record) {
  const cardId = normalizeCardId(record.cardId)
  const plate = normalizePlateNo(record.plate)
  const matched = findActiveRecord(record)
  if (matched) {
    matched.entryTime = matched.entryTime || record.timestamp
    matched.cardId = matched.cardId || cardId
    matched.plate = matched.plate || plate
  } else {
    state.records.unshift({
      id: `rec-${Date.now()}`,
      plate: record.plate || plate || '绑定车辆',
      cardId,
      status: 'active',
      entryTime: record.timestamp,
      exitTime: 0,
      fee: 0
    })
  }
  state.records = state.records.slice(0, 50)
  saveRecords()
}

function addExitRecord(record) {
  const cardId = normalizeCardId(record.cardId)
  const plate = normalizePlateNo(record.plate)
  const matched = findActiveRecord(record)
  if (!matched) return

  matched.status = 'completed'
  matched.exitTime = record.timestamp
  const hours = Math.max(1, Math.ceil((matched.exitTime - matched.entryTime) / 3600))
  matched.fee = Math.max(0, (hours - 1) * 3)
  saveRecords()
}

function renderRecords() {
  els.recordList.innerHTML = state.records.map(record => `
    <div class="record-item">
      <strong>${getRecordCardLabel(record)}</strong>
      <span>${record.status === 'active' ? '停车中' : '已离场'} · ${formatDurationText(record)}</span><br>
      <span>入场 ${formatRecordTime(record.entryTime)} · 离场 ${record.exitTime ? formatRecordTime(record.exitTime) : '未离场'}</span><br>
      <span>费用 ${record.status === 'active' ? '待结算' : `¥${record.fee || 0}`}</span>
    </div>
  `).join('')
}

function getRecordCardLabel(record) {
  return normalizeCardId(record.cardId || record.card_id) || '未绑定卡号'
}

function formatRecordTime(seconds) {
  if (!seconds) return '--'
  return new Date(seconds * 1000).toLocaleString()
}

function formatDurationText(record) {
  if (!record.entryTime) return '--'
  const end = record.exitTime || Math.floor(Date.now() / 1000)
  const minutes = Math.max(1, Math.round((end - record.entryTime) / 60))
  if (minutes < 60) return `${minutes}分钟`
  const hours = Math.floor(minutes / 60)
  const rest = minutes % 60
  return rest ? `${hours}小时${rest}分钟` : `${hours}小时`
}

function setNotice(message) {
  els.noticeText.textContent = message
}

function spotId(index) {
  const zone = String.fromCharCode(65 + Math.floor(index / 10))
  return `${zone}-${String((index % 10) + 1).padStart(2, '0')}`
}

function trimSlash(value) {
  return String(value || '').replace(/\/$/, '')
}
