#include "mtlx_eval.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- MtlxValue helpers ------------------------------------------------- */

static MtlxValue mv_zero(MtlxType t) { MtlxValue v; memset(&v, 0, sizeof(v)); v.type = t; return v; }
static MtlxValue mv_float(float f) { MtlxValue v = mv_zero(MV_FLOAT); v.v[0] = f; return v; }
static MtlxValue mv_color3(v3 c) { MtlxValue v = mv_zero(MV_COLOR3); v.v[0] = c.x; v.v[1] = c.y; v.v[2] = c.z; return v; }
static MtlxValue mv_vec3(v3 c) { MtlxValue v = mv_zero(MV_VEC3); v.v[0] = c.x; v.v[1] = c.y; v.v[2] = c.z; return v; }
static MtlxValue mv_vec2(float x, float y) { MtlxValue v = mv_zero(MV_VEC2); v.v[0] = x; v.v[1] = y; return v; }

static int ncomp_of(const MtlxValue *v) {
    switch (v->type) {
        case MV_VEC2: return 2;
        case MV_COLOR3: case MV_VEC3: return 3;
        case MV_COLOR4: case MV_VEC4: return 4;
        case MV_MATRIX33: return 9;
        case MV_MATRIX44: return 16;
        default: return 1;
    }
}
static float mv_as_float(const MtlxValue *v) {
    if (v->type == MV_FLOAT || v->type == MV_INT || v->type == MV_BOOL) return v->v[0];
    return (v->v[0] + v->v[1] + v->v[2]) * (1.0f / 3.0f);
}
static v3 mv_as_v3(const MtlxValue *v) {
    if (ncomp_of(v) == 1) return v3_splat(v->v[0]);
    return v3_make(v->v[0], v->v[1], v->v[2]);
}

/* component-wise binary/unary application preserving the type of `a`. */
static MtlxValue binop(MtlxValue a, MtlxValue b, float (*f)(float, float)) {
    int n = ncomp_of(&a), nb = ncomp_of(&b);
    for (int i = 0; i < n; i++) { float bv = (nb == 1) ? b.v[0] : b.v[i]; a.v[i] = f(a.v[i], bv); }
    return a;
}
static MtlxValue unop(MtlxValue a, float (*f)(float)) {
    int n = ncomp_of(&a);
    for (int i = 0; i < n; i++) a.v[i] = f(a.v[i]);
    return a;
}

static float f_add(float a, float b) { return a + b; }
static float f_sub(float a, float b) { return a - b; }
static float f_mul(float a, float b) { return a * b; }
static float f_div(float a, float b) { return b != 0.0f ? a / b : 0.0f; }
static float f_min(float a, float b) { return a < b ? a : b; }
static float f_max(float a, float b) { return a > b ? a : b; }
static float f_pow(float a, float b) { return powf(maxf(a, 0.0f), b); }
/* safepower: sign(in1) * pow(abs(in1), in2) */
static float f_safepow(float a, float b) { float p = powf(fabsf(a), b); return a < 0.0f ? -p : (a > 0.0f ? p : 0.0f); }
static float f_mod(float a, float b) { return b != 0.0f ? a - b * floorf(a / b) : 0.0f; }
static float f_atan2(float a, float b) { return atan2f(a, b); }
static float u_sin(float a) { return sinf(a); }
static float u_cos(float a) { return cosf(a); }
static float u_tan(float a) { return tanf(a); }
static float u_asin(float a) { return asinf(clampf(a, -1, 1)); }
static float u_acos(float a) { return acosf(clampf(a, -1, 1)); }
static float u_atan(float a) { return atanf(a); }
static float u_sqrt(float a) { return sqrtf(maxf(a, 0.0f)); }
static float u_ln(float a) { return logf(maxf(a, 1e-8f)); }
static float u_exp(float a) { return expf(a); }
static float u_abs(float a) { return fabsf(a); }
static float u_floor(float a) { return floorf(a); }
static float u_ceil(float a) { return ceilf(a); }
static float u_round(float a) { return roundf(a); }
static float u_sign(float a) { return a > 0 ? 1.0f : (a < 0 ? -1.0f : 0.0f); }

/* ---- color helpers ----------------------------------------------------- */
static v3 rgb_to_hsv(v3 c) {
    float r = c.x, g = c.y, b = c.z;
    float mx = maxf(r, maxf(g, b)), mn = minf(r, minf(g, b)), d = mx - mn;
    float h = 0, s = mx > 0 ? d / mx : 0, v = mx;
    if (d > 1e-8f) {
        if (mx == r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
    }
    return v3_make(h, s, v);
}
static v3 hsv_to_rgb(v3 c) {
    float h = c.x * 6, s = c.y, v = c.z;
    int i = (int)floorf(h);
    float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (((i % 6) + 6) % 6) {
        case 0: return v3_make(v, t, p);
        case 1: return v3_make(q, v, p);
        case 2: return v3_make(p, v, t);
        case 3: return v3_make(p, q, v);
        case 4: return v3_make(t, p, v);
        default: return v3_make(v, p, q);
    }
}

/* ---- MaterialX-compatible gradient (Perlin) noise ----------------------------
 * Ported bit-for-bit from MaterialX libraries/stdlib/genglsl/lib/mx_noise.glsl
 * (itself the OSL oslnoise): Bob Jenkins lookup3 hash + 3D gradient noise with
 * the 0.9820 normalization. Producing the same values as the reference renderers
 * is what makes procedural materials (e.g. standard_surface_marble_solid) match. */
static uint32_t mx_rotl32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
static uint32_t mx_bjfinal(uint32_t a, uint32_t b, uint32_t c) {
    c ^= b; c -= mx_rotl32(b, 14);
    a ^= c; a -= mx_rotl32(c, 11);
    b ^= a; b -= mx_rotl32(a, 25);
    c ^= b; c -= mx_rotl32(b, 16);
    a ^= c; a -= mx_rotl32(c, 4);
    b ^= a; b -= mx_rotl32(a, 14);
    c ^= b; c -= mx_rotl32(b, 24);
    return c;
}
static void mx_bjmix(uint32_t *a,uint32_t *b,uint32_t *c){
    *a-=*c;*a^=mx_rotl32(*c,4);*c+=*b;*b-=*a;*b^=mx_rotl32(*a,6);*a+=*c;
    *c-=*b;*c^=mx_rotl32(*b,8);*b+=*a;*a-=*c;*a^=mx_rotl32(*c,16);*c+=*b;
    *b-=*a;*b^=mx_rotl32(*a,19);*a+=*c;*c-=*b;*c^=mx_rotl32(*b,4);*b+=*a;
}
static v3 mx_cell_vec3(int x,int y,int z,int w,int has_w){
    uint32_t a=0xdeadbeefu+((has_w?5u:4u)<<2u)+13u,b=a,c=a;
    a+=(uint32_t)x;b+=(uint32_t)y;c+=(uint32_t)z;mx_bjmix(&a,&b,&c);
    if(has_w)a+=(uint32_t)w;
    if(has_w)return v3_make((float)mx_bjfinal(a,b,c)/(float)0xffffffffu,
        (float)mx_bjfinal(a,b+1u,c)/(float)0xffffffffu,
        (float)mx_bjfinal(a,b+2u,c)/(float)0xffffffffu);
    return v3_make((float)mx_bjfinal(a,b,c)/(float)0xffffffffu,
        (float)mx_bjfinal(a+1u,b,c)/(float)0xffffffffu,
        (float)mx_bjfinal(a+2u,b,c)/(float)0xffffffffu);
}
static uint32_t mx_hash_int3(int x, int y, int z) {
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (3u << 2u) + 13u;
    a += (uint32_t)x; b += (uint32_t)y; c += (uint32_t)z;
    return mx_bjfinal(a, b, c);
}
static uint32_t mx_hash_int2(int x, int y) {
    uint32_t a, b, c;
    a = b = c = 0xdeadbeefu + (2u << 2u) + 13u;
    a += (uint32_t)x; b += (uint32_t)y;
    return mx_bjfinal(a, b, c);
}
/* dot product of (x,y,z) with one of 16 edge-of-cube gradient vectors */
static float mx_gradient(uint32_t hash, float x, float y, float z) {
    uint32_t h = hash & 15u;
    float u = (h < 8u) ? x : y;
    float v = (h < 4u) ? y : (((h == 12u) || (h == 14u)) ? x : z);
    float r = (h & 1u) ? -u : u;
    r += (h & 2u) ? -v : v;
    return r;
}
static float mx_fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
static int mx_floorfrac(float x, float *f) { int i = (int)floorf(x); *f = x - (float)i; return i; }
static float mx_trilerp(float v0, float v1, float v2, float v3, float v4, float v5,
                        float v6, float v7, float s, float t, float r) {
    float s1 = 1.0f - s, t1 = 1.0f - t, r1 = 1.0f - r;
    return r1 * (t1 * (v0 * s1 + v1 * s) + t * (v2 * s1 + v3 * s)) +
           r  * (t1 * (v4 * s1 + v5 * s) + t * (v6 * s1 + v7 * s));
}
/* mx_perlin_noise_float(vec3): result in ~[-1,1] */
static uint32_t mx_hash3_channel(int x,int y,int z,int channel){uint32_t h=mx_hash_int3(x,y,z);return channel>=0?(h>>(channel*8))&255u:h;}
static float vnoise_channel(v3 p,int channel) {
    float fx, fy, fz;
    int X = mx_floorfrac(p.x, &fx), Y = mx_floorfrac(p.y, &fy), Z = mx_floorfrac(p.z, &fz);
    float u = mx_fade(fx), v = mx_fade(fy), w = mx_fade(fz);
    float res = mx_trilerp(
        mx_gradient(mx_hash3_channel(X,     Y,     Z,channel), fx,        fy,        fz       ),
        mx_gradient(mx_hash3_channel(X + 1, Y,     Z,channel), fx - 1.0f, fy,        fz       ),
        mx_gradient(mx_hash3_channel(X,     Y + 1, Z,channel), fx,        fy - 1.0f, fz       ),
        mx_gradient(mx_hash3_channel(X + 1, Y + 1, Z,channel), fx - 1.0f, fy - 1.0f, fz       ),
        mx_gradient(mx_hash3_channel(X,     Y,     Z + 1,channel), fx,        fy,        fz - 1.0f),
        mx_gradient(mx_hash3_channel(X + 1, Y,     Z + 1,channel), fx - 1.0f, fy,        fz - 1.0f),
        mx_gradient(mx_hash3_channel(X,     Y + 1, Z + 1,channel), fx,        fy - 1.0f, fz - 1.0f),
        mx_gradient(mx_hash3_channel(X + 1, Y + 1, Z + 1,channel), fx - 1.0f, fy - 1.0f, fz - 1.0f),
        u, v, w);
    return 0.9820f * res;
}
static float vnoise(v3 p){return vnoise_channel(p,-1);}
static float fractal(v3 p, int octaves, float lacunarity, float diminish) {
    float sum = 0, amp = 1;
    for (int o = 0; o < octaves; o++) { sum += amp * vnoise(p); amp *= diminish; p = v3_scale(p, lacunarity); }
    return sum;
}
static float gradient2(uint32_t hash, float x, float y) {
    uint32_t h=hash&7u;float u=h<4u?x:y,v=2.0f*(h<4u?y:x);
    return (h&1u?-u:u)+(h&2u?-v:v);
}
static float vnoise2(float x,float y,int channel) {
    int ix=(int)floorf(x),iy=(int)floorf(y);float fx=x-ix,fy=y-iy;
    float u=mx_fade(fx),v=mx_fade(fy);uint32_t h[4]={mx_hash_int2(ix,iy),mx_hash_int2(ix+1,iy),mx_hash_int2(ix,iy+1),mx_hash_int2(ix+1,iy+1)};
    if(channel>=0)for(int i=0;i<4;i++)h[i]=(h[i]>>(channel*8))&255u;
    float a=gradient2(h[0],fx,fy),b=gradient2(h[1],fx-1,fy),c=gradient2(h[2],fx,fy-1),d=gradient2(h[3],fx-1,fy-1);
    return 0.6616f*((1-v)*(a+(b-a)*u)+v*(c+(d-c)*u));
}
/* mx_cell_noise_float: hash of the integer lattice cell, mapped to [0,1] */
static float cellnoise(v3 p) {
    uint32_t h = mx_hash_int3((int)floorf(p.x), (int)floorf(p.y), (int)floorf(p.z));
    return (float)h / (float)0xffffffffu;
}
static float worley_cell_channel(int x,int y,int z,int d3,int channel){
    if(channel>=0){
        if(d3){
            v3 values=mx_cell_vec3(x,y,z,0,0);
            return channel==0?values.x:(channel==1?values.y:values.z);
        }
        return (float)mx_hash_int3(x,y,channel)/(float)0xffffffffu;
    }
    uint32_t h=d3?mx_hash_int3(x,y,z):mx_hash_int2(x,y);
    return (float)h/(float)0xffffffffu;
}
static MtlxValue worley_value(v3 p,int d3,float jitter,int style,MtlxType type){
    int ix=(int)floorf(p.x),iy=(int)floorf(p.y),iz=d3?(int)floorf(p.z):0;
    float local[3]={p.x-ix,p.y-iy,d3?p.z-iz:0},best[3]={1e6f,1e6f,1e6f};
    float feature[3]={0,0,0};
    for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y++)for(int z=d3?-1:0;z<=(d3?1:0);z++){
        float q[3]={x+(worley_cell_channel(ix+x,iy+y,iz+z,d3,0)-.5f)*jitter+.5f,
                    y+(worley_cell_channel(ix+x,iy+y,iz+z,d3,1)-.5f)*jitter+.5f,
                    d3?z+(worley_cell_channel(ix+x,iy+y,iz+z,d3,2)-.5f)*jitter+.5f:0};
        float dx=q[0]-local[0],dy=q[1]-local[1],dz=d3?q[2]-local[2]:0,dist=dx*dx+dy*dy+dz*dz;
        if(dist<best[0]){best[2]=best[1];best[1]=best[0];best[0]=dist;memcpy(feature,q,sizeof(q));}
        else if(dist<best[1]){best[2]=best[1];best[1]=dist;}else if(dist<best[2])best[2]=dist;
    }
    MtlxValue r=mv_zero(type);int nc=ncomp_of(&r);if(nc<1)nc=1;if(nc>3)nc=3;
    if(style==1){int hx=(int)floorf(feature[0]-local[0]+p.x),hy=(int)floorf(feature[1]-local[1]+p.y),hz=d3?(int)floorf(feature[2]-local[2]+p.z):0;for(int i=0;i<nc;i++)r.v[i]=worley_cell_channel(hx,hy,hz,d3,nc==1?-1:i);}
    else for(int i=0;i<nc;i++)r.v[i]=sqrtf(best[i]);
    return r;
}
/* MaterialX modulo is x-y*floor(x/y), unlike C fmod for negative UVs. */
static float pattern_mod(float x,float y){return x-y*floorf(x/y);}
static float pattern_shape(float x,float y,float radius,int kind){if(kind==2)return x*x+y*y<=radius*radius?1:0;if(kind==3){float sx=2*x,sy=2*y,dx[4]={sx+radius,sx-radius,sx,sx},dy[4]={sy,sy,sy-radius,sy+radius};for(int i=0;i<4;i++)if(dx[i]*dx[i]+dy[i]*dy[i]<=radius*radius)return 1;return 0;}float px=fabsf(y),py=fabsf(x),kx=-.866025f,ky=.5f,kz=.57735f,projection=fminf(kx*px+ky*py,0);px-=2*projection*kx;py-=2*projection*ky;px-=clampf(px,-kz*radius,kz*radius);py-=radius;return sqrtf(px*px+py*py)*(py<0?-1:1)<=0?1:0;}
static float pattern_value(float x,float y,float parameter,int staggered,int kind){if(kind<=1){if(staggered&&pattern_mod(y,2)>1)x+=.5f;float nx=2*pattern_mod(x,1)-1,ny=2*pattern_mod(y,1)-1;if(kind==0)return fabsf(nx)>1-parameter||fabsf(ny)>1-parameter?1:0;return fabsf(nx-ny)*.70710678f<=parameter||fabsf(nx+ny)*.70710678f<=parameter?1:0;}if(!staggered)return pattern_shape(2*pattern_mod(x,1)-1,2*pattern_mod(y,1)-1,parameter,kind);float half=kind==3?.5f:.866025f,shift=pattern_mod(y,2*half)>half?.5f:0,mx=pattern_mod(x+shift,1),my=pattern_mod(y,half),radius=parameter*.5f;return fmaxf(pattern_shape(mx,my,radius,kind),fmaxf(pattern_shape(1-mx,my,radius,kind),pattern_shape(mx-.5f,half-my,radius,kind)));}
static float randomfloat_value(float input, int seed, float lo, float hi) {
    uint32_t h = mx_hash_int2((int)floorf(input * 4096.0f), seed);
    float q = (float)h / (float)0xffffffffu;
    return lo + q * (hi - lo);
}
static int matrix_dim(MtlxType t){return t==MV_MATRIX33?3:(t==MV_MATRIX44?4:0);}
static float matrix_determinant(const MtlxValue *m){int n=matrix_dim(m->type);float a[4][4]={{0}},det=1;for(int r=0;r<n;r++)for(int c=0;c<n;c++)a[r][c]=m->v[c*n+r];for(int c=0;c<n;c++){int pivot=c;for(int r=c+1;r<n;r++)if(fabsf(a[r][c])>fabsf(a[pivot][c]))pivot=r;if(fabsf(a[pivot][c])<1e-12f)return 0;if(pivot!=c){for(int k=0;k<n;k++){float q=a[c][k];a[c][k]=a[pivot][k];a[pivot][k]=q;}det=-det;}float p=a[c][c];det*=p;for(int r=c+1;r<n;r++){float q=a[r][c]/p;for(int k=c+1;k<n;k++)a[r][k]-=q*a[c][k];}}return det;}
static MtlxValue matrix_inverse(MtlxValue m){int n=matrix_dim(m.type);float a[4][8]={{0}};for(int r=0;r<n;r++){for(int c=0;c<n;c++)a[r][c]=m.v[c*n+r];a[r][n+r]=1;}for(int c=0;c<n;c++){int pivot=c;for(int r=c+1;r<n;r++)if(fabsf(a[r][c])>fabsf(a[pivot][c]))pivot=r;if(fabsf(a[pivot][c])<1e-12f)return m;if(pivot!=c)for(int k=0;k<2*n;k++){float q=a[c][k];a[c][k]=a[pivot][k];a[pivot][k]=q;}float p=a[c][c];for(int k=0;k<2*n;k++)a[c][k]/=p;for(int r=0;r<n;r++)if(r!=c){float q=a[r][c];for(int k=0;k<2*n;k++)a[r][k]-=q*a[c][k];}}for(int r=0;r<n;r++)for(int c=0;c<n;c++)m.v[c*n+r]=a[r][n+c];return m;}

/* `lumacoeffs` when the node does not author it. The specification lets an
 * application supply the coefficients of its working color space in place of
 * the ACEScg default; this renderer works in linear Rec.709. */
static MtlxValue working_lumacoeffs(void) { return mv_color3(v3_make(0.2126f, 0.7152f, 0.0722f)); }

