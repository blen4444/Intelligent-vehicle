/*
 * image.c
 *  精简视觉: 自适应二值化 + 八邻域(1x7 窗口)边线追踪 + 丢线状态机 + 中线平均
 *  参照 vision.c 的思路重构, 去掉环岛识别/元素识别/中线加权与互补滤波等重活。
 */

#include "image.h"
#include "control.h"

//====================== 全局图像与结果 ======================
uint8 base_image[MT9V03X_H][MT9V03X_W];     // 原始灰度
uint8 image[MT9V03X_H][MT9V03X_W];          // 二值图: 0=黑(背景) 255=白(赛道)
uint8 img_threshold;                        // 屏幕显示的参考阈值
uint8 left_jidian;                          // 底行左边线基点
uint8 right_jidian;                         // 底行右边线基点
uint8 left_line_list[MT9V03X_H];
uint8 right_line_list[MT9V03X_H];
uint8 mid_line_list[MT9V03X_H];
float last_mid_line = MID_W;
float final_mid_line = MID_W;

//==================== 分带自适应二值化 ====================
#define IMG_BAND_NUM        4                               // 图像分 4 段, 每段单独求阈值
#define IMG_BAND_ROWS       (MT9V03X_H / IMG_BAND_NUM)      // 每段行数 120/4 = 30

static uint8 img_band_threshold[IMG_BAND_NUM];                              // 每段阈值
static uint8 img_last_band_threshold[IMG_BAND_NUM] = {128, 128, 128, 128};  // 每段上次有效阈值
static uint8 img_row_threshold[MT9V03X_H];                                  // 每行插值阈值
static uint8 last_threshold = 0;

/********************************************
 函数功能：对图像中的一段求大津阈值
 输入参数：data-原图像, start_row-起始行, rows-行数, valid-该段阈值是否可信
 返回值：  该段的二值化阈值
 备  注：  段内对比度太低(几乎全黑/全白)时 valid=0, 由调用者用邻段阈值代替
 *******************************************/
static uint8 otsu_band(const uint8 *data, uint16 start_row, uint16 rows, uint8 *valid)
{
    int pixelCount[256];
    int pixel_max = 0;
    int pixel_min = 255;
    int i, j;
    int threshold = 128;
    uint32 sum = 0;
    uint32 total = 0;

    for(i = 0; i < 256; i++)
    {
        pixelCount[i] = 0;
    }

    // 每隔一个像素采样, 计算量减到 1/4
    for(i = start_row; i < start_row + rows; i += 2)
    {
        for(j = 0; j < MT9V03X_W; j += 2)
        {
            uint8 v = data[i * MT9V03X_W + j];
            pixelCount[v]++;
            sum += v;
            total++;
            if(v > pixel_max) pixel_max = v;
            if(v < pixel_min) pixel_min = v;
        }
    }

    *valid = 0;
    if(total == 0 || (pixel_max - pixel_min) < 20)
    {
        return 128;     // 这一段几乎没有黑白对比, 阈值不可信
    }

    // 大津法: 取使类间方差最大的灰度值
    {
        float w0 = 0.0f;
        float u0tmp = 0.0f;
        float u1tmp, u0, u1, delta, delta_max = 0.0f;
        float mean_all = (float)sum / (float)total;
        int t_best = 128;

        for(j = pixel_min; j < pixel_max; j++)
        {
            float p = (float)pixelCount[j] / (float)total;
            w0 += p;
            u0tmp += (float)j * p;
            if(w0 <= 0.0f || w0 >= 1.0f)
            {
                continue;
            }
            u1tmp = mean_all - u0tmp;
            u0 = u0tmp / w0;
            u1 = u1tmp / (1.0f - w0);
            delta = w0 * (1.0f - w0) * (u0 - u1) * (u0 - u1);
            if(delta > delta_max)
            {
                delta_max = delta;
                t_best = j;
            }
        }
        threshold = t_best;
    }

    *valid = 1;
    return (uint8)threshold;
}

