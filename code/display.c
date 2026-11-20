/*
 * display.c
 *
 *  Created on: 2026年1月12日
 *      Author: wxl
 */
#include "Function.h"
#include "display.h"
#include "image.h"

// 画一个红色小十字, 用于标记切点/圆环点
static void mark_cross(uint16 x, uint16 y)
{
    uint16 x0 = (x > 3) ? (uint16)(x - 3) : 0;
    uint16 x1 = x + 3;
    uint16 y0 = (y > 3) ? (uint16)(y - 3) : 0;
    uint16 y1 = y + 3;
    if(x1 >= ips200_width_max)  x1 = (uint16)(ips200_width_max - 1);
    if(y1 >= ips200_height_max) y1 = (uint16)(ips200_height_max - 1);
    ips200_draw_line(x0, y, x1, y, RGB565_RED);
    ips200_draw_line(x, y0, x, y1, RGB565_RED);
}
/*********************************************************************
 函数功能：边线和中线显示
 函数名称：void draw_line(void)
 输入参数：无
 输出参数：无
 备  注：调用此函数后在屏幕上显示左右边线以及中线
 *********************************************************************/
 void draw_line(void)
 {
     // 边线/中线画成 5 像素宽的水平短横; 补线出屏的边(EDGE_OFFSCREEN)不画, 中线始终画
     // 注意: 坐标必须 < ips200_width_max / height_max, 否则库会断言报错
     uint16 w = ips200_width_max;
     uint16 h = ips200_height_max;

     for(uint8 i = MT9V03X_H - 1; i > search_end_line; i--)
     {
         uint16 yy = (uint16)(DISPLAY_SCALE * i + 0.5f);
         if(yy >= h) yy = (uint16)(h - 1);

         if(left_line_list[i] != EDGE_OFFSCREEN)
         {
             uint16 x  = (uint16)(DISPLAY_SCALE * left_line_list[i] + 0.5f);
             uint16 x0 = (x > 2) ? (uint16)(x - 2) : 0;
             uint16 x1 = (uint16)(x + 2);
             if(x1 >= w) x1 = (uint16)(w - 1);
             ips200_draw_line(x0, yy, x1, yy, RGB565_BLUE);
         }
         if(right_line_list[i] != EDGE_OFFSCREEN)
         {
             uint16 x  = (uint16)(DISPLAY_SCALE * right_line_list[i] + 0.5f);
             uint16 x0 = (x > 2) ? (uint16)(x - 2) : 0;
             uint16 x1 = (uint16)(x + 2);
             if(x1 >= w) x1 = (uint16)(w - 1);
             ips200_draw_line(x0, yy, x1, yy, RGB565_GREEN);
         }
         {
             uint16 x  = (uint16)(DISPLAY_SCALE * mid_line_list[i] + 0.5f);
             uint16 x0 = (x > 2) ? (uint16)(x - 2) : 0;
             uint16 x1 = (uint16)(x + 2);
             if(x1 >= w) x1 = (uint16)(w - 1);
             ips200_draw_line(x0, yy, x1, yy, RGB565_RED);
         }
     }

     // ==================== 切点 / 圆环点 红色标记 ====================
     {
         uint16 x, y;
         // 切线外推中的切点(红色小十字)
         if(tangent_point_l_active)
         {
             x = (uint16)(DISPLAY_SCALE * tangent_point_lx + 0.5f);
             y = (uint16)(DISPLAY_SCALE * tangent_point_ly + 0.5f);
             if(x >= w) x = (uint16)(w - 1);
             if(y >= h) y = (uint16)(h - 1);
             mark_cross(x, y);
         }
         if(tangent_point_r_active)
         {
             x = (uint16)(DISPLAY_SCALE * tangent_point_rx + 0.5f);
             y = (uint16)(DISPLAY_SCALE * tangent_point_ry + 0.5f);
             if(x >= w) x = (uint16)(w - 1);
             if(y >= h) y = (uint16)(h - 1);
             mark_cross(x, y);
         }
         // 环岛三拐点(拐点1/2/3) + 拉线
         if(ring_c1_ok || ring_c2_ok || ring_c3_ok)
         {
             uint16 ax = (uint16)(DISPLAY_SCALE * ring_c1_x + 0.5f);
             uint16 ay = (uint16)(DISPLAY_SCALE * ring_c1_y + 0.5f);
             uint16 bx = (uint16)(DISPLAY_SCALE * ring_c2_x + 0.5f);
             uint16 by = (uint16)(DISPLAY_SCALE * ring_c2_y + 0.5f);
             uint16 cx = (uint16)(DISPLAY_SCALE * ring_c3_x + 0.5f);
             uint16 cy = (uint16)(DISPLAY_SCALE * ring_c3_y + 0.5f);
             if(ax >= w) ax = (uint16)(w - 1);
             if(ay >= h) ay = (uint16)(h - 1);
             if(bx >= w) bx = (uint16)(w - 1);
             if(by >= h) by = (uint16)(h - 1);
             if(cx >= w) cx = (uint16)(w - 1);
             if(cy >= h) cy = (uint16)(h - 1);
             if(ring_c1_ok && ring_c2_ok) ips200_draw_line(ax, ay, bx, by, RGB565_RED);  // 拐点1-拐点2 拉线
             if(ring_c2_ok && ring_c3_ok) ips200_draw_line(bx, by, cx, cy, RGB565_RED);  // 拐点2-拐点3
             if(ring_c1_ok) mark_cross(ax, ay);
             if(ring_c2_ok) mark_cross(bx, by);
             if(ring_c3_ok) mark_cross(cx, cy);
         }
     }
}

