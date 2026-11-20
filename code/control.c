/*
 * control.c
 *
 *  Created on: 2026年1月12日
 *      Author: DELL
 */

#include "control.h"

uint8  start = 0;
vuint32 sys_tick = 0;

int  speed_level  = 0;                  // 档位索引: 0 -> 400 (4% 动力, 慢跑)
int  base_speed   = 400;                // 基础动力 0~10000
int  motor_duty_L = 0;
int  motor_duty_R = 0;
vuint32 last_frame_tick = 0;            // 最近一次处理完图像帧时的 sys_tick

// 动力 4% 8% 12% 16% 20% 24%
int  speed_table[SPEED_LEVEL_NUM] = {400, 800, 1200, 1600, 2000, 2400};

/* ====================== 运行时可调参数(屏幕菜单第 3 页) ======================
 * 这些量原来是 control.h 里的宏, 为了让屏幕菜单能在不重新烧录的情况下改参数, 改成变量。
 * 默认值仍在 control.h 的 *_DEF 宏里, 想改上电初值就改那里(菜单只改 RAM, 掉电恢复默认)。
 * 写: 主循环(按键)  读: 5ms 中断。32 位对齐的 int/float 存取是单条指令, 不需要关中断。 */
float OPEN_TURN_K        = OPEN_TURN_K_DEF;
float SPEED_K_GAIN       = SPEED_K_GAIN_DEF;
float STEER_CENTER_RATIO = STEER_CENTER_RATIO_DEF;
float STEER_KNEE_ERR     = STEER_KNEE_ERR_DEF;
float STEER_FF_K         = STEER_FF_K_DEF;
float STEER_FF_DEAD      = STEER_FF_DEAD_DEF;
float STEER_FF_GAIN_SPD  = STEER_FF_GAIN_SPD_DEF;
float STEER_KD           = STEER_KD_DEF;
int   STEER_SLEW         = STEER_SLEW_DEF;
int   duoji_mid          = DUOJI_MID_DEF;
int   CURVE_SLOW_GAIN    = CURVE_SLOW_GAIN_DEF;
int   CURVE_SLOW_MAX     = CURVE_SLOW_MAX_DEF;

void control_param_defaults(void)
{
    OPEN_TURN_K        = OPEN_TURN_K_DEF;
    SPEED_K_GAIN       = SPEED_K_GAIN_DEF;
    STEER_CENTER_RATIO = STEER_CENTER_RATIO_DEF;
    STEER_KNEE_ERR     = STEER_KNEE_ERR_DEF;
    STEER_FF_K         = STEER_FF_K_DEF;
    STEER_FF_DEAD      = STEER_FF_DEAD_DEF;
    STEER_FF_GAIN_SPD  = STEER_FF_GAIN_SPD_DEF;
    STEER_KD           = STEER_KD_DEF;
    STEER_SLEW         = STEER_SLEW_DEF;
    duoji_mid          = DUOJI_MID_DEF;
    CURVE_SLOW_GAIN    = CURVE_SLOW_GAIN_DEF;
    CURVE_SLOW_MAX     = CURVE_SLOW_MAX_DEF;
}

volatile uint8  sw_duty_L = 0;
volatile uint8  sw_duty_R = 0;
uint8           sw_phase  = 0;
volatile uint8  motor_pol = 1;          // 1=正转(默认) 0=倒车
volatile uint32 sw_isr_cnt  = 0;
volatile uint16 sw_isr_rate = 0;

/* Snapshot publication is single-producer (main) / single-consumer (5ms ISR)
 * on CPU0. Fill the inactive slot before atomically publishing its index. */
typedef struct
{
    float mid;
    float near_mid;
    float far_mid;
    uint8 geometry_valid;
    float error_delta;
} control_vision_frame;

static volatile control_vision_frame control_frames[2] =
    {{MID_W, MID_W, MID_W, 0, 0}, {MID_W, MID_W, MID_W, 0, 0}};
static volatile uint8 control_frame_index;
volatile int drive_speed_cmd = 0;    // PWM command, NOT measured wheel speed
volatile int control_near_offset_dbg = 0;
volatile int control_bend_dbg = 0;

/* ==================== 编码器速度闭环 ====================
 * control_drive_update() 先按视觉算出前馈占空比 ff(原有逻辑不动),
 * 闭环开启时最终输出 = ff + PI(目标计数 - 实测计数)。
 * 目标计数 = ff * k / 10000, k 是"满占空比时每 5ms 的计数";
 * 开环跑的时候每轮自动学习自己的 k, 所以不需要手工标定,
 * 顺带把左右电机的差异也一起吸收掉了。
 */
