/* SPDX-License-Identifier: MIT */
#include "image_score.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#define W TUDOR_IMAGE_WIDTH
#define H TUDOR_IMAGE_HEIGHT
#define N TUDOR_IMAGE_PIXELS
#define RADIUS 12
#define MIN_FRACTION 0.60

static void clear_samples(void *data, size_t size)
{
    volatile unsigned char *p=data;
    while (size--) *p++=0;
}

static int reflected(int x, int size)
{
    if (x < 0) return -x-1;
    if (x >= size) return 2*size-x-1;
    return x;
}

static void highpass(const uint8_t *input, double *output)
{
    double kernel[2*RADIUS+1], horizontal[N], total=0;
    for (int i=-RADIUS; i<=RADIUS; i++) {
        kernel[i+RADIUS]=exp(-(double)(i*i)/18.0);
        total+=kernel[i+RADIUS];
    }
    for (int i=0; i<2*RADIUS+1; i++) kernel[i]/=total;
    for (int y=0; y<H; y++) for (int x=0; x<W; x++) {
        double value=0;
        for (int i=-RADIUS; i<=RADIUS; i++)
            value+=kernel[i+RADIUS]*input[y*W+reflected(x+i,W)];
        horizontal[y*W+x]=value;
    }
    for (int y=0; y<H; y++) for (int x=0; x<W; x++) {
        double value=0;
        for (int i=-RADIUS; i<=RADIUS; i++)
            value+=kernel[i+RADIUS]*horizontal[reflected(y+i,H)*W+x];
        output[y*W+x]=input[y*W+x]-value;
    }
    clear_samples(horizontal,sizeof(horizontal));
}

static void rotated(const double *input, double *output, uint8_t *mask, int angle)
{
    double radians=(double)angle*0.017453292519943295;
    double c=cos(radians), s=sin(radians);
    for (int y=0; y<H; y++) for (int x=0; x<W; x++) {
        double cx=(double)x-(W-1)*0.5, cy=(double)y-(H-1)*0.5;
        double sx=c*cx-s*cy+(W-1)*0.5, sy=s*cx+c*cy+(H-1)*0.5;
        int at=y*W+x;
        output[at]=0; mask[at]=0;
        if (sx<0 || sy<0 || sx>W-1 || sy>H-1) continue;
        int x0=(int)sx, y0=(int)sy;
        int x1=x0+1<W ? x0+1 : x0, y1=y0+1<H ? y0+1 : y0;
        double fx=sx-x0, fy=sy-y0;
        output[at]=(1-fy)*((1-fx)*input[y0*W+x0]+fx*input[y0*W+x1])+
                   fy*((1-fx)*input[y1*W+x0]+fx*input[y1*W+x1]);
        mask[at]=1;
    }
}

static int correlation(const double *a, const double *b, const uint8_t *mask,
                       int dx, int dy, int stride, struct tudor_image_score *score)
{
    /* Visit exactly the same stride-aligned coordinates as the unbounded
     * loops. The rectangular intersection bounds the number of usable samples;
     * the rotation mask can only reduce it. Reject impossible overlap before
     * accumulation, preserving all surviving arithmetic and tie ordering. */
    int x_start=dx<0 ? ((-dx+stride-1)/stride)*stride : 0;
    int y_start=dy<0 ? ((-dy+stride-1)/stride)*stride : 0;
    int x_end=dx>0 ? W-dx : W;
    int y_end=dy>0 ? H-dy : H;
    int samples=((W+stride-1)/stride)*((H+stride-1)/stride);
    if (x_end<=x_start || y_end<=y_start) return 1;
    int possible=((x_end-x_start+stride-1)/stride)*((y_end-y_start+stride-1)/stride);
    if ((double)possible < MIN_FRACTION*samples) return 1;
    double sa=0,sb=0,saa=0,sbb=0,sab=0;
    int count=0;
    for (int y=y_start; y<y_end; y+=stride) {
        int by=y+dy;
        for (int x=x_start; x<x_end; x+=stride) {
            int bx=x+dx;
            if (!mask[by*W+bx]) continue;
            double av=a[y*W+x], bv=b[by*W+bx];
            sa+=av; sb+=bv; saa+=av*av; sbb+=bv*bv; sab+=av*bv; count++;
        }
    }
    if ((double)count < MIN_FRACTION*samples) return 1;
    double va=saa-sa*sa/count, vb=sbb-sb*sb/count;
    if (va<=count*1e-6 || vb<=count*1e-6) return 1;
    score->correlation=fmax(-1,fmin(1,(sab-sa*sb/count)/sqrt(va*vb)));
    score->overlap_fraction=(double)count/samples;
    return 0;
}

static int compare(const uint8_t *first, const uint8_t *second,
                   struct tudor_image_score *output, int self_check)
{
    if (!first || !second || (!output && !self_check)) return -1;
    double a[N], b[N], r[N];
    uint8_t mask[N];
    struct tudor_image_score best={.correlation=-2,.overlap_fraction=0};
    highpass(first,a);
    if (self_check) memcpy(b,a,sizeof(b));
    else highpass(second,b);
    for (int angle=-30; angle<=30; angle+=3) {
        rotated(b,r,mask,angle);
        struct candidate { double score; int dx,dy; } top[4];
        for (int i=0; i<4; i++) top[i]=(struct candidate){.score=-2};
        /* Translations beyond 40% of either side cannot have 60% overlap.
         * Four coarse seeds per angle reduce sensitivity to repeated ridges. */
        for (int dy=-34; dy<=34; dy+=2) for (int dx=-40; dx<=40; dx+=2) {
            struct tudor_image_score current;
            if (correlation(a,r,mask,dx,dy,2,&current)) continue;
            for (int i=0; i<4; i++) if (current.correlation>top[i].score) {
                for (int j=3; j>i; j--) top[j]=top[j-1];
                top[i]=(struct candidate){current.correlation,dx,dy}; break;
            }
        }
        for (int i=0; i<4; i++) if (top[i].score>=-1) {
            for (int dy=top[i].dy-1; dy<=top[i].dy+1; dy++)
                for (int dx=top[i].dx-1; dx<=top[i].dx+1; dx++) {
                    struct tudor_image_score current;
                    if (!correlation(a,r,mask,dx,dy,1,&current) && current.correlation>best.correlation) {
                        best=current;
                        /* Bank insertion only needs the original search's
                         * success/failure, not its maximum score. A successful
                         * fine candidate proves success; keep the exact coarse
                         * ranking and fine checks, including variance/overlap.
                         * Every exit must still wipe the temporary images. */
                        if (self_check) goto done;
                    }
                }
        }
    }
done:
    /* No persistent state, reference allocation, or data output from this API. */
    clear_samples(a,sizeof(a)); clear_samples(b,sizeof(b)); clear_samples(r,sizeof(r));
    if (best.correlation < -1) return 1;
    if (output) *output=best;
    return 0;
}

int tudor_image_compare(const uint8_t *first, const uint8_t *second,
                       struct tudor_image_score *output)
{
    return compare(first,second,output,0);
}

int tudor_image_self_check(const uint8_t *image)
{
    return compare(image,image,NULL,1);
}
