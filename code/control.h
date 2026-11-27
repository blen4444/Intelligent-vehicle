/*
 * control.h
 *  控制层: 舵机打角、差速/弯道减速、编码器速度闭环、电机引脚
 *  参数默认值在下面的 *_DEF 宏里, 运行中可长按 KEY_1 进屏幕第 3 页在线改(只改 RAM)。
 */

#ifndef CODE_CONTROL_H_
#define CODE_CONTROL_H_

#include "zf_common_headfile.h"

//====================== 转向参数 ======================
#define SPEED_K_GAIN_DEF          0.15f   // 速度增益: 越快打角越多(0.15 -> 满速约 1.75 倍)
#define STEER_CENTER_RATIO_DEF    0.55f   // 小偏差(直道)保留的增益比例, 越小越稳
#define STEER_KNEE_ERR_DEF        25.0f   // 偏差超过多少像素才恢复 100% 增益
#define STEER_FF_K_DEF            0.85f   // 曲率前馈: 远处每偏 1 像素预打的舵量(修复单侧出屏前馈后适当下调, 防切内线)
#define STEER_FF_DEAD_DEF         6.0f    // 前馈死区(像素), 直道 far-near 抖动不预打
#define STEER_FF_GAIN_SPD_DEF     0.25f   // 前馈速度缩放: 400档=0.25, 1600档=1.0(封顶), 低速提前打弯就调小
#define STEER_KD_DEF              1.50f   // 误差变化率阻尼系数
#define STEER_D_MAX               20.0f   // 阻尼项单帧限幅
#define STEER_SLEW_DEF            20      // 每 5ms 舵量最大变化(格)
#define CURVE_SLOW_START          6       // 偏差超过多少像素开始弯道减速
#define CURVE_SLOW_GAIN_DEF       2       // 偏差每多 1 像素, 多减百分之几
#define CURVE_SLOW_MAX_DEF        70      // 弯道减速上限(百分比)
#define CURVE_SLOW_MIN            350     // 减速后最低占空比, 防止弯中直接停住

extern float OPEN_TURN_K;               // 开环转向系数
extern float SPEED_K_GAIN;              // 转向速度增益
extern float STEER_CENTER_RATIO;        // 小偏差时的中心软化比例
extern float STEER_KNEE_ERR;            // 偏差膝点(像素)
extern float STEER_FF_DEAD;             // 曲率前馈死区
extern float STEER_FF_K;                // 曲率前馈系数
extern float STEER_FF_GAIN_SPD;         // 前馈速度缩放(低速压低前馈)
extern float STEER_KD;                  // 阻尼系数
extern int   STEER_SLEW;                // 舵量变化率限制
extern int   CURVE_SLOW_GAIN;           // 弯道减速增益
extern int   CURVE_SLOW_MAX;            // 弯道减速上限
void control_param_defaults(void);      // 恢复全部参数默认值
int  turn_last_value(void);             // 当前舵量(相对中值), 屏幕显示用

//====================== 舵机 ======================
#define turn_left_duoji_limit   100     // 左打角量程(相对中值)
#define turn_right_duoji_limit  100     // 右打角量程(相对中值)
#define DUOJI_MID_DEF           725     // 舵机中值(50Hz: 725 -> 1.45ms)
#define duoji_jiao              ATOM1_CH1_P33_9
extern int duoji_mid;                   // 舵机中值, 菜单 SV_MID 可调

//====================== 巡线 / 差速 ======================
#define OPEN_TURN_K_DEF           0.9f    // 开环转向系数: 中线偏差 1 像素 -> 0.9 格舵
#define DIFF_K                    8       // 差速系数: 中线偏差 1 像素 -> 8 格占空比差(设 0 = 关差速)
#define VISION_TIMEOUT            100     // 100 * 5ms = 0.5s 收不到图就判视觉失效