/* ---- node dispatch ---------------------------------------------------- */
typedef enum {
    OP_UNKNOWN = 0,
    OP_IMAGE, OP_HEXTILEDIMAGE, OP_NORMALMAP, OP_TEXCOORD, OP_POSITION, OP_NORMAL, OP_TANGENT,
    OP_BITANGENT, OP_GEOMCOLOR, OP_PLACE2D, OP_CONSTANT,
    OP_ADD, OP_SUBTRACT, OP_MULTIPLY, OP_DIVIDE, OP_MODULO, OP_POWER,
    OP_MIN, OP_MAX, OP_ATAN2,
    OP_SIN, OP_COS, OP_TAN, OP_ASIN, OP_ACOS, OP_ATAN, OP_SQRT, OP_LN, OP_EXP,
    OP_ABS, OP_FLOOR, OP_CEIL, OP_FRACT, OP_STEP, OP_ROUND, OP_SIGN,
    OP_INVERT, OP_NORMALIZE,
    OP_MAGNITUDE, OP_DOTPRODUCT, OP_CROSSPRODUCT,
    OP_MIX, OP_SCREEN, OP_OVERLAY, OP_BURN, OP_DODGE,
    OP_CLAMP, OP_SMOOTHSTEP, OP_REMAP, OP_LUMINANCE, OP_RGBTOHSV,
    OP_HSVTORGB, OP_HSVADJUST, OP_SATURATE, OP_CONTRAST, OP_RANGE,
    OP_SEPARATE, OP_COMBINE2, OP_COMBINE3, OP_COMBINE4, OP_EXTRACT, OP_CONVERT,
    OP_SWIZZLE, OP_NOISE2D, OP_NOISE3D, OP_FRACTAL2D, OP_FRACTAL3D, OP_CELLNOISE2D, OP_CELLNOISE3D,
    OP_WORLEYNOISE2D, OP_WORLEYNOISE3D,
    OP_UNIFIEDNOISE2D, OP_UNIFIEDNOISE3D,
    OP_RAMPLR, OP_RAMPTB, OP_SPLITLR, OP_SPLITTB,
    OP_IFGREATER, OP_IFGREATEREQ, OP_IFEQUAL, OP_SWITCH, OP_DOT, OP_RAMP4,
    OP_RAMP, OP_RAMP_GRADIENT, OP_CREATEMATRIX, OP_TRANSFORMMATRIX,
    OP_TRANSPOSE, OP_DETERMINANT, OP_INVERTMATRIX,
    OP_ROTATE2D, OP_ONEMINUS, OP_DISTANCE, OP_REFLECT, OP_REFRACT,
    OP_PREMULT, OP_UNPREMULT, OP_MINCOMPONENT, OP_MAXCOMPONENT,
    OP_AND, OP_OR, OP_XOR, OP_NOT, OP_INSIDE, OP_OUTSIDE, OP_TRIANGLEWAVE,
    OP_CHECKERBOARD, OP_DIFFERENCE, OP_PLUS, OP_MINUS, OP_SAFEPOWER, OP_IN, OP_MASK, OP_MATTE, OP_OUT, OP_OVER,
    OP_DISJOINTOVER, OP_CIRCLE, OP_LINE, OP_CLOVERLEAF, OP_HEXAGON,
    OP_GRID, OP_CROSSHATCH, OP_TILEDCIRCLES, OP_TILEDCLOVERLEAFS, OP_TILEDHEXAGONS,
    OP_COLORCORRECT, OP_BLUR, OP_FLAKE2D, OP_FLAKE3D, OP_RANDOMFLOAT,
    OP_RANDOMCOLOR, OP_LATLONGIMAGE, OP_TRIPLANARPROJECTION,
    OP_HEIGHTTONORMAL, OP_BUMP, OP_VIEWDIRECTION, OP_TIME, OP_FRAME,
    OP_TRANSFORMPOINT, OP_TRANSFORMVECTOR, OP_TRANSFORMNORMAL,
    OP_GEOMPROPVALUE, OP_BLACKBODY, OP_ROUGHNESS_ANISOTROPY,
    OP_ROUGHNESS_DUAL, OP_ARTISTIC_IOR, OP_GLOSSINESS_ANISOTROPY,
    OP_DEON_HAIR_ABSORPTION, OP_CHIANG_HAIR_ABSORPTION,
    OP_CHIANG_HAIR_ROUGHNESS
} NodeOp;

static NodeOp classify(const char *c) {
    /* textures */
    if (!strcmp(c, "hextiledimage")) return OP_HEXTILEDIMAGE;
    if (!strcmp(c, "image") || !strcmp(c, "tiledimage") ||
        !strcmp(c, "gltf_image") || !strcmp(c, "gltf_colorimage")) return OP_IMAGE;
    if (!strcmp(c, "normalmap") || !strcmp(c, "gltf_normalmap") ||
        !strcmp(c, "hextilednormalmap")) return OP_NORMALMAP;
    /* geometric */
    if (!strcmp(c, "texcoord") || !strcmp(c, "texcoord0") ||
        !strcmp(c, "texcoord1")) return OP_TEXCOORD;
    if (!strcmp(c, "position")) return OP_POSITION;
    if (!strcmp(c, "normal")) return OP_NORMAL;
    if (!strcmp(c, "tangent")) return OP_TANGENT;
    if (!strcmp(c, "bitangent")) return OP_BITANGENT;
    if (!strcmp(c, "geomcolor")) return OP_GEOMCOLOR;
    if (!strcmp(c, "place2d")) return OP_PLACE2D;
    if (!strcmp(c, "constant")) return OP_CONSTANT;
    /* math binary */
    if (!strcmp(c, "add")) return OP_ADD;
    if (!strcmp(c, "subtract")) return OP_SUBTRACT;
    if (!strcmp(c, "plus")) return OP_PLUS;
    if (!strcmp(c, "minus")) return OP_MINUS;
    if (!strcmp(c, "multiply")) return OP_MULTIPLY;
    if (!strcmp(c, "divide")) return OP_DIVIDE;
    if (!strcmp(c, "modulo")) return OP_MODULO;
    if (!strcmp(c, "power")) return OP_POWER;
    if (!strcmp(c, "safepower")) return OP_SAFEPOWER;
    if (!strcmp(c, "min") || !strcmp(c, "minimum")) return OP_MIN;
    if (!strcmp(c, "max") || !strcmp(c, "maximum")) return OP_MAX;
    if (!strcmp(c, "atan2") || !strcmp(c, "arctan2")) return OP_ATAN2;
    /* math unary */
    if (!strcmp(c, "sin")) return OP_SIN;
    if (!strcmp(c, "cos")) return OP_COS;
    if (!strcmp(c, "tan")) return OP_TAN;
    if (!strcmp(c, "asin")) return OP_ASIN;
    if (!strcmp(c, "acos")) return OP_ACOS;
    if (!strcmp(c, "atan")) return OP_ATAN;
    if (!strcmp(c, "sqrt")) return OP_SQRT;
    if (!strcmp(c, "ln") || !strcmp(c, "log") || !strcmp(c, "logarithm")) return OP_LN;
    if (!strcmp(c, "exp")) return OP_EXP;
    if (!strcmp(c, "absval") || !strcmp(c, "abs")) return OP_ABS;
    if (!strcmp(c, "floor")) return OP_FLOOR;
    if (!strcmp(c, "ceil") || !strcmp(c, "ceiling")) return OP_CEIL;
    if (!strcmp(c, "fract") || !strcmp(c, "fraction")) return OP_FRACT;
    if (!strcmp(c, "step")) return OP_STEP;
    if (!strcmp(c, "round")) return OP_ROUND;
    if (!strcmp(c, "sign")) return OP_SIGN;
    if (!strcmp(c, "invert")) return OP_INVERT;
    if (!strcmp(c, "normalize")) return OP_NORMALIZE;
    if (!strcmp(c, "magnitude") || !strcmp(c, "length")) return OP_MAGNITUDE;
    if (!strcmp(c, "dotproduct")) return OP_DOTPRODUCT;
    if (!strcmp(c, "crossproduct") || !strcmp(c, "cross")) return OP_CROSSPRODUCT;
    /* compositing / adjust */
    if (!strcmp(c, "mix")) return OP_MIX;
    if (!strcmp(c, "screen")) return OP_SCREEN;
    if (!strcmp(c, "overlay")) return OP_OVERLAY;
    if (!strcmp(c, "burn")) return OP_BURN;
    if (!strcmp(c, "dodge")) return OP_DODGE;
    if (!strcmp(c, "difference")) return OP_DIFFERENCE;
    if (!strcmp(c, "in")) return OP_IN;
    if (!strcmp(c, "mask")) return OP_MASK;
    if (!strcmp(c, "matte")) return OP_MATTE;
    if (!strcmp(c, "out")) return OP_OUT;
    if (!strcmp(c, "over")) return OP_OVER;
    if (!strcmp(c, "disjointover")) return OP_DISJOINTOVER;
    if (!strcmp(c, "clamp")) return OP_CLAMP;
    if (!strcmp(c, "smoothstep")) return OP_SMOOTHSTEP;
    if (!strcmp(c, "remap")) return OP_REMAP;
    if (!strcmp(c, "luminance")) return OP_LUMINANCE;
    if (!strcmp(c, "rgbtohsv")) return OP_RGBTOHSV;
    if (!strcmp(c, "hsvtorgb")) return OP_HSVTORGB;
    if (!strcmp(c, "hsvadjust")) return OP_HSVADJUST;
    if (!strcmp(c, "saturate")) return OP_SATURATE;
    if (!strcmp(c, "contrast")) return OP_CONTRAST;
    if (!strcmp(c, "range")) return OP_RANGE;
    /* channel */
    if (!strcmp(c, "separate2") || !strcmp(c, "separate3") || !strcmp(c, "separate4")) return OP_SEPARATE;
    if (!strcmp(c, "combine2")) return OP_COMBINE2;
    if (!strcmp(c, "combine3")) return OP_COMBINE3;
    if (!strcmp(c, "combine4")) return OP_COMBINE4;
    if (!strcmp(c, "extract")) return OP_EXTRACT;
    if (!strcmp(c, "convert") || !strncmp(c, "convert_", 8)) return OP_CONVERT;
    if (!strcmp(c, "swizzle")) return OP_SWIZZLE;
    /* procedural */
    if (!strcmp(c, "noise2d")) return OP_NOISE2D;
    if (!strcmp(c, "noise3d")) return OP_NOISE3D;
    if (!strcmp(c, "fractal2d")) return OP_FRACTAL2D;
    if (!strcmp(c, "fractal3d")) return OP_FRACTAL3D;
    if (!strcmp(c, "cellnoise2d")) return OP_CELLNOISE2D;
    if (!strcmp(c, "cellnoise3d")) return OP_CELLNOISE3D;
    if (!strcmp(c, "worleynoise2d")) return OP_WORLEYNOISE2D;
    if (!strcmp(c, "worleynoise3d")) return OP_WORLEYNOISE3D;
    if (!strcmp(c, "unifiednoise2d")) return OP_UNIFIEDNOISE2D;
    if (!strcmp(c, "unifiednoise3d")) return OP_UNIFIEDNOISE3D;
    if (!strcmp(c, "ramplr")) return OP_RAMPLR;
    if (!strcmp(c, "ramptb")) return OP_RAMPTB;
    if (!strcmp(c, "splitlr")) return OP_SPLITLR;
    if (!strcmp(c, "splittb")) return OP_SPLITTB;
    if (!strcmp(c, "ramp4")) return OP_RAMP4;
    if (!strcmp(c, "ramp")) return OP_RAMP;
    if (!strcmp(c, "ramp_gradient")) return OP_RAMP_GRADIENT;
    if (!strcmp(c, "creatematrix")) return OP_CREATEMATRIX;
    if (!strcmp(c, "transformmatrix")) return OP_TRANSFORMMATRIX;
    if (!strcmp(c, "transpose")) return OP_TRANSPOSE;
    if (!strcmp(c, "determinant")) return OP_DETERMINANT;
    if (!strcmp(c, "invertmatrix")) return OP_INVERTMATRIX;
    /* conditional / utility */
    if (!strcmp(c, "ifgreater")) return OP_IFGREATER;
    if (!strcmp(c, "ifgreatereq")) return OP_IFGREATEREQ;
    if (!strcmp(c, "ifequal")) return OP_IFEQUAL;
    if (!strcmp(c, "switch")) return OP_SWITCH;
    if (!strcmp(c, "dot")) return OP_DOT;
    if (!strcmp(c, "rotate2d")) return OP_ROTATE2D;
    if (!strcmp(c, "oneminus")) return OP_ONEMINUS;
    if (!strcmp(c, "distance")) return OP_DISTANCE;
    if (!strcmp(c, "reflect")) return OP_REFLECT;
    if (!strcmp(c, "refract")) return OP_REFRACT;
    if (!strcmp(c, "premult")) return OP_PREMULT;
    if (!strcmp(c, "unpremult")) return OP_UNPREMULT;
    if (!strcmp(c, "mincomponent")) return OP_MINCOMPONENT;
    if (!strcmp(c, "maxcomponent")) return OP_MAXCOMPONENT;
    if (!strcmp(c, "and")) return OP_AND;
    if (!strcmp(c, "or")) return OP_OR;
    if (!strcmp(c, "xor")) return OP_XOR;
    if (!strcmp(c, "not")) return OP_NOT;
    if (!strcmp(c, "inside")) return OP_INSIDE;
    if (!strcmp(c, "outside")) return OP_OUTSIDE;
    if (!strcmp(c, "trianglewave")) return OP_TRIANGLEWAVE;
    if (!strcmp(c, "checkerboard")) return OP_CHECKERBOARD;
    if (!strcmp(c, "circle")) return OP_CIRCLE;
    if (!strcmp(c, "line")) return OP_LINE;
    if (!strcmp(c, "cloverleaf")) return OP_CLOVERLEAF;
    if (!strcmp(c, "hexagon")) return OP_HEXAGON;
    if (!strcmp(c, "grid")) return OP_GRID;
    if (!strcmp(c, "crosshatch")) return OP_CROSSHATCH;
    if (!strcmp(c, "tiledcircles")) return OP_TILEDCIRCLES;
    if (!strcmp(c, "tiledcloverleafs")) return OP_TILEDCLOVERLEAFS;
    if (!strcmp(c, "tiledhexagons")) return OP_TILEDHEXAGONS;
    if (!strcmp(c, "colorcorrect")) return OP_COLORCORRECT;
    if (!strcmp(c, "blur")) return OP_BLUR;
    if (!strcmp(c, "flake2d")) return OP_FLAKE2D;
    if (!strcmp(c, "flake3d")) return OP_FLAKE3D;
    if (!strcmp(c, "randomfloat")) return OP_RANDOMFLOAT;
    if (!strcmp(c, "randomcolor")) return OP_RANDOMCOLOR;
    if (!strcmp(c, "latlongimage")) return OP_LATLONGIMAGE;
    if (!strcmp(c, "triplanarprojection")) return OP_TRIPLANARPROJECTION;
    if (!strcmp(c, "heighttonormal")) return OP_HEIGHTTONORMAL;
    if (!strcmp(c, "bump")) return OP_BUMP;
    if (!strcmp(c, "viewdirection") || !strcmp(c, "viewdir")) return OP_VIEWDIRECTION;
    if (!strcmp(c, "time")) return OP_TIME;
    if (!strcmp(c, "frame")) return OP_FRAME;
    if (!strcmp(c, "transformpoint")) return OP_TRANSFORMPOINT;
    if (!strcmp(c, "transformvector")) return OP_TRANSFORMVECTOR;
    if (!strcmp(c, "transformnormal")) return OP_TRANSFORMNORMAL;
    if (!strcmp(c, "geompropvalue") || !strcmp(c, "geompropvalueuniform"))
        return OP_GEOMPROPVALUE;
    if (!strcmp(c,"blackbody")) return OP_BLACKBODY;
    if (!strcmp(c,"roughness_anisotropy")) return OP_ROUGHNESS_ANISOTROPY;
    if (!strcmp(c,"roughness_dual")) return OP_ROUGHNESS_DUAL;
    if (!strcmp(c,"artistic_ior")) return OP_ARTISTIC_IOR;
    if (!strcmp(c,"glossiness_anisotropy")) return OP_GLOSSINESS_ANISOTROPY;
    if (!strcmp(c,"deon_hair_absorption_from_melanin")) return OP_DEON_HAIR_ABSORPTION;
    if (!strcmp(c,"chiang_hair_absorption_from_color")) return OP_CHIANG_HAIR_ABSORPTION;
    if (!strcmp(c,"chiang_hair_roughness")) return OP_CHIANG_HAIR_ROUGHNESS;
    return OP_UNKNOWN;
}

static MtlxValue eval_node(ShadeContext *ctx, int node_id);
static MtlxValue eval_flake_output(ShadeContext *ctx,const MtlxNode *n,
                                   const char *output);
static MtlxValue eval_artistic_ior(ShadeContext *ctx,const MtlxNode *n,
                                   int extinction);
static float in_float(ShadeContext *ctx, const MtlxNode *n, const char *name,
                      float fb);

static const MtlxInput *find_input(const MtlxNode *n, const char *name) {
    for (int i = 0; i < n->ninput; i++)
        if (!strcmp(n->inputs[i].name, name)) return &n->inputs[i];
    return NULL;
}

/* Apply a sub-output channel selection (separate3.outr, etc.). */
static MtlxValue swizzle_out(MtlxValue v, const char *out) {
    if (!out || !out[0] || !strcmp(out, "out")) return v;
    int idx = -1;
    if (!strcmp(out, "outr") || !strcmp(out, "outx")) idx = 0;
    else if (!strcmp(out, "outg") || !strcmp(out, "outy")) idx = 1;
    else if (!strcmp(out, "outb") || !strcmp(out, "outz")) idx = 2;
    else if (!strcmp(out, "outa") || !strcmp(out, "outw")) idx = 3;
    if (idx >= 0) return mv_float(v.v[idx]);
    return v;
}

static int channel_index(char c) {
    switch (c) {
        case 'r': case 'x': return 0;
        case 'g': case 'y': return 1;
        case 'b': case 'z': return 2;
        case 'a': case 'w': return 3;
        default: return -1;
    }
}

