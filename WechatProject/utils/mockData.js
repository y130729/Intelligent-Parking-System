const config = require('./config')
const util = require('./util')

const STORAGE_KEYS = {
  overview: 'parkingOverview',
  operations: 'barrierOperationLogs',
  records: 'parkingRecords',
  vehicle: 'boundVehicle',
  feedback: 'feedbackList'
}

const occupiedSeeds = {
  'A-01': true,
  'A-04': true,
  'A-08': true,
  'B-02': true,
  'B-06': true,
  'B-09': true,
  'C-03': true,
  'C-05': true,
  'C-10': true,
  'D-01': true,
  'D-07': true,
  'D-08': true,
  'E-02': true,
  'E-04': true,
  'E-06': true,
  'E-09': true,
  'A-10': true,
  'C-07': true
}

const formatSpotId = (zone, index) => `${zone}-${index < 10 ? `0${index}` : index}`

const nowSeconds = () => Math.floor(Date.now() / 1000)

const getDeviceId = () => {
  if (config.huaweiCloud && config.huaweiCloud.deviceId) {
    return config.huaweiCloud.deviceId
  }

  if (config.cloud && config.cloud.deviceId) {
    return config.cloud.deviceId
  }

  return 'parking_gateway_01'
}

const buildSpots = () => {
  const zones = ['A', 'B', 'C', 'D', 'E']
  const spots = []
  zones.forEach(zone => {
    for (let index = 1; index <= 10; index += 1) {
      const id = formatSpotId(zone, index)
      spots.push({
        id,
        zone,
        status: occupiedSeeds[id] ? 'occupied' : 'empty',
        updated_at: nowSeconds() - index * 42
      })
    }
  })
  return spots
}

const hydrateOverview = overview => {
  const spots = overview.spots || []
  const total = overview.total_spots || spots.length || config.parking.totalSpots
  const remaining = spots.filter(item => item.status === 'empty').length

  return Object.assign({}, overview, {
    parking_name: overview.parking_name || config.parking.name,
    address: overview.address || config.parking.address,
    fee_rule: overview.fee_rule || config.parking.feeRule,
    total_spots: total,
    remaining_spots: remaining,
    updated_at: overview.updated_at || nowSeconds()
  })
}

const createDefaultOverview = () => hydrateOverview({
  parking_name: config.parking.name,
  address: config.parking.address,
  fee_rule: config.parking.feeRule,
  spots: buildSpots()
})

const getParkingOverview = () => {
  const cached = wx.getStorageSync(STORAGE_KEYS.overview)
  if (cached && cached.spots && cached.spots.length) {
    return hydrateOverview(cached)
  }

  const overview = createDefaultOverview()
  wx.setStorageSync(STORAGE_KEYS.overview, overview)
  return overview
}

const saveParkingOverview = overview => {
  const nextOverview = hydrateOverview(overview)
  wx.setStorageSync(STORAGE_KEYS.overview, nextOverview)
  return nextOverview
}

const updateSpotStatus = ({ spot_id: spotId, status, timestamp }) => {
  const overview = getParkingOverview()
  const spots = overview.spots.map(spot => {
    if (spot.id !== spotId) {
      return spot
    }

    return Object.assign({}, spot, {
      status,
      updated_at: timestamp || nowSeconds()
    })
  })

  return saveParkingOverview(Object.assign({}, overview, {
    spots,
    updated_at: timestamp || nowSeconds()
  }))
}

const createRandomSpotUpdate = () => {
  const overview = getParkingOverview()
  const index = Math.floor(Math.random() * overview.spots.length)
  const spot = overview.spots[index]
  const nextStatus = spot.status === 'empty' ? 'occupied' : 'empty'
  const update = {
    spot_id: spot.id,
    status: nextStatus,
    timestamp: nowSeconds(),
    device_id: getDeviceId()
  }

  updateSpotStatus(update)
  return update
}

const createDefaultOperationLogs = () => {
  const now = Date.now()
  return [
    {
      id: `op-${now - 300000}`,
      action: '远程开闸',
      operator: '管理员',
      result: 'success',
      message: '道闸已开启',
      timestamp: Math.floor((now - 300000) / 1000)
    },
    {
      id: `op-${now - 840000}`,
      action: '入口联动',
      operator: '入口设备',
      result: 'success',
      message: '检测到车辆到达入口',
      timestamp: Math.floor((now - 840000) / 1000)
    }
  ]
}

const getOperationLogs = () => {
  const cached = wx.getStorageSync(STORAGE_KEYS.operations)
  if (cached && cached.length) {
    return cached
  }

  const logs = createDefaultOperationLogs()
  wx.setStorageSync(STORAGE_KEYS.operations, logs)
  return logs
}

const addOperationLog = log => {
  const logs = getOperationLogs()
  const nextLog = Object.assign({
    id: `op-${Date.now()}`,
    action: '远程开闸',
    operator: '管理员',
    result: 'success',
    message: '道闸已开启',
    timestamp: nowSeconds()
  }, log)
  const nextLogs = [nextLog].concat(logs).slice(0, 30)
  wx.setStorageSync(STORAGE_KEYS.operations, nextLogs)
  return nextLogs
}