//====================== 编码器 / 速度闭环 ======================
// 左编码器 TIM5 -> P10_3 / P10_1, 右编码器 TIM6 -> P20_3 / P20_0
// 架起车向前推, 第 2 页看 cnt L / R: 都应为正; 某一路为负就把对应的 ENC_x_SIGN 改成 -1
#define ENC_L_SIGN              1       // 左编码器计数符号
#define ENC_R_SIGN              1       // 右编码器计数符号

#define SPEED_LOOP_DEFAULT_ON   1       // 0=上电开环(边跑边学 k), 1=上电闭环; KEY_4 长按随时切换
#define SPEED_KP                0.60f   // 增量式 P: 每 1 计数误差 -> 占空比修正
#define SPEED_KI                0.80f   // 增量式 I: 每 1 计数误差 -> 占空比修正
#define SPEED_TRIM_LIMIT        2500    // 闭环修正量限幅(约 ±25% 占空比)
#define SPEED_STALL_TICKS       100     // 有指令却 0.5s 无计数 -> 判编码器异常, 自动退回开环
#define ENC_K_DEFAULT           200     // 满占空比时每 5ms 的计数初值(开环时自动学习)
#define ENC_K_MIN               20      // 学习值下限
#define ENC_K_MAX               4000    // 学习值上限

//====================== 电机引脚 ======================
// P21_3 / P21_5 = 速度输入(软件 PWM, 高电平有效, 200Hz)
// P21_2 / P21_4 = 方向输入(1=正转 0=倒车)
#define MotorL_speed_pin    P21_3
#define MotorR_speed_pin    P21_5
#define MotorL_dir_pin      P21_2
#define MotorR_dir_pin      P21_4

extern uint8 start;
#define SPEED_LEVEL_NUM 6
extern int speed_level;
extern int base_speed;
extern int motor_duty_L;
extern int motor_duty_R;
extern int speed_table[SPEED_LEVEL_NUM];
extern vuint32 last_frame_tick;
extern vuint32 sys_tick;

// 速度闭环状态(中断写, 主循环只读)
extern volatile uint8 speed_loop_on;    // 1=速度闭环
extern volatile uint8 speed_loop_fault; // 1=编码器异常, 已退回开环
extern volatile int   enc_cnt_L;        // 每 5ms 计数
extern volatile int   enc_cnt_R;
extern volatile int   enc_k_L;          // 学习的"满占空比计数"
extern volatile int   enc_k_R;
extern volatile int   speed_tgt_L;      // 目标计数
extern volatile int   speed_tgt_R;
extern volatile uint8 status_page;      // 屏幕页: 0=视觉 1=编码器 2=调参菜单

#define SW_PWM_STEPS 50                 // 软件 PWM 50 档, 100us 一档 -> 200Hz, 分辨率 2%
extern volatile uint8 sw_duty_L;        // 左轮占空比 0~SW_PWM_STEPS
extern volatile uint8 sw_duty_R;        // 右轮占空比 0~SW_PWM_STEPS
extern uint8          sw_phase;         // 软件 PWM 相位, 只在 CCU61_CH0 中断里改
extern volatile uint8 motor_pol;        // 行驶方向: 1=正转(默认) 0=倒车
extern volatile uint32 sw_isr_cnt;      // 软件 PWM 中断计数(调试用)
extern volatile uint16 sw_isr_rate;     // 每 5ms 的软件 PWM 中断次数(正常 50)

// 速度指令单位是 0..10000 占空比, 不依赖编码器
#define DRIVE_ACCEL_STEP        40      // 每 5ms 最大加速量(约 0.3s 到满速)
#define CURVE_SLOW_GEAR_BASE    400     // 减速上限随档位缩放的基准档位
#define CURVE_SLOW_GEAR_SPAN    1200    // 缩放跨度(基准档 + 跨度 -> 满额减速)
extern volatile int drive_speed_cmd;
extern volatile int control_near_offset_dbg;
extern volatile int control_bend_dbg;

void control_publish_vision(float mid, const uint8 *mid_rows,
                            const uint8 *left_rows, const uint8 *right_rows);
void control_drive_update(uint8 enabled, uint8 vision_ok);

void turn(float mid, float k);

#endif /* CODE_CONTROL_H_ */
