#include "cc2530_parser.h"
#include "project_config.h"
#include "state_machine.h"
#include "mqtt_report.h"
#include "USART.h"
#include <string.h>

#define RX_BUF_SIZE 32

static uint8_t rx_buf[RX_BUF_SIZE];
static uint8_t rx_len = 0;
static uint8_t parking_spots[MAX_SPOT_ID] = {0};

void CC2530_Init(void)
{
    rx_len = 0;
    memset(parking_spots, 0, sizeof(parking_spots));
}

static void UpdateRemainingSpots(void)
{
    uint8_t count = 0;
    for (int i = 0; i < MAX_SPOT_ID; i++)
    {
        if (parking_spots[i] == 0)
        {
            count++;
        }
    }
    StateMachine_UpdateRemainingSpots(count);
}

void CC2530_ParseFrame(uint8_t *data, uint16_t length)
{
    if (length < CC2530_FRAME_MIN_LEN) return;
    if (data[0] != CC2530_FRAME_HEADER) return;
    if (data[length-1] != CC2530_FRAME_TAIL) return;
    
    uint8_t xor_check = 0;
    for (int i = 1; i < length-2; i++)
    {
        xor_check ^= data[i];
    }
    
    if (xor_check != data[length-2])
    {
        USART_Printf("CC2530 checksum error\r\n");
        return;
    }
    
    uint8_t spot_id = data[1];
    uint8_t spot_state = data[2];
    
    if (spot_id > 0 && spot_id <= MAX_SPOT_ID)
    {
        // 只有状态变化时才上报
        if (parking_spots[spot_id - 1] != spot_state)
        {
            parking_spots[spot_id - 1] = spot_state;
            UpdateRemainingSpots();
            USART_Printf("Spot %d state changed to %d\r\n", spot_id, spot_state);
            // 上报单个车位状态到华为云
            MQTT_ReportSpotStatus(spot_id, spot_state);
        }
    }
}

void CC2530_HandleByte(uint8_t byte)
{
    if (rx_len < RX_BUF_SIZE - 1)
    {
        rx_buf[rx_len++] = byte;
        
        if (byte == CC2530_FRAME_TAIL)
        {
            CC2530_ParseFrame(rx_buf, rx_len);
            rx_len = 0;
        }
    }
    else
    {
        rx_len = 0;
    }
}