volatile uint8 speed_loop_on    = SPEED_LOOP_DEFAULT_ON;
volatile uint8 speed_loop_fault = 0;
volatile int   enc_cnt_L = 0;
volatile int   enc_cnt_R = 0;
volatile int   enc_k_L   = ENC_K_DEFAULT;
volatile int   enc_k_R   = ENC_K_DEFAULT;
volatile int32 enc_travel = 0;          // 编码器累计里程(环岛 FSM 计步)
volatile int   speed_tgt_L = 0;
volatile int   speed_tgt_R = 0;
volatile uint8 status_page = 0;

static float  spd_trim_L = 0.0f, spd_trim_R = 0.0f;    // 用 float 累加: 整数累加会有 1/Ki 左右的死区
static int    spd_last_err_L = 0, spd_last_err_R = 0;
static uint16 spd_stall_L = 0, spd_stall_R = 0;

// 开环时在线学习 k: k = 计数 * 10000 / 占空比
static void speed_learn_gain(int duty, int cnt, volatile int *k)
{
    int est;
    // 门限不能太高: 400 档(4% 占空比)是最常用的调车档位, 原来写 600 会一直学不到 k,
    // 闭环一开就用默认值 200 算目标, 判定"太快"-> 修正量顶到底 -> 车停 -> fault 退回开环,
    // 表现就是"加了编码器和没加一样"。
    if(duty < 300 || cnt < 5) return;                 // 占空比/计数太低时信噪比差, 不学
    est = cnt * 10000 / duty;
    if(est < ENC_K_MIN || est > ENC_K_MAX) return;    // 打滑/空转的野值直接丢
    *k = (*k * 7 + est) / 8;                          // 一阶滤波, 约 40 帧收敛
}

static void speed_loop_reset(void)
{
    spd_trim_L = 0.0f; spd_trim_R = 0.0f;
    spd_last_err_L = 0; spd_last_err_R = 0;
    spd_stall_L = 0; spd_stall_R = 0;
}

// 增量式 PI, 返回限幅后的修正量(累加量用 float, 只在这里取整)
static int speed_trim(int target, int cnt, float *trim, int *last_err)
{
    int err = target - cnt;
    *trim += SPEED_KP * (float)(err - *last_err) + SPEED_KI * (float)err;
    *last_err = err;
    if(*trim >  (float)SPEED_TRIM_LIMIT) *trim =  (float)SPEED_TRIM_LIMIT;
    if(*trim < -(float)SPEED_TRIM_LIMIT) *trim = -(float)SPEED_TRIM_LIMIT;
    return (int)*trim;
}

// 有指令却长时间没有计数 -> 编码器掉线/接线错, 宁可退回开环也不要让修正量一直顶满
static void speed_stall_watch(int target, int cnt, uint16 *ticks)
{
    if(target > 4 && cnt < 2)
    {
        if(++(*ticks) > SPEED_STALL_TICKS) speed_loop_fault = 1;
    }
    else *ticks = 0;
}

static uint8 control_band_mean(const uint8 *mid, const uint8 *left,
                               const uint8 *right, int first, int last, float *mean)
{
    int row, count = 0, sum = 0, run = 0, longest_run = 0;
    for(row = first; row <= last; row++)
    {
        int m = mid[row], l = left[row], r = right[row];
        /* 只拒绝 "这一行根本不是正常路面" 的样本。原来只要有一侧边线贴到
         * 图像边缘(l<=1 / r>=W-2)就把整行判废, 于是车沿外线斜着入弯时
         * near/far 两段全部无效 -> 曲率前馈和弯道减速同时归零 -> 打舵更晚,
         * 这正是 "拐弯过晚 / 一直走外线" 的根因。 */
        if(r - l < 8 || r - l > 170 || m < 3 || m > MT9V03X_W - 4 ||
           image[row][m] == 0) { run = 0; continue; }
        sum += m; count++; run++;
        if(run > longest_run) longest_run = run;
    }
    if(count < 6 || longest_run < 5) return 0;
    *mean = (float)sum / count;
    return 1;
}

