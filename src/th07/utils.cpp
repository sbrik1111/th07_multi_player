#include "utils.hpp"

#include "ZunMath.hpp"

// FUNCTION: TH07 0x00431930
f32 utils::AddNormalizeAngle(f32 a, f32 b)
{
    i32 i;

    i = 0;
    a += b;
    while (a > ZUN_PI)
    {
        a -= ZUN_2PI;
        if (i++ > 16)
        {
            break;
        }
    }
    while (a < -ZUN_PI)
    {
        a += ZUN_2PI;
        if (i++ > 16)
        {
            break;
        }
    }
    return a;
}

// FUNCTION: TH07 0x004319b0
void utils::Rotate(Float3 *out, Float3 *point, f32 angle)
{
    f32 sinAngle;
    f32 cosAngle;

    sinAngle = sinf(angle);
    cosAngle = cosf(angle);
    out->x = cosAngle * point->x + sinAngle * point->y;
    out->y = cosAngle * point->y - sinAngle * point->x;
}
