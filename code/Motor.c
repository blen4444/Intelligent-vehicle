/*
 * Motor.c
 *  只负责两路编码器的初始化与 5ms 采样。
 */

#include "Motor.h"

// 全局电机对象实例化(只用到编码器字段)
motor1 motor_l;
motor1 motor_r;

/*********************************************************************************************************
函数功能: 编码器初始化
*********************************************************************************************************/
void Encoder_Init(void)
{
    // 左编码器
    encoder_dir_init(TIM5_ENCODER, TIM5_ENCODER_CH1_P10_3, TIM5_ENCODER_CH2_P10_1);
    // 右编码器
    encoder_dir_init(TIM6_ENCODER, TIM6_ENCODER_CH1_P20_3, TIM6_ENCODER_CH2_P20_0);
}

/*********************************************************************************************************
函数功能: 编码器数值获取与滤波(每 5ms 调用一次)
*********************************************************************************************************/
void Encoder_Data_Get(void)
{
    // 先取计数再清零, 差值就是这 5ms 的脉冲数
    motor_l.encoder_raw = (int)encoder_get_count(TIM5_ENCODER) * ENC_L_SIGN;
    motor_r.encoder_raw = (int)encoder_get_count(TIM6_ENCODER) * ENC_R_SIGN;
    encoder_clear_count(TIM5_ENCODER);
    encoder_clear_count(TIM6_ENCODER);

    // 一阶低通(4 拍 ≈ 20ms), 压掉 ±1 计数的抖动, 又不会明显滞后
    motor_l.encoder_speed = (motor_l.encoder_speed * 3 + motor_l.encoder_raw) / 4;
    motor_r.encoder_speed = (motor_r.encoder_speed * 3 + motor_r.encoder_raw) / 4;
}