/* ====================== 屏幕调参菜单(第 3 页) ======================
 * 目的: 改参数不用重新烧录, 拿着车在赛道上边试边调。
 * 操作: 长按 KEY_1 >= 1s 进入/退出
 *       菜单内 KEY_1 = 上一项, KEY_2 = 下一项
 *              KEY_3 = 减,     KEY_4 = 加 (按住 1s 后每 100ms 连步进)
 * 安全: 菜单占用按键期间, 5ms 中断每拍强制 start = 0 且固定显示本页, 车不会动;
 *       退出后要再按一次 KEY_1 才会重新启动。
 * 说明: 参数只改 RAM, 掉电恢复默认值; 要固化就把屏幕上的数抄进 control.h 的 *_DEF。 */

volatile uint8 tune_menu_active  = 0;
volatile uint8 tune_menu_version = 0;

static uint8 tune_menu_index     = 0;       // 当前选中项
static uint8 tune_menu_prev_page = 0;       // 进菜单前的显示页, 退出时恢复

typedef struct
{
    const char *name;       // 显示名(最多 9 字符)
    void       *ptr;        // 参数地址
    uint8       is_int;     // 1 = int, 0 = float
    uint8       point;      // 显示小数位, 0 = 按整数显示
    float       min;        // 下限(含)
    float       max;        // 上限(含)
    float       step;       // 单步
} tune_item_struct;

