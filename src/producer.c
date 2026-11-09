/* Separate EGL/IVI producer. HMI surfaceComponent owns presentation/navigation.
 * Live screen order is checked before committing the private AA layer.
 * HMI focus and visibility control presentation.
 */
#define _GNU_SOURCE
#define MESA_EGL_NO_X11_HEADERS
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "scene-ilm-abi.h"
#include "window_ipc.h"
#include "surface_canvas.h"
#include "phone_child.h"
#include "frame_canvas.h"
#include "screen_output.h"

static volatile sig_atomic_t stopping;
static struct wl_display *native;
static struct wl_compositor *compositor;
static int bad,barrier_done;
static int64_t until;
static int64_t now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return -1;return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void stop_signal(int s){
    if(s==SIGALRM){const char message[]="PRODUCER_WATCHDOG\n";(void)write(2,message,sizeof(message)-1);_exit(124);}
    stopping=1;
}
static void global(void *d,struct wl_registry *r,uint32_t n,const char *name,uint32_t v){
    (void)d;(void)v;aa_screen_global(r,n,name);
    if(!strcmp(name,"wl_compositor")) {
        if(compositor){bad=1;return;}
        compositor=(struct wl_compositor*)wl_proxy_marshal_constructor((struct wl_proxy*)r,0,&wl_compositor_interface,n,name,1u,NULL);
    }
}
static void removed(void *d,struct wl_registry *r,uint32_t n){(void)d;(void)r;(void)n;bad=1;}
static const struct wl_registry_listener globals={global,removed};
static void sync_done(void *d,struct wl_callback *c,uint32_t n){(void)d;(void)c;(void)n;barrier_done=1;}
static const struct wl_callback_listener sync_listener={sync_done};
static int pump(int wait){
    if(stopping||bad||now_ms()>=until)return -1;
    while(wl_display_prepare_read(native))if(wl_display_dispatch_pending(native)<0)return -1;
    int flushed=wl_display_flush(native);
    if(flushed<0&&errno!=EAGAIN){wl_display_cancel_read(native);return -1;}
    struct pollfd p={wl_display_get_fd(native),POLLIN|(flushed<0?POLLOUT:0),0};int rc=poll(&p,1,wait);
    if(rc>0&&(p.revents&POLLIN)){if(wl_display_read_events(native)<0)return -1;}
    else wl_display_cancel_read(native);
    if((rc<0&&errno!=EINTR)||(p.revents&(POLLERR|POLLHUP|POLLNVAL)))return -1;
    return wl_display_dispatch_pending(native)<0?-1:0;
}
static int barrier(void){
    barrier_done=0;struct wl_callback *c=wl_display_sync(native);if(!c)return -1;
    wl_callback_add_listener(c,&sync_listener,NULL);int64_t end=now_ms()+1500;
    while(!barrier_done&&now_ms()<end)if(pump(20))break;
    wl_callback_destroy(c);return !barrier_done||bad?-1:0;
}
static int has(uint32_t id,int count,uint32_t *ids){for(int i=0;i<count;i++)if(ids[i]==id)return 1;return 0;}
static int send_packet(unsigned type,uint32_t lid,uint32_t sid){
    struct ow_packet p={.magic=OW_MAGIC,.type=type,.layer=lid,.surface=sid};
    return send(3,&p,sizeof(p),MSG_NOSIGNAL|MSG_DONTWAIT)==sizeof(p)?0:-1;
}
#define NEED(x,s) do {if(!(x)){fprintf(stderr,"PRODUCER_ERROR stage=%s errno=%d\n",s,errno);goto cleanup;}}while(0)
#define API(x) NEED((x)==0,#x)

