/*
 * image.h
 *  精简视觉接口: 自适应二值化 + 八邻域边线追踪 + 中线
 */

#ifndef CODE_IMAGE_H_
#define CODE_IMAGE_H_

#include "zf_common_headfile.h"

#define jidian_search_line 110     // 基点行搜索起点(最底有效行 = 109); 该行找不到边线时从这里往上逐行试
#define search_start_line  110     // 追踪起点行的名义值(109 行); 实际起点行见 search_top_row
#define JIDIAN_SCAN_TOP    70      // 自适应基点搜索下限: 底部整行全白时最多往上找 40 行
#define search_end_line    10      // 追踪到第 10 行为止(更远不追踪)。抬高+看远后可往上扩(改小), 让第二拐点更早出现
#define MID_W              90          // 车体正对时的中线目标值(2026-11-20 抬高摄像头后实测直线中线=83, 原 93)

#define EDGE_WINDOW_DEF    2       // 追踪窗口半宽(默认 1x7 窗口), 菜单 WIN
#define LOST_EXTRA_DEF     7       // 丢线后扩大窗口的额外半宽(±(WIN+LOST) 再找一次), 菜单 LOST
#define MID_SLEW_DEF       12      // 时序限幅: 每帧中线最多变化多少像素(抗噪声抖动), 菜单 M_SLEW
#define MID_WEIGHT_RATIO_DEF 0.30f // 中线加权: 远端权重/近端权重, 提前打弯调小, 出弯晚调大, 菜单 MW_RATIO
#define EDGE_OFFSCREEN     0xFF    // 补线出屏时边线数组的标记(屏幕不画, 中线不受影响)

extern uint8 base_image[MT9V03X_H][MT9V03X_W];   // 原始灰度
extern uint8 image[MT9V03X_H][MT9V03X_W];        // 二值图: 0=黑(背景) 255=白(赛道)
extern uint8 img_threshold;
extern uint8 left_jidian;
extern uint8 right_jidian;
extern uint8 jidian_valid;                 // 1=本帧基点来自真实边线跳变, 0=沿用上一帧/兜底值
extern uint8 search_top_row;               // 本帧实际追踪起点行(109=没跳过任何行)
extern uint8 left_line_list[MT9V03X_H];
extern uint8 right_line_list[MT9V03X_H];
extern uint8 mid_line_list[MT9V03X_H];
extern float last_mid_line;
extern float final_mid_line;

extern int edge_window;                               // WIN: 边线追踪窗口半宽
extern int lost_extra;                                // LOST: 丢线后扩大窗口的额外半宽
extern int mid_slew;                                  // M_SLEW: 每帧中线最大变化(像素)
extern float mid_weight_ratio;                        // MW_RATIO: 远端权重/近端权重

//==================== 切点 / 圆环点标记(方案A) ====================
extern uint8 tangent_point_l_active;   // 1=左边线正在切线外推(有切点)
extern uint8 tangent_point_r_active;   // 1=右边线正在切线外推(有切点)
extern int16 tangent_point_lx, tangent_point_ly;      // 左边线切点
extern int16 tangent_point_rx, tangent_point_ry;      // 右边线切点
extern uint8 ring_point_found;         // 1=本帧找到圆环点
extern uint8 ring_point_side;          // 0=环在左 1=环在右
extern int16 ring_point_x, ring_point_y;              // 圆环点
extern int16 ring_tangent_x, ring_tangent_y;          // 圆环点对应的切点(连线起点)

//==================== 环岛三拐点(红色显示, 供拉线) ====================
// 拐点1 = 消失点(近端丢线段顶部/最后真检测点); 拐点2 = 圆环出现点(环岛线段中点);
// 拐点3 = 远端丢线段底部(直道边界出现点)
extern uint8 ring_c1_ok, ring_c2_ok, ring_c3_ok;
extern int16 ring_c1_x, ring_c1_y;
extern int16 ring_c2_x, ring_c2_y;
extern int16 ring_c3_x, ring_c3_y;
extern uint8 ring_corner_side;         // 三拐点属于哪一侧: 0=左 1=右

//==================== 元素 FSM: 直行1/拐弯2/十字3/环岛4~8 ====================
#define ZT_STRAIGHT     1   // 直行: 正常巡线
#define ZT_CURVE        2   // 拐弯: 前方弯度大
#define ZT_CROSS        3   // 十字: 双侧长丢线
#define ZT_RING_NEAR    4   // 环岛①: 近端拐点检测 + 拉线
#define ZT_RING_FAR     5   // 环岛②: 远端拐点补线进环
#define ZT_RING_IN      6   // 环岛③: 环内(编码器计步, 普通循迹)
#define ZT_RING_EXIT    7   // 环岛④: 出环(赛道宽度突变, 补线出环)
#define ZT_RING_OUT     8   // 环岛⑤: 出环远端拐点补线(不入环) -> 回 zt1

extern uint8 zt_state;               // 当前 FSM 状态
extern uint8 zt_side;                // 环所在侧: 0=左 1=右

// 判据阈值(菜单可调, 默认值见 *_DEF)
#define RING_LOSS_MIN_DEF     30    // R_LOSS  : 环岛候选的单侧连续丢线行数下限
#define ZT_NEAR_ROW_DEF       88    // NEAR_ROW: 近端拐点最低行(y>=该值才算"近端", 排除远处弯道误判)
#define ZT_IN_STEP_DEF        900   // IN_STEP : zt5->zt6 进环所需编码器累计步数
#define ZT_OUT_STEP_DEF       500   // OUT_STEP: zt7->zt8 / zt8->zt1 出环所需编码器步数
#define ZT_WIDTH_JUMP_DEF     40    // W_JUMP  : zt6->zt7 赛道宽度突变阈值(像素)
#define ZT_CURVE_BEND_DEF     18    // CURVE   : 前方弯度(远/近中线差)超过多少判拐弯
#define RING_BIAS_DEF         15    // R_BIAS  : zt4/zt5 朝环侧偏置(像素, 进环)。过弯冲出去就加大
#define RING_BIAS2_DEF        12    // R_BIAS2 : zt7/zt8 离环侧偏置(像素, 出环)
#define CROSS_LOSS_MIN_DEF    25    // C_LOSS  : 判十字的双侧连续丢线行数下限

extern int   ring_loss_min;          // R_LOSS
extern int   zt_near_row;            // NEAR_ROW
extern int   zt_in_step;             // IN_STEP
extern int   zt_out_step;            // OUT_STEP
extern int   zt_width_jump;          // W_JUMP
extern int   zt_curve_bend;          // CURVE
extern int   ring_bias;              // R_BIAS
extern int   ring_bias2;             // R_BIAS2
extern int   cross_loss_min;         // C_LOSS
extern uint8 cross_state;            // sg: 0=无 1=十字(双侧长丢线)

uint8 Ostuf(uint8 *image);                                    // 分带大津求阈值(原图)
void  set_image_twovalues(uint8 value);                       // 3x3(八邻域)均值滤波后二值化
void  find_jidian(uint8 index[MT9V03X_H][MT9V03X_W]);         // 找底行边线基点
void  image_deal(uint8 index[MT9V03X_H][MT9V03X_W]);          // 八邻域追踪 + 丢线状态机
float find_mid_line_weight(void);                             // 中线 = 有效行左右边线中点平均

#endif /* CODE_IMAGE_H_ */
