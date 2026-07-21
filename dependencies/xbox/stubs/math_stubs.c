#include <math.h>

float __cdecl tanf(float x)   { return (float)tan((double)x); }
float __cdecl asinf(float x)  { return (float)asin((double)x); }
float __cdecl acosf(float x)  { return (float)acos((double)x); }
float __cdecl expf(float x)   { return (float)exp((double)x); }
float __cdecl logf(float x)   { return (float)log((double)x); }
float __cdecl ldexpf(float x, int e) { return (float)ldexp((double)x, e); }