int main(int argc,char **argv){
    /* fd 3 is inherited only from our PluginApp's socketpair. */
    if(argc!=2||strcmp(argv[1],"--plugin-fd3"))return 64;
    struct ucred peer;socklen_t plen=sizeof(peer);
    if(getsockopt(3,SOL_SOCKET,SO_PEERCRED,&peer,&plen)||peer.uid!=getuid()||peer.pid!=getppid())return 65;
    pid_t parent=getppid();if(prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent)return 65;
    signal(SIGTERM,stop_signal);signal(SIGINT,stop_signal);signal(SIGHUP,stop_signal);signal(SIGALRM,stop_signal);
    /* Includes a hard watchdog around synchronous legacy EGL/ILM calls. */
    alarm(12);until=now_ms()+10000;
    int result=2,ilm=0,lc=0,sc=0,ei=0,current=0,count=0,nlayers=0,nsurfaces=0;
    uint32_t *ids=NULL,*layers=NULL,*surfaces=NULL,lid=0,sid=0,sw=0,sh=0;
    struct wl_registry *registry=NULL;struct wl_surface *surface=NULL;struct wl_egl_window *window=NULL;
    EGLDisplay display=EGL_NO_DISPLAY;EGLSurface target=EGL_NO_SURFACE;EGLContext context=EGL_NO_CONTEXT;
    EGLConfig config;EGLint found=0,major,minor;
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
    const EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    /* USB child starts after display setup. */
    native=wl_display_connect(NULL);NEED(native,"connect");
    registry=wl_display_get_registry(native);NEED(registry,"registry");wl_registry_add_listener(registry,&globals,NULL);
    NEED(!barrier()&&!barrier()&&compositor,"compositor");
    API(ilm_initWithNativedisplay(native));ilm=1;
    API(ilm_getScreenIDs(&count,&ids));NEED(count==1&&ids[0]==0,"one-display");free(ids);ids=NULL;
    API(ilm_getScreenResolution(0,&sw,&sh));NEED(sw==OW_WIDTH&&sh==OW_HEIGHT,"display-1280x480");
    API(ilm_getLayerIDs(&nlayers,&layers));API(ilm_getSurfaceIDs(&nsurfaces,&surfaces));
    NEED(nlayers>=0&&nlayers<4096&&nsurfaces>=0&&nsurfaces<4096,"inventory-bound");
    /* No occupied ID is accepted. HMI derives layer ID by clearing low 4 bits.
     * Prefix is a search space, not a reservation of somebody else's ID. */
    unsigned seed=0;int random=open("/dev/urandom",O_RDONLY|O_CLOEXEC);NEED(random>=0,"random");
    ssize_t got=read(random,&seed,sizeof(seed));close(random);NEED(got==sizeof(seed),"random-read");
    for(unsigned i=0;i<65536;i++) {
        uint32_t candidate=0x4f000000u|(((seed+i)&0xffffu)<<4);
        if(!has(candidate,nlayers,layers)&&!has(candidate+1,nsurfaces,surfaces)){lid=candidate;sid=lid+1;break;}
    }
    free(layers);layers=NULL;free(surfaces);surfaces=NULL;NEED(lid&&sid,"unused-pair");
    surface=wl_compositor_create_surface(compositor);NEED(surface,"surface");
    struct wl_region *region=wl_compositor_create_region(compositor);NEED(region,"region");
    wl_surface_set_input_region(surface,region);wl_region_destroy(region);
    display=eglGetDisplay((EGLNativeDisplayType)native);NEED(display!=EGL_NO_DISPLAY,"egl-display");
    NEED(eglInitialize(display,&major,&minor),"egl-init");ei=1;NEED(eglBindAPI(EGL_OPENGL_ES_API),"egl-api");
    NEED(eglChooseConfig(display,attrs,&config,1,&found)&&found==1,"egl-config");
    context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attrs);NEED(context!=EGL_NO_CONTEXT,"egl-context");
    window=wl_egl_window_create(surface,OW_WIDTH,OW_HEIGHT);NEED(window,"egl-window");
    target=eglCreateWindowSurface(display,config,(EGLNativeWindowType)window,NULL);NEED(target!=EGL_NO_SURFACE,"egl-surface");
    NEED(eglMakeCurrent(display,target,target,context),"egl-current");current=1;
    NEED(aa_canvas_init(),"phone-canvas");
    uint32_t requested=sid;API(ilm_surfaceCreate(surface,OW_WIDTH,OW_HEIGHT,2,&sid));sc=1;
    NEED(sid==requested,"surface-id-changed");requested=lid;
    API(ilm_layerCreateWithDimension(&lid,OW_WIDTH,OW_HEIGHT));lc=1;NEED(lid==requested,"layer-id-changed");
    API(ilm_layerSetSourceRectangle(lid,0,0,OW_WIDTH,OW_HEIGHT));API(ilm_layerSetDestinationRectangle(lid,0,0,OW_WIDTH,OW_HEIGHT));
    API(ilm_surfaceSetSourceRectangle(sid,0,0,OW_WIDTH,OW_HEIGHT));API(ilm_surfaceSetDestinationRectangle(sid,0,0,OW_WIDTH,OW_HEIGHT));
    API(ilm_layerSetOpacity(lid,1.0));API(ilm_surfaceSetOpacity(sid,1.0));
    API(ilm_layerAddSurface(lid,sid));API(ilm_layerSetVisibility(lid,0));API(ilm_surfaceSetVisibility(sid,0));API(ilm_commitChanges());
    /* Fresh live screen order is applied below before USB takeover. */
    NEED(!barrier(),"created-barrier");
    NEED(aa_screen_attach(lid),"own-screen-attach");
    NEED(!barrier()&&!aa_screen_failed&&aa_screen_inventory(lid,"attached")==1,"own-screen-membership");NEED(phone_start(),"phone-child");NEED(!send_packet(OW_READY,lid,sid),"ready");
    fprintf(stderr,"PRODUCER_READY layer=%u surface=%u region=0,0,1280,480\n",lid,sid);
    until=now_ms()+1200000;alarm(1202);int active=0,frame_submitted=0,frame_reported=0;
    while(!stopping&&now_ms()<until){
        struct ow_packet p={0};int changed=0;
        /* Coalesce state updates before rendering. */
        for(unsigned batch=0;batch<128;batch++) {
            struct ow_packet next={0};ssize_t n=recv(3,&next,sizeof(next),MSG_DONTWAIT);
            if(n==0){result=0;goto cleanup;}
            if(n<0){if(errno==EAGAIN||errno==EINTR)break;goto cleanup;}
            NEED(n==sizeof(next)&&next.magic==OW_MAGIC,"packet");
            if(next.type==OW_STOP){result=0;goto cleanup;}
            NEED(next.type==OW_STATE&&next.active<=1,"state");
            p=next;changed=1;
        }
        int video_changed=0;
        NEED(phone_running(),"phone-session-ended");
        video_changed=aa_canvas_receive(phone_frame);
        if(changed||video_changed){
            // Bound a hung vendor call independently of the overall 20min lease.
            // Parent STOP closes only its IPC, never signals a potentially reused PID.
            alarm(3);
            if(changed&&active!=(int)p.active){
                if(p.active)NEED(aa_order_verify(),"screen-order-on-activation");
                API(ilm_surfaceSetVisibility(sid,p.active));API(ilm_layerSetVisibility(lid,p.active));API(ilm_commitChanges());active=p.active;
            }
            if(active){
                aa_canvas_paint();
                NEED(glGetError()==GL_NO_ERROR&&eglSwapBuffers(display,target),"draw");
                if(aa_has_frame){
                    unsigned count=atomic_fetch_add(&phone_frame->submitted,1)+1;
                    if(count==1)fprintf(stderr,"AA_FRAME_SUBMITTED width=%u height=%u LCD_not_verified=1\n",aa_width,aa_height);
                }
                /* A blank canvas is not phone video. OPEN may succeed only
                 * after a decoded phone frame has been swapped to our surface. */
                if(aa_has_frame&&!frame_submitted){
                    NEED(aa_order_verify(),"screen-order-first-frame");
                    frame_submitted=1;
                    fprintf(stderr,"FRAME_SUBMITTED first=1 swap_accepted=1 LCD_not_verified=1\n");
                }
            }
            int64_t remaining=until-now_ms();
            if(remaining<=0)goto cleanup;
            alarm((unsigned)(remaining/1000)+1);
        }
        if(frame_submitted&&!frame_reported&&!send_packet(OW_FRAME,lid,sid))frame_reported=1;
        struct pollfd ipc={3,POLLIN,0};int rc=poll(&ipc,1,20);
        if((rc<0&&errno!=EINTR)||(ipc.revents&(POLLERR|POLLHUP|POLLNVAL))){result=0;break;}
        NEED(!pump(0),"events");
    }
