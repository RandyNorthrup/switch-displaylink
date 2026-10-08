/* Time from a window repaint to when a root-window XGetImage sees it.
 * Shows how stale dl-mirror's grabs are (e.g. with KWin compositing on/off).
 * Build: cc -O2 -o capture-staleness scripts/capture-staleness.c -lX11
 * Run:   DISPLAY=:0 ./capture-staleness [changes]   (flashes a box at 600,300) */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
  int n = argc>1?atoi(argv[1]):30;
  Display*d=XOpenDisplay(NULL); int s=DefaultScreen(d); Window r=RootWindow(d,s);
  XSetWindowAttributes a; a.override_redirect=True; a.background_pixel=0xff0000;
  Window w=XCreateWindow(d,r,600,300,40,40,0,CopyFromParent,InputOutput,CopyFromParent,CWOverrideRedirect|CWBackPixel,&a);
  XMapRaised(d,w); XSync(d,False); usleep(500000);
  unsigned long cols[2]={0x00ff00,0x0000ff}; double sum=0,max=0; int miss=0;
  for(int i=0;i<n;i++){
    unsigned long c=cols[i&1];
    XSetWindowBackground(d,w,c); XClearWindow(d,w); XSync(d,False);
    double t0=now(),t;
    for(;;){ XImage*im=XGetImage(d,r,620,320,1,1,AllPlanes,ZPixmap);
      unsigned long p=XGetPixel(im,0,0)&0xffffff; XDestroyImage(im); t=now();
      if(p==c) break; if(t-t0>1.0){miss++;break;} usleep(1000);}
    double ms=(t-t0)*1e3; sum+=ms; if(ms>max)max=ms;
    usleep(150000 + (i%5)*200000); /* include pauses like typing */
  }
  printf("n=%d avg %.1f ms max %.1f ms, >1s misses %d\n",n,sum/n,max,miss);
  XDestroyWindow(d,w); XCloseDisplay(d); return 0;}
