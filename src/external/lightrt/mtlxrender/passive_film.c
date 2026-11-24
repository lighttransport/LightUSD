#include "passive_film.h"
#include <math.h>

#ifdef __FAST_MATH__
#error "passive_film.c requires strict floating-point compilation"
#endif

/* Work in double to keep float material inputs far from intermediate overflow.
 * Store the real admittance using its positive formula, avoiding cancellation
 * of the terms whose exact sum is (AD+BC)*Re(y)/|iCy+D|^2. */
static int transformed(double a, double b, double c, double d, double determinant,
                       double *real, double *imag) {
    double u=*real, v=*imag;
    double dr=d-c*v, di=c*u;
    double scale=fmax(fabs(dr),fabs(di));
    if (scale==0.0) return 0;
    dr/=scale; di/=scale;
    double denominator=dr*dr+di*di;
    double r=((determinant*u)/scale)/scale/denominator;
    double i=(((a*v+b)/scale)*dr-((a*u)/scale)*di)/denominator;
    if (!isfinite(r) || !isfinite(i) || r<0.0) return 0;
    *real=r; *imag=i;
    return 1;
}

static int power(double incident, double real, double imag, double *out) {
    double scale=fmax(incident,fmax(fabs(real),fabs(imag)));
    if (!(scale>0.0) || !isfinite(scale) || real<0.0) return 0;
    double a=incident/scale, r=real/scale, i=imag/scale;
    double denominator=(a+r)*(a+r)+i*i;
    if (!(denominator>0.0)) return 0;
    double transmitted=4.0*a*r/denominator;
    if (!isfinite(transmitted)) return 0;
    /* Only a rounding reserve: exact passivity gives transmitted in [0,1]. */
    transmitted=fmin(1.0,fmax(0.0,transmitted));
    *out=1.0-transmitted;
    return 1;
}

int mtlx_passive_film(float cosTheta, const float eta[3], const float extinction[3],
                      float thickness_nm, float film_ior, float out[3]) {
    if (!eta || !extinction || !out || !isfinite(cosTheta) ||
        !(cosTheta>0.0f && cosTheta<=1.0f) || !isfinite(thickness_nm) ||
        thickness_nm<0.0f || !isfinite(film_ior) || !(film_ior>0.0f)) return 0;
    for (int channel=0;channel<3;++channel)
        if (!isfinite(eta[channel]) || !(eta[channel]>0.0f) ||
            !isfinite(extinction[channel]) || extinction[channel]<0.0f) return 0;
    const double wavelengths[3]={620.0,550.0,460.0};
    const double two_pi=6.283185307179586476925286766559005768;
    double cosine=cosTheta;
    double tangential=(1.0-cosine)*(1.0+cosine);
    double nf=film_ior;
    double qf2=(nf-1.0)*(nf+1.0)+cosine*cosine;
    float result[3];
    for (int channel=0;channel<3;++channel) {
        double n=eta[channel], k=extinction[channel];
        double ar=(n-1.0)*(n+1.0)+cosine*cosine-k*k, ai=2.0*n*k;
        double length=hypot(ar,ai), u, v;
        if (ar>=0.0) {
            u=sqrt((length+ar)*0.5);
            v=u>0.0 ? ai/(2.0*u) : 0.0;
        } else {
            v=sqrt((length-ar)*0.5);
            u=v>0.0 ? ai/(2.0*v) : 0.0;
        }
        /* A lossless evanescent semi-infinite substrate carries no power. The
         * critical q=0 limit also reflects all power for positive incidence. */
        if (u==0.0) { result[channel]=1.0f; continue; }
        double qs=u*u+v*v;
        if (!(qs>0.0) || !isfinite(qs)) return 0;
        double ys_real=u, ys_imag=v;
        double yp_real=u*(1.0+tangential/qs);
        double yp_imag=v*(1.0-tangential/qs);
        double phase_scale=two_pi*(double)thickness_nm/wavelengths[channel];
        if (thickness_nm>0.0f && nf!=1.0) {
            if (qf2>0.0) {
                double q=sqrt(qf2), phase=phase_scale*q;
                double sine=sin(phase), co=cos(phase);
                double determinant=co*co+sine*sine;
                double yp=nf*nf/q;
                if (!transformed(co,-q*sine,-sine/q,co,determinant,&ys_real,&ys_imag) ||
                    !transformed(co,-yp*sine,-sine/yp,co,determinant,&yp_real,&yp_imag)) return 0;
            } else if (qf2<0.0) {
                double kap=sqrt(-qf2), t=tanh(phase_scale*kap);
                double determinant=(1.0-t)*(1.0+t);
                double gamma=nf*nf/kap;
                if (!transformed(1.0,kap*t,-t/kap,1.0,determinant,&ys_real,&ys_imag) ||
                    !transformed(1.0,-gamma*t,t/gamma,1.0,determinant,&yp_real,&yp_imag)) return 0;
            } else {
                if (!transformed(1.0,0.0,-phase_scale,1.0,1.0,&ys_real,&ys_imag) ||
                    !transformed(1.0,-nf*nf*phase_scale,0.0,1.0,1.0,&yp_real,&yp_imag)) return 0;
            }
        }
        double rs, rp;
        if (!power(cosine,ys_real,ys_imag,&rs) || !power(1.0/cosine,yp_real,yp_imag,&rp)) return 0;
        double reflected=(rs+rp)*0.5;
        if (!isfinite(reflected) || reflected<0.0 || reflected>1.0) return 0;
        result[channel]=(float)reflected;
    }
    for (int channel=0;channel<3;++channel) out[channel]=result[channel];
    return 1;
}