//-------------------------------------------------------------------------------------------------------------------
//  @brief     分带大津求阈值, 并生成每一行的阈值
//  @return    供屏幕显示的参考阈值(中间两段的平均)
//  Sample usage:   OTSU_Threshold = Ostuf(base_image[0]);
//-------------------------------------------------------------------------------------------------------------------
uint8 Ostuf(uint8 *image)   //注意计算阈值的一定要是原图像
{
    uint8 b, k, i;
    uint8 t;
    uint8 valid;
    uint8 t_raw[IMG_BAND_NUM];
    uint8 t_ok[IMG_BAND_NUM];
    uint8 t_smooth[IMG_BAND_NUM];
    int t_row;

    // 1. 每段单独求阈值
    for(b = 0; b < IMG_BAND_NUM; b++)
    {
        t = otsu_band(image, (uint16)b * IMG_BAND_ROWS, IMG_BAND_ROWS, &valid);
        if(valid && t >= 25 && t <= 235)
        {
            img_last_band_threshold[b] = t;     // 只在有效时更新记忆值
            t_raw[b] = t;
            t_ok[b] = 1;
        }
        else
        {
            t_raw[b] = img_last_band_threshold[b];
            t_ok[b] = 0;
        }
    }

    // 2. 无效段用最近的邻段阈值代替(优先向远处找, 再向近处找)
    for(b = 0; b < IMG_BAND_NUM; b++)
    {
        if(t_ok[b])
        {
            continue;
        }
        for(k = 1; k < IMG_BAND_NUM; k++)
        {
            if(b + k < IMG_BAND_NUM && t_ok[b + k])
            {
                t_raw[b] = t_raw[b + k];
                break;
            }
        }
        if(k >= IMG_BAND_NUM)
        {
            for(k = 1; k < IMG_BAND_NUM; k++)
            {
                if(b >= k && t_ok[b - k])
                {
                    t_raw[b] = t_raw[b - k];
                    break;
                }
            }
        }
    }

    // 3. 相邻段平滑(1:2:1), 避免段与段交界处阈值突变
    for(b = 0; b < IMG_BAND_NUM; b++)
    {
        int left  = t_raw[b > 0 ? b - 1 : b];
        int right = t_raw[b < IMG_BAND_NUM - 1 ? b + 1 : b];
        t_smooth[b] = (uint8)((left + 2 * t_raw[b] + right) / 4);
    }
    for(b = 0; b < IMG_BAND_NUM; b++)
    {
        img_band_threshold[b] = t_smooth[b];
    }

    // 4. 在段中心之间线性插值, 得到每一行的阈值
    for(i = 0; i < MT9V03X_H; i++)
    {
        if(i < IMG_BAND_ROWS / 2)
        {
            t_row = img_band_threshold[0];
        }
        else if(i < IMG_BAND_ROWS + IMG_BAND_ROWS / 2)
        {
            t_row = img_band_threshold[0] + (int)(img_band_threshold[1] - img_band_threshold[0]) * (i - IMG_BAND_ROWS / 2) / IMG_BAND_ROWS;
        }
        else if(i < 2 * IMG_BAND_ROWS + IMG_BAND_ROWS / 2)
        {
            t_row = img_band_threshold[1] + (int)(img_band_threshold[2] - img_band_threshold[1]) * (i - IMG_BAND_ROWS - IMG_BAND_ROWS / 2) / IMG_BAND_ROWS;
        }
        else if(i < 3 * IMG_BAND_ROWS + IMG_BAND_ROWS / 2)
        {
            t_row = img_band_threshold[2] + (int)(img_band_threshold[3] - img_band_threshold[2]) * (i - 2 * IMG_BAND_ROWS - IMG_BAND_ROWS / 2) / IMG_BAND_ROWS;
        }
        else
        {
            t_row = img_band_threshold[3];
        }
        img_row_threshold[i] = (uint8)Limit_int(10, t_row, 245);
    }

    last_threshold = (uint8)((img_band_threshold[1] + img_band_threshold[2]) / 2);
    return last_threshold;
}

/**************************************************
 函数功能：根据每行阈值把原图像二值化到 image, 并做 3x3 均值滤波
 输入参数：value-兜底阈值(每行阈值还没算出来时使用)
 返回值：  无
 备  注：  先 3x3(八邻域)均值再比较阈值, 可去掉零散噪点、平滑边缘锯齿
 *************************************************/
void set_image_twovalues (uint8 value)
{
    uint16 sum;
    uint8 cnt;
    uint8 temp_value;
    uint8 th;

    for(uint8 i=0;i<MT9V03X_H;i++)
    {
        th = img_row_threshold[i];
        if(th == 0)
        {
            th = value;     // 没算过分带阈值时用传入值兜底
        }
        for(uint8 j=0;j<MT9V03X_W;j++)
        {
            sum = base_image[i][j];
            cnt = 1;
            if(i > 0)                       { sum += base_image[i-1][j];   cnt++; }
            if(i < MT9V03X_H - 1)           { sum += base_image[i+1][j];   cnt++; }
            if(j > 0)                       { sum += base_image[i][j-1];   cnt++; }
            if(j < MT9V03X_W - 1)           { sum += base_image[i][j+1];   cnt++; }
            if(i > 0 && j > 0)              { sum += base_image[i-1][j-1]; cnt++; }
            if(i > 0 && j < MT9V03X_W - 1)  { sum += base_image[i-1][j+1]; cnt++; }
            if(i < MT9V03X_H - 1 && j > 0)  { sum += base_image[i+1][j-1]; cnt++; }
            if(i < MT9V03X_H - 1 && j < MT9V03X_W - 1) { sum += base_image[i+1][j+1]; cnt++; }
            temp_value = (uint8)(sum / cnt);
            if (temp_value < th)
            {
                image[i][j]=0;
            }
            else
            {
                image[i][j]=255;
            }
        }
    }
}

/*********************************************************************
函数功能：确定本帧追踪起点行, 并找左右边线基点
函数名称：void find_jidian(uint8 index[MT9V03X_H][MT9V03X_W])
输入参数：二值化后的图像数组
输出参数：left_jidian / right_jidian / jidian_valid / search_top_row
备  注：摄像头压低后, 图像最底部常常整行都是赛道(左右边线在视野之外),
        在这一行上根本找不到黑白跳变。所以不再只盯 jidian_search_line-1,
        而是从这一行往上逐行试, 取第一行"左右都能找到真实跳变"的行作为起点:
          - 左右宽度必须在 [JIDIAN_W_MIN, JIDIAN_W_MAX] 内
          - 两边之间必须基本是白(赛道), 防止噪声点被当成边线
          - 与上一帧宽度相差不能太大(JIDIAN_W_JUMP), 防止抓到阴影/反光
        最多往上试到 JIDIAN_SCAN_TOP 行; 都找不到时依次退化:
          上一帧的有效基点 -> 1 / MT9V03X_W-2 (等同"边线在视野外")
        起点行通过 search_top_row 交给下游, 后面的追踪/统计都以它为界。
*********************************************************************/
#define JIDIAN_W_MIN   20                       // 合法赛道宽度下限(像素)
#define JIDIAN_W_MAX   170                      // 合法赛道宽度上限(像素)
#define JIDIAN_W_JUMP  35                       // 与上一帧宽度允许的最大差(像素)

