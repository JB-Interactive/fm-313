/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdio.h>
#include "../../firmware/engine/fm313_dsp.h"
int main(){ static fm313_voice_t v; fm313_init(&v); v.p.cutoff=1; v.p.reso=0; v.p.envmod=0; v.p.accent=0;
 fm313_note_on(&v,84,0,0); static float b[44100*2]; fm313_render(&v,b,88200); fwrite(b+44100,4,32768,stdout);}