static MtlxValue swizzle_channels(MtlxValue v, const char *channels, MtlxType type) {
    if (!channels || !channels[0]) return v;

    float out[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int n = 0;
    for (; n < 4 && channels[n]; n++) {
        int idx = channel_index(channels[n]);
        out[n] = (idx >= 0) ? v.v[idx] : 0.0f;
    }
    if (n <= 1) return mv_float(out[0]);
    if (n == 2) return mv_vec2(out[0], out[1]);
    if (n == 3) {
        return (type == MV_VEC3) ? mv_vec3(v3_make(out[0], out[1], out[2]))
                                 : mv_color3(v3_make(out[0], out[1], out[2]));
    }
    {
        MtlxValue r = mv_zero((type == MV_VEC4) ? MV_VEC4 : MV_COLOR4);
        r.v[0] = out[0]; r.v[1] = out[1]; r.v[2] = out[2]; r.v[3] = out[3];
        return r;
    }
}

static MtlxValue eval_input(ShadeContext *ctx, const MtlxInput *in) {
    MtlxValue r;
    if (!in) return mv_zero(MV_NONE);
    if (in->src_node >= 0) {
        const MtlxNode *src=&ctx->doc->nodes[in->src_node];
        if ((!strcmp(src->category,"flake2d")||!strcmp(src->category,"flake3d")) &&
            in->src_output)
            r=eval_flake_output(ctx,src,in->src_output);
        else if(!strcmp(src->category,"artistic_ior")&&in->src_output)
            r=eval_artistic_ior(ctx,src,!strcmp(in->src_output,"extinction"));
        else if(!strcmp(src->category,"chiang_hair_roughness")&&in->src_output){
            r=eval_node(ctx,in->src_node);
            const char *scale_name =
                !strcmp(in->src_output,"roughness_TT") ? "scale_TT" :
                !strcmp(in->src_output,"roughness_TRT") ? "scale_TRT" : NULL;
            if(scale_name){
                const MtlxInput *scale_input=find_input(src,scale_name);
                float scale = !strcmp(scale_name,"scale_TT") ? 0.5f : 2.0f;
                if(scale_input){MtlxValue sv=eval_input(ctx,scale_input);
                    scale=mv_as_float(&sv);}
                r.v[0]*=scale*scale;
            }
        }
        else r=swizzle_out(eval_node(ctx,in->src_node),in->src_output);
    }
    else if (in->has_value) r = in->value;
    else r = mv_zero(in->type);
    return swizzle_channels(r, in->channels, in->type);
}
static MtlxValue in_or(ShadeContext *ctx, const MtlxNode *n, const char *name, MtlxValue dflt) {
    const MtlxInput *in = find_input(n, name);
    return in ? eval_input(ctx, in) : dflt;
}

static MtlxValue eval_flake_output(ShadeContext *ctx,const MtlxNode *n,
                                   const char *output){
    MtlxValue sv=in_or(ctx,n,"size",mv_float(.01f)),rv=in_or(ctx,n,"roughness",mv_float(.1f)),cv=in_or(ctx,n,"coverage",mv_float(.5f));
    int d3=!strcmp(n->category,"flake3d");MtlxValue pv=in_or(ctx,n,d3?"position":"texcoord",d3?mv_vec3(ctx->P):mv_vec2(ctx->uv[0],ctx->uv[1]));
    v3 pos=v3_make(pv.v[0],pv.v[1],d3?pv.v[2]:0),normal;
    MtlxValue nv=in_or(ctx,n,"normal",mv_vec3(ctx->Ns)),tv=in_or(ctx,n,"tangent",mv_vec3(v3_normalize(ctx->dpdu))),bv=in_or(ctx,n,"bitangent",mv_vec3(v3_normalize(ctx->dpdv)));
    normal=mv_as_v3(&nv);v3 tangent=mv_as_v3(&tv),bitangent=mv_as_v3(&bv);
    float size=fmaxf(fabsf(sv.v[0]),1e-8f),coverage=clampf(cv.v[0],0,1),xx=coverage*coverage;
    float probability=(-26.19771808f*xx+26.39663835f*coverage)/(85.53857017f*xx*coverage-102.35069432f*xx-101.42634862f*coverage+118.45082288f);
    v3 p=v3_scale(pos,1/size);int bx=(int)floorf(p.x),by=(int)floorf(p.y),bz=(int)floorf(p.z),cx=0,cy=0,cz=0;float priority=0,diameter=.86602540378f;
    for(int i=-1;i<=1;i++)for(int j=-1;j<=1;j++)for(int k=-1;k<=1;k++){
        int x=bx+i,y=by+j,z=bz+k;v3 q=v3_make(p.x-x-.5f,p.y-y-.5f,p.z-z-.5f);if(v3_dot(q,q)>=diameter*diameter*3)continue;
        if((float)mx_hash_int3(x,y,z)/(float)0xffffffffu>probability)continue;float candidate=mx_cell_vec3(x,y,z,3,1).x;if(candidate<priority)continue;
        v3 rot=mx_cell_vec3(x,y,z,0,0);float theta=2*(float)MTLX_PI*rot.x,phi=2*(float)MTLX_PI*rot.y,zz=2*rot.z,rr=sqrtf(zz),vx=sinf(phi)*rr,vy=cosf(phi)*rr,vz=sqrtf(2-zz),s=sinf(theta),c=cosf(theta),sx=vx*c-vy*s,sy=vx*s+vy*c;
        v3 qr=v3_make((vx*sx-c)*q.x+(vy*sx+s)*q.y+vz*sx*q.z,(vx*sy-s)*q.x+(vy*sy-c)*q.y+vz*sy*q.z,vx*vz*q.x+vy*vz*q.y+(1-zz)*q.z);
        if(fabsf(qr.x)<=diameter&&fabsf(qr.y)<=diameter&&fabsf(qr.z)<=diameter){priority=candidate;cx=x;cy=y;cz=z;}
    }
    if(priority<=0){if(!strcmp(output,"flakenormal"))return mv_vec3(normal);return mv_float(0);}
    v3 noise=mx_cell_vec3(cx,cy,cz,2,1);if(!strcmp(output,"id")){MtlxValue q=mv_zero(MV_INT);q.v[0]=floorf(noise.z*16777215);return q;}if(!strcmp(output,"rand"))return mv_float(noise.z);if(!strcmp(output,"presence"))return mv_float(priority);
    float phi=2*(float)MTLX_PI*noise.x,tan_theta=rv.v[0]*rv.v[0]*sqrtf(noise.y)/sqrtf(fmaxf(1-noise.y,1e-8f)),sin_theta=tan_theta/sqrtf(1+tan_theta*tan_theta),cos_theta=sqrtf(fmaxf(1-sin_theta*sin_theta,0));
    return mv_vec3(v3_normalize(v3_add(v3_add(v3_scale(tangent,cosf(phi)*sin_theta),v3_scale(bitangent,sinf(phi)*sin_theta)),v3_scale(normal,cos_theta))));
}

static MtlxValue eval_image(ShadeContext *ctx, const MtlxNode *n) {
    const MtlxInput *file = find_input(n, "file");
    int srgb = file ? file->colorspace_srgb : 0;
    const char *path = (file && file->value.s) ? file->value.s : NULL;
    /* honor an explicit texcoord input (e.g. from place2d) */
    float u = ctx->uv[0], v = ctx->uv[1];
    const MtlxInput *tc = find_input(n, "texcoord");
    if (tc && tc->src_node >= 0) { MtlxValue t = eval_input(ctx, tc); u = t.v[0]; v = t.v[1]; }
    float dudx = ctx->uv_dx[0], dvdx = ctx->uv_dx[1];
    float dudy = ctx->uv_dy[0], dvdy = ctx->uv_dy[1];
    if (!strcmp(n->category, "tiledimage")) {
        MtlxValue tiling = in_or(ctx, n, "uvtiling", mv_vec2(1, 1));
        MtlxValue offset = in_or(ctx, n, "uvoffset", mv_vec2(0, 0));
        MtlxValue image_size = in_or(ctx, n, "realworldimagesize", mv_vec2(1, 1));
        MtlxValue tile_size = in_or(ctx, n, "realworldtilesize", mv_vec2(1, 1));
        const float scale_u = tiling.v[0] / image_size.v[0] * tile_size.v[0];
        const float scale_v = tiling.v[1] / image_size.v[1] * tile_size.v[1];
        u = (u * tiling.v[0] - offset.v[0]) / image_size.v[0] * tile_size.v[0];
        v = (v * tiling.v[1] - offset.v[1]) / image_size.v[1] * tile_size.v[1];
        dudx *= scale_u; dudy *= scale_u;
        dvdx *= scale_v; dvdy *= scale_v;
    }
    float s[4];
    const MtlxInput *us = find_input(n, "uaddressmode");
    const MtlxInput *vs = find_input(n, "vaddressmode");
    const MtlxInput *filter = find_input(n, "filtertype");
    const char *filtertype = filter && filter->has_value && filter->value.s
                                 ? filter->value.s : "linear";
    const char *u_mode = us && us->has_value && us->value.s
                             ? us->value.s : "periodic";
    const char *v_mode = vs && vs->has_value && vs->value.s
                             ? vs->value.s : "periodic";
    /* MaterialX constant addressing returns this image node's typed default,
     * not a renderer-wide border color. Coordinates on the domain boundary
     * remain sampleable. */
    const int outside_constant =
        (!isfinite(u) || !isfinite(v)) ||
        ((!strcmp(u_mode, "constant")) && (u < 0.0f || u > 1.0f)) ||
        ((!strcmp(v_mode, "constant")) && (v < 0.0f || v > 1.0f));
    int sampled = 0;
    if (!outside_constant && path && !strstr(path, "<UDIM>")) {
        int id = texcache_get(ctx->tex, path, srgb);
        if (id >= 0) {
            if (ctx->has_uv_derivatives) {
                texcache_sample_address_grad_filter(ctx->tex, id, u, v,
                    dudx, dvdx, dudy, dvdy,
                    u_mode, v_mode, filtertype, s);
            } else {
                texcache_sample_address_filter(ctx->tex, id, u, v,
                    u_mode, v_mode, filtertype, s);
            }
            sampled = 1;
        }
    } else if (!outside_constant && path) {
        if (ctx->has_uv_derivatives) {
            sampled = texcache_sample_file_grad_filter(
                ctx->tex, path, srgb, u, v, ctx->uv_dx[0], ctx->uv_dx[1],
                ctx->uv_dy[0], ctx->uv_dy[1], filtertype, s);
        } else {
            sampled = texcache_sample_file_filter(ctx->tex, path, srgb,
                                                  u, v, filtertype, s);
        }
    }
    if (!sampled) { MtlxValue d = in_or(ctx, n, "default", mv_zero(n->type)); v3 dc = mv_as_v3(&d); s[0]=dc.x; s[1]=dc.y; s[2]=dc.z; s[3]=1; }
    switch (n->type) {
        case MV_FLOAT: return mv_float(s[0]);
        case MV_VEC2: return mv_vec2(s[0], s[1]);
        case MV_VEC3: return mv_vec3(v3_make(s[0], s[1], s[2]));
        case MV_COLOR4: { MtlxValue r = mv_zero(MV_COLOR4); r.v[0]=s[0];r.v[1]=s[1];r.v[2]=s[2];r.v[3]=s[3]; return r; }
        default: return mv_color3(v3_make(s[0], s[1], s[2]));
    }
}

static MtlxValue eval_latlongimage(ShadeContext *ctx, const MtlxNode *n) {
    const MtlxInput *file = find_input(n, "file");
    int srgb = file ? file->colorspace_srgb : 0;
    const char *path = (file && file->value.s) ? file->value.s : NULL;
    /* MaterialX's latlongimage defaults to the shading view direction. Keep a
     * deterministic +Z fallback for callers that do not populate the context
     * (standalone graph tests and malformed hit records). */
    v3 default_view = v3_len(ctx->V) > 1.0e-20f
        ? v3_normalize(ctx->V) : v3_make(0, 0, 1);
    MtlxValue vd = in_or(ctx, n, "viewdir", mv_vec3(default_view));
    MtlxValue rotation = in_or(ctx, n, "rotation", mv_float(0));
    float u = atan2f(vd.v[0], vd.v[2]) * -0.15915494f + 0.5f +
              rotation.v[0] * 0.00277778f;
    float v = asinf(clampf(vd.v[1], -1.0f, 1.0f)) * 0.31830989f + 0.5f;
    /* The stdlib requests periodic U and mirrored V.  Latitude is in [0,1]
     * for finite view directions; texcache's periodic sampling provides the
     * longitude seam behavior. */
    int id = path ? texcache_get(ctx->tex, path, srgb) : -1;
    float s[4];
    const MtlxInput *filter = find_input(n, "filtertype");
    const char *filtertype = filter && filter->has_value && filter->value.s
                                 ? filter->value.s : "linear";
    if (id >= 0) {
        if (ctx->has_view_derivatives) {
            const float x = vd.v[0], y = vd.v[1], z = vd.v[2];
            const float xz2 = fmaxf(x * x + z * z, 1.0e-8f);
            const float yden = fmaxf(sqrtf(fmaxf(1.0f - y * y, 1.0e-8f)),
                                     1.0e-4f);
            const float dudx = -(z * ctx->dVdx.x - x * ctx->dVdx.z) /
                               (2.0f * (float)MTLX_PI * xz2);
            const float dudy = -(z * ctx->dVdy.x - x * ctx->dVdy.z) /
                               (2.0f * (float)MTLX_PI * xz2);
            const float dvdx = ctx->dVdx.y / ((float)MTLX_PI * yden);
            const float dvdy = ctx->dVdy.y / ((float)MTLX_PI * yden);
            texcache_sample_address_grad_filter(ctx->tex, id, u, v, dudx, dvdx,
                dudy, dvdy, "periodic", "mirror", filtertype, s);
        }
        else texcache_sample_address_filter(ctx->tex, id, u, v,
            "periodic", "mirror", filtertype, s);
    }
    else {
        MtlxValue d = in_or(ctx, n, "default", mv_zero(MV_COLOR3));
        s[0] = d.v[0]; s[1] = d.v[1]; s[2] = d.v[2]; s[3] = 1.0f;
    }
    return mv_color3(v3_make(s[0], s[1], s[2]));
}

static MtlxValue eval_triplanarprojection(ShadeContext *ctx,
                                          const MtlxNode *n) {
    MtlxValue pv=in_or(ctx,n,"position",mv_vec3(ctx->P));
    MtlxValue nv=in_or(ctx,n,"normal",mv_vec3(ctx->Ns));
    MtlxValue bv=in_or(ctx,n,"blend",mv_float(.5f));
    MtlxValue uv=in_or(ctx,n,"upaxis",mv_float(1));
    v3 p=mv_as_v3(&pv),nn=v3_normalize(mv_as_v3(&nv));
    float w[3]={fabsf(nn.x),fabsf(nn.y),fabsf(nn.z)};
    float sum=w[0]+w[1]+w[2];if(sum<=1e-20f){w[0]=w[1]=0;w[2]=1;}else for(int i=0;i<3;i++)w[i]/=sum;
    float exponent=1.0f/fmaxf(bv.v[0],.03f);sum=0;for(int i=0;i<3;i++){w[i]=powf(w[i],exponent);sum+=w[i];}if(sum>1e-20f)for(int i=0;i<3;i++)w[i]/=sum;
    float tc[3][2]={{p.y,p.z},{p.x,p.z},{p.x,p.y}};
    float dtx[3][2]={{ctx->dPdx.y,ctx->dPdx.z},
                     {ctx->dPdx.x,ctx->dPdx.z},
                     {ctx->dPdx.x,ctx->dPdx.y}};
    float dty[3][2]={{ctx->dPdy.y,ctx->dPdy.z},
                     {ctx->dPdy.x,ctx->dPdy.z},
                     {ctx->dPdy.x,ctx->dPdy.y}};
    int up=(int)uv.v[0];
    if(up!=2){tc[0][0]=p.z;tc[0][1]=p.y;dtx[0][0]=ctx->dPdx.z;dtx[0][1]=ctx->dPdx.y;dty[0][0]=ctx->dPdy.z;dty[0][1]=ctx->dPdy.y;}
    if(up==0){tc[1][0]=p.z;tc[1][1]=p.x;tc[2][0]=-p.y;tc[2][1]=p.x;dtx[1][0]=ctx->dPdx.z;dtx[1][1]=ctx->dPdx.x;dtx[2][0]=-ctx->dPdx.y;dtx[2][1]=ctx->dPdx.x;dty[1][0]=ctx->dPdy.z;dty[1][1]=ctx->dPdy.x;dty[2][0]=-ctx->dPdy.y;dty[2][1]=ctx->dPdy.x;}
    MtlxValue sample[3];const char *files[3]={"filex","filey","filez"};
    const MtlxInput *filter=find_input(n,"filtertype");
    const char *filtertype=filter&&filter->has_value&&filter->value.s
                               ?filter->value.s:"linear";
    for(int axis=0;axis<3;axis++){
        const MtlxInput *file=find_input(n,files[axis]);const char *path=file&&file->value.s?file->value.s:NULL;int id=path?texcache_get(ctx->tex,path,file?file->colorspace_srgb:0):-1;float s[4];
        if(id>=0){
            if(ctx->has_position_derivatives)
                texcache_sample_address_grad_filter(ctx->tex,id,tc[axis][0],tc[axis][1],dtx[axis][0],dtx[axis][1],dty[axis][0],dty[axis][1],"periodic","periodic",filtertype,s);
            else texcache_sample_address_filter(ctx->tex,id,tc[axis][0],tc[axis][1],"periodic","periodic",filtertype,s);
        }else{MtlxValue d=in_or(ctx,n,"default",mv_zero(n->type));s[0]=d.v[0];s[1]=d.v[1];s[2]=d.v[2];s[3]=n->type==MV_COLOR4||n->type==MV_VEC4?d.v[3]:1;}
        sample[axis]=mv_zero(n->type);int nc=ncomp_of(&sample[axis]);if(nc<1)nc=1;for(int c=0;c<nc;c++)sample[axis].v[c]=s[c];
    }
    MtlxValue r=sample[0];int nc=ncomp_of(&r);if(nc<1)nc=1;for(int c=0;c<nc;c++)r.v[c]=sample[0].v[c]*w[0]+sample[1].v[c]*w[1]+sample[2].v[c]*w[2];return r;
}

/* ---- hex-tiling (Mikkelsen, Practical Real-Time Hex-Tiling, JCGT 2022) ---
 * Blend three randomly rotated/scaled/offset lookups of a tiled texture by the
 * simplex-grid barycentric weights, modulated by per-tap luminance, so the
 * obvious repetition of a plain tiled texture is hidden. Ported from MaterialX
 * mx_hextile.glsl / mx_hextiledimage.glsl. Derivatives (textureGrad) are dropped
 * -- they only affect mip selection, not the blend. */
static float hex_fract(float x) { return x - floorf(x); }
static void hex_hash(float px, float py, float *ox, float *oy) {
    float p3x = hex_fract(px * 0.1031f), p3y = hex_fract(py * 0.1030f), p3z = hex_fract(px * 0.0973f);
    float d = p3x * (p3y + 33.33f) + p3y * (p3z + 33.33f) + p3z * (p3x + 33.33f);
    p3x += d; p3y += d; p3z += d;
    *ox = hex_fract((p3x + p3y) * p3z);
    *oy = hex_fract((p3x + p3z) * p3y);
}
static float hex_schlick_gain(float x, float r) {
    float rr = clampf(r, 0.001f, 0.999f);
    float a = (1.0f / rr - 2.0f) * (1.0f - 2.0f * x);
    return (x < 0.5f) ? x / (a + 1.0f) : (a - x) / (a - 1.0f);
}
static float hin_f(ShadeContext *ctx, const MtlxNode *n, const char *nm, float fb) {
    MtlxValue v = in_or(ctx, n, nm, mv_float(fb));
    return mv_as_float(&v);
}
static MtlxValue eval_hextiledimage(ShadeContext *ctx, const MtlxNode *n) {
    const MtlxInput *file = find_input(n, "file");
    int srgb = file ? file->colorspace_srgb : 0;
    const char *path = (file && file->value.s) ? file->value.s : NULL;
    int id = path ? texcache_get(ctx->tex, path, srgb) : -1;
    if (id < 0) return in_or(ctx, n, "default", mv_zero(n->type));
    const MtlxInput *filter = find_input(n, "filtertype");
    const char *filtertype = filter && filter->has_value && filter->value.s
                                 ? filter->value.s : "linear";

    MtlxValue tv = in_or(ctx, n, "tiling", mv_vec2(1, 1));
    float tile_x = tv.v[0], tile_y = (tv.v[1] != 0.0f ? tv.v[1] : tv.v[0]);
    MtlxValue rr = in_or(ctx, n, "rotationrange", mv_vec2(0.0f, 360.0f));
    MtlxValue sr = in_or(ctx, n, "scalerange", mv_vec2(0.5f, 2.0f));
    MtlxValue orr = in_or(ctx, n, "offsetrange", mv_vec2(0.0f, 1.0f));
    float rotation = hin_f(ctx, n, "rotation", 1.0f);
    float scale = hin_f(ctx, n, "scale", 1.0f);
    float offset = hin_f(ctx, n, "offset", 1.0f);
    float falloff = hin_f(ctx, n, "falloff", 0.5f);
    float fcontrast = hin_f(ctx, n, "falloffcontrast", 0.5f);
    MtlxValue lv = in_or(ctx, n, "lumacoeffs", mv_color3(v3_make(0.2722287f, 0.6740818f, 0.0536895f)));
    v3 luma = mv_as_v3(&lv);

    float coord_x = ctx->uv[0] * tile_x, coord_y = ctx->uv[1] * tile_y;
    const float sqrt3_2 = 3.46410162f;
    /* skew into the simplex grid: to_skewed * (coord*sqrt3_2) */
    float stx = coord_x * sqrt3_2, sty = coord_y * sqrt3_2;
    float skx = stx - 0.57735027f * sty, sky = 1.15470054f * sty;
    float fracx = hex_fract(skx), fracy = hex_fract(sky);
    float tz = 1.0f - fracx - fracy;
    float s = (tz <= 0.0f) ? 1.0f : 0.0f, s2 = 2.0f * s - 1.0f;
    float w[3];
    w[0] = -tz * s2; w[1] = s - fracy * s2; w[2] = s - fracx * s2;
    int basex = (int)floorf(skx), basey = (int)floorf(sky), si = (int)s;
    int idx[3] = { basex + si, basex + si, basex + (1 - si) };
    int idy[3] = { basey + si, basey + (1 - si), basey + si };
    float cx[3], cy[3], local_cr[3], local_srn[3], local_sc[3];
    for (int i = 0; i < 3; i++) {
        /* tile center: inv_skewed * (id / sqrt3_2) */
        float ax = idx[i] / sqrt3_2, ay = idy[i] / sqrt3_2;
        float ctrx = ax + 0.5f * ay, ctry = 0.866025404f * ay;
        float rx, ry; hex_hash((float)idx[i] + 0.12345f, (float)idy[i] + 0.12345f, &rx, &ry);
        float rad0 = rr.v[0] * (float)MTLX_PI / 180.0f, rad1 = rr.v[1] * (float)MTLX_PI / 180.0f;
        float rot = rad0 + (rad1 - rad0) * (rx * rotation);
        float sc = 1.0f + ((sr.v[0] + (sr.v[1] - sr.v[0]) * ry) - 1.0f) * scale;
        float offx = orr.v[0] + (orr.v[1] - orr.v[0]) * (rx * offset);
        float offy = orr.v[0] + (orr.v[1] - orr.v[0]) * (ry * offset);
        float dx = coord_x - ctrx, dy = coord_y - ctry;
        float cr = cosf(rot), srn = sinf(rot);
        local_cr[i] = cr; local_srn[i] = srn; local_sc[i] = sc;
        cx[i] = (dx * cr - dy * srn) / sc + ctrx + offx;
        cy[i] = (dx * srn + dy * cr) / sc + ctry + offy;
    }
    float c[3][4];
    for (int i = 0; i < 3; i++) {
        if (ctx->has_uv_derivatives) {
            /* The transform is affine per tap; derive its Jacobian from the
             * same rotation and scale used to construct this lookup. */
            float dcxdu = tile_x * local_cr[i] / local_sc[i];
            float dcxdv = -tile_y * local_srn[i] / local_sc[i];
            float dcydu = tile_x * local_srn[i] / local_sc[i];
            float dcydv = tile_y * local_cr[i] / local_sc[i];
            texcache_sample_address_grad_filter(
                ctx->tex, id, cx[i], cy[i],
                dcxdu * ctx->uv_dx[0] + dcxdv * ctx->uv_dx[1],
                dcydu * ctx->uv_dx[0] + dcydv * ctx->uv_dx[1],
                dcxdu * ctx->uv_dy[0] + dcxdv * ctx->uv_dy[1],
                dcydu * ctx->uv_dy[0] + dcydv * ctx->uv_dy[1],
                "periodic", "periodic", filtertype, c[i]);
        } else {
            texcache_sample_address_filter(ctx->tex, id, cx[i], cy[i],
                                           "periodic", "periodic", filtertype, c[i]);
        }
    }
    /* blend weights: luminance(mix to 1 by contrast) * barycentric^7, normalized */
    float bw[3], sum = 1e-8f;
    for (int i = 0; i < 3; i++) {
        float cwi = c[i][0] * luma.x + c[i][1] * luma.y + c[i][2] * luma.z;
        cwi = 1.0f + (cwi - 1.0f) * fcontrast;
        bw[i] = cwi * powf(maxf(w[i], 0.0f), 7.0f);
        sum += bw[i];
    }
    for (int i = 0; i < 3; i++) bw[i] /= sum;
    if (falloff != 0.5f) {
        float s3 = 1e-8f;
        for (int i = 0; i < 3; i++) { bw[i] = hex_schlick_gain(bw[i], falloff); s3 += bw[i]; }
        for (int i = 0; i < 3; i++) bw[i] /= s3;
    }
    float o[4];
    for (int k = 0; k < 4; k++) o[k] = bw[0] * c[0][k] + bw[1] * c[1][k] + bw[2] * c[2][k];
    switch (n->type) {
        case MV_FLOAT: return mv_float(o[0]);
        case MV_VEC2: return mv_vec2(o[0], o[1]);
        case MV_VEC3: return mv_vec3(v3_make(o[0], o[1], o[2]));
        case MV_COLOR4: { MtlxValue r = mv_zero(MV_COLOR4); r.v[0]=o[0];r.v[1]=o[1];r.v[2]=o[2];r.v[3]=o[3]; return r; }
        default: return mv_color3(v3_make(o[0], o[1], o[2]));
    }
}

static MtlxValue eval_normalmap(ShadeContext *ctx, const MtlxNode *n) {
    /* `normalmap` takes an `in` tangent-space vector (usually from an image),
     * but the gltf convenience node `gltf_normalmap` embeds the texture `file`
     * directly. Sample it like an image node in that case, else no normal map
     * is ever applied for gltf assets. */
    const MtlxInput *inp = find_input(n, "in");
    MtlxValue in;
    if (inp && (inp->src_node >= 0 || inp->has_value)) in = eval_input(ctx, inp);
    else if (find_input(n, "file")) in = eval_image(ctx, n);
    else in = mv_vec3(v3_make(0.5f, 0.5f, 1.0f)); /* flat tangent-space normal */
    v3 t = mv_as_v3(&in);
    v3 ts = v3_make(2 * t.x - 1, 2 * t.y - 1, 2 * t.z - 1);
    const MtlxInput *scin = find_input(n, "scale");
    if (scin) { MtlxValue s = eval_input(ctx, scin); float sc = mv_as_float(&s); ts.x *= sc; ts.y *= sc; }
    v3 N = v3_normalize(ctx->Ns);
    v3 T = v3_sub(ctx->dpdu, v3_scale(N, v3_dot(N, ctx->dpdu))), B;
    if (!v3_is_finite(T) || v3_len(T) < 1e-6f) onb(N, &T, &B);
    else { T = v3_normalize(T); B = v3_cross(N, T); }
    v3 w = v3_normalize(v3_add(v3_add(v3_scale(T, ts.x), v3_scale(B, ts.y)), v3_scale(N, ts.z)));
    return mv_vec3(v3_is_finite(w) ? w : N);
}

static MtlxValue eval_height_normal(ShadeContext *ctx,const MtlxNode *n,
                                    const char *height_name,float scale){
    const MtlxInput *height=find_input(n,height_name);
    if(!height||height->src_node<0)return mv_vec3(v3_make(.5f,.5f,1));
    float old_u=ctx->uv[0],old_v=ctx->uv[1],eps=1.0f/1024.0f,h[4];
    const float du[4]={-eps,eps,0,0},dv[4]={0,0,-eps,eps};
    for(int i=0;i<4;i++){MtlxValue sample;ctx->uv[0]=old_u+du[i];ctx->uv[1]=old_v+dv[i];memset(ctx->memo_done,0,(size_t)ctx->doc->nnode);sample=eval_input(ctx,height);h[i]=mv_as_float(&sample);}
    ctx->uv[0]=old_u;ctx->uv[1]=old_v;memset(ctx->memo_done,0,(size_t)ctx->doc->nnode);
    float dx=(h[1]-h[0])*.5f*scale/16.0f,dy=(h[3]-h[2])*.5f*scale/16.0f;v3 q=v3_normalize(v3_make(-dx,-dy,1));return mv_vec3(v3_scale(v3_add(q,v3_make(1,1,1)),.5f));
}

static const char *string_input(const MtlxNode *n,const char *name,
                                const char *fallback){
    const MtlxInput *in=find_input(n,name);
    return in&&in->has_value&&in->value.s?in->value.s:fallback;
}

static v3 transform_affine(const float m[16],v3 p,float w){
    return v3_make(m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12]*w,
                   m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13]*w,
                   m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]*w);
}