uint8 jidian_valid   = 0;                       // 1=本帧基点来自真实跳变
uint8 search_top_row = jidian_search_line - 1;  // 本帧实际追踪起点行
static uint8 jidian_last_l  = 1;                // 上一帧有效基点(跨帧记忆)
static uint8 jidian_last_r  = MT9V03X_W - 2;
static uint8 jidian_last_w  = 0;                // 上一帧有效宽度, 0=还没有有效值
static uint8 jidian_last_top = jidian_search_line - 1;  // 上一帧追踪起点行
static uint8 jidian_last_ok = 0;

// 判断 [l,r] 之间是不是基本为白: 抽点统计, 允许少量噪声
static uint8 jidian_white_mid(const uint8 *row, int16 l, int16 r)
{
    int16 x, white = 0, total = 0;
    for(x = l + 2; x <= r - 2; x += 4)
    {
        total++;
        if(row[x] == 255) white++;
    }
    return (uint8)(total > 0 && (white * 10) >= (total * 8));   // >= 80% 白
}

void find_jidian(uint8 index[MT9V03X_H][MT9V03X_W])
{
    int16 y, x;
    uint8 found = 0;
    uint8 pass;

    jidian_valid = 0;

    // 1. 从近到远逐行找"左右边线都真实存在"的行
    //    pass 0: 带宽度连续性约束(抗阴影/反光)
    //    pass 1: 去掉连续性约束(十字/宽窄突变处宽度本来就跳, 避免被上一帧宽度锁死)
    for(pass = 0; pass < 2 && !found; pass++)
    {
        for(y = jidian_search_line - 1; y >= JIDIAN_SCAN_TOP; y--)
        {
            int16 l = -1, r = -1;

            // 从左向右扫第一个 黑->白(赛道左边界)。不再从图像中心扫:
            // 急弯里赛道可能整体偏到图像一侧, 中心另一侧根本没有赛道,
            // 从中心扫会找不到另一边 -> 基点查找失败 -> 兜底直线冲出弯道。
            for(x = 1; x <= MT9V03X_W - 2; x++)
            {
                if(index[y][x - 1] == 0 && index[y][x] == 255) { l = x; break; }
            }
            if(l < 0) continue;                 // 整行没有左边界(全白/全黑) -> 换下一行

            // 从右向左扫第一个 白->黑(赛道右边界)
            for(x = MT9V03X_W - 2; x >= 1; x--)
            {
                if(index[y][x] == 255 && index[y][x + 1] == 0) { r = x; break; }
            }
            if(r < 0 || r <= l) continue;       // 右边界必须在左边界右侧

            if(r - l < JIDIAN_W_MIN || r - l > JIDIAN_W_MAX) continue;    // 宽度不合理
            if(!jidian_white_mid(index[y], l, r)) continue;               // 中间不是赛道
            if(pass == 0 && jidian_last_ok)
            {
                int16 dw = (int16)(r - l) - (int16)jidian_last_w;
                if(dw < 0) dw = -dw;
                if(dw > JIDIAN_W_JUMP) continue;                          // 宽度突变: 多半抓到阴影/反光
            }

            left_jidian    = (uint8)l;
            right_jidian   = (uint8)r;
            search_top_row = (uint8)y;
            jidian_valid   = 1;
            found          = 1;
            break;
        }
    }

    // 2. 全都找不到: 用上一帧有效基点, 单帧全白/脏点不会把追踪带飞
    if(!found)
    {
        search_top_row = jidian_last_top;      // 保持上一帧起点, 别硬跳回 109
        if(jidian_last_ok)
        {
            left_jidian  = jidian_last_l;
            right_jidian = jidian_last_r;
        }
        else
        {
            left_jidian  = 1;                   // 边线在视野外, 取图像边缘当种子
            right_jidian = MT9V03X_W - 2;
        }
        return;
    }

    jidian_last_l  = left_jidian;
    jidian_last_r  = right_jidian;
    jidian_last_w  = (uint8)(right_jidian - left_jidian);
    jidian_last_top = search_top_row;
    jidian_last_ok = 1;
}

//==================== 八邻域(1x7 窗口)边线追踪 + 丢线补线 ====================
// 从上一行边线点出发, 先在 ±WIN(默认 3, 即 1x7 窗口)内找黑白跳变沿;
// 找不到再在 ±(WIN+LOST)(默认 ±10)内找一次; 仍找不到判定本行该侧丢线。
// 丢线补线规则(方案A: 纯切线外推, 无宽度钳制):
//   - 单侧丢线: 沿该侧切线(最近两次真检测点间的斜率)继续外推, 得到真值(允许出屏, 即 <0 或 >=188)
//   - 丢线瞬间记录切点(最后真检测点, 红色显示), 丢线侧重新检测到边线时记为圆环点(红色)
//   - 双侧都丢: 沿用上一行的左右边线(即保持上一行中线)
//   - 中线始终用"真值"计算, 不因补线出屏而被钳到屏幕边;
//   - 补线若出屏, 该边线不画(数组写 EDGE_OFFSCREEN), 但中线不受影响。
int edge_window = EDGE_WINDOW_DEF;
int lost_extra  = LOST_EXTRA_DEF;
int mid_slew    = MID_SLEW_DEF;
float mid_weight_ratio = MID_WEIGHT_RATIO_DEF;

