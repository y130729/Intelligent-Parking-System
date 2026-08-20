const config = {
  useMock: false,
  enableWechatLogin: true,
  fallbackToMockWhenCloudIncomplete: true,
  parking: {
    name: '广东工业大学智慧停车场',
    address: '广东工业大学大学城校区西门停车区',
    feeRule: '首小时免费，之后 3 元/小时，24 小时封顶 30 元',
    totalSpots: 50
  },
  huaweiCloud: {
    region: 'cn-south-1',
    iamEndpoint: 'https://iam.cn-south-1.myhuaweicloud.com',
    iotdaEndpoint: 'https://21210fb4d9.st1.iotda-app.cn-south-1.myhuaweicloud.com',
    projectId: '019e1ae75fe870118dd0f8e45abc6472',
    domainName: 'GT-Blisher',
    iamUserName: 'Blisher',
    iamPassword: 'Yzh781245369',
    deviceId: '6a02cb70c00ccb6d4b5364bb_HW_MQTT',
    instanceId: '',
    requestTimeout: 25000,
    pollInterval: 3000,
    tokenCacheKey: 'huaweiCloudIamToken',
    tokenExpireBuffer: 30 * 60 * 1000,
    shadow: {
      serviceId: 'Spot_state',
      spotIdProperty: 'Parking_id',
      spotStateProperty: 'Parking_state',
      cardIdProperty: 'card_id',
      plateNoProperty: 'plate_no',
      spotsProperty: 'spots',
      totalProperty: 'total_spots',
      remainingProperty: 'remaining_spots',
      updatedAtProperty: 'updated_at',
      statusMap: {
        empty: 'empty',
        '空闲': 'empty',
        '空': 'empty',
        '未占用': 'empty',
        idle: 'empty',
        free: 'empty',
        available: 'empty',
        '0': 'empty',
        false: 'empty',
        occupied: 'occupied',
        '占用': 'occupied',
        '已占用': 'occupied',
        busy: 'occupied',
        used: 'occupied',
        '1': 'occupied',
        true: 'occupied'
      }
    },
    command: {
      serviceId: 'ParkingBarrier',
      commandName: 'open_barrier',
      paras: {
        value: 'open'
      }
    }
  }
}

module.exports = config
