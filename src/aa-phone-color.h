/* Own integer RGB conversion. Kr/Kb and unspecified-matrix fallback checked
 * against retained FFmpeg 7.1.1 libavutil/csp.c and libswscale/yuv2rgb.c.
 * BT.601 limited-range conversion uses integer arithmetic. No HDR support. */
#include <stdint.h>
#include <libavutil/pixfmt.h>
struct AaColor {int offset,y,rv,gu,gv,bu;};
static int aa_color_setup(struct AaColor *c,int range,int space,int transfer,int phone){
 if(!phone&&(range!=AVCOL_RANGE_MPEG||space!=AVCOL_SPC_SMPTE170M))return 0;
 if(phone&&transfer!=AVCOL_TRC_UNSPECIFIED&&transfer!=AVCOL_TRC_BT709&&
    transfer!=AVCOL_TRC_SMPTE170M&&transfer!=AVCOL_TRC_IEC61966_2_1)return 0;
 if(range!=AVCOL_RANGE_UNSPECIFIED&&range!=AVCOL_RANGE_MPEG&&range!=AVCOL_RANGE_JPEG)return 0;
 int hd=space==AVCOL_SPC_BT709;
 if(!hd&&space!=AVCOL_SPC_UNSPECIFIED&&space!=AVCOL_SPC_SMPTE170M&&space!=AVCOL_SPC_BT470BG)return 0;
 if(range==AVCOL_RANGE_JPEG)*c=hd?(struct AaColor){0,256,403,48,120,475}:(struct AaColor){0,256,359,88,183,454};
 else *c=hd?(struct AaColor){16,298,459,55,136,541}:(struct AaColor){16,298,409,100,208,516};
 return 1;
}
static unsigned aa_color_clamp(int v){return v<0?0:v>255?255:(unsigned)v;}
static uint32_t aa_color_rgb(const struct AaColor *m,int y,int u,int v){
 int c=y-m->offset,d=u-128,e=v-128;
 unsigned r=aa_color_clamp((m->y*c+m->rv*e+128)>>8);
 unsigned g=aa_color_clamp((m->y*c-m->gu*d-m->gv*e+128)>>8);
 unsigned b=aa_color_clamp((m->y*c+m->bu*d+128)>>8);
 return (r<<16)|(g<<8)|b;
}