static const tune_item_struct tune_items[] =
{
    {"K",        &OPEN_TURN_K,        0, 2, 0.40f,  2.00f,  0.02f},
    {"SPD_GAIN", &SPEED_K_GAIN,       0, 2, 0.00f,  0.60f,  0.01f},
    {"CENTER",   &STEER_CENTER_RATIO, 0, 2, 0.30f,  1.00f,  0.02f},
    {"KNEE",     &STEER_KNEE_ERR,     0, 0, 5.0f,  60.0f,  1.00f},
    {"FF",       &STEER_FF_K,         0, 2, 0.00f,  3.00f,  0.10f},
    {"KD",       &STEER_KD,           0, 2, 0.00f,  5.00f,  0.10f},
    {"SLEW",     &STEER_SLEW,         1, 0, 4.0f,  60.0f,  1.00f},
    {"SV_MID",   &duoji_mid,          1, 0, 650.0f, 800.0f, 2.00f},
    {"SLOW_G",   &CURVE_SLOW_GAIN,    1, 0, 0.0f,   6.0f,  1.00f},
    {"SLOW_MAX", &CURVE_SLOW_MAX,     1, 0, 0.0f,  80.0f,  5.00f},
    {"FF_DEAD",  &STEER_FF_DEAD,      0, 2, 0.0f,  20.0f,  0.50f},
    {"FF_SPD",   &STEER_FF_GAIN_SPD,  0, 2, 0.00f,  1.00f,  0.05f},
    {"WIN",      &edge_window,        1, 0, 1.0f,   8.0f,  1.00f},
    {"LOST",     &lost_extra,         1, 0, 0.0f,  20.0f,  1.00f},
    {"M_SLEW",   &mid_slew,           1, 0, 2.0f,  40.0f,  1.00f},
    {"MW_RATIO", &mid_weight_ratio,   0, 2, 0.05f, 1.00f,  0.05f},
    {"R_LOSS",   &ring_loss_min,      1, 0, 10.0f, 80.0f,  1.00f},
    {"NEAR_ROW", &zt_near_row,        1, 0, 60.0f, 118.0f, 1.00f},
    {"IN_STEP",  &zt_in_step,         1, 0, 100.0f, 5000.0f, 50.0f},
    {"OUT_STEP", &zt_out_step,        1, 0, 100.0f, 3000.0f, 50.0f},
    {"W_JUMP",   &zt_width_jump,      1, 0, 10.0f, 80.0f,  1.00f},
    {"CURVE",    &zt_curve_bend,      1, 0, 5.0f,  60.0f,  1.00f},
    {"R_BIAS",   &ring_bias,          1, 0, 0.0f,  30.0f,  1.00f},
    {"R_BIAS2",  &ring_bias2,         1, 0, 0.0f,  30.0f,  1.00f},
    {"C_LOSS",   &cross_loss_min,     1, 0, 10.0f, 60.0f,  1.00f},
};
#define TUNE_ITEM_NUM   ((uint8)(sizeof(tune_items) / sizeof(tune_items[0])))

static float tune_item_get(const tune_item_struct *item)
{
    return item->is_int ? (float)(*(const int *)item->ptr) : *(const float *)item->ptr;
}

static void tune_item_set(const tune_item_struct *item, float value)
{
    if(value < item->min) value = item->min;
    if(value > item->max) value = item->max;
    if(item->is_int)
    {
        *(int *)item->ptr = (int)(value + 0.5f);
    }
    else
    {
        // 按两位小数取整, 否则 0.02 累加会攒出 0.9000001 这种尾巴
        *(float *)item->ptr = (float)((int)(value * 100.0f + 0.5f)) / 100.0f;
    }
}

static void tune_menu_adjust(int8 dir)
{
    const tune_item_struct *item = &tune_items[tune_menu_index];
    tune_item_set(item, tune_item_get(item) + (float)dir * item->step);
    tune_menu_version ++;
}

static void tune_menu_move(int8 dir)
{
    if(dir < 0)
        tune_menu_index = (tune_menu_index == 0) ? (uint8)(TUNE_ITEM_NUM - 1) : (uint8)(tune_menu_index - 1);
    else
        tune_menu_index = (uint8)((tune_menu_index + 1) % TUNE_ITEM_NUM);
    tune_menu_version ++;
}