cleanup:
    /* Remove only successfully created own objects. Never restore a scene snapshot. */
    alarm(3);until=now_ms()+2000;stopping=0;int clean=1;
    if(!phone_stop())clean=0;
    if(current)aa_canvas_close();
    free(ids);free(layers);free(surfaces);
    if(sc&&ilm_surfaceSetVisibility(sid,0))clean=0;
    if(lc&&ilm_layerSetVisibility(lid,0))clean=0;
    if(lc&&sc&&ilm_layerRemoveSurface(lid,sid))clean=0;
    if(lc&&ilm_layerRemove(lid))clean=0;
    if(sc&&ilm_surfaceRemove(sid))clean=0;
    if(ilm&&(sc||lc)) {
        if(ilm_commitChanges()||barrier())clean=0;
        int remaining=0;uint32_t *check=NULL;
        if(lc){if(ilm_getLayerIDs(&remaining,&check)||has(lid,remaining,check))clean=0;free(check);check=NULL;}
        if(sc){if(ilm_getSurfaceIDs(&remaining,&check)||has(sid,remaining,check))clean=0;free(check);}
    }
    if(lc&&aa_screen_inventory(lid,"removed")!=0)clean=0;
    aa_screen_close();
    if(display!=EGL_NO_DISPLAY){
        if(current)eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        if(target!=EGL_NO_SURFACE)eglDestroySurface(display,target);
        if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);
        if(ei)eglTerminate(display);
    }
    if(window)wl_egl_window_destroy(window);if(surface)wl_surface_destroy(surface);
    if(ilm&&ilm_destroy())clean=0;
    if(registry)wl_registry_destroy(registry);if(native)wl_display_disconnect(native);
    send_packet(result==0&&clean?OW_DONE:OW_ERROR,lid,sid);close(3);
    fprintf(stderr,"PRODUCER_END result=%d cleanup=%d\n",result,clean);return result||!clean?2:0;
}
