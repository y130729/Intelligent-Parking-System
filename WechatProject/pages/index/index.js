const cloudApi = require('../../utils/cloudApi')
const config = require('../../utils/config')
const realtime = require('../../utils/realtime')
const util = require('../../utils/util')

const statusMap = {
  empty: {
    text: '空闲',
    className: 'empty'
  },
  occupied: {
    text: '占用',
    className: 'occupied'
  }
}

const ACCESS_EVENT_SNAPSHOT_KEY = 'lastAccessEventSnapshotV1'

Page({
  data: {
    overview: {
      parking_name: '智慧停车场',
      address: '',
      fee_rule: '',
      total_spots: 0,
      remaining_spots: 0
    },
    spots: [],
    visibleSpots: [],
    filter: 'all',
    filterTabs: [
      { label: '全部', value: 'all', activeClass: 'active' },
      { label: '空闲', value: 'empty', activeClass: '' },
      { label: '占用', value: 'occupied', activeClass: '' }
    ],
    occupiedCount: 0,
    emptyCount: 0,
    occupancyRate: 0,
    lastSyncText: '--',
    lastNotice: '等待云平台车位数据',
    connectionText: '连接中',
    connectionClass: 'pending',
    isLoading: false
  },
  onLoad() {
    this.usingMock = cloudApi.isUsingMock()
    if (this.usingMock) {
      this.unsubscribe = realtime.onMessage(this.handleRealtimeMessage.bind(this))
      realtime.connect()
    }

    this.syncConnectionText()
    this.loadOverview(true)
    if (!this.usingMock) {
      this.startCloudPolling()
    }
  },
  onShow() {
    this.syncConnectionText()
  },
  onUnload() {
    if (this.unsubscribe) {
      this.unsubscribe()
    }
    if (this.pollTimer) {
      clearInterval(this.pollTimer)
    }
  },
  onPullDownRefresh() {
    this.loadOverview(false).finally(() => {
      wx.stopPullDownRefresh()
    })
  },
  startCloudPolling() {
    if (this.pollTimer) {
      clearInterval(this.pollTimer)
    }

    this.pollTimer = setInterval(() => {
      this.loadOverview(false)
    }, config.huaweiCloud.pollInterval)
  },
  loadOverview(showLoading) {
    if (showLoading) {
      this.setData({ isLoading: true })
    }

    return cloudApi.getParkingOverview().then(overview => {
      this.applyOverview(overview)
      this.notifyBoundCardAccess(overview)
      this.setData({
        lastNotice: this.usingMock ? '本地模拟数据已刷新' : this.getCloudNotice(overview)
      })
    }).catch(error => {
      const message = error.message || '车位数据获取失败'
      this.setData({
        lastNotice: message
      })
      if (!this.usingMock && !this.hasShownCloudError) {
        this.hasShownCloudError = true
        wx.showModal({
          title: '华为云访问失败',
          content: message,
          showCancel: false
        })
      }
      wx.showToast({
        title: '车位数据获取失败',
        icon: 'none'
      })
    }).finally(() => {
      this.setData({ isLoading: false })
    })
  },
  refreshOverview() {
    this.loadOverview(true)
  },
  getCloudNotice(overview) {
    return '设备影子数据已同步'
  },
  notifyBoundCardAccess(overview) {
    if (this.usingMock) {
      return
    }

    const cardId = normalizeCardId(overview.last_card_id)
    const plateNo = normalizePlateNo(overview.last_plate_no)
    const currentSnapshot = createAccessSnapshot(overview, cardId, plateNo)
    const previousSnapshot = wx.getStorageSync(ACCESS_EVENT_SNAPSHOT_KEY)

    if (previousSnapshot && previousSnapshot.eventKey === currentSnapshot.eventKey) {
      return
    }

    wx.setStorageSync(ACCESS_EVENT_SNAPSHOT_KEY, currentSnapshot)
    if (!previousSnapshot || hasSpotChanged(previousSnapshot, currentSnapshot)) {
      return
    }

    if (!cardId && !plateNo) {
      return
    }

    const vehicle = wx.getStorageSync('boundVehicle') || {}
    if (!vehicle.notify_enabled) {
      return
    }

    const boundCardId = normalizeCardId(vehicle.card_id)
    const boundPlateNo = normalizePlateNo(vehicle.plate_no)
    const cardMatched = cardId && boundCardId && cardId === boundCardId
    const plateMatched = plateNo && boundPlateNo && plateNo === boundPlateNo
    if (!cardMatched && !plateMatched) {
      return
    }

    const plateText = vehicle.plate_no ? `车辆 ${vehicle.plate_no}` : '绑定车辆'
    const now = Math.floor(Date.now() / 1000)
    const isInside = vehicle.parking_status === 'inside'
    const nextStatus = isInside ? 'outside' : 'inside'
    const nextVehicle = Object.assign({}, vehicle, {
      parking_status: nextStatus,
      last_access_time: now
    })
    wx.setStorageSync('boundVehicle', nextVehicle)

    const action = isInside ? '离场' : '入场'
    const message = `${plateText} 已${action}`
    const record = {
      plate_no: vehicle.plate_no || '',
      card_id: vehicle.card_id || overview.last_card_id || ''
    }

    if (isInside) {
      cloudApi.addExitRecord(Object.assign({}, record, {
        exit_time: now,
        status: 'completed'
      }))
    } else {
      cloudApi.addEntryRecord(Object.assign({}, record, {
        spot_id: '',
        entry_time: now,
        status: 'active'
      }))
    }

    wx.showModal({
      title: `${action}通知`,
      content: message,
      showCancel: false
    })
  },
  applyOverview(overview) {
    const spots = (overview.spots || []).map(this.formatSpot)
    const total = overview.total_spots || spots.length
    const emptyCount = spots.filter(spot => spot.status === 'empty').length
    const occupiedCount = total - emptyCount
    const occupancyRate = total ? Math.round((occupiedCount / total) * 100) : 0

    this.setData({
      overview: Object.assign({}, overview, {
        total_spots: total,
        remaining_spots: emptyCount
      }),
      spots,
      emptyCount,
      occupiedCount,
      occupancyRate,
      lastSyncText: util.formatMinute(new Date())
    }, () => {
      this.applyFilter()
    })
  },
  formatSpot(spot) {
    const status = statusMap[spot.status] || statusMap.empty
    return Object.assign({}, spot, {
      statusText: status.text,
      statusClass: status.className,
      updatedText: spot.updated_at ? util.formatMinute(new Date(spot.updated_at * 1000)) : '--'
    })
  },
  setFilter(e) {
    const filter = e.currentTarget.dataset.filter
    this.setData({
      filter,
      filterTabs: this.data.filterTabs.map(item => Object.assign({}, item, {
        activeClass: item.value === filter ? 'active' : ''
      }))
    }, () => {
      this.applyFilter()
    })
  },
  applyFilter() {
    const filter = this.data.filter
    const visibleSpots = filter === 'all'
      ? this.data.spots
      : this.data.spots.filter(spot => spot.status === filter)

    this.setData({ visibleSpots })
  },
  selectSpot(e) {
    const spotId = e.currentTarget.dataset.id
    const spot = this.data.spots.find(item => item.id === spotId)
    if (!spot) {
      return
    }

    wx.showModal({
      title: `车位 ${spot.id}`,
      content: `当前状态：${spot.statusText}\n更新时间：${spot.updatedText}`,
      showCancel: false
    })
  },
  handleRealtimeMessage(message) {
    if (message.type === 'connection_state') {
      this.syncConnectionText()
      return
    }

    if (message.type === 'spot_status_update') {
      cloudApi.updateLocalSpotStatus(message.data).then(overview => {
        this.applyOverview(overview)
        const status = statusMap[message.data.status] || statusMap.empty
        this.setData({
          lastNotice: `${message.data.spot_id} 更新为${status.text}`
        })
      })
      return
    }

    if (message.type === 'entry_detected') {
      this.setData({
        lastNotice: message.data.message || '入口检测到车辆'
      })
      return
    }

    if (message.type === 'control_response') {
      this.setData({
        lastNotice: message.data.message || '收到道闸控制响应'
      })
    }
  },
  syncConnectionText() {
    const connectionText = cloudApi.getConnectionText()
    this.setData({
      connectionText,
      connectionClass: connectionText === '华为云在线' ? 'empty' : 'pending'
    })
  }
})

function normalizeCardId(cardId) {
  return String(cardId || '').trim().toUpperCase()
}

function normalizePlateNo(plateNo) {
  return String(plateNo || '').replace(/\s/g, '').toUpperCase()
}

function normalizeSpotState(value) {
  return String(value || '').trim().toLowerCase()
}

function createAccessSnapshot(overview, cardId, plateNo) {
  const spotId = String(overview.last_spot_id || '').trim().toUpperCase()
  const spotState = normalizeSpotState(overview.last_spot_state)
  const eventKey = overview.last_shadow_event_time ||
    `${cardId || ''}|${plateNo || ''}|${spotId}|${spotState}`

  return {
    eventKey,
    cardId,
    plateNo,
    spotId,
    spotState
  }
}

function hasSpotChanged(previous, current) {
  return previous.spotId !== current.spotId ||
    previous.spotState !== current.spotState
}