/* 5ms 中断里调: 菜单态自己吃掉按键, 返回 1 让中断跳过正常按键逻辑 */
uint8 tune_menu_key_scan(void)
{
    static uint8  k1_long_latch = 0;
    static uint32 repeat_tick  = 0;
    uint8 k1_long = (uint8)(key_get_state(KEY_1) == KEY_LONG_PRESS);

    if(!tune_menu_active)
    {
        if(k1_long && !k1_long_latch)                       // 长按 KEY_1: 进入菜单
        {
            tune_menu_active    = 1;
            tune_menu_prev_page = (uint8)((status_page <= 1) ? status_page : 0);
            tune_menu_version ++;
            repeat_tick = sys_tick;
        }
        k1_long_latch = k1_long;
        return tune_menu_active;
    }

    if(k1_long && !k1_long_latch)                           // 菜单里再长按 KEY_1: 退出
    {
        tune_menu_active = 0;
        status_page      = tune_menu_prev_page;
        tune_menu_version ++;
        k1_long_latch = k1_long;
        return 0;
    }
    k1_long_latch = k1_long;

    if(key_get_state(KEY_1) == KEY_SHORT_PRESS) tune_menu_move(-1);
    if(key_get_state(KEY_2) == KEY_SHORT_PRESS) tune_menu_move(1);
    if(key_get_state(KEY_3) == KEY_SHORT_PRESS)
    {
        tune_menu_adjust(-1);
        repeat_tick = sys_tick;
    }
    if(key_get_state(KEY_4) == KEY_SHORT_PRESS)
    {
        tune_menu_adjust(1);
        repeat_tick = sys_tick;
    }
    if(key_get_state(KEY_3) == KEY_LONG_PRESS && (uint32)(sys_tick - repeat_tick) >= TUNE_REPEAT_TICKS)
    {
        repeat_tick = sys_tick;
        tune_menu_adjust(-1);
    }
    if(key_get_state(KEY_4) == KEY_LONG_PRESS && (uint32)(sys_tick - repeat_tick) >= TUNE_REPEAT_TICKS)
    {
        repeat_tick = sys_tick;
        tune_menu_adjust(1);
    }
    return 1;
}

/* 名字补空格到 9 字符: 翻页时新名字比旧名字短, 不清掉会留残影 */
static void tune_menu_name(char *dst, const char *src)
{
    uint8 n = 0;
    while(src[n] != 0 && n < 9)
    {
        dst[n] = src[n];
        n ++;
    }
    while(n < 9)
    {
        dst[n] = ' ';
        n ++;
    }
    dst[n] = 0;
}

void tune_menu_draw(void)
{
    char   name[10];
    uint8  row, item_index, view_top;
    uint16 y;
    float  value;

    view_top = (uint8)((tune_menu_index / TUNE_ROWS) * TUNE_ROWS);

    ips200_set_color(RGB565_BLACK, RGB565_WHITE);

    // 标题行: 项序号 + 两个实时量(sv = 舵机指令, er = 中线偏差)
    ips200_show_string(0, 160, "MENU");
    ips200_show_int(40, 160, (int32)(tune_menu_index + 1), 2);
    ips200_show_string(64, 160, "/");
    ips200_show_int(72, 160, (int32)TUNE_ITEM_NUM, 2);
    ips200_show_string(96, 160, "sv:");
    ips200_show_int(120, 160, (int32)turn_last_value(), 4);
    ips200_show_string(160, 160, "er:");
    ips200_show_int(184, 160, (int32)(MID_W - final_mid_line), 4);

    for(row = 0; row < TUNE_ROWS; row ++)
    {
        item_index = (uint8)(view_top + row);
        y = (uint16)(182 + (uint16)row * 22);

        tune_menu_name(name, (item_index < TUNE_ITEM_NUM) ? tune_items[item_index].name : "");

        if(item_index < TUNE_ITEM_NUM)
        {
            ips200_set_color((item_index == tune_menu_index) ? RGB565_RED : RGB565_BLACK, RGB565_WHITE);
            ips200_show_string(0, y, (item_index == tune_menu_index) ? ">" : " ");
            ips200_show_string(12, y, name);
            value = tune_item_get(&tune_items[item_index]);
            if(tune_items[item_index].point == 0)
                ips200_show_int(170, y, (int32)(value + 0.5f), 4);
            else
                ips200_show_float(150, y, (double)value, 3, 2);
            if(item_index == tune_menu_index)
                ips200_draw_line(0, (uint16)(y + 16), 239, (uint16)(y + 16), RGB565_RED);
        }
        else
        {
            ips200_set_color(RGB565_BLACK, RGB565_WHITE);
            ips200_show_string(0, y, " ");
            ips200_show_string(12, y, name);
            ips200_show_string(150, y, "          ");
        }
    }

    ips200_set_color(RGB565_BLACK, RGB565_WHITE);
    ips200_show_string(0, 296, "K3- K4+  hold K1=exit");
}
