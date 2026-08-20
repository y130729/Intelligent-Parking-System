const config = require('./config')
const mockData = require('./mockData')

const legacyCloud = config.cloud || {}
const huaweiCloud = config.huaweiCloud || {}
const topics = legacyCloud.topics || {
  spotStatus: 'parking/gdut-smart-parking/spot/status',
  barrierControl: 'parking/gdut-smart-parking/barrier/control',
  controlResponse: 'parking/gdut-smart-parking/barrier/response',
  entryNotice: 'parking/gdut-smart-parking/entry/notice'
}

let socketTask = null
let connected = false
let mode = 'mock'
let mockTimer = null
let initialized = false

const subscribers = []

const nowSeconds = () => Math.floor(Date.now() / 1000)

const emit = message => {
  subscribers.slice().forEach(listener => {
    listener(message)
  })
}

const emitConnectionState = () => {
  emit({
    type: 'connection_state',
    data: {
      connected,
      mode
    }
  })
}

const parseMessage = raw => {
  if (!raw) {
    return null
  }

  if (typeof raw === 'object' && raw.type) {
    return raw
  }

  try {
    const parsed = typeof raw === 'string' ? JSON.parse(raw) : raw
    if (parsed.type) {
      return parsed
    }

    if (parsed.payload) {
      return typeof parsed.payload === 'string' ? JSON.parse(parsed.payload) : parsed.payload
    }

    return parsed
  } catch (error) {
    return {
      type: 'system_notice',
      data: {
        message: '收到无法解析的云平台消息',
        raw
      }
    }
  }
}

const startMockStream = () => {
  mode = 'mock'
  connected = false
  emitConnectionState()

  if (mockTimer) {
    return
  }

  mockTimer = setInterval(() => {
    const update = mockData.createRandomSpotUpdate()
    emit({
      type: 'spot_status_update',
      data: update
    })

    if (Math.random() > 0.72) {
      emit({
        type: 'entry_detected',
        data: {
          message: '入口检测到车辆，请刷卡或扫码入场',
          timestamp: nowSeconds()
        }
      })
    }
  }, 8000)
}

const stopMockStream = () => {
  if (mockTimer) {
    clearInterval(mockTimer)
    mockTimer = null
  }
}

const sendSocketMessage = data => {
  if (!socketTask || !connected) {
    return false
  }

  socketTask.send({
    data: JSON.stringify(data)
  })
  return true
}

const connect = () => {
  if (config.useMock || !legacyCloud.mqttWsUrl) {
    startMockStream()
    return Promise.resolve({ mode: 'mock' })
  }

  if (socketTask) {
    return Promise.resolve({ mode, connected })
  }

  mode = 'cloud'
  stopMockStream()

  return new Promise(resolve => {
    socketTask = wx.connectSocket({
      url: legacyCloud.mqttWsUrl,
      success: () => resolve({ mode: 'cloud' }),
      fail: () => {
        socketTask = null
        startMockStream()
        resolve({ mode: 'mock' })
      }
    })

    socketTask.onOpen(() => {
      connected = true
      emitConnectionState()
      sendSocketMessage({
        action: 'subscribe',
        topics: [
          topics.spotStatus,
          topics.controlResponse,
          topics.entryNotice
        ]
      })
    })

    socketTask.onMessage(event => {
      const message = parseMessage(event.data)
      if (message) {
        emit(message)
      }
    })

    socketTask.onClose(() => {
      connected = false
      socketTask = null
      emitConnectionState()
      startMockStream()
    })

    socketTask.onError(() => {
      connected = false
      socketTask = null
      emitConnectionState()
      startMockStream()
    })
  })
}

const init = () => {
  if (initialized) {
    return
  }

  initialized = true
  connect()
}

const onMessage = listener => {
  subscribers.push(listener)
  listener({
    type: 'connection_state',
    data: {
      connected,
      mode
    }
  })

  return () => {
    const index = subscribers.indexOf(listener)
    if (index >= 0) {
      subscribers.splice(index, 1)
    }
  }
}

const publishOpenBarrier = ({ operator }) => {
  const commandId = `open-${Date.now()}`
  const payload = {
    type: 'open_barrier',
    data: {
      command_id: commandId,
      command: 'open_barrier',
      operator,
      timestamp: nowSeconds(),
      device_id: huaweiCloud.deviceId || legacyCloud.deviceId || 'parking_gateway_01'
    }
  }

  const sent = sendSocketMessage({
    action: 'publish',
    topic: topics.barrierControl,
    payload
  })

  if (!sent) {
    setTimeout(() => {
      emit({
        type: 'control_response',
        data: {
          command_id: commandId,
          command: 'open_barrier',
          result: 'success',
          timestamp: nowSeconds(),
          message: '道闸已开启'
        }
      })
    }, 900)
  }

  return Promise.resolve({
    commandId,
    payload,
    mode: sent ? 'cloud' : 'mock'
  })
}

const getConnectionState = () => ({
  connected,
  mode
})

module.exports = {
  init,
  connect,
  onMessage,
  publishOpenBarrier,
  getConnectionState
}
