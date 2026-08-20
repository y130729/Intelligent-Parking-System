const cloudApi = require('../../utils/cloudApi')
const util = require('../../utils/util')

Page({
  data: {
    connectionText: '连接中',
    connectionClass: 'pending',
    isOpening: false,
    lastResponse: {
      state: 'idle',
      title: '道闸待命',
      message: '等待远程开闸指令'
    },
    logs: []
  },
  onLoad() {
    this.syncConnectionText()
    this.loadLogs()
  },
  onShow() {
    this.syncConnectionText()
    this.loadLogs()
  },
  loadLogs() {
    cloudApi.getOperationLogs().then(logs => {
      this.setData({
        logs: logs.map(this.formatLog)
      })
    })
  },
  formatLog(log) {
    const resultText = log.result === 'failed' ? '失败' : '成功'
    return Object.assign({}, log, {
      resultText,
      resultClass: log.result === 'failed' ? 'failed' : 'success',
      timeText: log.timestamp ? util.formatMinute(new Date(log.timestamp * 1000)) : '--'
    })
  },
  openBarrier() {
    if (this.data.isOpening) {
      return
    }

    const app = getApp()
    const operator = app.globalData.userInfo && app.globalData.userInfo.nickName
      ? app.globalData.userInfo.nickName
      : '授权管理员'

    this.setData({
      isOpening: true,
      lastResponse: {
        state: 'pending',
        title: '指令下发中',
        message: cloudApi.isUsingMock()
          ? '正在模拟道闸控制响应'
          : '正在通过华为云 IoTDA 下发设备命令'
      }
    })

    cloudApi.openBarrier(operator).then(response => {
      const result = response.result || 'success'
      return cloudApi.addOperationLog({
        id: response.command_id || `op-${Date.now()}`,
        action: '远程开闸',
        operator,
        result,
        message: response.message || (result === 'success' ? '道闸已开启' : '道闸开启失败'),
        timestamp: response.timestamp || Math.floor(Date.now() / 1000)
      }).then(logs => {
        this.setData({
          isOpening: false,
          lastResponse: {
            state: result === 'success' ? 'success' : 'failed',
            title: result === 'success' ? '执行成功' : '执行失败',
            message: response.message || (result === 'success' ? '道闸已开启' : '设备未返回成功状态')
          },
          logs: logs.map(this.formatLog)
        })

        wx.showToast({
          title: result === 'success' ? '道闸已开启' : '开闸失败',
          icon: result === 'success' ? 'success' : 'none'
        })
      })
    }).catch(error => {
      this.setData({
        isOpening: false,
        lastResponse: {
          state: 'failed',
          title: '下发失败',
          message: error.message || '请检查 IAM、IoTDA Endpoint 或设备在线状态'
        }
      })

      wx.showToast({
        title: '开闸失败',
        icon: 'none'
      })
    })
  },
  syncConnectionText() {
    const connectionText = cloudApi.getConnectionText()
    this.setData({
      connectionText,
      connectionClass: connectionText === '华为云在线' ? 'empty' : 'pending'
    })
  }
})