void control_publish_vision(float mid, const uint8 *mid_rows,
                            const uint8 *left_rows, const uint8 *right_rows)
{
    static float previous_error = 0.0f, filtered_delta = 0.0f;
    static uint32 previous_tick = 0;
    uint32 elapsed = sys_tick - previous_tick;
    float error = MID_W - mid;
    uint8 next = (uint8)(control_frame_index ^ 1u);
    if(previous_tick && elapsed > 0 && elapsed <= 40)
        filtered_delta = 0.5f * filtered_delta + 0.5f * (error - previous_error) * 4.0f / elapsed;
    else filtered_delta = 0.0f;
    previous_tick = sys_tick; previous_error = error;
    control_frames[next].error_delta = Limit_float(-10.0f, filtered_delta, 10.0f);
    /* near/far 两段相对"实际追踪起点行"取, 不再写死 85~105 / 55~75:
     * 摄像头压低时底部整行全白, find_jidian 会把起点行往上抬(search_top_row < 109),
     * 写死行号会取到根本没跟踪的行 -> geometry_valid 恒 0 -> 曲率前馈与弯道减速失效。
     * 默认 search_top_row = 109 时等价于原来的 85~105 / 55~75。 */
    int top = (int)search_top_row;
    int near_first = top - 24, near_last = top - 4;
    int far_first  = top - 54, far_last  = top - 34;
    float near_mid = mid, far_mid = mid;
    uint8 near_ok = 0, far_ok = 0;
    if(near_first < (int)search_end_line) near_first = (int)search_end_line;
    if(far_first  < (int)search_end_line) far_first  = (int)search_end_line;
    if(near_last  > top) near_last = top;
    if(far_last   > top) far_last  = top;
    if(far_last  >= far_first)  far_ok  = control_band_mean(mid_rows, left_rows, right_rows, far_first,  far_last,  &far_mid);
    if(near_last >= near_first) near_ok = control_band_mean(mid_rows, left_rows, right_rows, near_first, near_last, &near_mid);
    control_frames[next].mid = mid;
    control_frames[next].near_mid = near_mid;
    control_frames[next].far_mid = far_mid;
    control_frames[next].geometry_valid = (uint8)(near_ok && far_ok);
    control_frame_index = next;
    last_frame_tick = sys_tick;      // publish freshness AFTER the complete result
}

/* 把 motor_duty_L/R 从"前馈量"改写成"最终输出量"; 必须在 drive 计算之后、ISR 取用之前调用
 * 注意: 这里按 motor_duty_L 配 enc_cnt_L、motor_duty_R 配 enc_cnt_R 做闭环。
 * 差速那一段的注释说明本车 P21_3(sw_duty_L) 实际驱动右轮, 所以启用闭环前
 * 先用第 2 页确认 TIM5(enc_cnt_L) 与 P21_3 是同一个轮子, 否则把下面两路配对对调。 */
static void speed_loop_apply(uint8 running)
{
    int ff_l, ff_r, dir, cnt_l, cnt_r;

    if(!running)
    {
        motor_duty_L = 0;
        motor_duty_R = 0;
        speed_tgt_L = 0;
        speed_tgt_R = 0;
        speed_loop_reset();
        return;
    }

    /* 倒车时(KEY_3 换向)车轮反转, 编码器计数变负; 这里统一折算成"沿指令方向为正",
     * 否则闭环会把负计数当成"太快了", 修正量一路顶到 -2500。 */
    dir   = motor_pol ? 1 : -1;
    cnt_l = enc_cnt_L * dir;
    cnt_r = enc_cnt_R * dir;

    ff_l = motor_duty_L;
    ff_r = motor_duty_R;
    speed_tgt_L = ff_l * enc_k_L / 10000;
    speed_tgt_R = ff_r * enc_k_R / 10000;

    if(!speed_loop_on)                        // 开环: 只学习, 不修正
    {
        speed_learn_gain(ff_l, cnt_l, &enc_k_L);
        speed_learn_gain(ff_r, cnt_r, &enc_k_R);
        speed_loop_reset();
        return;
    }
    if(speed_loop_fault)                      // 编码器异常: 保持前馈
    {
        speed_loop_reset();
        return;
    }

    speed_stall_watch(speed_tgt_L, cnt_l, &spd_stall_L);
    speed_stall_watch(speed_tgt_R, cnt_r, &spd_stall_R);
    if(speed_loop_fault)
    {
        speed_loop_reset();
        return;
    }

    motor_duty_L = Limit_int(0, ff_l + speed_trim(speed_tgt_L, cnt_l, &spd_trim_L, &spd_last_err_L), 5000);
    motor_duty_R = Limit_int(0, ff_r + speed_trim(speed_tgt_R, cnt_r, &spd_trim_R, &spd_last_err_R), 5000);
}

