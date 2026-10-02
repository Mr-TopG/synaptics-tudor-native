# SPDX-License-Identifier: MIT
"""Fixed experimental grayscale transforms. No biometric matching or thresholds."""
import math


def local_contrast(pixels, width, height, radius=7):
    if len(pixels) != width*height or width <= 0 or height <= 0 or not 1 <= radius <= 31:
        raise ValueError('Invalid local-contrast image or radius')
    stride = width+1
    sums = [0]*((width+1)*(height+1))
    squares = [0]*len(sums)
    for y in range(height):
        total = total_sq = 0
        for x in range(width):
            value = pixels[y*width+x]
            total += value; total_sq += value*value
            at = (y+1)*stride+x+1
            sums[at] = sums[at-stride]+total
            squares[at] = squares[at-stride]+total_sq
    out = bytearray(len(pixels))
    for y in range(height):
        y0, y1 = max(0,y-radius), min(height,y+radius+1)
        for x in range(width):
            x0, x1 = max(0,x-radius), min(width,x+radius+1)
            count = (x1-x0)*(y1-y0)
            corners = (y1*stride+x1,y0*stride+x1,y1*stride+x0,y0*stride+x0)
            def rectangle(table):
                a,b,c,d=corners
                return table[a]-table[b]-table[c]+table[d]
            mean = rectangle(sums)/count
            variance = max(0,rectangle(squares)/count-mean*mean)
            # Fixed gain and noise floor. No adaptation based on match outcomes.
            value = 128+48*(pixels[y*width+x]-mean)/max(8,math.sqrt(variance))
            out[y*width+x] = max(0,min(255,round(value)))
    return bytes(out)


def enlarge(pixels, width, height, factor):
    if len(pixels) != width*height or width <= 0 or height <= 0 or factor not in (1,2,3):
        raise ValueError('Invalid image or scale')
    if factor == 1: return pixels
    out_width, out_height = width*factor, height*factor
    out = bytearray(out_width*out_height)
    for y in range(out_height):
        sy = max(0,min(height-1,(y+0.5)/factor-0.5))
        y0 = int(sy); y1 = min(height-1,y0+1); fy = sy-y0
        for x in range(out_width):
            sx = max(0,min(width-1,(x+0.5)/factor-0.5))
            x0 = int(sx); x1 = min(width-1,x0+1); fx = sx-x0
            a = pixels[y0*width+x0]*(1-fx)+pixels[y0*width+x1]*fx
            b = pixels[y1*width+x0]*(1-fx)+pixels[y1*width+x1]*fx
            out[y*out_width+x] = round(a*(1-fy)+b*fy)
    return bytes(out)