static v3 transform_covector(const float inverse[16],v3 n){
    return v3_make(inverse[0]*n.x+inverse[1]*n.y+inverse[2]*n.z,
                   inverse[4]*n.x+inverse[5]*n.y+inverse[6]*n.z,
                   inverse[8]*n.x+inverse[9]*n.y+inverse[10]*n.z);
}

static v3 transform_space(ShadeContext *ctx,v3 value,const char *from,
                          const char *to,int kind){
    if(!ctx->has_space_transforms||!strcmp(from,to))return value;
    v3 world=value;
    if(!strcmp(from,"object"))world=kind==2?
        transform_covector(ctx->world_to_object,value):
        transform_affine(ctx->object_to_world,value,kind==0?1.0f:0.0f);
    else if(!strcmp(from,"view"))world=kind==2?
        transform_covector(ctx->world_to_view,value):
        transform_affine(ctx->view_to_world,value,kind==0?1.0f:0.0f);
    v3 result=world;
    if(!strcmp(to,"object"))result=kind==2?
        transform_covector(ctx->object_to_world,world):
        transform_affine(ctx->world_to_object,world,kind==0?1.0f:0.0f);
    else if(!strcmp(to,"view"))result=kind==2?
        transform_covector(ctx->view_to_world,world):
        transform_affine(ctx->world_to_view,world,kind==0?1.0f:0.0f);
    return kind==2?v3_normalize(result):result;
}

static v3 eval_blackbody(float kelvin){
    float temperature=fminf(fmaxf(kelvin,800),25000),t=1000/temperature,
          t2=t*t,t3=t2*t;
    float x=temperature<4000?
        -0.2661239f*t3-0.2343580f*t2+0.8776956f*t+0.179910f:
        -3.0258469f*t3+2.1070379f*t2+0.2226347f*t+0.240390f;
    float x2=x*x,x3=x2*x,y=temperature<2222?
        -1.1063814f*x3-1.34811020f*x2+2.18555832f*x-0.20219683f:
        (temperature<4000?-0.9549476f*x3-1.37418593f*x2+2.09137015f*x-0.16748867f:
         3.0817580f*x3-5.87338670f*x2+3.75112997f*x-0.37001483f);
    if(y<=0)return v3_make(1,1,1);float X=x/y,Z=(1-x-y)/y;
    return v3_make(fmaxf(3.2406f*X-1.5372f-.4986f*Z,0),
                   fmaxf(-.9689f*X+1.8758f+.0415f*Z,0),
                   fmaxf(.0557f*X-.2040f+1.0570f*Z,0));
}

static MtlxValue eval_artistic_ior(ShadeContext *ctx,const MtlxNode *n,
                                   int extinction){
    MtlxValue rv=in_or(ctx,n,"reflectivity",mv_color3(v3_make(.944f,.776f,.373f))),
              ev=in_or(ctx,n,"edge_color",mv_color3(v3_make(1,1,1))),out=mv_zero(MV_COLOR3);
    for(int lane=0;lane<3;lane++){float q=fminf(fmaxf(rv.v[lane],0),.99f),s=sqrtf(q),nmin=(1-q)/(1+q),nmax=(1+s)/(1-s),ior=nmax+(nmin-nmax)*ev.v[lane],k2=fmaxf(((ior+1)*(ior+1)*q-(ior-1)*(ior-1))/(1-q),0);out.v[lane]=extinction?sqrtf(k2):ior;}
    return out;
}

