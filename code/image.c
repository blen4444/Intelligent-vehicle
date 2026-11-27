/*
 * image.c
 *  竞速巡线视觉: 分带自适应二值化 + 八邻域/赛道内扫边线追踪 + 透视半宽单侧推算 + 加权中线
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
    uint16 th9;
    uint8 cnt;
    uint8 th;
    uint8 i, j;

    for(i = 0; i < MT9V03X_H; i++)
    {
        th = img_row_threshold[i];
        if(th == 0)
        {
            th = value;     // 没算过分带阈值时用传入值兜底
        }

        if(i > 0 && i < MT9V03X_H - 1)
        {
            const uint8 *r0 = base_image[i - 1];
            const uint8 *r1 = base_image[i];
            const uint8 *r2 = base_image[i + 1];
            uint8 *dst = image[i];

            // 左边界 j = 0 (6 邻域)
            sum = (uint16)r0[0] + r0[1] + r1[0] + r1[1] + r2[0] + r2[1];
            dst[0] = (sum < (uint16)th * 6u) ? 0 : 255;

            // 内圈固定 9 邻域: 用 sum < th*9 消除逐像素分支与除法
            th9 = (uint16)th * 9u;
            for(j = 1; j < MT9V03X_W - 1; j++)
            {
                sum = (uint16)r0[j - 1] + r0[j] + r0[j + 1]
                    + (uint16)r1[j - 1] + r1[j] + r1[j + 1]
                    + (uint16)r2[j - 1] + r2[j] + r2[j + 1];
                dst[j] = (sum < th9) ? 0 : 255;
            }

            // 右边界 j = MT9V03X_W - 1 (6 邻域)
            j = MT9V03X_W - 1;
            sum = (uint16)r0[j - 1] + r0[j] + r1[j - 1] + r1[j] + r2[j - 1] + r2[j];
            dst[j] = (sum < (uint16)th * 6u) ? 0 : 255;
        }
        else
        {
            for(j = 0; j < MT9V03X_W; j++)
            {
                sum = base_image[i][j];
                cnt = 1;
                if(i > 0)                                  { sum += base_image[i - 1][j];     cnt++; }
                if(i < MT9V03X_H - 1)                      { sum += base_image[i + 1][j];     cnt++; }
                if(j > 0)                                  { sum += base_image[i][j - 1];     cnt++; }
                if(j < MT9V03X_W - 1)                      { sum += base_image[i][j + 1];     cnt++; }
                if(i > 0 && j > 0)                         { sum += base_image[i - 1][j - 1]; cnt++; }
                if(i > 0 && j < MT9V03X_W - 1)             { sum += base_image[i - 1][j + 1]; cnt++; }
                if(i < MT9V03X_H - 1 && j > 0)             { sum += base_image[i + 1][j - 1]; cnt++; }
                if(i < MT9V03X_H - 1 && j < MT9V03X_W - 1) { sum += base_image[i + 1][j + 1]; cnt++; }
                image[i][j] = (sum < (uint16)th * (uint16)cnt) ? 0 : 255;
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
          - 优先从图像中心向两侧找边(中心在赛道内时), 只落在当前赛道的边上,
            不会越过赛道外的大片白色区域; 中心不在赛道内(急弯)时回退全图扫描
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

// 在 [y_start, y_end] (y_start >= y_end) 内找"单侧存在真实跳变、另一侧全白出屏"的起点行
static uint8 find_jidian_single_side(uint8 index[MT9V03X_H][MT9V03X_W], int16 y_start, int16 y_end)
{
    int16 y, x;
    int16 cmid = MT9V03X_W / 2;

    for(y = y_start; y >= y_end; y--)
    {
        int16 l = -1, r = -1;

        if(index[y][cmid] == 255)
        {
            for(x = cmid; x >= 1; x--)
            {
                if(index[y][x] == 255 && index[y][x - 1] == 0) { l = x; break; }
            }
            for(x = cmid; x <= MT9V03X_W - 2; x++)
            {
                if(index[y][x] == 255 && index[y][x + 1] == 0) { r = x; break; }
            }
        }
        else if(index[y][0] == 255)
        {
            for(x = 0; x <= MT9V03X_W - 2; x++)
            {
                if(index[y][x] == 255 && index[y][x + 1] == 0) { r = x; break; }
            }
        }
        else if(index[y][MT9V03X_W - 1] == 255)
        {
            for(x = MT9V03X_W - 1; x >= 1; x--)
            {
                if(index[y][x] == 255 && index[y][x - 1] == 0) { l = x; break; }
            }
        }

        if(l < 0 && r >= JIDIAN_W_MIN && jidian_white_mid(index[y], 0, r))
        {
            left_jidian     = 1;
            right_jidian    = (uint8)r;
            search_top_row  = (uint8)y;
            jidian_valid    = 1;
            jidian_last_top = search_top_row;
            return 1;       // 单侧出屏不覆盖 jidian_last_w, 保留上次真实赛道宽度
        }
        if(r < 0 && l >= 1 && (MT9V03X_W - 1 - l) >= JIDIAN_W_MIN &&
           jidian_white_mid(index[y], l, MT9V03X_W - 1))
        {
            left_jidian     = (uint8)l;
            right_jidian    = MT9V03X_W - 2;
            search_top_row  = (uint8)y;
            jidian_valid    = 1;
            jidian_last_top = search_top_row;
            return 1;
        }
    }
    return 0;
}

// 在 [y_start, y_end] 内找"双侧边线都真实存在"的起点行
static uint8 find_jidian_both_sides(uint8 index[MT9V03X_H][MT9V03X_W], int16 y_start, int16 y_end)
{
    int16 y, x;
    uint8 pass;

    // pass 0: 带宽度连续性约束(抗阴影/反光)
    // pass 1: 去掉连续性约束(十字/宽窄突变处避免被上一帧宽度锁死)
    for(pass = 0; pass < 2; pass++)
    {
        for(y = y_start; y >= y_end; y--)
        {
            int16 l = -1, r = -1;
            int16 cmid = MT9V03X_W / 2;

            if(index[y][cmid] == 255)
            {
                for(x = cmid; x >= 1; x--)
                {
                    if(index[y][x] == 255 && index[y][x - 1] == 0) { l = x; break; }
                }
                for(x = cmid; x <= MT9V03X_W - 2; x++)
                {
                    if(index[y][x] == 255 && index[y][x + 1] == 0) { r = x; break; }
                }
            }

            // 回退: 中心不在赛道内(急弯里赛道整体偏到一侧) -> 全图扫
            if(l < 0 || r < 0 || r <= l)
            {
                l = -1; r = -1;
                for(x = 1; x <= MT9V03X_W - 2; x++)
                {
                    if(index[y][x - 1] == 0 && index[y][x] == 255) { l = x; break; }
                }
                if(l < 0) continue;

                for(x = MT9V03X_W - 2; x >= 1; x--)
                {
                    if(index[y][x] == 255 && index[y][x + 1] == 0) { r = x; break; }
                }
                if(r < 0 || r <= l) continue;
            }

            if(r - l < JIDIAN_W_MIN || r - l > JIDIAN_W_MAX) continue;
            if(!jidian_white_mid(index[y], l, r)) continue;
            if(pass == 0 && jidian_last_ok)
            {
                int16 dw = (int16)(r - l) - (int16)jidian_last_w;
                if(dw < 0) dw = -dw;
                if(dw > JIDIAN_W_JUMP) continue;
            }

            left_jidian     = (uint8)l;
            right_jidian    = (uint8)r;
            search_top_row  = (uint8)y;
            jidian_valid    = 1;
            jidian_last_l   = left_jidian;
            jidian_last_r   = right_jidian;
            jidian_last_w   = (uint8)(right_jidian - left_jidian);
            jidian_last_top = search_top_row;
            jidian_last_ok  = 1;
            return 1;
        }
    }
    return 0;
}

void find_jidian(uint8 index[MT9V03X_H][MT9V03X_W])
{
    int16 near_end = jidian_search_line - 15;   // 近端优先区(109..95)
    if(near_end < JIDIAN_SCAN_TOP) near_end = JIDIAN_SCAN_TOP;

    jidian_valid = 0;

    // 1. 先在近端(109..95)找双侧有效行
    if(find_jidian_both_sides(index, jidian_search_line - 1, near_end)) return;

    // 2. 近端没有双侧行时, 优先在近端做单侧出屏兜底:
    //    环岛入口(如图 1.3/1.5)或急弯底部常是一侧全白+另一侧清晰边线,
    //    若继续往上爬到 70 行去抓岛心黑弧, 会吃掉底部 30~40 行丢线段导致 zt4 误退出!
    if(find_jidian_single_side(index, jidian_search_line - 1, near_end)) return;

    // 3. 近端双侧和单侧都找不到(真正底部整行全白或全黑)才往上爬到 JIDIAN_SCAN_TOP
    if(near_end > JIDIAN_SCAN_TOP)
    {
        if(find_jidian_both_sides(index, near_end - 1, JIDIAN_SCAN_TOP)) return;
        if(find_jidian_single_side(index, near_end - 1, JIDIAN_SCAN_TOP)) return;
    }

    // 4. 全都找不到: 用上一帧有效基点, 单帧全白/脏点不会把追踪带飞
    search_top_row = jidian_last_top;
    if(jidian_last_ok)
    {
        left_jidian  = jidian_last_l;
        right_jidian = jidian_last_r;
    }
    else
    {
        left_jidian  = 1;
        right_jidian = MT9V03X_W - 2;
    }
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

static uint8 mid_conf[MT9V03X_H];    // 中线可信度: 2=双侧真值 1=单侧推算 0=双侧都丢(沿用上一行)

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

/*********************************************************************
函数功能：从赛道内部(对侧边线)向该侧扫描, 取"连续白色赛道的端点"作为该侧边线
备  注：赛道外的白纸/光斑与赛道之间一般有黑色缝, 从赛道内向外扫时
        会先在黑缝处停下, 因此外部白色区域不会被当成当前赛道的边。
        from_x = 对侧边线位置(本行预测), last = 该侧上一行位置, win = 允许的最大变化。
*********************************************************************/
static int16 edge_left_span(const uint8 index[MT9V03X_H][MT9V03X_W], int16 y, int16 from_x, int16 last, int16 win)
{
    int16 x, lo = last - win, hi = last + win;
    if(lo < 1) lo = 1;
    if(hi > MT9V03X_W - 2) hi = MT9V03X_W - 2;
    if(from_x > MT9V03X_W - 2) from_x = MT9V03X_W - 2;
    if(from_x < lo) return -1;                     // 对侧位置已经跑到该侧窗口外, 不可信
    for(x = from_x; x >= lo; x--)
    {
        if(index[y][x] == 255 && index[y][x - 1] == 0)
        {
            // 跳过赛道内部 1px 孤立黑噪点, 避免把噪点当边线触发假丢线
            if(x >= 2 && index[y][x - 2] == 255) continue;
            // 真正的黑边必须落在 [lo, hi] 窗口内; 若提前撞到黑区则直接停下, 不跨黑缝抓外部白纸
            return (x <= hi) ? x : -1;
        }
    }
    return -1;
}

