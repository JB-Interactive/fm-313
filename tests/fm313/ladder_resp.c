/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdlib.h>
#include "../../firmware/engine/fm313_dsp.c"
#include <stdio.h>
#include <math.h>
/* white noise through the ladder at fc, k -> magnitude response via averaged FFT in python */
int main(int argc,char**argv){
  float fc=atof(argv[1]), res=atof(argv[2]), lvl=atof(argv[3]);
  fm313_voice_t v; fm313_init(&v);
  float rr=res*(2-res), k=rr*0.985f*k_crit;
  float a=fast_tan(PI_F*fc/FM313_FS_INT);
  float fbc=onepole_coef(LAD_FB_HP,FM313_FS_INT);
  for(int n=0;n<88200*4;n++){ float x=lvl*frand(&v); float y=ladder_step(&v,a,k,x); v.fb_lp+=fbc*(y-v.fb_lp);
    if(n>=88200) {fwrite(&x,4,1,stdout); fwrite(&y,4,1,stdout);} }
}