static MtlxValue eval_node(ShadeContext *ctx, int node_id) {
    if (node_id < 0 || node_id >= ctx->doc->nnode) return mv_zero(MV_NONE);
    if (ctx->memo_done[node_id]) return ctx->memo[node_id];
    ctx->memo_done[node_id] = 1;
    ctx->memo[node_id] = mv_zero(MV_NONE); /* cycle sentinel */

    const MtlxNode *n = &ctx->doc->nodes[node_id];
    MtlxValue a, b, r = mv_zero(n->type);
    /* nodegraph interface input (category "input"): forward its single
     * self-input (a literal value or an upstream connection). */
    if (!strcmp(n->category, "input")) {
        r = (n->ninput > 0) ? eval_input(ctx, &n->inputs[0]) : mv_zero(n->type);
        ctx->memo[node_id] = r;
        return r;
    }
    NodeOp node_op=classify(n->category);
    switch (node_op) {
        case OP_IMAGE: r = eval_image(ctx, n); break;
        case OP_LATLONGIMAGE: r = eval_latlongimage(ctx, n); break;
        case OP_TRIPLANARPROJECTION: r=eval_triplanarprojection(ctx,n); break;
        case OP_HEIGHTTONORMAL: {MtlxValue s=in_or(ctx,n,"scale",mv_float(1));r=eval_height_normal(ctx,n,"in",s.v[0]);break;}
        case OP_BUMP: {MtlxValue s=in_or(ctx,n,"scale",mv_float(1)),enc=eval_height_normal(ctx,n,"height",1),nval=in_or(ctx,n,"normal",mv_vec3(ctx->Ns)),tval=in_or(ctx,n,"tangent",mv_vec3(v3_normalize(ctx->dpdu))),bval=in_or(ctx,n,"bitangent",mv_vec3(v3_normalize(ctx->dpdv)));v3 ts=v3_make(enc.v[0]*2-1,enc.v[1]*2-1,enc.v[2]*2-1),nn=mv_as_v3(&nval),tt=mv_as_v3(&tval),bb=mv_as_v3(&bval);ts.x*=s.v[0];ts.y*=s.v[0];r=mv_vec3(v3_normalize(v3_add(v3_add(v3_scale(tt,ts.x),v3_scale(bb,ts.y)),v3_scale(nn,ts.z))));break;}
        case OP_VIEWDIRECTION: r=mv_vec3(v3_normalize(ctx->V));break;
        case OP_TIME: r=mv_float(ctx->time);break;
        case OP_FRAME: r=mv_float(ctx->frame);break;
        case OP_TRANSFORMPOINT:
        case OP_TRANSFORMVECTOR:
        case OP_TRANSFORMNORMAL: {a=in_or(ctx,n,"in",mv_zero(MV_VEC3));int kind=node_op==OP_TRANSFORMPOINT?0:(node_op==OP_TRANSFORMVECTOR?1:2);r=mv_vec3(transform_space(ctx,mv_as_v3(&a),string_input(n,"fromspace","object"),string_input(n,"tospace","world"),kind));break;}
        case OP_GEOMPROPVALUE: {const char *name=string_input(n,"geomprop","");const MtlxInput *fallback=find_input(n,"default");r=fallback?eval_input(ctx,fallback):mv_zero(n->type);if(ctx->geomprop&&name[0]){MtlxValue q;if(ctx->geomprop(ctx->geomprop_user,name,n->type,&q))r=q;}break;}
        case OP_BLACKBODY: {a=in_or(ctx,n,"temperature",mv_float(5000));r=mv_color3(eval_blackbody(a.v[0]));break;}
        case OP_ROUGHNESS_ANISOTROPY: {a=in_or(ctx,n,"roughness",mv_float(0));b=in_or(ctx,n,"anisotropy",mv_float(0));float q=fminf(fmaxf(a.v[0]*a.v[0],1e-8f),1),aspect=b.v[0]>0?sqrtf(1-fminf(fmaxf(b.v[0],0),.98f)):1;r=mv_vec2(fminf(q/aspect,1),q*aspect);break;}
        case OP_ROUGHNESS_DUAL: {a=in_or(ctx,n,"roughness",mv_vec2(0,-1));float y=a.v[1]<0?a.v[0]:a.v[1];r=mv_vec2(fminf(fmaxf(a.v[0]*a.v[0],1e-8f),1),fminf(fmaxf(y*y,1e-8f),1));break;}
        case OP_ARTISTIC_IOR: r=eval_artistic_ior(ctx,n,0);break;
        case OP_GLOSSINESS_ANISOTROPY: {a=in_or(ctx,n,"glossiness",mv_float(1));b=in_or(ctx,n,"anisotropy",mv_float(0));float rough=1-a.v[0],q=fminf(fmaxf(rough*rough,1e-8f),1),aspect=b.v[0]>0?sqrtf(1-fminf(fmaxf(b.v[0],0),.98f)):1;r=mv_vec2(fminf(q/aspect,1),q*aspect);break;}
        case OP_DEON_HAIR_ABSORPTION: {MtlxValue concentration=in_or(ctx,n,"melanin_concentration",mv_float(.25f)),redness=in_or(ctx,n,"melanin_redness",mv_float(.5f)),eu=in_or(ctx,n,"eumelanin_color",mv_color3(v3_make(.657704f,.498077f,.254107f))),ph=in_or(ctx,n,"pheomelanin_color",mv_color3(v3_make(.829444f,.67032f,.349938f)));r=mv_zero(MV_VEC3);for(int lane=0;lane<3;lane++)r.v[lane]=concentration.v[0]*((1-redness.v[0])*-logf(fmaxf(eu.v[lane],1e-6f))+redness.v[0]*-logf(fmaxf(ph.v[lane],1e-6f)));break;}
        case OP_CHIANG_HAIR_ABSORPTION: {a=in_or(ctx,n,"color",mv_color3(v3_make(1,1,1)));b=in_or(ctx,n,"azimuthal_roughness",mv_float(.2f));float beta=b.v[0],b2=beta*beta,b3=b2*beta,b4=b3*beta,b5=b4*beta,den=5.969f-.215f*beta+2.532f*b2-10.73f*b3+5.574f*b4+.245f*b5;r=mv_zero(MV_VEC3);for(int lane=0;lane<3;lane++){float q=logf(fmaxf(a.v[lane],1e-6f))/fmaxf(fabsf(den),1e-6f);r.v[lane]=q*q;}break;}
        case OP_CHIANG_HAIR_ROUGHNESS: {
            const float longitudinal = clampf(
                in_float(ctx, n, "longitudinal", 0.1f), 0.0f, 1.0f);
            const float azimuthal = clampf(
                in_float(ctx, n, "azimuthal", 0.2f), 0.0f, 1.0f);
            const float scale_tt = in_float(ctx, n, "scale_TT", 0.5f);
            const float scale_trt = in_float(ctx, n, "scale_TRT", 2.0f);
            const float l2 = longitudinal * longitudinal;
            const float l20 = powf(longitudinal, 20.0f);
            const float a2 = azimuthal * azimuthal;
            const float a22 = powf(azimuthal, 22.0f);
            const float variance = powf(0.726f * longitudinal +
                                            0.812f * l2 + 3.7f * l20, 2.0f);
            const float logistic = 0.265f * azimuthal +
                                   1.194f * a2 + 5.372f * a22;
            /* The test API exposes roughness_R; graph connections select the
             * TT/TRT outputs through the same multi-output convention. */
            r = mv_vec2(variance, logistic);
            break;
        }
        case OP_HEXTILEDIMAGE: r = eval_hextiledimage(ctx, n); break;
        case OP_NORMALMAP: r = eval_normalmap(ctx, n); break;
        case OP_TEXCOORD: r = mv_vec2(ctx->uv[0], ctx->uv[1]); break;
        case OP_POSITION: r = mv_vec3(ctx->P); break;
        case OP_NORMAL: r = mv_vec3(ctx->Ns); break;
        case OP_TANGENT: r = mv_vec3(v3_normalize(ctx->dpdu)); break;
        case OP_BITANGENT: r = mv_vec3(v3_normalize(ctx->dpdv)); break;
        case OP_GEOMCOLOR: r = mv_color3(v3_splat(1.0f)); break;
        case OP_CONSTANT: r = in_or(ctx, n, "value", mv_zero(n->type)); break;
        case OP_PLACE2D: {
            MtlxValue tc = in_or(ctx, n, "texcoord", mv_vec2(ctx->uv[0], ctx->uv[1]));
            MtlxValue piv = in_or(ctx, n, "pivot", mv_vec2(0, 0));
            MtlxValue scl = in_or(ctx, n, "scale", mv_vec2(1, 1));
            MtlxValue off = in_or(ctx, n, "offset", mv_vec2(0, 0));
            MtlxValue rot = in_or(ctx, n, "rotate", mv_float(0));
            float x = tc.v[0] - piv.v[0], y = tc.v[1] - piv.v[1];
            float ang = rot.v[0] * (MTLX_PI / 180.0f), cs = cosf(ang), sn = sinf(ang);
            float rx = x * cs - y * sn, ry = x * sn + y * cs;
            rx = rx / (scl.v[0] != 0 ? scl.v[0] : 1) + piv.v[0] + off.v[0];
            ry = ry / (scl.v[1] != 0 ? scl.v[1] : 1) + piv.v[1] + off.v[1];
            r = mv_vec2(rx, ry); break;
        }
        case OP_ADD: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(0)); r = binop(a,b,f_add); break;
        case OP_SUBTRACT: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(0)); r = binop(a,b,f_sub); break;
        case OP_MULTIPLY: {
            a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(1));
            int dim = matrix_dim(a.type);
            if (dim && b.type == a.type) {
                /* Matrix values hold the numbers of the value string in order
                 * (row-major, MaterialX.Specification "Matrix types"); the
                 * node is the standard product in1 * in2 of those matrices. */
                r = mv_zero(a.type);
                for (int i = 0; i < dim; i++) for (int j = 0; j < dim; j++) {
                    float acc = 0.0f;
                    for (int k = 0; k < dim; k++) acc += a.v[i*dim+k] * b.v[k*dim+j];
                    r.v[i*dim+j] = acc;
                }
            } else r = binop(a,b,f_mul);
            break;
        }
        case OP_DIVIDE: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(1)); r = binop(a,b,f_div); break;
        case OP_MODULO: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(1)); r = binop(a,b,f_mod); break;
        case OP_POWER: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(1)); r = binop(a,b,f_pow); break;
        case OP_SAFEPOWER: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(1)); r = binop(a,b,f_safepow); break;
        case OP_MIN: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(0)); r = binop(a,b,f_min); break;
        case OP_MAX: a = in_or(ctx,n,"in1",mv_zero(n->type)); b = in_or(ctx,n,"in2",mv_float(0)); r = binop(a,b,f_max); break;
        case OP_ATAN2: { /* MaterialX names the inputs iny / inx */ int std=find_input(n,"iny")||find_input(n,"inx"); a = in_or(ctx,n,std?"iny":"in1",mv_float(0)); b = in_or(ctx,n,std?"inx":"in2",mv_float(1)); r = binop(a,b,f_atan2); break; }
        case OP_SIN: r = unop(in_or(ctx,n,"in",mv_float(0)), u_sin); break;
        case OP_COS: r = unop(in_or(ctx,n,"in",mv_float(0)), u_cos); break;
        case OP_TAN: r = unop(in_or(ctx,n,"in",mv_float(0)), u_tan); break;
        case OP_ASIN: r = unop(in_or(ctx,n,"in",mv_float(0)), u_asin); break;
        case OP_ACOS: r = unop(in_or(ctx,n,"in",mv_float(0)), u_acos); break;
        case OP_ATAN: r = unop(in_or(ctx,n,"in",mv_float(0)), u_atan); break;
        case OP_SQRT: r = unop(in_or(ctx,n,"in",mv_float(0)), u_sqrt); break;
        case OP_LN: r = unop(in_or(ctx,n,"in",mv_float(0)), u_ln); break;
        case OP_EXP: r = unop(in_or(ctx,n,"in",mv_float(0)), u_exp); break;
        case OP_ABS: r = unop(in_or(ctx,n,"in",mv_float(0)), u_abs); break;
        case OP_FLOOR: r = unop(in_or(ctx,n,"in",mv_float(0)), u_floor); break;
        case OP_CEIL: r = unop(in_or(ctx,n,"in",mv_float(0)), u_ceil); break;
        case OP_FRACT: { a=in_or(ctx,n,"in",mv_float(0)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++) r.v[i]=a.v[i]-floorf(a.v[i]); break; }
        case OP_STEP: { a=in_or(ctx,n,"in",mv_float(0)); b=in_or(ctx,n,"edge",mv_float(0)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++){float edge=ncomp_of(&b)==1?b.v[0]:b.v[i];r.v[i]=a.v[i]<edge?0.0f:1.0f;} break; }
        case OP_ROUND: r = unop(in_or(ctx,n,"in",mv_float(0)), u_round); break;
        case OP_SIGN: r = unop(in_or(ctx,n,"in",mv_float(0)), u_sign); break;
        case OP_INVERT: { a = in_or(ctx,n,"in",mv_float(0)); MtlxValue amt = in_or(ctx,n,"amount",mv_float(1)); int nc=ncomp_of(&a); for(int i=0;i<nc;i++) a.v[i]=(nc==ncomp_of(&amt)?amt.v[i]:amt.v[0])-a.v[i]; r=a; break; }
        case OP_NORMALIZE: { a = in_or(ctx,n,"in",mv_zero(MV_VEC3)); int nc=ncomp_of(&a); float l2=0; for(int i=0;i<nc;i++) l2+=a.v[i]*a.v[i]; float inv=l2>0?1.0f/sqrtf(l2):0.0f; r=a; for(int i=0;i<nc;i++) r.v[i]*=inv; break; }
        case OP_MAGNITUDE: { a = in_or(ctx,n,"in",mv_zero(MV_VEC3)); int nc=ncomp_of(&a); float l2=0; for(int i=0;i<nc;i++) l2+=a.v[i]*a.v[i]; r = mv_float(sqrtf(l2)); break; }
        case OP_DOTPRODUCT: { a = in_or(ctx,n,"in1",mv_zero(MV_VEC3)); b = in_or(ctx,n,"in2",mv_zero(MV_VEC3)); int nc=ncomp_of(&a); float acc=0.0f; for(int i=0;i<nc;i++) acc+=a.v[i]*b.v[i]; r = mv_float(acc); break; }
        case OP_CROSSPRODUCT: a = in_or(ctx,n,"in1",mv_zero(MV_VEC3)); b = in_or(ctx,n,"in2",mv_zero(MV_VEC3)); r = mv_vec3(v3_cross(mv_as_v3(&a), mv_as_v3(&b))); break;
        case OP_MIX: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(n->type)), bg=in_or(ctx,n,"bg",mv_zero(n->type)), m=in_or(ctx,n,"mix",mv_float(0)); int nc=ncomp_of(&fg); r=fg; for(int i=0;i<nc;i++){ float t=(ncomp_of(&m)==1)?m.v[0]:m.v[i]; r.v[i]=bg.v[i]*(1-t)+fg.v[i]*t; } break; }
        case OP_SCREEN: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(n->type)),bg=in_or(ctx,n,"bg",mv_zero(n->type)),m=in_or(ctx,n,"mix",mv_float(1)); int nc=ncomp_of(&fg); r=bg; for(int i=0;i<nc;i++){ float t=ncomp_of(&m)==1?m.v[0]:m.v[i],v=1-(1-fg.v[i])*(1-bg.v[i]); r.v[i]=bg.v[i]+t*(v-bg.v[i]); } break; }
        case OP_OVERLAY: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(n->type)),bg=in_or(ctx,n,"bg",mv_zero(n->type)),m=in_or(ctx,n,"mix",mv_float(1)); int nc=ncomp_of(&fg); r=bg; for(int i=0;i<nc;i++){ float t=ncomp_of(&m)==1?m.v[0]:m.v[i],v=bg.v[i]>=0.5f?1-2*(1-fg.v[i])*(1-bg.v[i]):2*fg.v[i]*bg.v[i]; r.v[i]=bg.v[i]+t*(v-bg.v[i]); } break; }
        case OP_BURN: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(n->type)),bg=in_or(ctx,n,"bg",mv_zero(n->type)),m=in_or(ctx,n,"mix",mv_float(1)); int nc=ncomp_of(&fg); r=bg; for(int i=0;i<nc;i++){ float t=ncomp_of(&m)==1?m.v[0]:m.v[i]; r.v[i]=fabsf(fg.v[i])<1e-6f?0:t*(1-(1-bg.v[i])/fg.v[i])+(1-t)*bg.v[i]; } break; }
        case OP_DODGE: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(n->type)),bg=in_or(ctx,n,"bg",mv_zero(n->type)),m=in_or(ctx,n,"mix",mv_float(1)); int nc=ncomp_of(&fg); r=bg; for(int i=0;i<nc;i++){ float t=ncomp_of(&m)==1?m.v[0]:m.v[i]; r.v[i]=fabsf(1-fg.v[i])<1e-6f?0:t*(bg.v[i]/(1-fg.v[i]))+(1-t)*bg.v[i]; } break; }
        /* Blend nodes: mix between bg (mix=0) and the operator result (mix=1). */
        case OP_PLUS: case OP_MINUS: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(n->type)),bg=in_or(ctx,n,"bg",mv_zero(n->type)),m=in_or(ctx,n,"mix",mv_float(1));int nc=ncomp_of(&fg);r=bg;for(int i=0;i<nc;i++){float t=ncomp_of(&m)==1?m.v[0]:m.v[i],q=node_op==OP_PLUS?bg.v[i]+fg.v[i]:bg.v[i]-fg.v[i];r.v[i]=bg.v[i]+t*(q-bg.v[i]);}break; }
        case OP_DIFFERENCE: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(n->type)),bg=in_or(ctx,n,"bg",mv_zero(n->type)),m=in_or(ctx,n,"mix",mv_float(1));int nc=ncomp_of(&fg);r=bg;for(int i=0;i<nc;i++){float t=ncomp_of(&m)==1?m.v[0]:m.v[i],q=fabsf(fg.v[i]-bg.v[i]);r.v[i]=bg.v[i]+t*(q-bg.v[i]);}break; }
        case OP_IN: case OP_MASK: case OP_MATTE: case OP_OUT: case OP_OVER: case OP_DISJOINTOVER: { MtlxValue fg=in_or(ctx,n,"fg",mv_zero(MV_COLOR4)),bg=in_or(ctx,n,"bg",mv_zero(MV_COLOR4)),m=in_or(ctx,n,"mix",mv_float(1)),q=mv_zero(MV_COLOR4);NodeOp op=classify(n->category);if(op==OP_IN){for(int i=0;i<4;i++)q.v[i]=fg.v[i]*bg.v[3];}else if(op==OP_MASK){for(int i=0;i<4;i++)q.v[i]=bg.v[i]*fg.v[3];}else if(op==OP_OUT){for(int i=0;i<4;i++)q.v[i]=fg.v[i]*(1-bg.v[3]);}else if(op==OP_DISJOINTOVER){float s=fg.v[3]+bg.v[3];if(s<=1){for(int i=0;i<3;i++)q.v[i]=fg.v[i]+bg.v[i];}else if(fabsf(bg.v[3])>=1e-6f){float x=(1-fg.v[3])/bg.v[3];for(int i=0;i<3;i++)q.v[i]=fg.v[i]+bg.v[i]*x;}q.v[3]=fminf(s,1);}else{for(int i=0;i<3;i++)q.v[i]=(op==OP_MATTE?fg.v[i]*fg.v[3]:fg.v[i])+bg.v[i]*(1-fg.v[3]);q.v[3]=fg.v[3]+bg.v[3]*(1-fg.v[3]);}r=bg;for(int i=0;i<4;i++){float t=ncomp_of(&m)==1?m.v[0]:m.v[i];r.v[i]=bg.v[i]+t*(q.v[i]-bg.v[i]);}break; }
        case OP_CLAMP: { a=in_or(ctx,n,"in",mv_float(0)); MtlxValue lo=in_or(ctx,n,"low",mv_float(0)), hi=in_or(ctx,n,"high",mv_float(1)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++){ float l=(ncomp_of(&lo)==1)?lo.v[0]:lo.v[i], h=(ncomp_of(&hi)==1)?hi.v[0]:hi.v[i]; r.v[i]=clampf(a.v[i],l,h);} break; }
        case OP_SMOOTHSTEP: { a=in_or(ctx,n,"in",mv_float(0)); MtlxValue lo=in_or(ctx,n,"low",mv_float(0)), hi=in_or(ctx,n,"high",mv_float(1)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++){ float l=(ncomp_of(&lo)==1)?lo.v[0]:lo.v[i], h=(ncomp_of(&hi)==1)?hi.v[0]:hi.v[i]; float t=clampf((a.v[i]-l)/(h-l!=0?h-l:1),0,1); r.v[i]=t*t*(3-2*t);} break; }
        case OP_REMAP: { a=in_or(ctx,n,"in",mv_float(0)); MtlxValue il=in_or(ctx,n,"inlow",mv_float(0)),ih=in_or(ctx,n,"inhigh",mv_float(1)),ol=in_or(ctx,n,"outlow",mv_float(0)),oh=in_or(ctx,n,"outhigh",mv_float(1)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++){ float l0=ncomp_of(&il)==1?il.v[0]:il.v[i],h0=ncomp_of(&ih)==1?ih.v[0]:ih.v[i],l1=ncomp_of(&ol)==1?ol.v[0]:ol.v[i],h1=ncomp_of(&oh)==1?oh.v[0]:oh.v[i]; float t=(a.v[i]-l0)/((h0-l0)!=0?h0-l0:1); r.v[i]=l1+t*(h1-l1);} break; }
        case OP_LUMINANCE: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR3)); MtlxValue k=in_or(ctx,n,"lumacoeffs",working_lumacoeffs()); float l=a.v[0]*k.v[0]+a.v[1]*k.v[1]+a.v[2]*k.v[2]; r=a; r.v[0]=r.v[1]=r.v[2]=l; break; }
        case OP_RGBTOHSV: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR3)); v3 q=rgb_to_hsv(mv_as_v3(&a)); r=a; r.v[0]=q.x;r.v[1]=q.y;r.v[2]=q.z; break; } /* alpha kept */
        case OP_HSVTORGB: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR3)); v3 q=hsv_to_rgb(mv_as_v3(&a)); r=a; r.v[0]=q.x;r.v[1]=q.y;r.v[2]=q.z; break; }
        case OP_HSVADJUST: { a=in_or(ctx,n,"in",mv_zero(n->type));MtlxValue amount=in_or(ctx,n,"amount",mv_vec3(v3_make(0,1,1)));v3 hsv=rgb_to_hsv(mv_as_v3(&a));hsv.x=hsv.x+amount.v[0];hsv.y=hsv.y*amount.v[1];hsv.z=hsv.z*amount.v[2];v3 rgb=hsv_to_rgb(hsv);r=a;r.v[0]=rgb.x;r.v[1]=rgb.y;r.v[2]=rgb.z;break; }
        case OP_SATURATE: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR3)); MtlxValue am=in_or(ctx,n,"amount",mv_float(1)),k=in_or(ctx,n,"lumacoeffs",working_lumacoeffs()); float l=a.v[0]*k.v[0]+a.v[1]*k.v[1]+a.v[2]*k.v[2]; r=a; for(int i=0;i<3;i++) r.v[i]=l+(a.v[i]-l)*am.v[0]; break; }
        case OP_CONTRAST: { a=in_or(ctx,n,"in",mv_zero(n->type)); MtlxValue am=in_or(ctx,n,"amount",mv_float(1)),pv=in_or(ctx,n,"pivot",mv_float(0.5f)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++){ float am_i=ncomp_of(&am)==1?am.v[0]:am.v[i],pv_i=ncomp_of(&pv)==1?pv.v[0]:pv.v[i]; r.v[i]=(a.v[i]-pv_i)*am_i+pv_i; } break; }
        case OP_RANGE: { a=in_or(ctx,n,"in",mv_zero(n->type)); MtlxValue il=in_or(ctx,n,"inlow",mv_float(0)),ih=in_or(ctx,n,"inhigh",mv_float(1)),ol=in_or(ctx,n,"outlow",mv_float(0)),oh=in_or(ctx,n,"outhigh",mv_float(1)),g=in_or(ctx,n,"gamma",mv_float(1)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++){ float t=clampf((a.v[i]-il.v[0])/((ih.v[0]-il.v[0])!=0?ih.v[0]-il.v[0]:1),0,1); t=powf(t,g.v[0]); r.v[i]=ol.v[0]+t*(oh.v[0]-ol.v[0]);} break; }
        case OP_SEPARATE: r = in_or(ctx, n, "in", mv_zero(MV_VEC3)); break; /* consumer swizzles */
        case OP_COMBINE2: { a=in_or(ctx,n,"in1",mv_float(0)); b=in_or(ctx,n,"in2",mv_float(0)); if(n->type==MV_VEC2||n->type==MV_NONE){ r=mv_vec2(a.v[0],b.v[0]); } else { r=mv_zero(n->type); int na=ncomp_of(&a),nb=ncomp_of(&b),k=0; for(int i=0;i<na&&k<4;i++) r.v[k++]=a.v[i]; for(int i=0;i<nb&&k<4;i++) r.v[k++]=b.v[i]; } break; }
        case OP_COMBINE3: { MtlxValue i1=in_or(ctx,n,"in1",mv_float(0)),i2=in_or(ctx,n,"in2",mv_float(0)),i3=in_or(ctx,n,"in3",mv_float(0)); r=(n->type==MV_VEC3)?mv_vec3(v3_make(i1.v[0],i2.v[0],i3.v[0])):mv_color3(v3_make(i1.v[0],i2.v[0],i3.v[0])); break; }
        case OP_COMBINE4: { MtlxValue i1=in_or(ctx,n,"in1",mv_float(0)),i2=in_or(ctx,n,"in2",mv_float(0)),i3=in_or(ctx,n,"in3",mv_float(0)),i4=in_or(ctx,n,"in4",mv_float(0)); r=mv_zero(MV_COLOR4); r.v[0]=i1.v[0];r.v[1]=i2.v[0];r.v[2]=i3.v[0];r.v[3]=i4.v[0]; break; }
        case OP_EXTRACT: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR3)); MtlxValue idx=in_or(ctx,n,"index",mv_float(0)); int i=(int)(idx.v[0]+0.5f); r=mv_float(a.v[i&3]); break; }
        case OP_CONVERT: {
            /* A scalar is copied to every channel; otherwise the leading
             * channels are copied, missing ones are 0 and a missing fourth
             * channel is 1 (MaterialX.StandardNodes "convert"). */
            a=in_or(ctx,n,"in",mv_zero(MV_COLOR3)); r=mv_zero(n->type);
            int ns=ncomp_of(&a), nd=ncomp_of(&r);
            if (n->type==MV_NONE) { r=a; break; }
            for (int i=0;i<nd;i++) r.v[i] = ns==1 ? a.v[0] : (i<ns ? a.v[i] : (i==3 ? 1.0f : 0.0f));
            break;
        }
        case OP_SWIZZLE: { const MtlxInput *ch=find_input(n,"channels"); const char *s=(ch && ch->has_value && ch->value.s)?ch->value.s:NULL; a=in_or(ctx,n,"in",mv_zero(n->type)); r=swizzle_channels(a,s,n->type); break; }
        case OP_NOISE2D: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),amp=in_or(ctx,n,"amplitude",mv_float(1)),piv=in_or(ctx,n,"pivot",mv_float(0));r=mv_zero(n->type);int nc=ncomp_of(&r);if(nc<1)nc=1;if(nc==1)r.v[0]=vnoise2(tc.v[0],tc.v[1],-1)*amp.v[0]+piv.v[0];else if(nc==2){r.v[0]=vnoise2(tc.v[0],tc.v[1],-1)*amp.v[0]+piv.v[0];r.v[1]=vnoise2(tc.v[0]+19,tc.v[1]+193,-1)*(ncomp_of(&amp)==1?amp.v[0]:amp.v[1])+(ncomp_of(&piv)==1?piv.v[0]:piv.v[1]);}else{for(int i=0;i<3;i++)r.v[i]=vnoise2(tc.v[0],tc.v[1],i)*(ncomp_of(&amp)==1?amp.v[0]:amp.v[i])+(ncomp_of(&piv)==1?piv.v[0]:piv.v[i]);if(nc==4)r.v[3]=vnoise2(tc.v[0]+19,tc.v[1]+193,-1)*(ncomp_of(&amp)==1?amp.v[0]:amp.v[3])+(ncomp_of(&piv)==1?piv.v[0]:piv.v[3]);}break; }
        case OP_NOISE3D: { MtlxValue pos=in_or(ctx,n,"position",mv_vec3(ctx->P)),amp=in_or(ctx,n,"amplitude",mv_float(1)),piv=in_or(ctx,n,"pivot",mv_float(0));v3 p=mv_as_v3(&pos);r=mv_zero(n->type);int nc=ncomp_of(&r);if(nc<1)nc=1;if(nc==1)r.v[0]=vnoise(p)*amp.v[0]+piv.v[0];else if(nc==2){r.v[0]=vnoise(p)*amp.v[0]+piv.v[0];r.v[1]=vnoise(v3_add(p,v3_make(19,193,17)))*(ncomp_of(&amp)==1?amp.v[0]:amp.v[1])+(ncomp_of(&piv)==1?piv.v[0]:piv.v[1]);}else{for(int i=0;i<3;i++)r.v[i]=vnoise_channel(p,i)*(ncomp_of(&amp)==1?amp.v[0]:amp.v[i])+(ncomp_of(&piv)==1?piv.v[0]:piv.v[i]);if(nc==4)r.v[3]=vnoise(v3_add(p,v3_make(19,193,17)))*(ncomp_of(&amp)==1?amp.v[0]:amp.v[3])+(ncomp_of(&piv)==1?piv.v[0]:piv.v[3]);}break; }
        case OP_FRACTAL3D: {
            /* mx_fractal3d_noise_{float,vec2,vec3,vec4}: three-channel outputs
             * sum mx_perlin_noise_vec3 (one hash byte per channel); the extra
             * channel of vec2/vec4 is the float noise at p + (19, 193, 17). */
            MtlxValue pos=in_or(ctx,n,"position",mv_vec3(ctx->P)),amp=in_or(ctx,n,"amplitude",mv_float(1)),oc=in_or(ctx,n,"octaves",mv_float(3)),lac=in_or(ctx,n,"lacunarity",mv_float(2)),dim=in_or(ctx,n,"diminish",mv_float(0.5f));
            r=mv_zero(n->type); int nc=ncomp_of(&r); if(nc<1)nc=1;
            v3 p=mv_as_v3(&pos); float w=1;
            for(int o=0;o<(int)oc.v[0]&&o<16;o++){
                v3 q=v3_add(p,v3_make(19,193,17));
                if(nc==1)r.v[0]+=w*vnoise_channel(p,-1);
                else if(nc==2){r.v[0]+=w*vnoise_channel(p,-1);r.v[1]+=w*vnoise_channel(q,-1);}
                else{for(int i=0;i<3;i++)r.v[i]+=w*vnoise_channel(p,i);if(nc==4)r.v[3]+=w*vnoise_channel(q,-1);}
                w*=dim.v[0];p=v3_scale(p,lac.v[0]);
            }
            for(int i=0;i<nc;i++)r.v[i]*=ncomp_of(&amp)==1?amp.v[0]:amp.v[i];
            break;
        }
        case OP_FRACTAL2D: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),amp=in_or(ctx,n,"amplitude",mv_float(1)),oc=in_or(ctx,n,"octaves",mv_float(3)),lac=in_or(ctx,n,"lacunarity",mv_float(2)),dim=in_or(ctx,n,"diminish",mv_float(0.5f));r=mv_zero(n->type);int nc=ncomp_of(&r);if(nc<1)nc=1;float px=tc.v[0],py=tc.v[1],w=1;for(int o=0;o<(int)oc.v[0]&&o<16;o++){if(nc==1)r.v[0]+=w*vnoise2(px,py,-1);else if(nc==2){r.v[0]+=w*vnoise2(px,py,-1);r.v[1]+=w*vnoise2(px+19,py+193,-1);}else{for(int i=0;i<3;i++)r.v[i]+=w*vnoise2(px,py,i);if(nc==4)r.v[3]+=w*vnoise2(px+19,py+193,-1);}px*=lac.v[0];py*=lac.v[0];w*=dim.v[0];}for(int i=0;i<nc;i++)r.v[i]*=ncomp_of(&amp)==1?amp.v[0]:amp.v[i];break; }
        case OP_CELLNOISE2D: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1]));uint32_t h=mx_hash_int2((int)floorf(tc.v[0]),(int)floorf(tc.v[1]));r=mv_float((float)h/(float)0xffffffffu);break; }
        case OP_CELLNOISE3D: { MtlxValue pos=in_or(ctx,n,"position",mv_vec3(ctx->P)); float c=cellnoise(mv_as_v3(&pos)); r=(n->type==MV_FLOAT)?mv_float(c):mv_color3(v3_splat(c)); break; }
        case OP_WORLEYNOISE2D: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),j=in_or(ctx,n,"jitter",mv_float(1)),s=in_or(ctx,n,"style",mv_float(0));r=worley_value(v3_make(tc.v[0],tc.v[1],0),0,j.v[0],(int)s.v[0],n->type);break; }
        case OP_WORLEYNOISE3D: { MtlxValue pos=in_or(ctx,n,"position",mv_vec3(ctx->P)),j=in_or(ctx,n,"jitter",mv_float(1)),s=in_or(ctx,n,"style",mv_float(0));r=worley_value(mv_as_v3(&pos),1,j.v[0],(int)s.v[0],n->type);break; }
        case OP_UNIFIEDNOISE2D:
        case OP_UNIFIEDNOISE3D: { int d3=classify(n->category)==OP_UNIFIEDNOISE3D;MtlxValue coord=in_or(ctx,n,d3?"position":"texcoord",d3?mv_vec3(ctx->P):mv_vec2(ctx->uv[0],ctx->uv[1]));MtlxValue freq=in_or(ctx,n,"freq",d3?mv_vec3(v3_make(1,1,1)):mv_vec2(1,1)),off=in_or(ctx,n,"offset",d3?mv_vec3(v3_make(0,0,0)):mv_vec2(0,0)),jit=in_or(ctx,n,"jitter",mv_float(1)),kind=in_or(ctx,n,"type",mv_float(0)),style=in_or(ctx,n,"style",mv_float(0));v3 p=v3_make(coord.v[0]*freq.v[0]+off.v[0],coord.v[1]*freq.v[1]+off.v[1],d3?coord.v[2]*freq.v[2]+off.v[2]:0);float angle=(jit.v[0]-1)*90000.0f,rad=angle*(float)MTLX_PI/180.0f,cs=cosf(rad),sn=sinf(rad);v3 jp;if(d3){v3 axis=v3_normalize(v3_make(.1f,1,0));float dot=v3_dot(axis,p);jp=v3_add(v3_add(v3_scale(p,cs),v3_scale(v3_cross(axis,p),sn)),v3_scale(axis,dot*(1-cs)));}else jp=v3_make(p.x*cs-p.y*sn,p.x*sn+p.y*cs,0);float q=0;switch((int)kind.v[0]){case 1:q=d3?cellnoise(jp):(float)mx_hash_int2((int)floorf(jp.x),(int)floorf(jp.y))/(float)0xffffffffu;break;case 2:q=worley_value(p,d3,jit.v[0],(int)style.v[0],MV_FLOAT).v[0];break;case 3:{MtlxValue oc=in_or(ctx,n,"octaves",mv_float(3)),lac=in_or(ctx,n,"lacunarity",mv_float(2)),dim=in_or(ctx,n,"diminish",mv_float(.5f));v3 fp=d3?jp:v3_make(p.x,p.y,angle);q=fractal(fp,(int)oc.v[0],lac.v[0],dim.v[0]);break;}default:q=.5f*(d3?vnoise(jp):vnoise2(jp.x,jp.y,-1))+.5f;break;}MtlxValue lo=in_or(ctx,n,"outmin",mv_float(0)),hi=in_or(ctx,n,"outmax",mv_float(1)),cl=in_or(ctx,n,"clampoutput",mv_float(1));q=lo.v[0]+q*(hi.v[0]-lo.v[0]);if(cl.v[0]!=0)q=clampf(q,fminf(lo.v[0],hi.v[0]),fmaxf(lo.v[0],hi.v[0]));r=mv_float(q);break; }
        case OP_RAMPLR: { MtlxValue l=in_or(ctx,n,"valuel",mv_zero(n->type)),rr=in_or(ctx,n,"valuer",mv_zero(n->type)),tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])); float t=clampf(tc.v[0],0,1); int nc=ncomp_of(&l); r=l; for(int i=0;i<nc;i++) r.v[i]=l.v[i]*(1-t)+rr.v[i]*t; break; }
        case OP_RAMPTB: { MtlxValue tval=in_or(ctx,n,"valuet",mv_zero(n->type)),bval=in_or(ctx,n,"valueb",mv_zero(n->type)),tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])); float t=clampf(tc.v[1],0,1); int nc=ncomp_of(&tval); r=tval; for(int i=0;i<nc;i++) r.v[i]=bval.v[i]*(1-t)+tval.v[i]*t; break; }
        case OP_SPLITLR: { MtlxValue l=in_or(ctx,n,"valuel",mv_zero(n->type)),rr=in_or(ctx,n,"valuer",mv_zero(n->type)),ct=in_or(ctx,n,"center",mv_float(0.5f)),tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])); r=(tc.v[0]<ct.v[0])?l:rr; break; }
        case OP_SPLITTB: { MtlxValue tval=in_or(ctx,n,"valuet",mv_zero(n->type)),bval=in_or(ctx,n,"valueb",mv_zero(n->type)),ct=in_or(ctx,n,"center",mv_float(0.5f)),tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])); r=(tc.v[1]<ct.v[0])?bval:tval; break; }
        case OP_RAMP4: { MtlxValue tl=in_or(ctx,n,"valuetl",mv_zero(n->type)),tr=in_or(ctx,n,"valuetr",mv_zero(n->type)),bl=in_or(ctx,n,"valuebl",mv_zero(n->type)),br=in_or(ctx,n,"valuebr",mv_zero(n->type)),tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])); float u=clampf(tc.v[0],0,1),vv=clampf(tc.v[1],0,1); int nc=ncomp_of(&tl); r=tl; for(int i=0;i<nc;i++){ float top=tl.v[i]*(1-u)+tr.v[i]*u, bot=bl.v[i]*(1-u)+br.v[i]*u; r.v[i]=bot*(1-vv)+top*vv; } break; }
        case OP_RAMP: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),kind=in_or(ctx,n,"type",mv_zero(MV_INT)),interp=in_or(ctx,n,"interpolation",mv_float(1)),countv=in_or(ctx,n,"num_intervals",mv_float(2));float dx=tc.v[0]-.5f,dy=tc.v[1]-.5f,x=tc.v[0];switch((int)kind.v[0]){case 1:x=atan2f(dy,dx)/(2*(float)MTLX_PI)+.5f;break;case 2:x=sqrtf(dx*dx+dy*dy)*1.41421356f;break;case 3:x=2*fmaxf(fabsf(dx),fabsf(dy));break;default:break;}int count=(int)countv.v[0];if(count<2)count=2;if(count>10)count=10;r=in_or(ctx,n,"color1",mv_zero(MV_COLOR4));for(int k=1;k<count;k++){char lo_name[16],hi_name[16],color_name[16];snprintf(lo_name,sizeof(lo_name),"interval%d",k);snprintf(hi_name,sizeof(hi_name),"interval%d",k+1);snprintf(color_name,sizeof(color_name),"color%d",k+1);MtlxValue lo=in_or(ctx,n,lo_name,mv_float(k==1?0:1)),hi=in_or(ctx,n,hi_name,mv_float(1)),next=in_or(ctx,n,color_name,mv_zero(MV_COLOR4));if(x<=lo.v[0])continue;float t;if((int)interp.v[0]==2)t=x>=hi.v[0]?1:0;else{t=clampf((x-lo.v[0])/((hi.v[0]-lo.v[0])!=0?hi.v[0]-lo.v[0]:1),0,1);if((int)interp.v[0]==1)t=t*t*(3-2*t);}for(int i=0;i<4;i++)r.v[i]=r.v[i]+t*(next.v[i]-r.v[i]);}r.type=MV_COLOR4;break; }
        case OP_RAMP_GRADIENT: { MtlxValue x=in_or(ctx,n,"x",mv_float(0)),lo=in_or(ctx,n,"interval1",mv_float(0)),hi=in_or(ctx,n,"interval2",mv_float(1)),c1=in_or(ctx,n,"color1",mv_zero(MV_COLOR4)),c2=in_or(ctx,n,"color2",mv_zero(MV_COLOR4)),interp=in_or(ctx,n,"interpolation",mv_float(1)),prev=in_or(ctx,n,"prev_color",mv_zero(MV_COLOR4)),inum=in_or(ctx,n,"interval_num",mv_float(1)),count=in_or(ctx,n,"num_intervals",mv_float(2));r=prev;if(inum.v[0]<count.v[0]&&x.v[0]>lo.v[0]){float t;if((int)interp.v[0]==2)t=x.v[0]>=hi.v[0]?1:0;else{t=clampf((x.v[0]-lo.v[0])/((hi.v[0]-lo.v[0])!=0?hi.v[0]-lo.v[0]:1),0,1);if((int)interp.v[0]==1)t=t*t*(3-2*t);}r=c1;for(int i=0;i<4;i++)r.v[i]=c1.v[i]+t*(c2.v[i]-c1.v[i]);}r.type=MV_COLOR4;break; }
        case OP_CREATEMATRIX: { int dim=matrix_dim(n->type);r=mv_zero(n->type);for(int col=0;col<dim;col++){char nm[8];snprintf(nm,sizeof(nm),"in%d",col+1);MtlxValue q=in_or(ctx,n,nm,mv_zero(dim==3?MV_VEC3:MV_VEC4));for(int row=0;row<dim;row++)r.v[col*dim+row]=(row<ncomp_of(&q))?q.v[row]:((row==3&&col==3)?1:0);}break; }
        case OP_TRANSFORMMATRIX: { a=in_or(ctx,n,"in",mv_zero(n->type));MtlxValue m=in_or(ctx,n,"mat",mv_zero(MV_MATRIX44));int dim=matrix_dim(m.type),outc=ncomp_of(&a);r=mv_zero(n->type);for(int row=0;row<outc;row++){float q=0;for(int col=0;col<dim;col++){float vc=col<outc?a.v[col]:((col==dim-1&&outc+1==dim)?1:0);q+=m.v[col*dim+row]*vc;}r.v[row]=q;}break; }
        case OP_TRANSPOSE: { a=in_or(ctx,n,"in",mv_zero(n->type));int dim=matrix_dim(a.type);r=a;for(int col=0;col<dim;col++)for(int row=0;row<dim;row++)r.v[col*dim+row]=a.v[row*dim+col];break; }
        case OP_DETERMINANT: { a=in_or(ctx,n,"in",mv_zero(MV_MATRIX44));r=mv_float(matrix_determinant(&a));break; }
        case OP_INVERTMATRIX: { a=in_or(ctx,n,"in",mv_zero(n->type));r=matrix_inverse(a);break; }
        case OP_CIRCLE: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),center=in_or(ctx,n,"center",mv_vec2(0,0)),radius=in_or(ctx,n,"radius",mv_float(0.5f));float x=tc.v[0]-center.v[0],y=tc.v[1]-center.v[1];r=mv_float(x*x+y*y>radius.v[0]*radius.v[0]?0.0f:1.0f);break; }
        case OP_LINE: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),center=in_or(ctx,n,"center",mv_vec2(0,0)),radius=in_or(ctx,n,"radius",mv_float(0.1f)),p1=in_or(ctx,n,"point1",mv_vec2(0.25f,0.25f)),p2=in_or(ctx,n,"point2",mv_vec2(0.75f,0.75f));float px=tc.v[0]-center.v[0]-p1.v[0],py=tc.v[1]-center.v[1]-p1.v[1],bx=p2.v[0]-p1.v[0],by=p2.v[1]-p1.v[1],bb=bx*bx+by*by,t=clampf(bb>1e-12f?(px*bx+py*by)/bb:0,0,1),dx=px-bx*t,dy=py-by*t;r=mv_float(sqrtf(dx*dx+dy*dy)>radius.v[0]?0.0f:1.0f);break; }
        case OP_CLOVERLEAF: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),center=in_or(ctx,n,"center",mv_vec2(.5f,.5f)),radius=in_or(ctx,n,"radius",mv_float(.5f));float sx=2*tc.v[0],sy=2*tc.v[1],cx=2*center.v[0],cy=2*center.v[1],rr=radius.v[0],r2=rr*rr,dx[4]={sx+rr-cx,sx-rr-cx,sx-cx,sx-cx},dy[4]={sy-cy,sy-cy,sy-rr-cy,sy+rr-cy},inside=0;for(int i=0;i<4;i++)if(dx[i]*dx[i]+dy[i]*dy[i]<=r2)inside=1;r=mv_float(inside);break; }
        case OP_HEXAGON: { MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),center=in_or(ctx,n,"center",mv_vec2(.5f,.5f)),radius=in_or(ctx,n,"radius",mv_float(.5f));float px=fabsf(tc.v[1]-center.v[1]),py=fabsf(tc.v[0]-center.v[0]),kx=-.866025f,ky=.5f,kz=.57735f,rr=radius.v[0],projection=fminf(kx*px+ky*py,0);px-=2*projection*kx;py-=2*projection*ky;px-=clampf(px,-kz*rr,kz*rr);py-=rr;float sd=sqrtf(px*px+py*py)*(py<0?-1:1);r=mv_float(sd<=0?1:0);break; }
        case OP_GRID: case OP_CROSSHATCH: case OP_TILEDCIRCLES: case OP_TILEDCLOVERLEAFS: case OP_TILEDHEXAGONS: { NodeOp po=classify(n->category);int kind=po-OP_GRID;MtlxValue tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1])),tiling=in_or(ctx,n,"uvtiling",mv_vec2(1,1)),offset=in_or(ctx,n,"uvoffset",mv_vec2(0,0)),parameter=in_or(ctx,n,kind<=1?"thickness":"size",mv_float(kind<=1?.05f:.5f)),staggered=in_or(ctx,n,"staggered",mv_float(0));float q=pattern_value(tc.v[0]*tiling.v[0]-offset.v[0],tc.v[1]*tiling.v[1]-offset.v[1],parameter.v[0],staggered.v[0]!=0,kind);r=mv_color3(v3_make(q,q,q));break; }
        case OP_COLORCORRECT: { a=in_or(ctx,n,"in",mv_zero(n->type));MtlxValue hue=in_or(ctx,n,"hue",mv_float(0)),sat=in_or(ctx,n,"saturation",mv_float(1)),gamma=in_or(ctx,n,"gamma",mv_float(1)),lift=in_or(ctx,n,"lift",mv_float(0)),gain=in_or(ctx,n,"gain",mv_float(1)),contrast=in_or(ctx,n,"contrast",mv_float(1)),pivot=in_or(ctx,n,"contrastpivot",mv_float(0.5f)),exposure=in_or(ctx,n,"exposure",mv_float(0));v3 hsv=rgb_to_hsv(mv_as_v3(&a));hsv.x=hsv.x+hue.v[0]-floorf(hsv.x+hue.v[0]);v3 rgb=hsv_to_rgb(hsv);float lum=luminance(rgb),scale=powf(2.0f,exposure.v[0]),recip=fabsf(gamma.v[0])>1e-6f?1.0f/gamma.v[0]:0.0f;r=a;for(int i=0;i<3;i++){float x=lum+sat.v[0]*((i==0?rgb.x:(i==1?rgb.y:rgb.z))-lum);x=(x<0?-1.0f:1.0f)*powf(fabsf(x),recip);x=(x*(1-lift.v[0])+lift.v[0])*gain.v[0];r.v[i]=((x-pivot.v[0])*contrast.v[0]+pivot.v[0])*scale;}break; }
        case OP_BLUR: r=in_or(ctx,n,"in",mv_zero(n->type));break;
        case OP_FLAKE2D: case OP_FLAKE3D:r=eval_flake_output(ctx,n,"flakenormal");break;
        case OP_RANDOMFLOAT: { a=in_or(ctx,n,"in",mv_float(0));MtlxValue seed=in_or(ctx,n,"seed",mv_zero(MV_INT)),lo=in_or(ctx,n,"min",mv_float(0)),hi=in_or(ctx,n,"max",mv_float(1));int x=(int)floorf(a.v[0]*(a.type==MV_INT?1.0f:4096.0f)),y=(int)floorf(seed.v[0]);float q=(float)mx_hash_int2(x,y)/(float)0xffffffffu;r=mv_float(lo.v[0]+q*(hi.v[0]-lo.v[0]));break; }
        case OP_RANDOMCOLOR: { a=in_or(ctx,n,"in",mv_float(0));MtlxValue seed=in_or(ctx,n,"seed",mv_zero(MV_INT)),hl=in_or(ctx,n,"huelow",mv_float(0)),hh=in_or(ctx,n,"huehigh",mv_float(1)),sl=in_or(ctx,n,"saturationlow",mv_float(0.825f)),sh=in_or(ctx,n,"saturationhigh",mv_float(1)),bl=in_or(ctx,n,"brightnesslow",mv_float(1)),bh=in_or(ctx,n,"brightnesshigh",mv_float(1));int s=(int)floorf(seed.v[0]);float h=randomfloat_value(a.v[0],(int)ceilf((float)s+413.3f),hl.v[0],hh.v[0]),ss=randomfloat_value(a.v[0],(int)ceilf((float)s+1522.4f),sl.v[0],sh.v[0]),v=randomfloat_value(a.v[0],(int)ceilf((float)s+1813.8f),bl.v[0],bh.v[0]);r=mv_color3(hsv_to_rgb(v3_make(h,ss,v)));break; }
        case OP_IFGREATER: { MtlxValue v1=in_or(ctx,n,"value1",mv_float(0)),v2=in_or(ctx,n,"value2",mv_float(0)); r=(mv_as_float(&v1)>mv_as_float(&v2))?in_or(ctx,n,"in1",mv_zero(n->type)):in_or(ctx,n,"in2",mv_zero(n->type)); break; }
        case OP_IFGREATEREQ: { MtlxValue v1=in_or(ctx,n,"value1",mv_float(0)),v2=in_or(ctx,n,"value2",mv_float(0)); r=(mv_as_float(&v1)>=mv_as_float(&v2))?in_or(ctx,n,"in1",mv_zero(n->type)):in_or(ctx,n,"in2",mv_zero(n->type)); break; }
        case OP_IFEQUAL: { MtlxValue v1=in_or(ctx,n,"value1",mv_float(0)),v2=in_or(ctx,n,"value2",mv_float(0)); r=(mv_as_float(&v1)==mv_as_float(&v2))?in_or(ctx,n,"in1",mv_zero(n->type)):in_or(ctx,n,"in2",mv_zero(n->type)); break; }
        case OP_SWITCH: { MtlxValue w=in_or(ctx,n,"which",mv_float(0)); int k=(int)floorf(mv_as_float(&w)); if(k<0)k=0; if(k>9)k=9; char nm[8]; snprintf(nm,sizeof(nm),"in%d",k+1); r=in_or(ctx,n,nm,mv_zero(n->type)); break; }
        case OP_DOT: r=in_or(ctx,n,"in",mv_zero(n->type)); break;
        case OP_ONEMINUS: { a=in_or(ctx,n,"in",mv_zero(n->type)); int nc=ncomp_of(&a); r=a; for(int i=0;i<nc;i++) r.v[i]=1.0f-a.v[i]; break; }
        case OP_ROTATE2D: { a=in_or(ctx,n,"in",mv_vec2(0,0)); MtlxValue am=in_or(ctx,n,"amount",mv_float(0)); float ang=am.v[0]*(MTLX_PI/180.0f),cs=cosf(ang),sn=sinf(ang); r=mv_vec2(a.v[0]*cs-a.v[1]*sn, a.v[0]*sn+a.v[1]*cs); break; } /* "Positive values rotate counter-clockwise" (StandardNodes rotate2d); the reference mx_rotate_vector2 turns the other way */
        case OP_DISTANCE: { a=in_or(ctx,n,"in1",mv_zero(MV_VEC3));b=in_or(ctx,n,"in2",mv_zero(MV_VEC3));int nc=ncomp_of(&a);float l2=0;for(int i=0;i<nc;i++){float dlt=a.v[i]-b.v[i];l2+=dlt*dlt;}r=mv_float(sqrtf(l2));break; }
        case OP_REFLECT: { a=in_or(ctx,n,"in",mv_zero(MV_VEC3));b=in_or(ctx,n,"normal",mv_vec3(v3_make(0,0,1)));v3 av=mv_as_v3(&a),bv=mv_as_v3(&b);r=mv_vec3(v3_sub(av,v3_scale(bv,2*v3_dot(av,bv))));break; }
        case OP_REFRACT: { a=in_or(ctx,n,"in",mv_zero(MV_VEC3));b=in_or(ctx,n,"normal",mv_vec3(v3_make(0,0,1)));MtlxValue et=in_or(ctx,n,"ior",mv_float(1));v3 av=mv_as_v3(&a),bv=mv_as_v3(&b);float d=v3_dot(av,bv),e=et.v[0],k=1-e*e*(1-d*d);r=mv_vec3(k<0?v3_make(0,0,0):v3_sub(v3_scale(av,e),v3_scale(bv,e*d+sqrtf(k))));break; }
        case OP_PREMULT: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR4));r=a;r.v[0]*=r.v[3];r.v[1]*=r.v[3];r.v[2]*=r.v[3];break; }
        case OP_UNPREMULT: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR4));r=a;if(r.v[3]!=0.0f){float q=1/r.v[3];r.v[0]*=q;r.v[1]*=q;r.v[2]*=q;}break; } /* alpha 0: passed through unchanged */
        case OP_MINCOMPONENT: case OP_MAXCOMPONENT: { a=in_or(ctx,n,"in",mv_zero(MV_COLOR3));int nc=ncomp_of(&a);float q=a.v[0];NodeOp op=classify(n->category);for(int i=1;i<nc;i++)q=op==OP_MINCOMPONENT?fminf(q,a.v[i]):fmaxf(q,a.v[i]);r=mv_float(q);break; }
        case OP_AND: case OP_OR: case OP_XOR: { a=in_or(ctx,n,"in1",mv_float(0));b=in_or(ctx,n,"in2",mv_float(0));int av=mv_as_float(&a)!=0,bv=mv_as_float(&b)!=0;NodeOp op=classify(n->category);int q=op==OP_AND?(av&&bv):(op==OP_OR?(av||bv):(av!=bv));r=mv_float((float)q);break; }
        case OP_NOT: { a=in_or(ctx,n,"in",mv_float(0));r=mv_float(mv_as_float(&a)==0?1:0);break; }
        case OP_INSIDE: case OP_OUTSIDE: { a=in_or(ctx,n,"in",mv_zero(n->type));b=in_or(ctx,n,"mask",mv_float(0));float q=classify(n->category)==OP_INSIDE?b.v[0]:1-b.v[0];r=a;for(int i=0;i<ncomp_of(&r);i++)r.v[i]*=q;break; }
        case OP_TRIANGLEWAVE: { a=in_or(ctx,n,"in",mv_float(0));float q=fmodf(fabsf(a.v[0]),1.0f);r=mv_float(0.5f-fabsf(q-0.5f));break; }
        case OP_CHECKERBOARD: { MtlxValue c1=in_or(ctx,n,"color1",mv_color3(v3_splat(1))),c2=in_or(ctx,n,"color2",mv_color3(v3_splat(0))),tile=in_or(ctx,n,"uvtiling",mv_vec2(8,8)),off=in_or(ctx,n,"uvoffset",mv_vec2(0,0)),tc=in_or(ctx,n,"texcoord",mv_vec2(ctx->uv[0],ctx->uv[1]));float ix=floorf(tc.v[0]*tile.v[0]-off.v[0]),iy=floorf(tc.v[1]*tile.v[1]-off.v[1]),sum=ix+iy,q=sum-2.0f*floorf(sum/2.0f);r=q!=0.0f?c1:c2;break; }
        case OP_UNKNOWN:
        default:
            if (n->ninput > 0) r = eval_input(ctx, &n->inputs[0]);
            break;
    }
    ctx->memo[node_id] = r;
    return r;
}