static int16 edge_right_span(const uint8 index[MT9V03X_H][MT9V03X_W], int16 y, int16 from_x, int16 last, int16 win)
{
    int16 x, lo = last - win, hi = last + win;
    if(lo < 1) lo = 1;
    if(hi > MT9V03X_W - 2) hi = MT9V03X_W - 2;
    if(from_x < 1) from_x = 1;
    if(from_x > hi) return -1;
    for(x = from_x; x <= hi; x++)
    {
        if(index[y][x] == 255 && index[y][x + 1] == 0)
        {
            // 跳过赛道内部 1px 孤立黑噪点
            if(x <= MT9V03X_W - 3 && index[y][x + 2] == 255) continue;
            return (x >= lo) ? x : -1;
        }
    }
    return -1;
}

// 有符号除法, 向最近取整(b > 0), 用于算相邻真检测点间的斜率(每行 dx)
static int16 slope_div(int16 a, int16 b)
{
    if(a >= 0) return (int16)((a + b / 2) / b);
    return (int16)((a - b / 2) / b);
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

    // 赛道外剔除用: 宽度参考 / 上一行边线 / 远端断开标志
    int16 w_prev    = (int16)(right_jidian - left_jidian);  // 最近一次双侧有效的间距
    int16 w_prev_y  = top_y;                                // w_prev 对应的行号
    int16 w_slope_q4 = 12;                                  // 透视收缩率(Q4定点: 12 => 0.75 px/行), 在线平滑更新
    int16 prev_lx   = left_jidian, prev_rx = right_jidian;  // 上一行的左右边线
    uint8 dead_run  = 0;                                    // 连续双侧检测失败的行数
    uint8 track_dead = 0;                                   // 1=远端接不上, 停止追踪
    // 单侧出屏基点(left_jidian<=1 或 right_jidian>=W-2)时, right-left 不是真实赛道宽度, 改用上一帧有效宽度
    if(left_jidian <= 1 || right_jidian >= MT9V03X_W - 2)
    {
        w_prev = (jidian_last_ok && jidian_last_w >= TRACK_W_MIN && jidian_last_w <= TRACK_W_MAX)
                 ? (int16)jidian_last_w : 120;
    }
    if(w_prev < TRACK_W_MIN || w_prev > TRACK_W_MAX) w_prev = 0;   // 起点宽度不可信则不参与突变校验

    // 方案A: 斜率继承(纯切线外推, 无宽度钳制)
    int16 slope_l = 0, slope_r = 0;                       // 每侧冻结的每行斜率 dx/dy
    int16 real_lx = left_jidian, real_rx = right_jidian;  // 最近一次"真检测"到的边线
    int16 real_ly = top_y;
    int16 real_ry = top_y;

    // 单侧丢线计数(用于限制切线外推长度, 防止漂出赛道)
    uint8  prev_lost_l = 0, prev_lost_r = 0;
    uint16 lost_run_l  = 0, lost_run_r  = 0;

    for(y = top_y; y >= search_end_line; y--)
    {
        // 搜索种子: 钳到屏幕内, 避免出屏坐标让窗口越界
        int16 sl = (lx < 1) ? 1 : lx;  if(sl > MT9V03X_W - 2) sl = MT9V03X_W - 2;
        int16 sr = (rx < 1) ? 1 : rx;  if(sr > MT9V03X_W - 2) sr = MT9V03X_W - 2;

        // 1) 左右边线: 优先"从对侧(赛道内)向外扫连续白色赛道的端点"。
        //    赛道外的白纸/光斑与赛道之间有黑缝, 这样扫会在赛道边停下, 不会跑到赛道外;
        //    对侧位置本行不在白色赛道内时, 回退到原来的窗口搜索。
        if(index[y][sr] == 255)
        {
            lf = edge_left_span(index, y, sr, sl, win_big);
        }
        else
        {
            lf = edge_left(index, y, sl, win_small);
            if(lf < 0) lf = edge_left(index, y, sl, win_big);
        }
        if(index[y][sl] == 255)
        {
            rf = edge_right_span(index, y, sl, sr, win_big);
        }
        else
        {
            rf = edge_right(index, y, sr, win_small);
            if(rf < 0) rf = edge_right(index, y, sr, win_big);
        }

        // 1.5) 赛道外剔除: 贴图像边缘 / 左右间距异常的"边线"当丢线处理。
        //      场地上的白纸/背景板边界也是黑白跳变, 不加约束时会沿着它跑偏。
        if(lf >= 0 && (lf <= 1 || lf >= MT9V03X_W - 2)) lf = -1;
        if(rf >= 0 && (rf <= 1 || rf >= MT9V03X_W - 2)) rf = -1;
        if(lf >= 0 && rf >= 0)
        {
            int16 w = (int16)(rf - lf);
            uint8 bad = (uint8)((w < TRACK_W_MIN) || (w > TRACK_W_MAX));
            if(!bad && w_prev > 0)
            {
                int16 dw2 = (w > w_prev) ? (int16)(w - w_prev) : (int16)(w_prev - w);
                if(dw2 > TRACK_W_STEP) bad = 1;                 // 间距突变: 某侧跳到了别的边沿
            }
            if(bad)
            {
                // 丢弃偏离上一行更远的那一侧(通常就是跑到赛道外白区的那条)
                int16 dl = (lf >= prev_lx) ? (int16)(lf - prev_lx) : (int16)(prev_lx - lf);
                int16 dr = (rf >= prev_rx) ? (int16)(rf - prev_rx) : (int16)(prev_rx - rf);
                if(dl >= dr) lf = -1;
                else         rf = -1;
            }
        }
        else if(lf >= 0 && !prev_lost_r && prev_rx > 1 && prev_rx < MT9V03X_W - 2)
        {
            int16 w = (int16)(prev_rx - lf);
            if(w < TRACK_W_MIN || w > TRACK_W_MAX) lf = -1;
        }
        else if(rf >= 0 && !prev_lost_l && prev_lx > 1 && prev_lx < MT9V03X_W - 2)
        {
            int16 w = (int16)(rf - prev_lx);
            if(w < TRACK_W_MIN || w > TRACK_W_MAX) rf = -1;
        }

        // 检测到的边更新真值, 并用相邻两次真检测点更新该侧斜率(切线)
        // 斜率做指数平滑 + 限幅: 单帧噪声点不会让补线方向突然翻转(切点一抖中线就到处跳)
        if(lf >= 0)
        {
            int16 dy = (int16)(real_ly - y);
            if(dy > 0)
            {
                int16 ns = slope_div((int16)(lf - real_lx), dy);
                slope_l = (int16)((slope_l * TRACK_SLOPE_SMOOTH + ns) / (TRACK_SLOPE_SMOOTH + 1));
                if(slope_l > 8) slope_l = 8;
                if(slope_l < -8) slope_l = -8;
            }
            real_lx = lf;
            real_ly = y;
            lx = lf;
        }
        if(rf >= 0)
        {
            int16 dy = (int16)(real_ry - y);
            if(dy > 0)
            {
                int16 ns = slope_div((int16)(rf - real_rx), dy);
                slope_r = (int16)((slope_r * TRACK_SLOPE_SMOOTH + ns) / (TRACK_SLOPE_SMOOTH + 1));
                if(slope_r > 8) slope_r = 8;
                if(slope_r < -8) slope_r = -8;
            }
            real_rx = rf;
            real_ry = y;
            rx = rf;
        }

        {
            uint8 lost_l = (lf < 0) ? 1 : 0;
            uint8 lost_r = (rf < 0) ? 1 : 0;

            // 2) 单侧丢线短距离切线外推(超过 TRACK_LOST_MAX 停止外推, 防止漂到赛道外白区)
            if(lost_l && !lost_r)
            {
                if(lost_run_l < TRACK_LOST_MAX) lx = lx + slope_l;
            }
            else if(!lost_l && lost_r)
            {
                if(lost_run_r < TRACK_LOST_MAX) rx = rx + slope_r;
            }

            if(lost_l) lost_run_l++;
            else       lost_run_l = 0;

            if(lost_r) lost_run_r++;
            else       lost_run_r = 0;

            prev_lost_l = lost_l;
            prev_lost_r = lost_r;
        }

        // 3) 中线: 双侧真值直接平均; 单侧真值时用"真值 ± 透视修正半宽"推算。
        //    不再依赖切线外推 —— 弯道里切线不跟曲率, 会把中线一路拽到赛道外;
        //    同时按透视收缩率缩放 w_prev, 防止近端大宽度沿用到远端把中线推到赛道外。
        {
            int16 mid;
            if(lf >= 0 && rf >= 0)
            {
                mid = (int16)((lx + rx) / 2);
            }
            else if((lf >= 0 || rf >= 0) && w_prev >= TRACK_W_MIN)
            {
                int16 dy_w = (int16)(w_prev_y - y);
                int16 w_est = w_prev;
                if(dy_w > 0)
                {
                    w_est = (int16)(w_prev - (int16)(((int32)dy_w * w_slope_q4) >> 4));
                    if(w_est < TRACK_W_MIN) w_est = TRACK_W_MIN;
                    if(w_est > w_prev)      w_est = w_prev;
                }
                mid = (lf >= 0) ? (int16)(lx + w_est / 2) : (int16)(rx - w_est / 2);
            }
            else
            {
                mid = (int16)((lx + rx) / 2);
            }
            if(mid < 0) mid = 0;
            if(mid > MT9V03X_W - 1) mid = MT9V03X_W - 1;
            mid_line_list[y] = (uint8)mid;
            // 可信度: 双侧真值 > 单侧真值推算 > 都丢(沿用上一行, 不参与中线平均)
            if(lf >= 0 && rf >= 0)
            {
                mid_conf[y] = 2;
            }
            else if(lf >= 0 || rf >= 0)
            {
                mid_conf[y] = 1;
            }
            else
            {
                mid_conf[y] = 0;
            }
        }

        // 4) 边线显示值: 出屏不画
        left_line_list[y]  = (lx >= 0 && lx < MT9V03X_W) ? (uint8)lx : (uint8)EDGE_OFFSCREEN;
        right_line_list[y] = (rx >= 0 && rx < MT9V03X_W) ? (uint8)rx : (uint8)EDGE_OFFSCREEN;

        // 5) 赛道外剔除状态更新 + 远端断开判定
        prev_lx = lx;  prev_rx = rx;
        if(lf >= 0 && rf >= 0)
        {
            int16 w_now = (int16)(rf - lf);
            if(w_prev >= TRACK_W_MIN && w_prev_y > y)
            {
                int16 dy_w = (int16)(w_prev_y - y);
                int16 dw_w = (int16)(w_prev - w_now);
                if(dw_w > 0 && dw_w < TRACK_W_STEP)
                {
                    int16 s_q4 = (int16)(((int32)dw_w << 4) / dy_w);
                    if(s_q4 < 4)  s_q4 = 4;
                    if(s_q4 > 24) s_q4 = 24;
                    w_slope_q4 = (int16)((w_slope_q4 * 3 + s_q4) / 4);
                }
            }
            w_prev = w_now;
            w_prev_y = y;
        }
        if(lf < 0 && rf < 0) dead_run++;
        else                 dead_run = 0;
        if(dead_run >= TRACK_DEAD_RUN && y <= (int16)top_y - TRACK_DEAD_NEAR)
        {
            track_dead = 1;
            break;                    // 远端接不上了: 停止追踪, 不再往上看/画乱线
        }
    }

    // 远端断开后: 把包含连续 dead_run 在内的远端行一并清理, 避免末端冻结线把中线/画线带飞
    if(track_dead)
    {
        int16 y_clean = (int16)(y + (int16)dead_run - 1);
        uint8 m_last;
        if(y_clean > top_y) y_clean = top_y;
        m_last = mid_line_list[(y_clean < top_y) ? (y_clean + 1) : top_y];
        for(y = y_clean; y >= search_end_line; y--)
        {
            left_line_list[y]  = EDGE_OFFSCREEN;
            right_line_list[y] = EDGE_OFFSCREEN;
            mid_line_list[y]   = m_last;
            mid_conf[y]        = 0;
        }
    }

    // 起点行以下的近场行(y > top_y)直接复制起点行(仅影响画线连续性, 中线统计从 top_y 往上)
    for(y = MT9V03X_H - 1; y > top_y; y--)
    {
        left_line_list[y]  = left_line_list[top_y];
        right_line_list[y] = right_line_list[top_y];
        mid_line_list[y]   = mid_line_list[top_y];
        mid_conf[y]        = 0;
    }

}