//==================== 元素 FSM: 直行1/拐弯2/十字3/环岛4~8 ====================
uint8 zt_state = ZT_STRAIGHT;      // 当前状态
uint8 zt_side  = 0;                // 环所在侧: 0=左 1=右
int   ring_loss_min  = RING_LOSS_MIN_DEF;
int   zt_near_row    = ZT_NEAR_ROW_DEF;
int   zt_in_step     = ZT_IN_STEP_DEF;
int   zt_out_step    = ZT_OUT_STEP_DEF;
int   zt_width_jump  = ZT_WIDTH_JUMP_DEF;
int   zt_curve_bend  = ZT_CURVE_BEND_DEF;
int   ring_bias      = RING_BIAS_DEF;
int   ring_bias2     = RING_BIAS2_DEF;
int   cross_loss_min = CROSS_LOSS_MIN_DEF;
uint8 cross_state    = 0;          // sg: 0=无 1=十字(双侧长丢线)

// 切点/圆环点标记(方案A, 红色显示)
uint8 tangent_point_l_active = 0;
uint8 tangent_point_r_active = 0;
int16 tangent_point_lx = 0, tangent_point_ly = 0;
int16 tangent_point_rx = 0, tangent_point_ry = 0;
uint8 ring_point_found = 0;
uint8 ring_point_side  = 0;
int16 ring_point_x = 0, ring_point_y = 0;
int16 ring_tangent_x = 0, ring_tangent_y = 0;

// 环岛三拐点(红色显示, 供拉线)
uint8 ring_c1_ok = 0, ring_c2_ok = 0, ring_c3_ok = 0;
int16 ring_c1_x = 0, ring_c1_y = 0;
int16 ring_c2_x = 0, ring_c2_y = 0;
int16 ring_c3_x = 0, ring_c3_y = 0;
uint8 ring_corner_side = 0;

static uint8 left_lost[MT9V03X_H];   // 1=该行左边线丢失(被补线)
static uint8 right_lost[MT9V03X_H];  // 1=该行右边线丢失(被补线)

static void element_detect(void);    // 元素 FSM(定义在 image_deal 之后)

static int16 edge_left(const uint8 index[MT9V03X_H][MT9V03X_W], int16 y, int16 last, int16 win)
{
    int16 lo = last - win, hi = last + win, x;
    int16 best = -1, bestd = 0x7FFF;
    if(lo < 1) lo = 1;
    if(hi > MT9V03X_W - 2) hi = MT9V03X_W - 2;
    // 取窗口内"离预测位置最近"的黑->白沿, 抗噪: 噪声制造的杂边不会抢走真正那条边
    for(x = lo; x <= hi; x++)
    {
        if(index[y][x - 1] == 0 && index[y][x] == 255)
        {
            int16 d = (x >= last) ? (int16)(x - last) : (int16)(last - x);
            if(d < bestd) { bestd = d; best = x; }
        }
    }
    return best;
}

static int16 edge_right(const uint8 index[MT9V03X_H][MT9V03X_W], int16 y, int16 last, int16 win)
{
    int16 lo = last - win, hi = last + win, x;
    int16 best = -1, bestd = 0x7FFF;
    if(lo < 1) lo = 1;
    if(hi > MT9V03X_W - 2) hi = MT9V03X_W - 2;
    // 取窗口内"离预测位置最近"的白->黑沿, 抗噪
    for(x = hi; x >= lo; x--)
    {
        if(index[y][x] == 255 && index[y][x + 1] == 0)
        {
            int16 d = (x >= last) ? (int16)(x - last) : (int16)(last - x);
            if(d < bestd) { bestd = d; best = x; }
        }
    }
    return best;
}

// 有符号除法, 向最近取整(b > 0), 用于算丢线断点之间的平均斜率(每行 dx)
static int16 slope_div(int16 a, int16 b)
{
    if(a >= 0) return (int16)((a + b / 2) / b);
    return (int16)((a - b / 2) / b);
}

// 收集一侧沿 y 从下到上的丢线段(最多 max 段), lo[]=段顶(小y) hi[]=段底(大y)
// 返回段数; run[0] 是最靠下的(近端)段
static uint8 collect_lost_runs(const uint8 *lost, int16 *lo, int16 *hi, uint8 max)
{
    int16 y, run_hi = -1;
    uint8 n = 0;
    for(y = (int16)search_top_row; y >= search_end_line; y--)
    {
        if(lost[y])
        {
            if(run_hi < 0) run_hi = y;      // 该段最底行(大y)
        }
        else if(run_hi >= 0)
        {
            if(n < max) { hi[n] = run_hi; lo[n] = y + 1; n++; }
            run_hi = -1;
        }
    }
    if(run_hi >= 0 && n < max) { hi[n] = run_hi; lo[n] = search_end_line; n++; }
    return n;
}