/* 沿用车上的电机接线与软件 PWM 通道。宽弯但近处正对的情况下不再像"整体偏移"那样
 * 猛减速; 几何量无效时退回按中线偏差减速的老规则。 */

void control_drive_update(uint8 enabled, uint8 vision_ok)
{
    uint8 slot = control_frame_index;
    float mid = control_frames[slot].mid;
    int requested = base_speed, diff = 0, reduce = 0, err;
    if(enabled && vision_ok)
    {
        if(control_frames[slot].geometry_valid)
        {
            int near_err = (int)(control_frames[slot].near_mid - MID_W);
            int bend = (int)(control_frames[slot].far_mid - control_frames[slot].near_mid);
            int bend_reduce;
            if(near_err < 0) near_err = -near_err;
            if(bend < 0) bend = -bend;
            control_near_offset_dbg = near_err;
            control_bend_dbg = bend;
            reduce = (near_err - CURVE_SLOW_START) * CURVE_SLOW_GAIN;
            bend_reduce = (bend - CURVE_SLOW_START) * CURVE_SLOW_GAIN;
            if(bend_reduce > reduce) reduce = bend_reduce;
        }
        else
        {
            err = (int)(MID_W - mid);
            if(err < 0) err = -err;
            control_near_offset_dbg = err;
            control_bend_dbg = 0;
            reduce = (err - CURVE_SLOW_START) * CURVE_SLOW_GAIN;
        }
        /* A fixed 70% cut is right for 1600 but far too much for 1200:
         * scale the ceiling with gear so low gears only lift off slightly,
         * while 1600 and above can still use the full reduction. */
        {
            int reduce_cap = CURVE_SLOW_MAX * (base_speed - CURVE_SLOW_GEAR_BASE) /
                             CURVE_SLOW_GEAR_SPAN;
            if(reduce_cap > CURVE_SLOW_MAX) reduce_cap = CURVE_SLOW_MAX;
            if(reduce_cap < 0) reduce_cap = 0;
            reduce = Limit_int(0, reduce, reduce_cap);
        }
        requested = base_speed * (100 - reduce) / 100;
        if(requested < CURVE_SLOW_MIN) requested = CURVE_SLOW_MIN;
        if(requested > drive_speed_cmd + DRIVE_ACCEL_STEP)
            drive_speed_cmd += DRIVE_ACCEL_STEP;
        else drive_speed_cmd = requested;   // reductions take effect immediately
        /* 差速方向(按实车实测修正):
         * err = MID_W - mid > 0 表示弯在左侧 -> 左轮是内圈必须减速, 右轮是外圈必须加速。
         * 原来"减速"写在 motor_duty_L 上, 实车表现却是外圈慢、内圈快,
         * 说明本车 P21_3(motor_duty_L) 实际驱动的是右轮, P21_5(motor_duty_R) 才是左轮。
         * 所以内圈减速要落在 motor_duty_R 上; 若以后重新接线又反了, 把下面两行的 +/- 对调即可。 */
        diff = Limit_int(-drive_speed_cmd / 2,
                         (int)(DIFF_K * (MID_W - mid)),
                         drive_speed_cmd / 2);
        motor_duty_L = Limit_int(0, drive_speed_cmd + diff, 5000);   // 右轮(外圈): 加速
        motor_duty_R = Limit_int(0, drive_speed_cmd - diff, 5000);   // 左轮(内圈): 减速
    }
    else
    {
        drive_speed_cmd = 0;
        motor_duty_L = 0;
        motor_duty_R = 0;
    }
    speed_loop_apply((uint8)(enabled && vision_ok));   // 编码器闭环叠加在这里
    if(vision_ok) turn(mid, OPEN_TURN_K);
    else turn(MID_W, 1.0f);
}

/**********************************************
函数功能：舵机开环转向
函数名称：void turn(float mid, float k)
输入参数：中线值，开环转向系数
输出参数：无
备 注：直接按中线偏差打角，不依赖编码器/陀螺仪
 **********************************************/
static int steer_last = 0;                              // 上次舵机指令(相对中值), 用于变化率限制

int turn_last_value(void)                               // 舵机指令(相对中值), 屏幕显示用
{
    return steer_last;
}