MtlxValue mtlx_eval_node_test(ShadeContext *ctx, int node_id) {
    memset(ctx->memo_done, 0, (size_t)ctx->doc->nnode);
    return eval_node(ctx, node_id);
}

/* ---- public surface evaluation ---------------------------------------- */

void openpbr_defaults(OpenPBRParams *p) {
    memset(p, 0, sizeof(*p));
    p->base_weight = 1.0f; p->base_color = v3_make(0.8f, 0.8f, 0.8f);
    p->specular_weight = 1.0f; p->specular_color = v3_splat(1.0f);
    p->specular_roughness = 0.3f; p->specular_ior = 1.5f;
    p->transmission_color = v3_splat(1.0f);
    p->subsurface_color = v3_splat(0.8f); p->subsurface_radius = v3_splat(1.0f); p->subsurface_scale = 1.0f;
    p->coat_color = v3_splat(1.0f); p->coat_roughness = 0.1f; p->coat_ior = 1.5f;
    p->sheen_color = v3_splat(1.0f); p->sheen_roughness = 0.3f;
    p->thin_film_ior = 1.5f; /* weight/thickness default to 0 (off) via memset */
    p->emission_color = v3_splat(1.0f); p->opacity = 1.0f;
}

static float in_float(ShadeContext *ctx, const MtlxNode *n, const char *name, float fb) {
    const MtlxInput *in = find_input(n, name);
    if (!in) return fb;
    MtlxValue v = eval_input(ctx, in);
    return mv_as_float(&v);
}
static v3 in_color(ShadeContext *ctx, const MtlxNode *n, const char *name, v3 fb) {
    const MtlxInput *in = find_input(n, name);
    if (!in) return fb;
    MtlxValue v = eval_input(ctx, in);
    return mv_as_v3(&v);
}

