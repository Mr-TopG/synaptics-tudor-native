/* SPDX-License-Identifier: MIT */
#include "image_quality.h"
#include <math.h>
#define W TUDOR_IMAGE_WIDTH
#define H TUDOR_IMAGE_HEIGHT
#define N TUDOR_IMAGE_PIXELS

static double coherence(double xx, double yy, double xy)
{
    double energy=xx+yy;
    return energy>1e-9 ? fmin(1,hypot(xx-yy,2*xy)/energy) : 0;
}

int tudor_image_assess(const uint8_t *image, size_t size, struct tudor_image_quality *output)
{
    if (!output) return -1;
    *output=(struct tudor_image_quality){.flags=TUDOR_QUALITY_INVALID};
    if (!image || size!=N) return -1;
    struct tudor_image_quality q={0};
    double sum=0,squares=0,total_xx=0,total_yy=0,total_xy=0,weighted=0;
    unsigned clipped=0,tiles=0,active=0;
    for (size_t i=0; i<size; i++) {
        double value=image[i]; sum+=value; squares+=value*value;
        if (image[i]==0 || image[i]==255) clipped++;
    }
    double mean=sum/N;
    q.standard_deviation=sqrt(fmax(0,squares/N-mean*mean));
    q.clipped_fraction=(double)clipped/N;
    for (int by=1; by<H-1; by+=8) for (int bx=1; bx<W-1; bx+=8) {
        double xx=0,yy=0,xy=0; unsigned count=0;
        for (int y=by; y<by+8 && y<H-1; y++) for (int x=bx; x<bx+8 && x<W-1; x++) {
            double dx=((double)image[y*W+x+1]-image[y*W+x-1])*0.5;
            double dy=((double)image[(y+1)*W+x]-image[(y-1)*W+x])*0.5;
            xx+=dx*dx; yy+=dy*dy; xy+=dx*dy; count++;
        }
        double energy=xx+yy;
        tiles++;
        if (energy/(double)count>=4) active++;
        weighted+=energy*coherence(xx,yy,xy);
        total_xx+=xx; total_yy+=yy; total_xy+=xy;
    }
    double energy=total_xx+total_yy;
    q.active_fraction=(double)active/tiles;
    q.local_coherence=energy>1e-9 ? weighted/energy : 0;
    q.global_coherence=coherence(total_xx,total_yy,total_xy);
    if (q.standard_deviation<5) q.flags|=TUDOR_QUALITY_LOW_CONTRAST;
    if (q.active_fraction<0.4) q.flags|=TUDOR_QUALITY_LOW_COVERAGE;
    if (q.local_coherence<0.25) q.flags|=TUDOR_QUALITY_LOW_STRUCTURE;
    if (q.global_coherence>0.995) q.flags|=TUDOR_QUALITY_DIRECTIONAL;
    if (q.clipped_fraction>0.70) q.flags|=TUDOR_QUALITY_CLIPPED;
    *output=q;
    return 0;
}