/*********************************************************************
函数功能：从底行向上逐行追踪左右边线, 生成左/右/中线数组
函数名称：void image_deal(uint8 index[MT9V03X_H][MT9V03X_W])
输入参数：二值化后的图像数组
输出参数：无
备  注：调用此函数后得出左右边线数组与中线数组
*********************************************************************/
void image_deal(uint8 index[MT9V03X_H][MT9V03X_W])
{
    int16 top_y = (int16)search_top_row;         // 本帧追踪起点行(find_jidian 自适应给出)
    int16 lx = left_jidian, rx = right_jidian;   // 左右边线真值(可能出屏)
    int16 y, lf, rf;
    int16 win_small = (int16)edge_window;
    int16 win_big   = (int16)(edge_window + lost_extra);

    // 方案A: 斜率继承(纯切线外推, 无宽度钳制)
    int16 slope_l = 0, slope_r = 0;                       // 每侧冻结的每行斜率 dx/dy
    int16 real_lx = left_jidian, real_rx = right_jidian;  // 最近一次"真检测"到的边线
    int16 real_ly = top_y;
    int16 real_ry = top_y;

    // 丢线段状态(用于切点/圆环点标记)
    uint8  prev_lost_l = 0, prev_lost_r = 0;
    uint16 lost_run_l  = 0, lost_run_r  = 0;
    int16  tan_lx = 0, tan_ly = 0, tan_rx = 0, tan_ry = 0;   // 切点(最后真检测点)

    tangent_point_l_active = 0;
    tangent_point_r_active = 0;
    ring_point_found = 0;

    for(y = top_y; y >= search_end_line; y--)
    {
        // 搜索种子: 钳到屏幕内, 避免出屏坐标让窗口越界
        int16 sl = (lx < 1) ? 1 : lx;  if(sl > MT9V03X_W - 2) sl = MT9V03X_W - 2;
        int16 sr = (rx < 1) ? 1 : rx;  if(sr > MT9V03X_W - 2) sr = MT9V03X_W - 2;

        // 1) 窗口追踪左右边线
        lf = edge_left(index, y, sl, win_small);
        if(lf < 0) lf = edge_left(index, y, sl, win_big);
        rf = edge_right(index, y, sr, win_small);
        if(rf < 0) rf = edge_right(index, y, sr, win_big);

        // 检测到的边更新真值, 并用相邻两次真检测点更新该侧斜率(切线)
        if(lf >= 0)
        {
            int16 dy = (int16)(real_ly - y);
            if(dy > 0) slope_l = slope_div((int16)(lf - real_lx), dy);
            real_lx = lf;
            real_ly = y;
            lx = lf;
        }
        if(rf >= 0)
        {
            int16 dy = (int16)(real_ry - y);
            if(dy > 0) slope_r = slope_div((int16)(rf - real_rx), dy);
            real_rx = rf;
            real_ry = y;
            rx = rf;
        }

        {
            uint8 lost_l = (lf < 0) ? 1 : 0;
            uint8 lost_r = (rf < 0) ? 1 : 0;
            left_lost[y]  = lost_l;      // 记录本行该侧是否真丢线(供状态机)
            right_lost[y] = lost_r;

            // 找到->丢失 跳变: 记录切点(最后真检测点)
            if(!prev_lost_l && lost_l) { tan_lx = real_lx; tan_ly = real_ly; tangent_point_l_active = 1; }
            if(!prev_lost_r && lost_r) { tan_rx = real_rx; tan_ry = real_ry; tangent_point_r_active = 1; }

            // 2) 补线(方案A: 纯切线外推)
            if(lost_l && !lost_r)      lx = lx + slope_l;
            else if(!lost_l && lost_r) rx = rx + slope_r;
            // 双边都有效: lx/rx 已是真检测值, 无需补
            // 两边都丢: 沿用上一行的 lx/rx(即保持上一行中线)

            // 丢线段计数 + 丢失->找到 跳变: 丢线够长则判为圆环点
            if(lost_l)
            {
                lost_run_l++;
            }
            else
            {
                if(prev_lost_l)
                {
                    tangent_point_l_active = 0;
                    if(lost_run_l >= (uint16)ring_loss_min)
                    {
                        ring_point_x = lf;  ring_point_y = y;
                        ring_tangent_x = tan_lx;  ring_tangent_y = tan_ly;
                        ring_point_side = 0;  ring_point_found = 1;
                    }
                }
                lost_run_l = 0;
            }
            if(lost_r)
            {
                lost_run_r++;
            }
            else
            {
                if(prev_lost_r)
                {
                    tangent_point_r_active = 0;
                    if(lost_run_r >= (uint16)ring_loss_min)
                    {
                        ring_point_x = rf;  ring_point_y = y;
                        ring_tangent_x = tan_rx;  ring_tangent_y = tan_ry;
                        ring_point_side = 1;  ring_point_found = 1;
                    }
                }
                lost_run_r = 0;
            }
            prev_lost_l = lost_l;
            prev_lost_r = lost_r;
        }

        // 3) 中线用真值算, 只把结果钳进 uint8 范围
        {
            int16 mid = (int16)((lx + rx) / 2);
            if(mid < 0) mid = 0;
            if(mid > MT9V03X_W - 1) mid = MT9V03X_W - 1;
            mid_line_list[y] = (uint8)mid;
        }

        // 4) 边线显示值: 出屏不画
        left_line_list[y]  = (lx >= 0 && lx < MT9V03X_W) ? (uint8)lx : (uint8)EDGE_OFFSCREEN;
        right_line_list[y] = (rx >= 0 && rx < MT9V03X_W) ? (uint8)rx : (uint8)EDGE_OFFSCREEN;
    }

    // 起点行以下的近场行(y > top_y)直接复制起点行:
    //   - 底部整行全白时 top_y 会被抬高(< 109), 这些行本来就没有可用信息
    //   - 只影响画线(好看、不断)和"贴底"判据, 中线统计从 top_y 往上, 不受影响
    //   - 顺便把 lost 清 0, 避免上一帧的残留值污染丢线段统计
    for(y = MT9V03X_H - 1; y > top_y; y--)
    {
        left_line_list[y]  = left_line_list[top_y];
        right_line_list[y] = right_line_list[top_y];
        mid_line_list[y]   = mid_line_list[top_y];
        left_lost[y]       = 0;
        right_lost[y]      = 0;
    }

    // ===== 环岛三拐点提取 + 拐点1-拐点2 拉线(替换丢线段的切线外推) =====
    ring_c1_ok = ring_c2_ok = ring_c3_ok = 0;
    {
        int16 loL[4], hiL[4], loR[4], hiR[4];
        uint8 nL2 = collect_lost_runs(left_lost,  loL, hiL, 4);
        uint8 nR2 = collect_lost_runs(right_lost, loR, hiR, 4);
        uint16 lenL2 = (nL2) ? (uint16)(hiL[0] - loL[0] + 1) : 0;
        uint16 lenR2 = (nR2) ? (uint16)(hiR[0] - loR[0] + 1) : 0;
        int16 *lo = 0, *hi = 0;
        uint8  n = 0, side = 0;

        // 只对"单侧长丢线"做环岛拉线(排除弯道/十字)
        if(lenL2 >= (uint16)ring_loss_min && lenR2 < (uint16)ring_loss_min)      { side = 0; lo = loL; hi = hiL; n = nL2; }
        else if(lenR2 >= (uint16)ring_loss_min && lenL2 < (uint16)ring_loss_min) { side = 1; lo = loR; hi = hiR; n = nR2; }

        if(n)
        {
            const uint8 *line = (side == 0) ? left_line_list : right_line_list;
            int16 c1r = hi[0] + 1;                    // 拐点1行: 近端丢线段下方(消失点)
            int16 c2r = lo[0] - 1;                    // 拐点2行: 近端丢线段上方(圆环出现点)
            int16 c3r = (n >= 2) ? (hi[1] + 1) : -1;  // 拐点3行: 远端丢线段下方

            if(c1r <= top_y && line[c1r] != EDGE_OFFSCREEN)
            { ring_c1_x = line[c1r]; ring_c1_y = c1r; ring_c1_ok = 1; }
            if(c2r >= search_end_line && line[c2r] != EDGE_OFFSCREEN)
            { ring_c2_x = line[c2r]; ring_c2_y = c2r; ring_c2_ok = 1; }
            if(c3r >= search_end_line && c3r <= top_y && line[c3r] != EDGE_OFFSCREEN)
            { ring_c3_x = line[c3r]; ring_c3_y = c3r; ring_c3_ok = 1; }
            ring_corner_side = side;

            // 拐点1-拐点2 直线拉线, 覆盖丢线段 [lo[0]..hi[0]]
            if(ring_c1_ok && ring_c2_ok && (ring_c1_y - ring_c2_y) >= 2)
            {
                int16 x0 = ring_c1_x, y0 = ring_c1_y;   // 下(拐点1)
                int16 x1 = ring_c2_x, y1 = ring_c2_y;   // 上(拐点2)
                for(y = y1 + 1; y <= y0 - 1; y++)
                {
                    int16 x = x1 + (int16)(((int32)(x0 - x1) * (y - y1)) / (y0 - y1));
                    int16 other, m;
                    if(side == 0)
                    {
                        left_line_list[y]  = (x >= 0 && x < MT9V03X_W) ? (uint8)x : (uint8)EDGE_OFFSCREEN;
                        other = (right_line_list[y] != EDGE_OFFSCREEN) ? (int16)right_line_list[y] : -1;
                    }
                    else
                    {
                        right_line_list[y] = (x >= 0 && x < MT9V03X_W) ? (uint8)x : (uint8)EDGE_OFFSCREEN;
                        other = (left_line_list[y] != EDGE_OFFSCREEN) ? (int16)left_line_list[y] : -1;
                    }
                    if(other >= 0)
                    {
                        m = (int16)((x + other) / 2);
                        if(m < 0) m = 0; if(m > MT9V03X_W - 1) m = MT9V03X_W - 1;
                        mid_line_list[y] = (uint8)m;
                    }
                }
            }

            // 红色标记与拐点一致
            ring_point_found = (uint8)(ring_c1_ok && ring_c2_ok);
            ring_point_side  = side;
            ring_tangent_x = ring_c1_x; ring_tangent_y = ring_c1_y;
            ring_point_x   = ring_c2_x; ring_point_y   = ring_c2_y;
        }
    }

    element_detect();
}