/*********************************************************************
函数功能：中线 = 有效行左右边线中点的加权平均 + 可信度加权 + 3帧中值
函数名称：float find_mid_line_weight(void)
输入参数：无
输出参数：最终中线值
*********************************************************************/
float find_mid_line_weight(void)
{
    float sum  = 0.0f;
    float wsum = 0.0f;
    int y;
    float mid;
    static float mid_med[3] = {MID_W, MID_W, MID_W};   // 3 帧中值缓冲
    static uint8 mid_med_idx = 0;

    /* 加权中线: 近端(车底)权重大、远端权重小。抬高摄像头后远端行先看到弯,
     * 整段等权平均会被远端提前拉偏 -> 提前打弯; 改成近端权重 1、远端权重
     * mid_weight_ratio(默认 0.3), 既压掉多余超前, 又保留部分前瞻(不会像只取
     * 近端那样完全丢前瞻而冲出赛道)。mid_weight_ratio=1 时退化为等权平均。 */
    int top  = (int)search_top_row;
    int span = top - (int)search_end_line;
    if(span <= 0) span = 1;
    for(y = top; y >= (int)search_end_line; y--)
    {
        float t = (float)(top - y) / (float)span;               // 0=近端 1=远端
        float w = 1.0f - (1.0f - mid_weight_ratio) * t;         // 近端=1 远端=RATIO
        /* 可信度权重: 双侧真值行按正常权重, 单侧外推行大幅降权, 双侧都丢的行直接剔除。
         * 远端补线的切点/斜率抖一点, 不会再被等权平均放大成中线的左右跳。 */
        float c = (mid_conf[y] == 2) ? 1.0f : ((mid_conf[y] == 1) ? MID_CONF_SINGLE : 0.0f);
        if(c <= 0.0f) continue;
        sum  += (float)mid_line_list[y] * w * c;
        wsum += w * c;
    }
    mid = (wsum > 0.05f) ? (sum / wsum) : last_mid_line;        // 有效行太少就保持上一帧, 不用正前硬跳

    // 3 帧中值滤波: 单帧的"切点跳变/假边界"会被中值直接剔除, 连续同向变化只延迟半帧
    {
        float a, b, c, med;
        mid_med[mid_med_idx] = mid;
        mid_med_idx = (uint8)((mid_med_idx + 1) % 3);
        a = mid_med[0]; b = mid_med[1]; c = mid_med[2];
        if((a >= b && a <= c) || (a <= b && a >= c))      med = a;
        else if((b >= a && b <= c) || (b <= a && b >= c)) med = b;
        else                                              med = c;
        mid = med;
    }

    // 时序限幅: 相对上一帧中线, 每帧最多变化 mid_slew 像素, 抑制噪声导致的帧间抖动(线飞)
    if(mid > last_mid_line + (float)mid_slew) mid = last_mid_line + (float)mid_slew;
    if(mid < last_mid_line - (float)mid_slew) mid = last_mid_line - (float)mid_slew;
    last_mid_line = mid;

    if(mid < 1.0f) mid = 1.0f;
    if(mid > (float)(MT9V03X_W - 2)) mid = (float)(MT9V03X_W - 2);
    return mid;
}
