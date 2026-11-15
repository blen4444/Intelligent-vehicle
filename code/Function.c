/*
 * function.c
 *
 *  Created on: 2026年1月12日
 *      Author: wxl
 */
#include "Function.h"

/*********************************************************************
函数功能：限制uint8类型数值范围
函数名称：Limit_uint8(uint8 a, uint8 b, uint8 c)
输入参数：a-最小值，b-待限制值，c-最大值
输出参数：无
返回值：限幅后的uint8类型数值
备  注：将b限制在a~c范围内
*********************************************************************/
uint8 Limit_uint8(uint8 a, uint8 b, uint8 c)
{
    if ((b >= a) && (b <= c))
    {
        return b;
    }
    else if (b < a)
    {
        return a;
    }
    else if (b > c)
    {
        return c;
    }
    return 0;
}

/*********************************************************************
函数功能：限制float类型数值范围
函数名称：Limit_float(float a, float b, float c)
输入参数：a-最小值，b-待限制值，c-最大值
输出参数：无
返回值：限幅后的float类型数值
备  注：将b限制在a~c范围内
*********************************************************************/
float Limit_float(float a, float b, float c)
{
    if ((b >= a) && (b <= c))
    {
        return b;
    }
    else if (b < a)
    {
        return a;
    }
    else if (b > c)
    {
        return c;
    }
    return 0.0f;
}


int Limit_int(int a, int b, int c)
{
    if(b >= a && b <= c) return b;
    else if(b < a) return a;
    else return c;
}