/*********************************************************************
函数功能：找一侧最长的一段连续丢线
*********************************************************************/
static void find_longest_loss(const uint8 *lost, uint16 *best, int16 *s_start, int16 *s_end)
{
    int16 y;
    uint16 len = 0;
    int16 start = -1;

    *best = 0; *s_start = -1; *s_end = -1;
    for(y = (int16)search_top_row; y >= search_end_line; y--)
    {
        if(lost[y])
        {
            len++;
            if(start < 0) start = y;
        }
        else if(len > 0)
        {
            if(len > *best) { *best = len; *s_start = start; *s_end = y + 1; }
            len = 0; start = -1;
        }
    }
    if(len > *best) { *best = len; *s_start = start; *s_end = search_end_line; }
}

/*********************************************************************
函数功能：估计当前赛道宽度(左右边线间距, 取有效行平均)
*********************************************************************/
static int16 track_width_est(void)
{
    int16 y, sum = 0, n = 0;
    for(y = (int16)search_top_row; y >= search_end_line; y--)
    {
        uint8 l = left_line_list[y], r = right_line_list[y];
        if(l != EDGE_OFFSCREEN && r != EDGE_OFFSCREEN && r > l)
        {
            int16 w = (int16)r - (int16)l;
            if(w >= 15 && w <= 180) { sum += w; n++; }
        }
    }
    return (n > 0) ? (int16)(sum / n) : 0;
}

