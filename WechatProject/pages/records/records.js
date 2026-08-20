const cloudApi = require('../../utils/cloudApi')
const util = require('../../utils/util')

Page({
  data: {
    isLoading: false,
    filter: 'all',
    filterTabs: [
      { label: '全部', value: 'all', activeClass: 'active' },
      { label: '停车中', value: 'active', activeClass: '' },
      { label: '已离场', value: 'completed', activeClass: '' }
    ],
    records: [],
    visibleRecords: [],
    hasVisibleRecords: false,
    boundPlate: '',
    boundCardId: '',
    emptyText: '暂无停车记录'
  },
  onLoad() {
    this.loadRecords(true)
  },
  onShow() {
    this.loadRecords(false)
  },
  onPullDownRefresh() {
    this.loadRecords(false).finally(() => {
      wx.stopPullDownRefresh()
    })
  },
  loadRecords(showLoading) {
    if (showLoading) {
      this.setData({ isLoading: true })
    }

    return Promise.all([
      cloudApi.getParkingRecords(),
      cloudApi.getVehicle()
    ]).then(([records, vehicle]) => {
      const boundPlate = normalizePlate(vehicle && vehicle.plate_no)
      const boundCardId = normalizeCardId(vehicle && vehicle.card_id)
      const displayRecords = cloudApi.isUsingMock()
        ? records
        : records.filter(record => {
          const recordPlate = normalizePlate(record.plate_no)
          const recordCardId = normalizeCardId(record.card_id)
          return (boundPlate && recordPlate === boundPlate) ||
            (boundCardId && recordCardId === boundCardId)
        })

      this.setData({
        boundPlate,
        boundCardId,
        emptyText: getEmptyText(boundPlate, boundCardId),
        records: displayRecords.map(this.formatRecord)
      }, () => {
        this.applyFilter()
      })
    }).catch(() => {
      wx.showToast({
        title: '记录获取失败',
        icon: 'none'
      })
    }).finally(() => {
      this.setData({ isLoading: false })
    })
  },
  formatRecord(record) {
    const entryAt = record.entry_time ? new Date(record.entry_time * 1000) : null
    const exitAt = record.exit_time ? new Date(record.exit_time * 1000) : null
    const duration = entryAt ? (exitAt || new Date()).getTime() - entryAt.getTime() : 0

    return Object.assign({}, record, {
      statusText: record.status === 'active' ? '停车中' : '已离场',
      statusClass: record.status === 'active' ? 'active' : 'completed',
      entryText: entryAt ? util.formatMinute(entryAt) : '--',
      exitText: exitAt ? util.formatMinute(exitAt) : '未离场',
      durationText: util.formatDuration(duration),
      feeText: record.status === 'active' ? '待结算' : `¥${record.fee || 0}`
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
    const visibleRecords = filter === 'all'
      ? this.data.records
      : this.data.records.filter(record => record.status === filter)

    this.setData({
      visibleRecords,
      hasVisibleRecords: visibleRecords.length > 0
    })
  }
})

function normalizePlate(plateNo) {
  return String(plateNo || '').replace(/\s/g, '').toUpperCase()
}

function normalizeCardId(cardId) {
  return String(cardId || '').trim().toUpperCase()
}

function getEmptyText(boundPlate, boundCardId) {
  if (!cloudApi.isUsingMock() && !boundPlate && !boundCardId) {
    return '请绑定车牌以获取记录'
  }

  return boundPlate ? `${boundPlate} 暂无停车记录` : '暂无停车记录'
}
