/*
 * wifi_image.h
 *  高速 WIFI6B21-SPI 图传 + 逐飞助手上位机接口
 *  只运行在 CPU0(core0_main), 不阻塞 CPU1 的 50fps 实时视觉与控制。
 *
 *  硬件默认引脚(详见 libraries/zf_device/zf_device_wifi_spi.h, 与 IPS200 SPI2 无冲突):
 *    SPI_3: SCK=P22_3  MOSI=P22_0  MISO=P22_1  CS=P22_2  INT=P15_8  RST=P23_1  5V  GND
 */

#ifndef CODE_WIFI_IMAGE_H_
#define CODE_WIFI_IMAGE_H_

#include "zf_common_typedef.h"

//====================== 图传总开关与网络配置(按实际热点/电脑IP修改) ======================
#define WIFI_IMAGE_ENABLE               1                   // 1: 开启图传  0: 关闭图传(不初始化模块)
#define WIFI_IMAGE_SSID                 "676767"          // 目标 WiFi 名称(电脑热点或路由器 SSID)
#define WIFI_IMAGE_PASSWORD             "gw2005050411"       // 目标 WiFi 密码(无密码填 NULL)

#define WIFI_IMAGE_USE_UDP              0                   // 0: TCP 模式(推荐, 对应逐飞助手 TCP Server)  1: UDP 模式
#define WIFI_IMAGE_TARGET_IP            "10.255.209.80"     // 电脑 IP(Win10/11 移动热点默认 192.168.137.1)
#define WIFI_IMAGE_TARGET_PORT          "8086"              // 逐飞助手监听端口(默认 8086)
#define WIFI_IMAGE_LOCAL_PORT           "6666"              // 模块本地端口

//====================== 图传内容与频率配置 ======================
#define WIFI_IMAGE_SEND_BINARY          1                   // 1: 发二值化图 image[][]  0: 发原始灰度图 base_image[][]
#define WIFI_IMAGE_SEND_OSCILLOSCOPE    1                   // 1: 同步发虚拟示波器数据(中线/状态/阈值/编码器)

// sys_tick 为 5ms 一拍: 停车 10 ticks = 50ms (20Hz), 跑动 20 ticks = 100ms (10Hz)
#define WIFI_IMAGE_STOP_TICKS           10u
#define WIFI_IMAGE_RUN_TICKS            20u

// 0: 未启用或失败  1: WiFi+Socket 已连接就绪
extern volatile uint8 wifi_image_ready;

// 初始化 WIFI6B21-SPI 与逐飞助手(失败不会死循环卡死, 返回 0=成功 非0=失败)
uint8 wifi_image_init(void);

// 周期图传任务(在 core0_main 主循环调用, 内部按节拍且有新帧时才发送)
void  wifi_image_send_task(uint32 now_tick, uint8 running);

#endif /* CODE_WIFI_IMAGE_H_ */
