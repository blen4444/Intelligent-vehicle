/*
 * wifi_image.c
 *  高速 WIFI6B21-SPI 图传 + 逐飞助手 V2 上位机发送实现
 */

#include "zf_common_headfile.h"
#include "wifi_image.h"
#include "vision_runtime.h"

#pragma section all "cpu0_dsram"
volatile uint8 wifi_image_ready = 0;

#if WIFI_IMAGE_ENABLE
// 发送专用快照缓冲(放 CPU0 DSRAM, 避免占 CPU1 内存): 防止发送期间与 CPU1 写冲突
static uint8 wifi_img_buf[MT9V03X_H][MT9V03X_W];
static seekfree_assistant_camera_boundary_uint8_struct wifi_bound_l[MT9V03X_H];
static seekfree_assistant_camera_boundary_uint8_struct wifi_bound_m[MT9V03X_H];
static seekfree_assistant_camera_boundary_uint8_struct wifi_bound_r[MT9V03X_H];

static seekfree_assistant_camera_struct          wifi_camera_info;
static seekfree_assistant_camera_boundary_struct wifi_bound_l_info;
static seekfree_assistant_camera_boundary_struct wifi_bound_m_info;
static seekfree_assistant_camera_boundary_struct wifi_bound_r_info;

#if WIFI_IMAGE_SEND_OSCILLOSCOPE
#define WIFI_OSC_CHANNEL_NUM    5
static seekfree_assistant_oscilloscope_struct    wifi_osc_info;
static float wifi_osc_buf[WIFI_OSC_CHANNEL_NUM];
#endif

static uint32 wifi_last_send_tick = 0;
static uint32 wifi_last_frame_cnt = 0;
#endif
#pragma section all restore

#if WIFI_IMAGE_ENABLE

static void wifi_prepare_frame(void)
{
    uint16 y;

#if WIFI_IMAGE_SEND_BINARY
    memcpy(wifi_img_buf[0], image[0], MT9V03X_IMAGE_SIZE);
#else
    memcpy(wifi_img_buf[0], base_image[0], MT9V03X_IMAGE_SIZE);
#endif

    for(y = 0; y < MT9V03X_H; y++)
    {
        uint8 l = left_line_list[y];
        uint8 m = mid_line_list[y];
        uint8 r = right_line_list[y];

        // 逐飞助手 V2 边线使用 (x, y) 坐标对, 出屏标记 EDGE_OFFSCREEN(0xFF) 钳到图像边缘
        wifi_bound_l[y].x = (l < MT9V03X_W) ? l : 0u;
        wifi_bound_l[y].y = (uint8)y;

        wifi_bound_m[y].x = (m < MT9V03X_W) ? m : (uint8)(MT9V03X_W / 2);
        wifi_bound_m[y].y = (uint8)y;

        wifi_bound_r[y].x = (r < MT9V03X_W) ? r : (uint8)(MT9V03X_W - 1);
        wifi_bound_r[y].y = (uint8)y;
    }
}

