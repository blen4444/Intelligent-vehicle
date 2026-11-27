/*
 * image.h
 *  竞速巡线视觉接口: 分带自适应二值化 + 八邻域/赛道内扫边线追踪 + 加权中线
 */

#ifndef CODE_IMAGE_H_
#define CODE_IMAGE_H_

#include "zf_common_headfile.h"

#define jidian_search_line 110     // 基点行搜索起点(最底有效行 = 109); 该行找不到边线时从这里往上逐行试
#define search_start_line  110     // 追踪起点行的名义值(109 行); 实际起点行见 search_top_row
#define JIDIAN_SCAN_TOP    70      // 自适应基点搜索下限: 底部整行全白时最多往上找 40 行
#define search_end_line    10      // 追踪到第 10 行为止(更远不追踪)
#define MID_W              90      // 车体正对时的中线目标值

#define EDGE_WINDOW_DEF    2       // 追踪窗口半宽, 菜单 WIN
#define LOST_EXTRA_DEF     7       // 丢线后扩大窗口的额外半宽(±(WIN+LOST) 再找一次), 菜单 LOST
#define MID_SLEW_DEF       8       // 时序限幅: 每帧中线最多变化多少像素(抗噪声抖动), 菜单 M_SLEW
#define MID_WEIGHT_RATIO_DEF 0.30f // 中线加权: 远端权重/近端权重, 提前打弯调小, 出弯晚调大, 菜单 MW_RATIO
#define EDGE_OFFSCREEN     0xFF    // 边线出屏标记(屏幕不画, 中线不受影响)

//==================== 赛道外剔除(只关注当前赛道) ====================
#define TRACK_W_MIN        14      // 有效左右边线间距下限(像素), 再小是噪声/伪边
#define TRACK_W_MAX        165     // 有效间距上限(像素), 超过说明某侧跑到赛道外白区
#define TRACK_W_STEP       26      // 相邻两行间距最大突变(像素), 超过则丢弃偏离更大的一侧
#define TRACK_DEAD_RUN     10      // 远端连续多少行双侧都失败后停止追踪(不再画远端乱线)
#define TRACK_DEAD_NEAR    35      // 停止判定只在距起点行这么多行以外生效(保护近端)
#define TRACK_LOST_MAX     20      // 单侧连续丢线超过这么多行后停止切线外推(防止外推漂到赛道外)
#define TRACK_SLOPE_SMOOTH 2       // 切线斜率平滑系数: (slope*N + 新值)/(N+1), 越大越稳
#define MID_CONF_SINGLE    0.35f   // 单侧推算行的中线权重系数(双侧真值=1.0)

extern uint8 base_image[MT9V03X_H][MT9V03X_W];   // 原始灰度
extern uint8 image[MT9V03X_H][MT9V03X_W];        // 二值图: 0=黑(背景) 255=白(赛道)
extern uint8 img_threshold;
extern uint8 left_jidian;
extern uint8 right_jidian;
extern uint8 jidian_valid;                       // 1=本帧基点来自真实边线跳变, 0=沿用上一帧/兜底值
extern uint8 search_top_row;                     // 本帧实际追踪起点行(109=没跳过任何行)
extern uint8 left_line_list[MT9V03X_H];
extern uint8 right_line_list[MT9V03X_H];
extern uint8 mid_line_list[MT9V03X_H];
extern float last_mid_line;
extern float final_mid_line;

extern int   edge_window;                        // WIN: 边线追踪窗口半宽
extern int   lost_extra;                         // LOST: 丢线后扩大窗口的额外半宽
extern int   mid_slew;                           // M_SLEW: 每帧中线最大变化(像素)
extern float mid_weight_ratio;                   // MW_RATIO: 远端权重/近端权重

uint8 Ostuf(uint8 *image);                                    // 分带大津求阈值(原图)
void  set_image_twovalues(uint8 value);                       // 3x3 均值滤波后二值化
void  find_jidian(uint8 index[MT9V03X_H][MT9V03X_W]);         // 找底行边线基点(含单侧出屏兜底)
void  image_deal(uint8 index[MT9V03X_H][MT9V03X_W]);          // 边线追踪 + 单侧透视半宽中线
float find_mid_line_weight(void);                             // 加权中线 + 可信度 + 3帧中值 + 限幅

#endif /* CODE_IMAGE_H_ */