const createDefaultRecords = () => {
  const now = Date.now()
  return [
    {
      id: 'rec-1001',
      plate_no: '粤A8P36K',
      spot_id: 'A-03',
      entry_time: Math.floor((now - 5400000) / 1000),
      exit_time: 0,
      fee: 0,
      status: 'active'
    },
    {
      id: 'rec-1002',
      plate_no: '粤B2K81Q',
      spot_id: 'C-06',
      entry_time: Math.floor((now - 10800000) / 1000),
      exit_time: Math.floor((now - 1800000) / 1000),
      fee: 24,
      status: 'completed'
    },
    {
      id: 'rec-1003',
      plate_no: '粤A6M92X',
      spot_id: 'D-04',
      entry_time: Math.floor((now - 21600000) / 1000),
      exit_time: Math.floor((now - 14400000) / 1000),
      fee: 18,
      status: 'completed'
    },
    {
      id: 'rec-1004',
      plate_no: '粤A1T73N',
      spot_id: 'B-08',
      entry_time: Math.floor((now - 2400000) / 1000),
      exit_time: 0,
      fee: 0,
      status: 'active'
    }
  ]
}

const getParkingRecords = () => {
  const cached = wx.getStorageSync(STORAGE_KEYS.records)
  if (cached && cached.length) {
    return cached
  }

  const records = createDefaultRecords()
  wx.setStorageSync(STORAGE_KEYS.records, records)
  return records
}

const addEntryRecord = record => {
  const records = getParkingRecords()
  const cardId = String(record.card_id || '').trim().toUpperCase()
  const plateNo = String(record.plate_no || '').trim().toUpperCase()
  const matchedIndex = records.findIndex(item => {
    if (item.status !== 'active') {
      return false
    }

    const itemCardId = String(item.card_id || '').trim().toUpperCase()
    const itemPlateNo = String(item.plate_no || '').trim().toUpperCase()
    return (cardId && itemCardId === cardId) || (plateNo && itemPlateNo === plateNo)
  })

  if (matchedIndex >= 0) {
    const nextRecords = records.slice()
    nextRecords[matchedIndex] = Object.assign({}, nextRecords[matchedIndex], {
      plate_no: plateNo || nextRecords[matchedIndex].plate_no,
      card_id: cardId || nextRecords[matchedIndex].card_id,
      spot_id: record.spot_id || nextRecords[matchedIndex].spot_id,
      entry_time: nextRecords[matchedIndex].entry_time || record.entry_time || nowSeconds(),
      status: 'active'
    })
    wx.setStorageSync(STORAGE_KEYS.records, nextRecords)
    return nextRecords
  }

  const nextRecord = Object.assign({
    id: `rec-${Date.now()}`,
    plate_no: plateNo,
    card_id: cardId,
    spot_id: record.spot_id || '',
    entry_time: record.entry_time || nowSeconds(),
    exit_time: 0,
    fee: 0,
    status: 'active'
  }, record)
  const nextRecords = [nextRecord].concat(records).slice(0, 50)
  wx.setStorageSync(STORAGE_KEYS.records, nextRecords)
  return nextRecords
}

const addExitRecord = record => {
  const records = getParkingRecords()
  const cardId = String(record.card_id || '').trim().toUpperCase()
  const plateNo = String(record.plate_no || '').trim().toUpperCase()
  const exitTime = record.exit_time || nowSeconds()
  const matchedIndex = records.findIndex(item => {
    if (item.status !== 'active') {
      return false
    }

    const itemCardId = String(item.card_id || '').trim().toUpperCase()
    const itemPlateNo = String(item.plate_no || '').trim().toUpperCase()
    return (cardId && itemCardId === cardId) || (plateNo && itemPlateNo === plateNo)
  })

  if (matchedIndex < 0) {
    return records
  }

  const nextRecords = records.slice()
  const entryTime = nextRecords[matchedIndex].entry_time || exitTime
  const hours = Math.max(1, Math.ceil((exitTime - entryTime) / 3600))
  nextRecords[matchedIndex] = Object.assign({}, nextRecords[matchedIndex], {
    exit_time: exitTime,
    fee: Math.max(0, (hours - 1) * 3),
    status: 'completed'
  })
  wx.setStorageSync(STORAGE_KEYS.records, nextRecords)
  return nextRecords
}

const getVehicle = () => wx.getStorageSync(STORAGE_KEYS.vehicle) || {
  plate_no: '',
  card_id: '',
  parking_status: 'outside',
  bind_type: 'wechat',
  notify_type: 'wechat',
  notify_enabled: true,
  wechat_nickName: '',
  wechat_avatarUrl: ''
}

const saveVehicle = vehicle => {
  const nextVehicle = Object.assign({}, getVehicle(), vehicle, {
    parking_status: vehicle.parking_status || getVehicle().parking_status || 'outside',
    bind_type: 'wechat',
    notify_type: 'wechat',
    notify_enabled: true,
    updated_at: util.formatTime(new Date())
  })
  delete nextVehicle.phone
  nextVehicle.card_id = String(nextVehicle.card_id || '').trim().toUpperCase()
  wx.setStorageSync(STORAGE_KEYS.vehicle, nextVehicle)
  return nextVehicle
}

const submitFeedback = feedback => {
  const cached = wx.getStorageSync(STORAGE_KEYS.feedback) || []
  const nextFeedback = Object.assign({
    id: `fb-${Date.now()}`,
    content: '',
    timestamp: nowSeconds(),
    status: 'submitted'
  }, feedback)
  const nextList = [nextFeedback].concat(cached).slice(0, 20)
  wx.setStorageSync(STORAGE_KEYS.feedback, nextList)
  return nextFeedback
}

module.exports = {
  getParkingOverview,
  saveParkingOverview,
  updateSpotStatus,
  createRandomSpotUpdate,
  getOperationLogs,
  addOperationLog,
  getParkingRecords,
  addEntryRecord,
  addExitRecord,
  getVehicle,
  saveVehicle,
  submitFeedback
}