/*********************************************************************
函数功能：估计前方弯度(|远端中线 - 近端中线|), 用于直行/拐弯判定
*********************************************************************/
static int16 mid_bend_est(void)
{
    int16 top    = (int16)search_top_row;    // 追踪起点行(底部全白时会被抬高)
    int16 near_y = top - 10;
    int16 far_y  = top - 70;
    int16 d;
    if(near_y < search_end_line) near_y = top;
    if(far_y  < search_end_line) far_y  = search_end_line;
    d = (int16)mid_line_list[far_y] - (int16)mid_line_list[near_y];
    if(d < 0) d = -d;
    return d;
}

// 对侧边线在 [lo,hi] 行内是否基本直线(起伏 <= tol), 用于区分"环岛入口"和"弯道"
#define RING_STRAIGHT_TOL   18
static uint8 edge_straight(const uint8 *line, int16 lo, int16 hi, int16 tol)
{
    int16 y, x0 = -1, x1 = -1;
    uint16 n = 0, span;
    if(lo > hi) { int16 t = lo; lo = hi; hi = t; }
    for(y = lo; y <= hi; y++)
    {
        if(line[y] != EDGE_OFFSCREEN)
        {
            if(n == 0) x0 = line[y];
            x1 = line[y];
            n++;
        }
    }
    if(n < 12) return 0;
    span = (x1 > x0) ? (uint16)(x1 - x0) : (uint16)(x0 - x1);
    return (span <= (uint16)tol) ? 1 : 0;
}

/*********************************************************************
函数功能：严格环岛候选判定
  条件: 单侧连续丢线 >= R_LOSS; 近端拐点在下方(y >= NEAR_ROW)但**不贴图像最底**
        (贴底的多是弯道内侧/底部缺角); 对侧不丢长线且基本是直线(区分弯道)
*********************************************************************/
static uint8 ring_candidate(uint16 lenL, int16 nL, int16 fL, uint16 lenR, int16 nR, int16 fR, uint8 *side)
{
    int16 top      = (int16)search_top_row;
    int16 bot      = top - 10;                                                  // 近端丢线底部最多到这里, 再靠下就是底部伪影/弯道内侧
    int16 near_min = top - ((search_start_line - 1) - (int16)zt_near_row);      // 默认 top=109 / NEAR_ROW=88 -> 88
    if(bot < near_min + 1) bot = near_min + 1;                                  // 起点行抬高后 bot 可能反超, 保证判据可满足
    if(lenL >= (uint16)ring_loss_min && nL >= near_min && nL <= bot &&
       lenR < (uint16)ring_loss_min && edge_straight(right_line_list, fL, nL, RING_STRAIGHT_TOL))
    {
        *side = 0;
        return 1;
    }
    if(lenR >= (uint16)ring_loss_min && nR >= near_min && nR <= bot &&
       lenL < (uint16)ring_loss_min && edge_straight(left_line_list, fR, nR, RING_STRAIGHT_TOL))
    {
        *side = 1;
        return 1;
    }
    return 0;
}

