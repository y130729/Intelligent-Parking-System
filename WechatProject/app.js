const cloudApi = require('./utils/cloudApi')
const config = require('./utils/config')
const realtime = require('./utils/realtime')

App({
  onLaunch() {
    const logs = wx.getStorageSync('logs') || []
    logs.unshift(Date.now())
    wx.setStorageSync('logs', logs.slice(0, 20))

    const savedUser = wx.getStorageSync('userInfo')
    if (savedUser) {
      this.globalData.userInfo = savedUser
    }

    if (cloudApi.isUsingMock()) {
      realtime.init()
    }

    if (config.enableWechatLogin) {
      wx.login({
        timeout: 5000,
        success: res => {
          this.globalData.loginCode = res.code
          cloudApi.login(res.code).then(session => {
            this.globalData.userSession = session
          }).catch(() => {
            this.globalData.userSession = null
          })
        },
        fail: () => {
          this.globalData.userSession = null
        }
      })
    } else {
      this.globalData.userSession = {
        token: cloudApi.isUsingMock() ? 'mock-token' : '',
        role: 'admin'
      }
    }
  },
  globalData: {
    loginCode: '',
    userInfo: null,
    userSession: null
  }
})
