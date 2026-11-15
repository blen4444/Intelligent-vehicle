/*
 * Motor.h
 *  编码器对象与采样接口。电机速度由 control.c 算占空比, isr.c 用软件 PWM 输出,
 *  本文件不再包含硬件 PWM 电机驱动。
 */

#ifndef CODE_MOTOR_H_
#define CODE_MOTOR_H_

#include "zf_common_headfile.h"

typedef struct motor1
{
    int encoder_speed;   // 编码器滤波速度(每 5ms 计数)
    int encoder_raw;     // 编码器原始计数(每 5ms, 已乘方向系数)
} motor1;

extern struct motor1 motor_l;
extern struct motor1 motor_r;

void Encoder_Init(void);
void Encoder_Data_Get(void);

#endif /* CODE_MOTOR_H_ */
