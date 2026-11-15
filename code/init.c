/*
 * init.c
 *
 *  Created on: 2026年1月12日
 *      Author: DELL
 */

#include "init.h"

uint8 camera_init_state = 0;   // 摄像头初始化结果:0=成功 1=失败

void init(void)
{
    //舵机初始化
    pwm_init(duoji_jiao, 50, duoji_mid);

    //==== 电机接口 (实测) ====
    // P21_3 / P21_5 = 速度输入 (软件PWM, 高电平有效)
    // P21_2 / P21_4 = 方向输入 (1=正转 0=倒车)
    gpio_init(MotorL_speed_pin, GPO, 0, GPO_PUSH_PULL);   // 上电停转
    gpio_init(MotorR_speed_pin, GPO, 0, GPO_PUSH_PULL);
    gpio_init(MotorL_dir_pin,   GPO, 0, GPO_PUSH_PULL);
    gpio_init(MotorR_dir_pin,   GPO, 1, GPO_PUSH_PULL);
    gpio_set_level(MotorL_speed_pin, 0);
    gpio_set_level(MotorR_speed_pin, 0);
    gpio_set_level(MotorL_dir_pin, 1);                    // 上电默认正转
    gpio_set_level(MotorR_dir_pin, 1);

    //编码器初始化 (左 TIM5 P10_3/P10_1, 右 TIM6 P20_3/P20_0)
    Encoder_Init();

    //按键初始化
    key_init(5);

    //定时器中断初始化
    pit_ms_init(CCU60_CH0, 5);

    // 软件PWM定时中断: 100us 一步, 50 步 = 200Hz (分辨率 2%)
    pit_us_init(CCU61_CH0, 100);

    camera_init_state = mt9v03x_init();   // 0=成功 1=失败
}
