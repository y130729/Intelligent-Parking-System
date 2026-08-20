const cloudApi = require('../../utils/cloudApi')

const defaultAvatarUrl = '/assets/tab/mine-active.png'

Page({
  data: {
    userInfo: {
      avatarUrl: defaultAvatarUrl,
      nickName: ''
    },
    vehicle: {
      plate_no: '',
      card_id: '',
      parking_status: 'outside',
      bind_type: 'wechat',
      notify_type: 'wechat',
      notify_enabled: true,
      wechat_nickName: '',
      wechat_avatarUrl: ''
    },
    feedback: '',
    wechatBindText: '当前微信用户',
    parkingStatusText: '离场',
    noticeStatusText: '已启用',
    noticeStatusClass: 'notify-status enabled',
    canIUseNicknameComp: wx.canIUse('input.type.nickname'),
    savingVehicle: false,
    submittingFeedback: false
  },
  onLoad() {
    const app = getApp()
    const savedUser = wx.getStorageSync('userInfo') || app.globalData.userInfo
    if (savedUser) {
      this.setData({ userInfo: savedUser })
    }

    cloudApi.getVehicle().then(vehicle => {
      this.setVehicleState(vehicle)
    })
  },
  onChooseAvatar(e) {
    this.setData({
      'userInfo.avatarUrl': e.detail.avatarUrl
    })
  },
  onNicknameInput(e) {
    this.setData({
      'userInfo.nickName': e.detail.value
    })
  },
  saveProfile() {
    const userInfo = this.data.userInfo
    if (!userInfo.nickName) {
      wx.showToast({
        title: '请输入昵称',
        icon: 'none'
      })
      return
    }

    wx.setStorageSync('userInfo', userInfo)
    getApp().globalData.userInfo = userInfo
    wx.showToast({
      title: '资料已保存',
      icon: 'success'
    })
  },
  onPlateInput(e) {
    this.setData({
      'vehicle.plate_no': e.detail.value.toUpperCase()
    })
  },
  onCardInput(e) {
    this.setData({
      'vehicle.card_id': e.detail.value.trim().toUpperCase()
    })
  },
  saveVehicle() {
    const userInfo = this.data.userInfo || {}
    const currentVehicle = this.data.vehicle || {}
    const vehicle = Object.assign({}, currentVehicle, {
      bind_type: 'wechat',
      notify_type: 'wechat',
      notify_enabled: true,
      parking_status: currentVehicle.parking_status || 'outside',
      wechat_nickName: userInfo.nickName || '微信用户',
      wechat_avatarUrl: userInfo.avatarUrl || defaultAvatarUrl
    })
    if (!vehicle.plate_no || vehicle.plate_no.length < 7) {
      wx.showToast({
        title: '请输入完整车牌',
        icon: 'none'
      })
      return
    }
    if (!vehicle.card_id) {
      wx.showToast({
        title: '请输入卡号',
        icon: 'none'
      })
      return
    }

    this.setData({ savingVehicle: true })
    cloudApi.saveVehicle(vehicle).then(nextVehicle => {
      this.setVehicleState(nextVehicle)
      wx.showToast({
        title: '车辆已绑定',
        icon: 'success'
      })
    }).catch(error => {
      wx.showToast({
        title: error.message || '绑定失败',
        icon: 'none'
      })
    }).finally(() => {
      this.setData({ savingVehicle: false })
    })
  },
  onFeedbackInput(e) {
    this.setData({
      feedback: e.detail.value
    })
  },
  submitFeedback() {
    const content = this.data.feedback.trim()
    if (!content) {
      wx.showToast({
        title: '请填写反馈内容',
        icon: 'none'
      })
      return
    }

    this.setData({ submittingFeedback: true })
    cloudApi.submitFeedback({
      content,
      plate_no: this.data.vehicle.plate_no
    }).then(() => {
      this.setData({ feedback: '' })
      wx.showToast({
        title: '反馈已提交',
        icon: 'success'
      })
    }).finally(() => {
      this.setData({ submittingFeedback: false })
    })
  },
  setVehicleState(vehicle) {
    const nextVehicle = Object.assign({
      plate_no: '',
      card_id: '',
      parking_status: 'outside',
      bind_type: 'wechat',
      notify_type: 'wechat',
      notify_enabled: true,
      wechat_nickName: '',
      wechat_avatarUrl: ''
    }, vehicle)
    delete nextVehicle.phone

    const userInfo = this.data.userInfo || {}
    const nickName = nextVehicle.wechat_nickName || userInfo.nickName
    this.setData({
      vehicle: nextVehicle,
      wechatBindText: nickName ? `绑定到 ${nickName}` : '绑定到当前微信用户',
      parkingStatusText: nextVehicle.parking_status === 'inside' ? '入场' : '离场',
      noticeStatusText: nextVehicle.notify_enabled ? '已启用' : '未启用',
      noticeStatusClass: nextVehicle.notify_enabled ? 'notify-status enabled' : 'notify-status'
    })
  }
})