uint8 wifi_image_init(void)
{
    uint8 retry;
    uint8 state = 1;

    wifi_image_ready = 0;

    // 1. 初始化 WIFI6B21-SPI 并连接热点(最多试 2 次, 失败不阻塞跑车)
    for(retry = 0; retry < 2; retry++)
    {
        state = wifi_spi_init(WIFI_IMAGE_SSID, WIFI_IMAGE_PASSWORD);
        if(0 == state)
        {
            break;
        }
        system_delay_ms(200);
    }
    if(0 != state)
    {
        return 1;
    }

    // 2. 建立 Socket 连接(若 zf_device_wifi_spi.h 中 WIFI_SPI_AUTO_CONNECT == 0 则由这里连接)
#if (0 == WIFI_SPI_AUTO_CONNECT)
    for(retry = 0; retry < 2; retry++)
    {
#if WIFI_IMAGE_USE_UDP
        state = wifi_spi_socket_connect("UDP", WIFI_IMAGE_TARGET_IP, WIFI_IMAGE_TARGET_PORT, WIFI_IMAGE_LOCAL_PORT);
#else
        state = wifi_spi_socket_connect("TCP", WIFI_IMAGE_TARGET_IP, WIFI_IMAGE_TARGET_PORT, WIFI_IMAGE_LOCAL_PORT);
#endif
        if(0 == state)
        {
            break;
        }
        system_delay_ms(200);
    }
    if(0 != state)
    {
        return 2;
    }
#endif

    // 3. 绑定逐飞助手 V2 传输层为高速 WIFI-SPI
    seekfree_assistant_interface_init(SEEKFREE_ASSISTANT_WIFI_SPI);

    // 4. 配置图像信息(总钻风 188x120 灰度格式; 二值图 0/255 同样用 MT9V03X 灰度格式显示)
    seekfree_assistant_camera_config(
        &wifi_camera_info,
        SEEKFREE_ASSISTANT_CAMERA_TYPE_MT9V03X,
        MT9V03X_W,
        MT9V03X_H,
        wifi_img_buf[0]);

    // 5. 配置三条边线(左蓝 / 中红 / 右绿, 每条 120 个 (x, y) 点)
    seekfree_assistant_camera_boundary_config(
        &wifi_bound_l_info,
        SEEKFREE_ASSISTANT_DATA_TYPE_UINT8,
        RGB565_BLUE,
        MT9V03X_H,
        wifi_bound_l);
    seekfree_assistant_camera_boundary_config(
        &wifi_bound_m_info,
        SEEKFREE_ASSISTANT_DATA_TYPE_UINT8,
        RGB565_RED,
        MT9V03X_H,
        wifi_bound_m);
    seekfree_assistant_camera_boundary_config(
        &wifi_bound_r_info,
        SEEKFREE_ASSISTANT_DATA_TYPE_UINT8,
        RGB565_GREEN,
        MT9V03X_H,
        wifi_bound_r);

#if WIFI_IMAGE_SEND_OSCILLOSCOPE
    seekfree_assistant_oscilloscope_config(&wifi_osc_info, WIFI_OSC_CHANNEL_NUM, wifi_osc_buf);
#endif

    wifi_image_ready = 1;
    return 0;
}

void wifi_image_send_task(uint32 now_tick, uint8 running)
{
    uint32 period;
    uint32 frame_cnt;

    if(!wifi_image_ready)
    {
        return;
    }

    frame_cnt = vision_processed_count;
    if(frame_cnt == 0u || frame_cnt == wifi_last_frame_cnt)
    {
        return;
    }

    period = running ? WIFI_IMAGE_RUN_TICKS : WIFI_IMAGE_STOP_TICKS;
    if((uint32)(now_tick - wifi_last_send_tick) < period)
    {
        return;
    }

    wifi_last_send_tick = now_tick;
    wifi_last_frame_cnt = frame_cnt;

    wifi_prepare_frame();

    // V2 协议要求: 先发送边线, 最后发送图像, 上位机才能正确将边线叠加到本帧图像上
    seekfree_assistant_camera_boundary_send(&wifi_bound_l_info);
    seekfree_assistant_camera_boundary_send(&wifi_bound_m_info);
    seekfree_assistant_camera_boundary_send(&wifi_bound_r_info);
    seekfree_assistant_camera_send(&wifi_camera_info);

#if WIFI_IMAGE_SEND_OSCILLOSCOPE
    wifi_osc_buf[0] = final_mid_line;
    wifi_osc_buf[1] = (float)zt_state;
    wifi_osc_buf[2] = (float)img_threshold;
    wifi_osc_buf[3] = (float)enc_cnt_L;
    wifi_osc_buf[4] = (float)enc_cnt_R;
    seekfree_assistant_oscilloscope_send(&wifi_osc_info);
#endif

#if WIFI_IMAGE_USE_UDP
    wifi_spi_udp_send_now();
#endif
}

#else

uint8 wifi_image_init(void)
{
    wifi_image_ready = 0;
    return 0;
}

void wifi_image_send_task(uint32 now_tick, uint8 running)
{
    (void)now_tick;
    (void)running;
}

#endif
