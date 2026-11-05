#include <assert.h>
#include <stdio.h>
#include "capture-size.h"
int main(void)
{
	uint32_t w=3200,h=2136;
	screen_limit_size(&w,&h,false);
	assert(w==1616 && h==1080);
	w=3840; h=2160; screen_limit_size(&w,&h,true);
	assert(w==3840 && h==2160);
	w=1280; h=720; screen_limit_size(&w,&h,false);
	assert(w==1280 && h==720);
	for (uint32_t a=64; a<=8192; a+=67) {
		for (uint32_t b=64; b<=8192; b+=79) {
			w=a; h=b; screen_limit_size(&w,&h,false);
			assert(w>0 && h>0 && w<=a && h<=b);
			assert(w<=(a>=b?1920u:1080u) && h<=(a>=b?1080u:1920u));
			// Quantisation changes each dimension by less than two pixels.
			double ratio=(double)a/b;
			assert((double)w/(h+2) <= ratio && (double)(w+2)/h >= ratio);
		}
	}
	puts("PASS: tablet, native resolution, no upscaling, landscape/portrait bounds and aspect ratio");
	return 0;
}