/*********************************************************************
函数功能：元素 FSM
  zt1 直行 / zt2 拐弯 / zt3 十字 / zt4~8 环岛
  zt4 近端拐点拉线 -> zt5 远端拐点补线进环 -> zt6 环内(编码器计步)
  -> zt7 赛道宽度突变补线出环 -> zt8 出环远端拐点补线 -> zt1
*********************************************************************/
static void element_detect(void)
{
    static int32 enc_mark = 0;       // 进入当前阶段时的编码器里程
    static int16 prev_wid = 0;
    static uint8 wid_valid = 0;
    uint16 lenL = 0, lenR = 0;
    int16  nL = -1, fL = -1, nR = -1, fR = -1;
    uint8  side = 0;
    int16  wid, dw = 0;

    // 左右各自最长连续丢线( n*=近端/下方行, f*=远端/上方行 )
    find_longest_loss(left_lost,  &lenL, &nL, &fL);
    find_longest_loss(right_lost, &lenR, &nR, &fR);

    // 十字(双侧长丢线)
    cross_state = (lenL >= (uint16)cross_loss_min && lenR >= (uint16)cross_loss_min) ? 1 : 0;

    // 赛道宽度 + 突变量(供 zt6->zt7)
    wid = track_width_est();
    if(wid > 20 && wid_valid)
    {
        dw = (int16)(wid - prev_wid);
        if(dw < 0) dw = -dw;
    }
    if(wid > 20) { prev_wid = wid; wid_valid = 1; }

    switch(zt_state)
    {
        case ZT_STRAIGHT:
            if(cross_state)
            {
                zt_state = ZT_CROSS;
            }
            else if(ring_candidate(lenL, nL, fL, lenR, nR, fR, &side))
            {
                zt_state = ZT_RING_NEAR;      // 找到近端丢线边拐点, 对侧不丢线且为直线
                zt_side  = side;
                enc_mark = enc_travel;
            }
            else if(mid_bend_est() >= (int16)zt_curve_bend)
            {
                zt_state = ZT_CURVE;
            }
            break;

        case ZT_CURVE:
            if(mid_bend_est() < (int16)zt_curve_bend) zt_state = ZT_STRAIGHT;
            break;

        case ZT_CROSS:
            if(!cross_state) zt_state = ZT_STRAIGHT;
            break;

        case ZT_RING_NEAR:                    // zt4: 近端拐点拉线阶段
        {
            // "第一拐点消失": 环侧最长丢线段的底部贴到图像最底 -> 转 zt5 找远端拐点
            int16  nb = (zt_side == 0) ? nL : nR;
            uint16 len_side = (zt_side == 0) ? lenL : lenR;
            if(nb >= (int16)search_top_row - 3)
            {
                zt_state = ZT_RING_FAR;
            }
            else if(len_side < (uint16)(ring_loss_min / 2))
            {
                // 丢线消失说明是误判(普通弯道), 退回直行, 避免卡死在 zt4
                zt_state = ZT_STRAIGHT;
            }
            break;
        }

        case ZT_RING_FAR:                     // zt5: 远端拐点补线进环
            // 编码器累计步数达到阈值 -> 停止补线, 进普通循迹(zt6)
            if((int32)(enc_travel - enc_mark) >= zt_in_step)
            {
                zt_state = ZT_RING_IN;
                enc_mark = enc_travel;
            }
            break;

        case ZT_RING_IN:                      // zt6: 环内普通循迹
            // 赛道宽度突变 -> 出环补线(zt7)
            if(wid_valid && dw >= (int16)zt_width_jump)
            {
                zt_state = ZT_RING_EXIT;
                enc_mark = enc_travel;
            }
            break;

        case ZT_RING_EXIT:                    // zt7: 出环补线
            if((int32)(enc_travel - enc_mark) >= zt_out_step)
            {
                zt_state = ZT_RING_OUT;
                enc_mark = enc_travel;
            }
            break;

        case ZT_RING_OUT:                     // zt8: 出环远端拐点补线(不入环)
        default:
            if((int32)(enc_travel - enc_mark) >= zt_out_step)
            {
                zt_state = ZT_STRAIGHT;       // 结束, 出环回 zt1
            }
            break;
    }
}

/*********************************************************************
函数功能：中线 = 有效行左右边线中点的平均值(去掉加权与滤波, 省算力)
函数名称：float find_mid_line_weight(void)
输入参数：无
输出参数：最终中线值
*********************************************************************/
float find_mid_line_weight(void)
{
    float sum  = 0.0f;
    float wsum = 0.0f;
    uint8 y;
    float mid;

    /* 加权中线: 近端(车底)权重大、远端权重小。抬高摄像头后远端行先看到弯,
     * 整段等权平均会被远端提前拉偏 -> 提前打弯; 改成近端权重 1、远端权重
     * mid_weight_ratio(默认 0.3), 既压掉多余超前, 又保留部分前瞻(不会像只取
     * 近端那样完全丢前瞻而冲出赛道)。mid_weight_ratio=1 时退化为等权平均。 */
    int top  = (int)search_top_row;
    int span = top - (int)search_end_line;
    for(y = search_top_row; y > search_end_line; y--)
    {
        float t = (float)(top - (int)y) / (float)span;          // 0=近端 1=远端
        float w = 1.0f - (1.0f - mid_weight_ratio) * t;         // 近端=1 远端=RATIO
        sum  += (float)mid_line_list[y] * w;
        wsum += w;
    }
    mid = (wsum > 0.0f) ? (sum / wsum) : (float)MID_W;

    // 时序限幅: 相对上一帧中线, 每帧最多变化 mid_slew 像素, 抑制噪声导致的帧间抖动(线飞)。
    // 限幅作用在"原始中线"上, 环岛偏置在后面加, 不拖慢进环打角。
    if(mid > last_mid_line + (float)mid_slew) mid = last_mid_line + (float)mid_slew;
    if(mid < last_mid_line - (float)mid_slew) mid = last_mid_line - (float)mid_slew;
    last_mid_line = mid;                 // 存原始(未偏置)中线, 供下一帧限幅参考

    // 环岛: zt4/zt5 朝环侧偏 R_BIAS(进环); zt7/zt8 离环侧偏 R_BIAS2(出环)
    if(zt_state == ZT_RING_NEAR || zt_state == ZT_RING_FAR)
    {
        mid += (zt_side == 0) ? -(float)ring_bias : (float)ring_bias;
    }
    else if(zt_state == ZT_RING_EXIT || zt_state == ZT_RING_OUT)
    {
        mid += (zt_side == 0) ? (float)ring_bias2 : -(float)ring_bias2;
    }

    if(mid < 1.0f) mid = 1.0f;
    if(mid > (float)(MT9V03X_W - 2)) mid = (float)(MT9V03X_W - 2);
    return mid;
}