static const MtlxNode *input_node(ShadeContext *ctx, const MtlxNode *n,
                                  const char *name) {
    const MtlxInput *in = find_input(n, name);
    if (!in || in->src_node < 0 || in->src_node >= ctx->doc->nnode) return NULL;
    return &ctx->doc->nodes[in->src_node];
}

static v3 eval_edf(ShadeContext *ctx, const MtlxNode *n, int depth) {
    if (!n || depth > 32) return v3_splat(0.0f);
    const char *cat = n->category;
    if (!strcmp(cat, "add") || !strcmp(cat, "layer")) {
        const char *a_name = !strcmp(cat, "layer") ? "base" : "in1";
        const char *b_name = !strcmp(cat, "layer") ? "top" : "in2";
        return v3_add(eval_edf(ctx, input_node(ctx, n, a_name), depth + 1),
                      eval_edf(ctx, input_node(ctx, n, b_name), depth + 1));
    }
    if (!strcmp(cat, "mix")) {
        const float amount = fmaxf(0.0f, fminf(1.0f,
            in_float(ctx, n, "mix", in_float(ctx, n, "amount", 0.5f))));
        const v3 bg = eval_edf(ctx, input_node(ctx, n, "bg"), depth + 1);
        const v3 fg = eval_edf(ctx, input_node(ctx, n, "fg"), depth + 1);
        return v3_lerp(bg, fg, amount);
    }
    if (!strcmp(cat, "multiply")) {
        const MtlxInput *weight = find_input(n, "in2");
        const MtlxValue value = weight ? eval_input(ctx, weight) : mv_float(1.0f);
        return v3_mul(eval_edf(ctx, input_node(ctx, n, "in1"), depth + 1),
                      mv_as_v3(&value));
    }
    if (!strcmp(cat, "light")) {
        const v3 edf = eval_edf(ctx, input_node(ctx, n, "edf"), depth + 1);
        const v3 intensity = in_color(ctx, n, "intensity", v3_splat(1.0f));
        const float exposure = in_float(ctx, n, "exposure", 0.0f);
        return v3_mul(edf, v3_scale(intensity, powf(2.0f, exposure)));
    }
    if (!strcmp(cat, "uniform_edf") || !strcmp(cat, "measured_edf"))
        return in_color(ctx, n, "color", v3_splat(1.0f));
    if (!strcmp(cat, "conical_edf")) {
        const v3 color = in_color(ctx, n, "color", v3_splat(1.0f));
        v3 normal = in_color(ctx, n, "normal", ctx->Ns);
        normal = v3_normalize(normal);
        const float inner = in_float(ctx, n, "inner_angle", 60.0f);
        const float outer = in_float(ctx, n, "outer_angle", 0.0f);
        const float cosine = fmaxf(0.0f, fminf(1.0f,
                                                v3_dot(normal, ctx->V)));
        // MaterialX specifies full cone angles, so convert their half angles
        // to cosine boundaries. Reversed/equal bounds use a hard cutoff;
        // strictly wider outer cones use Hermite smoothstep in cosine space.
        const float inner_cos = cosf(inner * (3.14159265358979323846f / 360.0f));
        const float outer_cos = cosf(outer * (3.14159265358979323846f / 360.0f));
        float falloff;
        if (outer <= inner) {
            falloff = cosine >= inner_cos ? 1.0f : 0.0f;
        } else {
            const float t = fmaxf(0.0f, fminf(1.0f,
                (cosine - outer_cos) / fmaxf(inner_cos - outer_cos, 1.0e-6f)));
            falloff = t * t * (3.0f - 2.0f * t);
        }
        return v3_scale(color, falloff);
    }
    if (!strcmp(cat, "generalized_schlick_edf")) {
        const v3 base = eval_edf(ctx, input_node(ctx, n, "base"), depth + 1);
        const v3 c0 = in_color(ctx, n, "color0", v3_splat(1.0f));
        const v3 c90 = in_color(ctx, n, "color90", v3_splat(1.0f));
        const float exponent = in_float(ctx, n, "exponent", 5.0f);
        const float cos_theta = fmaxf(0.0f, fminf(1.0f,
            v3_dot(v3_normalize(ctx->Ns), v3_normalize(ctx->V))));
        const float f = powf(1.0f - cos_theta, exponent);
        return v3_mul(base, v3_lerp(c0, c90, f));
    }
    return v3_splat(0.0f);
}

static const char *in_string(const MtlxNode *n, const char *name,
                             const char *fb) {
    const MtlxInput *in = find_input(n, name);
    return (in && in->has_value && in->value.s) ? in->value.s : fb;
}

static int scatter_mode_has(const char *mode, char component) {
    if (!mode) return 0;
    if (!strcmp(mode, "R")) return component == 'R';
    if (!strcmp(mode, "T")) return component == 'T';
    if (!strcmp(mode, "RT")) return component == 'R' || component == 'T';
    return 0;
}

static float in_component0(ShadeContext *ctx, const MtlxNode *n,
                           const char *name, float fb) {
    const MtlxInput *in = find_input(n, name);
    if (!in) return fb;
    const MtlxValue value = eval_input(ctx, in);
    return value.v[0];
}

static void apply_normal_input(ShadeContext *ctx, const MtlxNode *n,
                               const char *name, OpenPBRParams *out);

/* Volume closure parameters must not use the unknown-node fallback. */
static int supported_volume_value(ShadeContext *ctx, const MtlxNode *n,
                                  int depth, size_t *budget) {
    if (!n) return 1;
    if (depth > 32 || !*budget) return 0;
    --*budget;
    if (strcmp(n->category, "input") && classify(n->category) == OP_UNKNOWN)
        return 0;
    for (int i = 0; i < n->ninput; ++i) {
        if (!*budget) return 0;
        --*budget;
        const int src = n->inputs[i].src_node;
        if (n->inputs[i].has_connection && src < 0) return 0;
        if (src >= ctx->doc->nnode) return 0;
        if (src >= 0 && !supported_volume_value(ctx, &ctx->doc->nodes[src],
                                                 depth + 1, budget)) return 0;
    }
    return 1;
}

static int supported_closure_values(ShadeContext *ctx, const MtlxNode *n,
                                    int depth, size_t *budget) {
    for (int i = 0; i < n->ninput; ++i) {
        if (!*budget) return 0;
        --*budget;
        const MtlxInput *in = &n->inputs[i];
        if (in->has_connection && in->src_node < 0) return 0;
        if (in->type == MV_NONE || in->src_node < 0) continue;
        if (in->src_node >= ctx->doc->nnode ||
            !supported_volume_value(ctx, &ctx->doc->nodes[in->src_node],
                                    depth + 1, budget)) return 0;
    }
    return 1;
}

/* Validate the closure branches consumed by eval_edf for volume emission. */
static int supported_edf(ShadeContext *ctx, const MtlxNode *n, int depth,
                         size_t *budget) {
    if (!n) return 1;
    if (depth > 32 || !*budget) return 0;
    --*budget;
    if (!supported_closure_values(ctx, n, depth, budget)) return 0;
    const char *cat = n->category;
    /* Measured EDF needs IES evaluation; eval_edf's color fallback is not a
     * faithful volume emission result. */
    if (!strcmp(cat, "measured_edf")) return 0;
    if (!strcmp(cat, "uniform_edf") || !strcmp(cat, "conical_edf")) return 1;
    if (!strcmp(cat, "add") || !strcmp(cat, "layer")) {
        const char *a = !strcmp(cat, "layer") ? "base" : "in1";
        const char *b = !strcmp(cat, "layer") ? "top" : "in2";
        return supported_edf(ctx, input_node(ctx, n, a), depth + 1, budget) &&
               supported_edf(ctx, input_node(ctx, n, b), depth + 1, budget);
    }
    if (!strcmp(cat, "mix"))
        return supported_edf(ctx, input_node(ctx, n, "bg"), depth + 1, budget) &&
               supported_edf(ctx, input_node(ctx, n, "fg"), depth + 1, budget);
    if (!strcmp(cat, "multiply")) {
        return supported_edf(ctx, input_node(ctx, n, "in1"), depth + 1, budget);
    }
    if (!strcmp(cat, "light"))
        return supported_edf(ctx, input_node(ctx, n, "edf"), depth + 1, budget);
    if (!strcmp(cat, "generalized_schlick_edf"))
        return supported_edf(ctx, input_node(ctx, n, "base"), depth + 1, budget);
    return 0;
}

/* Check the same VDF branches that eval_vdf traverses.  Unknown closures and
 * cycles must not silently turn into a transparent medium. */
static int supported_vdf(ShadeContext *ctx, const MtlxNode *n, int depth,
                         size_t *budget) {
    if (!n) return 1;
    if (depth > 32 || !*budget) return 0;
    --*budget;
    if (!supported_closure_values(ctx, n, depth, budget)) return 0;
    const char *cat = n->category;
    if (!strcmp(cat, "absorption_vdf") || !strcmp(cat, "anisotropic_vdf") ||
        !strcmp(cat, "subsurface_vdf")) return 1;
    if (!strcmp(cat, "add") || !strcmp(cat, "layer")) {
        const char *a = !strcmp(cat, "layer") ? "base" : "in1";
        const char *b = !strcmp(cat, "layer") ? "top" : "in2";
        return supported_vdf(ctx, input_node(ctx, n, a), depth + 1, budget) &&
               supported_vdf(ctx, input_node(ctx, n, b), depth + 1, budget);
    }
    if (!strcmp(cat, "mix"))
        return supported_vdf(ctx, input_node(ctx, n, "bg"), depth + 1, budget) &&
               supported_vdf(ctx, input_node(ctx, n, "fg"), depth + 1, budget);
    if (!strcmp(cat, "multiply")) {
        return supported_vdf(ctx, input_node(ctx, n, "in1"), depth + 1, budget);
    }
    return 0;
}

static void eval_vdf(ShadeContext *ctx, const MtlxNode *n, v3 scale,
                     MtlxVolumeParams *out, int depth) {
    if (!n || depth > 32 ||
        (scale.x == 0.0f && scale.y == 0.0f && scale.z == 0.0f)) return;
    const char *cat = n->category;
    if (!strcmp(cat, "add") || !strcmp(cat, "layer")) {
        const char *a_name = !strcmp(cat, "layer") ? "base" : "in1";
        const char *b_name = !strcmp(cat, "layer") ? "top" : "in2";
        eval_vdf(ctx, input_node(ctx, n, a_name), scale, out, depth + 1);
        eval_vdf(ctx, input_node(ctx, n, b_name), scale, out, depth + 1);
        return;
    }
    if (!strcmp(cat, "mix")) {
        const float amount = fmaxf(0.0f, fminf(1.0f,
            in_float(ctx, n, "mix", in_float(ctx, n, "amount", 0.5f))));
        eval_vdf(ctx, input_node(ctx, n, "bg"), v3_scale(scale, 1.0f - amount),
                 out, depth + 1);
        eval_vdf(ctx, input_node(ctx, n, "fg"), v3_scale(scale, amount), out,
                 depth + 1);
        return;
    }
    if (!strcmp(cat, "multiply")) {
        const MtlxInput *factor = find_input(n, "in2");
        const MtlxValue factor_value = factor ? eval_input(ctx, factor) : mv_float(1.0f);
        const v3 weight = mv_as_v3(&factor_value);
        eval_vdf(ctx, input_node(ctx, n, "in1"), v3_mul(scale, weight), out, depth + 1);
        return;
    }
    if (!strcmp(cat, "absorption_vdf") || !strcmp(cat, "anisotropic_vdf") ||
        !strcmp(cat, "subsurface_vdf")) {
        out->absorption = v3_add(out->absorption,
            v3_mul(in_color(ctx, n, "absorption", v3_splat(0.0f)), scale));
        if (!strcmp(cat, "anisotropic_vdf") || !strcmp(cat, "subsurface_vdf")) {
            const v3 scattering = v3_mul(in_color(ctx, n, "scattering", v3_splat(0.0f)), scale);
            const float anisotropy = in_float(ctx, n, "anisotropy", 0.0f);
            out->scattering = v3_add(out->scattering, scattering);
            out->anisotropy = anisotropy;
            if (scattering.x != 0.0f || scattering.y != 0.0f || scattering.z != 0.0f) {
                int i;
                for (i = 0; i < out->lobe_count; ++i)
                    if (out->lobes[i].anisotropy == anisotropy) break;
                if (i == out->lobe_count) {
                    if (i >= MTLX_MAX_VOLUME_SCATTERING_LOBES) {
                        out->lobe_overflow = 1;
                        return;
                    }
                    ++out->lobe_count;
                    out->lobes[i].anisotropy = anisotropy;
                }
                out->lobes[i].scattering = v3_add(out->lobes[i].scattering, scattering);
            }
        }
    }
}