void turn(float mid, float k)
{
    float err     = MID_W - mid;                        // >0: 赛道在左侧
    float abs_err = (err < 0.0f) ? -err : err;
    float spd     = (float)base_speed / 400.0f;         // 400 档 -> 1.0, 1200 档 -> 3.0
    float damping = control_frames[control_frame_index].error_delta;
    float k_speed, center, k_eff, d_term, target;
    int   final_duty;

    // 1) 速度增益: 保留"越快打角越多", 但幅度收窄(0.35 -> 0.15)。
    //    原来 1200 档就把 P 放大 1.7 倍, 直道小偏差被当成大弯处理, 于是左右摆。
    k_speed = k * (1.0f + SPEED_K_GAIN * (spd - 1.0f));

    // 2) 中心软化: 偏差小(直道)只给部分增益, 偏差大(弯道)才恢复全力。
    //    原来的 1.25 + 0.025*err 恰好相反, 把直道小偏差的增益抬到 1.25 倍以上。
    center = (abs_err >= STEER_KNEE_ERR) ? 1.0f : (abs_err / STEER_KNEE_ERR);
    /* 前瞻中心软化: 只在"车还没偏、但路已经弯了"时软化, 直道只保留 55% 增益,
     * 否则入弯前段舵量不足, 车被推到外侧线才开始转 -> 转弯压线。
     * 远端中线已经偏出时说明前方是弯, 此时不再按当前偏差缩小增益。
     * 用 far-near 的差值判断前方弯向, 避免某行瞬时值突然跳变。 */
    {
        int bend = (int)(control_frames[control_frame_index].far_mid -
                         control_frames[control_frame_index].near_mid);
        float bend_ratio;
        if(bend < 0) bend = -bend;
        bend_ratio = (float)bend / STEER_KNEE_ERR;
        if(bend_ratio > 1.0f) bend_ratio = 1.0f;
        if(bend_ratio > center) center = bend_ratio;
    }
    k_eff  = k_speed * (STEER_CENTER_RATIO + (1.0f - STEER_CENTER_RATIO) * center);

    // 3) 阻尼: 误差变化率给反向修正(标准 PD 的 D), 系数随速度放大。
    //    原来固定 0.25*damping, 满量程只有 +-2.5 格, 等于没有阻尼。
    d_term = STEER_KD * (0.4f + 0.6f * spd) * damping;
    d_term = Limit_float(-STEER_D_MAX, d_term, STEER_D_MAX);

    target = k_eff * err + d_term;

    /* 5) 曲率前馈: far-near 的差值代表"前方赛道弯的弧度",
     *    只在偏差同向时预打舵: 入弯前车还没偏出去, err 很小 -> 中心软化 -> 反馈来不及给舵,
     *    这是"转弯压线"的直接原因(实测: 偏差 4px 时中心软化后只给 2~4 格舵)。
     *    这里按前瞻弧度直接预打一个量, 不再等偏差长大再补, 直到舵机限幅/限速把舵封顶。 */
    {
        float bend_f = (float)control_frames[control_frame_index].near_mid -
                       (float)control_frames[control_frame_index].far_mid;   // >0: near 与 far 之差, 用于判定前方弯向
        float mag = (bend_f < 0.0f) ? -bend_f : bend_f;
        if(mag > STEER_FF_DEAD)
        {
            /* 前馈随速度缩放: 实测 400 档提前 40cm、1600 档提前 10cm,
             * 说明几何前馈在低速时相对过强(慢速时舵机有充足时间把预打量
             * 全部打完, 高速时被 STEER_SLEW 拖慢反而显得刚好)。
             * 低速压低前馈、高速恢复满额, 让各档入弯时机一致。 */
            float ff_gain = spd * STEER_FF_GAIN_SPD;   // 400档=0.25, 1600档=1.0
            if(ff_gain > 1.0f) ff_gain = 1.0f;
            float ff = STEER_FF_K * (mag - STEER_FF_DEAD) * ff_gain;
            target += (bend_f < 0.0f) ? -ff : ff;
        }
    }

    // 4) 变化率限制: 单帧中线跳变不会立刻变成满舵; 正常打角 25ms 内可达满量程。
    if(target > (float)(steer_last + STEER_SLEW)) target = (float)(steer_last + STEER_SLEW);
    if(target < (float)(steer_last - STEER_SLEW)) target = (float)(steer_last - STEER_SLEW);
    target = Limit_float(-(float)turn_left_duoji_limit, target, (float)turn_right_duoji_limit);
    steer_last = (int)target;

    final_duty = Limit_int(duoji_mid - turn_left_duoji_limit,
                           duoji_mid + steer_last,
                           duoji_mid + turn_right_duoji_limit);
    pwm_set_duty(duoji_jiao, final_duty);
}
