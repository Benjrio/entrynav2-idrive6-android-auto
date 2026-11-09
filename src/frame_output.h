/* AA decoder adapter. EGL and the HMI socket remain owned by window producer. */
#include "frame_bridge.h"
#include "aa-phone-color.h"
static struct ow_aa_frame *window_frame;
static unsigned graphics_ok, graphics_blocked, present_count, bridge_drops;
static int graphics_open(void) {
    window_frame = ow_aa_map(4);
    graphics_ok = window_frame != NULL;
    if (!graphics_ok) puts("OWN_WINDOW_BRIDGE_ERROR fd=4");
    return graphics_ok;
}
static int graphics_hide(int hide) {
    if (window_frame) atomic_store(&window_frame->visible, !hide);
    return graphics_ok;
}
static void graphics_reset(void) {
    if (!window_frame) return;
    atomic_store(&window_frame->visible, 0);
    atomic_fetch_add(&window_frame->epoch, 1);
}
static int graphics_draw(const AVFrame *f, const char *out) {
    (void)out;
    if (!window_frame) return 0;
    struct AaColor color;
    if (f->width <= 0 || f->height <= 0 || f->width > (int)OW_AA_MAX_WIDTH ||
        f->height > (int)OW_AA_MAX_HEIGHT || f->format != AV_PIX_FMT_YUV420P ||
        !aa_color_setup(&color, f->color_range, f->colorspace, f->color_trc, 1)) {
        puts("OWN_WINDOW_FRAME_ERROR dimensions_or_color=1"); cancelled=1; return 0;
    }
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&window_frame->slot, &expected, 1,
            memory_order_acquire, memory_order_relaxed)) { bridge_drops++; return 1; }
    window_frame->width = f->width; window_frame->height = f->height;
    window_frame->frame_epoch = atomic_load(&window_frame->epoch);
    for (int y=0; y<f->height; y++) for (int x=0; x<f->width; x++) {
        uint32_t rgb = aa_color_rgb(&color, f->data[0][y*f->linesize[0]+x],
            f->data[1][(y/2)*f->linesize[1]+x/2], f->data[2][(y/2)*f->linesize[2]+x/2]);
        unsigned i=((unsigned)y*f->width+x)*3;
        window_frame->rgb[i]=rgb>>16; window_frame->rgb[i+1]=rgb>>8; window_frame->rgb[i+2]=rgb;
    }
    atomic_store_explicit(&window_frame->slot, 2, memory_order_release);
    atomic_store(&window_frame->visible, 1);
    present_count = atomic_load(&window_frame->submitted);
    return 1;
}
static void graphics_pump(void) {
    if (window_frame && atomic_load(&window_frame->stop)) cancelled=1;
}
static int graphics_close(void) {
    if (window_frame) {
        present_count=atomic_load(&window_frame->submitted);
        atomic_store(&window_frame->visible, 0);
        munmap(window_frame, sizeof(*window_frame)); window_frame=NULL;
    }
    graphics_ok=0;
    printf("OWN_WINDOW_BRIDGE_END dropped=%u submitted=%u\n", bridge_drops, present_count);
    return 1;
}