static void blend_lobe(float *total, v3 *dst, v3 color, float weight) {
    if (weight <= 0.0f) return;
    const float old = *total;
    const float sum = old + weight;
    *dst = sum > 0.0f ? v3_scale(v3_add(v3_scale(*dst, old),
                                       v3_scale(color, weight)), 1.0f / sum)
                      : color;
    *total = sum;
}

static float blend_scalar(float old_weight, float old_value, float value,
                          float weight) {
    const float sum = old_weight + weight;
    return sum > 0.0f ? (old_value * old_weight + value * weight) / sum
                      : value;
}

static void apply_bsdf(ShadeContext *ctx, const MtlxNode *n, float scale,
                       OpenPBRParams *out, int depth) {
    if (!n || depth > 32 || scale <= 0.0f) return;
    const char *cat = n->category;
    if (!strcmp(cat, "add")) {
        apply_bsdf(ctx, input_node(ctx, n, "in1"), scale, out, depth + 1);
        apply_bsdf(ctx, input_node(ctx, n, "in2"), scale, out, depth + 1);
        return;
    }
    if (!strcmp(cat, "mix")) {
        const float mix = fmaxf(0.0f, fminf(1.0f,
            in_float(ctx, n, "mix", in_float(ctx, n, "amount", 0.5f))));
        apply_bsdf(ctx, input_node(ctx, n, "bg"), scale * (1.0f - mix),
                   out, depth + 1);
        apply_bsdf(ctx, input_node(ctx, n, "fg"), scale * mix,
                   out, depth + 1);
        return;
    }
    if (!strcmp(cat, "multiply")) {
        const MtlxNode *a = input_node(ctx, n, "in1");
        const MtlxNode *b = input_node(ctx, n, "in2");
        const MtlxInput *scalar = find_input(n, a ? "in2" : "in1");
        MtlxValue scalar_value;
        memset(&scalar_value, 0, sizeof(scalar_value));
        if (scalar) scalar_value = eval_input(ctx, scalar);
        float factor = scalar ? mv_as_float(&scalar_value) : 1.0f;
        apply_bsdf(ctx, a ? a : b, scale * fmaxf(0.0f, factor), out,
                   depth + 1);
        return;
    }
    if (!strcmp(cat, "layer")) {
        apply_bsdf(ctx, input_node(ctx, n, "base"), scale, out, depth + 1);
        apply_bsdf(ctx, input_node(ctx, n, "top"), scale, out, depth + 1);
        return;
    }

    const float weight = scale * fmaxf(0.0f, in_float(ctx, n, "weight", 1.0f));
    if (!strcmp(cat, "diffuse_bsdf") ||
        !strcmp(cat, "oren_nayar_diffuse_bsdf") ||
        !strcmp(cat, "burley_diffuse_bsdf")) {
        const float old = out->base_weight;
        blend_lobe(&out->base_weight, &out->base_color,
                   in_color(ctx, n, "color", v3_splat(0.18f)), weight);
        out->diffuse_roughness = (out->diffuse_roughness * old +
            in_float(ctx, n, "roughness", 0.0f) * weight) /
            fmaxf(out->base_weight, 1.0e-6f);
    } else if (!strcmp(cat, "translucent_bsdf")) {
        blend_lobe(&out->transmission, &out->transmission_color,
                   in_color(ctx, n, "color", v3_splat(1.0f)), weight);
    } else if (!strcmp(cat, "dielectric_bsdf")) {
        const char *mode = in_string(n, "scatter_mode", "R");
        const float roughness = in_component0(ctx, n, "roughness", 0.05f);
        if (scatter_mode_has(mode, 'R')) {
            const float old_weight = out->specular_weight;
            const float old_roughness = out->specular_roughness;
            blend_lobe(&out->specular_weight, &out->specular_color,
                       in_color(ctx, n, "tint", v3_splat(1.0f)), weight);
            out->specular_roughness = blend_scalar(
                old_weight, old_roughness, roughness, weight);
        }
        if (scatter_mode_has(mode, 'T')) {
            blend_lobe(&out->transmission, &out->transmission_color,
                       in_color(ctx, n, "tint", v3_splat(1.0f)), weight);
        }
        out->specular_ior = in_float(ctx, n, "ior", 1.5f);
        out->thin_film_thickness = in_float(ctx, n, "thinfilm_thickness", 0.0f);
        out->thin_film_ior = in_float(ctx, n, "thinfilm_ior", 1.5f);
        out->thin_film_weight = out->thin_film_thickness > 0.0f ? weight : 0.0f;
    } else if (!strcmp(cat, "conductor_bsdf")) {
        const v3 eta = in_color(ctx, n, "ior", v3_make(0.183f, 0.421f, 1.373f));
        const v3 k = in_color(ctx, n, "extinction", v3_make(3.424f, 2.346f, 1.770f));
        const v3 one = v3_splat(1.0f);
        const v3 em = v3_sub(eta, one), ep = v3_add(eta, one);
        const v3 k2 = v3_mul(k, k);
        const v3 num = v3_add(v3_mul(em, em), k2);
        const v3 den = v3_add(v3_mul(ep, ep), k2);
        const v3 f0 = v3_make(num.x / fmaxf(den.x, 1.0e-6f),
                              num.y / fmaxf(den.y, 1.0e-6f),
                              num.z / fmaxf(den.z, 1.0e-6f));
        const float old_base_weight = out->base_weight;
        const float old_roughness = out->specular_roughness;
        blend_lobe(&out->base_weight, &out->base_color, f0, weight);
        out->metalness = 1.0f;
        out->specular_roughness = blend_scalar(
            old_base_weight, old_roughness,
            in_component0(ctx, n, "roughness", 0.05f), weight);
    } else if (!strcmp(cat, "generalized_schlick_bsdf")) {
        const char *mode = in_string(n, "scatter_mode", "R");
        const v3 c0 = in_color(ctx, n, "color0", v3_splat(1.0f));
        if (scatter_mode_has(mode, 'R')) {
            const float old_weight = out->specular_weight;
            const float old_roughness = out->specular_roughness;
            blend_lobe(&out->specular_weight, &out->specular_color, c0, weight);
            out->specular_roughness = blend_scalar(
                old_weight, old_roughness,
                in_component0(ctx, n, "roughness", 0.05f), weight);
        }
        if (scatter_mode_has(mode, 'T'))
            blend_lobe(&out->transmission, &out->transmission_color,
                       v3_splat(1.0f), weight);
    } else if (!strcmp(cat, "subsurface_bsdf")) {
        blend_lobe(&out->subsurface, &out->subsurface_color,
                   in_color(ctx, n, "color", v3_splat(0.18f)), weight);
        out->subsurface_radius = in_color(ctx, n, "radius", v3_splat(1.0f));
        out->subsurface_scale = in_float(ctx, n, "scale", 1.0f);
        out->subsurface_anisotropy = in_float(ctx, n, "anisotropy", 0.0f);
    } else if (!strcmp(cat, "sheen_bsdf")) {
        const float old_weight = out->sheen_weight;
        blend_lobe(&out->sheen_weight, &out->sheen_color,
                   in_color(ctx, n, "color", v3_splat(1.0f)), weight);
        out->sheen_roughness = blend_scalar(
            old_weight, out->sheen_roughness,
            in_float(ctx, n, "roughness", 0.3f), weight);
    } else if (!strcmp(cat, "chiang_hair_bsdf")) {
        const v3 r = in_color(ctx, n, "tint_R", v3_splat(1.0f));
        const v3 tt = in_color(ctx, n, "tint_TT", v3_splat(1.0f));
        const float old_sheen_weight = out->sheen_weight;
        blend_lobe(&out->sheen_weight, &out->sheen_color, r, 0.5f * weight);
        blend_lobe(&out->transmission, &out->transmission_color, tt,
                   0.5f * weight);
        out->specular_ior = in_float(ctx, n, "ior", 1.55f);
        out->sheen_roughness = blend_scalar(
            old_sheen_weight, out->sheen_roughness,
            in_component0(ctx, n, "roughness_R", 0.1f), 0.5f * weight);
    }
    apply_normal_input(ctx, n, "normal", out);
}

int mtlx_eval_volume(ShadeContext *ctx, int volume_node,
                     MtlxVolumeParams *out) {
    if (!ctx || !ctx->doc || !ctx->memo || !ctx->memo_done || !out || volume_node < 0 ||
        volume_node >= ctx->doc->nnode) return 1;
    memset(out, 0, sizeof(*out));
    memset(ctx->memo_done, 0, (size_t)ctx->doc->nnode);
    const MtlxNode *volume = &ctx->doc->nodes[volume_node];
    if (strcmp(volume->category, "volume")) return 1;
    /* Bound traversal of shared DAGs before the recursive closure evaluator. */
    size_t budget = 100000;
    if (!supported_closure_values(ctx, volume, 0, &budget)) return 1;
    const MtlxNode *vdf = input_node(ctx, volume, "vdf");
    const MtlxNode *edf = input_node(ctx, volume, "edf");
    if (!supported_vdf(ctx, vdf, 0, &budget) ||
        !supported_edf(ctx, edf, 0, &budget)) return 1;
    eval_vdf(ctx, vdf, v3_splat(1.0f), out, 0);
    if (out->lobe_overflow) return 1;
    out->emission = eval_edf(ctx, edf, 0);
    return 0;
}

static void apply_normal_input(ShadeContext *ctx, const MtlxNode *n, const char *name, OpenPBRParams *out) {
    const MtlxInput *nin = find_input(n, name);
    if (nin && nin->src_node >= 0) {
        MtlxValue nv = eval_node(ctx, nin->src_node);
        v3 wn = mv_as_v3(&nv);
        if (v3_is_finite(wn) && v3_len(wn) > 0.1f) out->normal = v3_normalize(wn);
    }
}

int mtlx_eval_surface(ShadeContext *ctx, int surface_node, OpenPBRParams *out) {
    openpbr_defaults(out);
    out->normal = v3_normalize(ctx->Ns);
    if (surface_node < 0 || surface_node >= ctx->doc->nnode) return 1;
    memset(ctx->memo_done, 0, (size_t)ctx->doc->nnode);

    const MtlxNode *n = &ctx->doc->nodes[surface_node];
    const char *cat = n->category;

    if (!strcmp(cat, "surface")) {
        out->base_weight = 0.0f;
        out->specular_weight = 0.0f;
        out->transmission = 0.0f;
        out->subsurface = 0.0f;
        out->sheen_weight = 0.0f;
        apply_bsdf(ctx, input_node(ctx, n, "bsdf"), 1.0f, out, 0);
        out->emission_color = eval_edf(ctx, input_node(ctx, n, "edf"), 0);
        out->emission = v3_maxc(out->emission_color) > 0.0f ? 1.0f : 0.0f;
        out->opacity = in_float(ctx, n, "opacity", 1.0f);
    } else if (!strcmp(cat, "surface_unlit")) {
        out->base_weight = 0.0f;
        out->specular_weight = 0.0f;
        out->emission = in_float(ctx, n, "emission", 1.0f);
        out->emission_color = in_color(ctx, n, "emission_color", v3_splat(1.0f));
        out->transmission = in_float(ctx, n, "transmission", 0.0f);
        out->transmission_color = in_color(ctx, n, "transmission_color", v3_splat(1.0f));
        out->opacity = in_float(ctx, n, "opacity", 1.0f);
    } else if (!strcmp(cat, "open_pbr_surface")) {
        out->base_weight = in_float(ctx, n, "base_weight", 1.0f);
        out->base_color = in_color(ctx, n, "base_color", out->base_color);
        out->diffuse_roughness = in_float(ctx, n, "base_diffuse_roughness", 0.0f);
        out->metalness = in_float(ctx, n, "base_metalness", 0.0f);
        out->specular_weight = in_float(ctx, n, "specular_weight", 1.0f);
        out->specular_color = in_color(ctx, n, "specular_color", out->specular_color);
        out->specular_roughness =
            in_float(ctx, n, "specular_roughness",
                     in_float(ctx, n, "base_roughness", 0.3f));
        out->specular_ior = in_float(ctx, n, "specular_ior", 1.5f);
        out->transmission = in_float(ctx, n, "transmission_weight", 0.0f);
        out->transmission_color = in_color(ctx, n, "transmission_color", out->transmission_color);
        out->transmission_depth = in_float(ctx, n, "transmission_depth", 0.0f);
        out->transmission_scatter = in_color(ctx, n, "transmission_scatter", v3_splat(0.0f));
        out->transmission_scatter_anisotropy = in_float(ctx, n, "transmission_scatter_anisotropy", 0.0f);
        out->subsurface = in_float(ctx, n, "subsurface_weight", 0.0f);
        out->subsurface_color = in_color(ctx, n, "subsurface_color", out->subsurface_color);
        out->subsurface_radius = v3_mul(
            in_color(ctx, n, "subsurface_radius", out->subsurface_radius),
            in_color(ctx, n, "subsurface_radius_scale", v3_splat(1.0f)));
        out->subsurface_scale = in_float(ctx, n, "subsurface_scale", 1.0f);
        out->coat_weight = in_float(ctx, n, "coat_weight", 0.0f);
        out->coat_color = in_color(ctx, n, "coat_color", out->coat_color);
        out->coat_roughness = in_float(ctx, n, "coat_roughness", 0.0f);
        out->coat_ior = in_float(ctx, n, "coat_ior", 1.6f);
        out->sheen_weight = in_float(ctx, n, "fuzz_weight", 0.0f);
        out->sheen_color = in_color(ctx, n, "fuzz_color", out->sheen_color);
        out->sheen_roughness = in_float(ctx, n, "fuzz_roughness", 0.3f);
        out->thin_film_weight = in_float(ctx, n, "thin_film_weight", 0.0f);
        out->thin_film_thickness = in_float(ctx, n, "thin_film_thickness", 0.0f);
        out->thin_film_ior = in_float(ctx, n, "thin_film_ior", 1.4f);
        out->emission = in_float(ctx, n, "emission_luminance", 0.0f);
        out->emission_color = in_color(ctx, n, "emission_color", out->emission_color);
        out->opacity = in_float(ctx, n, "geometry_opacity", 1.0f);
        apply_normal_input(ctx, n, "geometry_normal", out);
    } else if (!strcmp(cat, "gltf_pbr")) {
        out->base_color = in_color(ctx, n, "base_color", out->base_color);
        out->metalness = in_float(ctx, n, "metallic", 1.0f);
        out->specular_roughness = in_float(ctx, n, "roughness", 0.5f);
        out->specular_weight = in_float(ctx, n, "specular", 1.0f);
        out->specular_color = in_color(ctx, n, "specular_color", out->specular_color);
        out->specular_ior = in_float(ctx, n, "ior", 1.5f);
        out->transmission = in_float(ctx, n, "transmission", 0.0f);
        out->coat_weight = in_float(ctx, n, "clearcoat", 0.0f);
        out->coat_roughness = in_float(ctx, n, "clearcoat_roughness", 0.0f);
        out->sheen_weight = in_float(ctx, n, "sheen_color", 0.0f) > 0 ? 1.0f : 0.0f;
        out->sheen_color = in_color(ctx, n, "sheen_color", out->sheen_color);
        { float es = in_float(ctx, n, "emissive_strength", 1.0f);
          v3 em = in_color(ctx, n, "emissive", v3_splat(0.0f));
          out->emission_color = em; out->emission = (luminance(em) > 0 ? es : 0.0f); }
        apply_normal_input(ctx, n, "normal", out);
    } else if (!strcmp(cat, "UsdPreviewSurface")) {
        out->base_color = in_color(ctx, n, "diffuseColor", out->base_color);
        out->metalness = in_float(ctx, n, "metallic", 0.0f);
        out->specular_roughness = in_float(ctx, n, "roughness", 0.5f);
        out->specular_color = in_color(ctx, n, "specularColor", out->specular_color);
        out->specular_ior = in_float(ctx, n, "ior", 1.5f);
        out->coat_weight = in_float(ctx, n, "clearcoat", 0.0f);
        out->coat_roughness = in_float(ctx, n, "clearcoatRoughness", 0.01f);
        out->opacity = in_float(ctx, n, "opacity", 1.0f);
        { v3 em = in_color(ctx, n, "emissiveColor", v3_splat(0.0f));
          out->emission_color = em; out->emission = (luminance(em) > 0 ? 1.0f : 0.0f); }
        apply_normal_input(ctx, n, "normal", out);
    } else if (!strcmp(cat, "disney_principled")) {
        out->base_color = in_color(ctx, n, "baseColor", out->base_color);
        out->metalness = in_float(ctx, n, "metallic", 0.0f);
        out->specular_roughness = in_float(ctx, n, "roughness", 0.5f);
        out->specular_ior = in_float(ctx, n, "ior", 1.5f);
        out->transmission = in_float(ctx, n, "specTrans", 0.0f);
        out->coat_weight = in_float(ctx, n, "clearcoat", 0.0f);
        out->coat_roughness = 1.0f - in_float(ctx, n, "clearcoatGloss", 0.9f);
        out->sheen_weight = in_float(ctx, n, "sheen", 0.0f);
        out->subsurface = in_float(ctx, n, "subsurface", 0.0f);
        apply_normal_input(ctx, n, "normal", out);
    } else { /* standard_surface (default) */
        out->base_weight = in_float(ctx, n, "base", 1.0f);
        out->base_color = in_color(ctx, n, "base_color", out->base_color);
        out->metalness = in_float(ctx, n, "metalness", 0.0f);
        out->diffuse_roughness = in_float(ctx, n, "diffuse_roughness", 0.0f);
        out->specular_weight = in_float(ctx, n, "specular", 1.0f);
        out->specular_color = in_color(ctx, n, "specular_color", out->specular_color);
        out->specular_roughness =
            in_float(ctx, n, "specular_roughness",
                     in_float(ctx, n, "roughness", 0.2f));
        out->specular_ior =
            in_float(ctx, n, "specular_IOR",
                     in_float(ctx, n, "specular_ior", 1.5f));
        out->transmission = in_float(ctx, n, "transmission", 0.0f);
        out->transmission_color = in_color(ctx, n, "transmission_color", out->transmission_color);
        out->transmission_depth = in_float(ctx, n, "transmission_depth", 0.0f);
        out->transmission_scatter = in_color(ctx, n, "transmission_scatter", v3_splat(0.0f));
        out->transmission_scatter_anisotropy = in_float(ctx, n, "transmission_scatter_anisotropy", 0.0f);
        out->subsurface = in_float(ctx, n, "subsurface", 0.0f);
        out->subsurface_color = in_color(ctx, n, "subsurface_color", out->subsurface_color);
        out->subsurface_radius = in_color(ctx, n, "subsurface_radius", v3_splat(1.0f));
        out->subsurface_scale = in_float(ctx, n, "subsurface_scale", 1.0f);
        out->coat_weight = in_float(ctx, n, "coat", 0.0f);
        out->coat_color = in_color(ctx, n, "coat_color", out->coat_color);
        out->coat_roughness = in_float(ctx, n, "coat_roughness", 0.1f);
        out->coat_ior =
            in_float(ctx, n, "coat_IOR",
                     in_float(ctx, n, "coat_ior", 1.5f));
        out->sheen_weight = in_float(ctx, n, "sheen", 0.0f);
        out->sheen_color = in_color(ctx, n, "sheen_color", out->sheen_color);
        out->sheen_roughness = in_float(ctx, n, "sheen_roughness", 0.3f);
        out->thin_film_thickness = in_float(ctx, n, "thin_film_thickness", 0.0f); /* already nm */
        out->thin_film_ior = in_float(ctx, n, "thin_film_IOR", 1.5f);
        out->thin_film_weight = (out->thin_film_thickness > 0.0f) ? 1.0f : 0.0f;
        out->emission = in_float(ctx, n, "emission", 0.0f);
        out->emission_color = in_color(ctx, n, "emission_color", out->emission_color);
        out->opacity = in_float(ctx, n, "opacity", 1.0f);
        apply_normal_input(ctx, n, "normal", out);
    }

    out->specular_roughness = clampf(out->specular_roughness, 0.01f, 1.0f);
    out->coat_roughness = clampf(out->coat_roughness, 0.01f, 1.0f);
    out->metalness = clampf(out->metalness, 0.0f, 1.0f);
    return 0;
}
