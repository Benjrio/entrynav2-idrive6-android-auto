/* Place the private AA layer first on the sole display using live order.
 * IMPORTANT: vendor add_layer modifies a private snapshot; commit replaces
 * the shared order. This is neither atomic append nor a z-order guarantee.
 * Validate live order before committing changes to the shared screen. */
#pragma once
#include "screen_protocol.h"
#include "screen_order.h"
static int barrier(void);
static struct ivi_controller *aa_controller;
static struct ivi_controller_screen *aa_screen;
static struct ivi_controller_layer *aa_layer;
static struct wl_output *aa_output;
static unsigned aa_screens,aa_outputs,aa_screen_failed;
static void aa_output_geometry(void*d,struct wl_output*o,int32_t x,int32_t y,int32_t w,int32_t h,int32_t sub,const char*m,const char*model,int32_t tr){(void)d;(void)o;(void)x;(void)y;(void)w;(void)h;(void)sub;(void)m;(void)model;(void)tr;}
static void aa_output_mode(void*d,struct wl_output*o,uint32_t f,int32_t w,int32_t h,int32_t r){(void)d;(void)o;fprintf(stderr,"AA_OUTPUT_MODE flags=%u width=%d height=%d refresh=%d\n",f,w,h,r);}
static const struct wl_output_listener aa_output_events={.geometry=aa_output_geometry,.mode=aa_output_mode};
static void aa_screen_event(void*d,struct ivi_controller*c,uint32_t id,struct ivi_controller_screen*s){
    (void)d;(void)c;(void)id;aa_screens++;if(aa_screens==1)aa_screen=s;else aa_screen_failed=1;
}
static void aa_object_event(void*d,struct ivi_controller*c,uint32_t id){(void)d;(void)c;(void)id;}
static void aa_controller_error(void*d,struct ivi_controller*c,int32_t a,int32_t b,int32_t e,const char*s){
    (void)d;(void)c;aa_screen_failed=1;fprintf(stderr,"AA_SCREEN_ERROR object=%d operation=%d code=%d message=%s\n",a,b,e,s?s:"");
}
static const struct ivi_controller_listener aa_controller_events={aa_screen_event,aa_object_event,aa_object_event,aa_controller_error};
static void aa_layer_int(void*d,struct ivi_controller_layer*l,int32_t v){(void)d;(void)l;(void)v;}
static void aa_layer_rect(void*d,struct ivi_controller_layer*l,int32_t x,int32_t y,int32_t w,int32_t h){(void)d;(void)l;(void)x;(void)y;(void)w;(void)h;}
static void aa_layer_size(void*d,struct ivi_controller_layer*l,int32_t w,int32_t h){(void)d;(void)l;(void)w;(void)h;}
static void aa_layer_screen(void*d,struct ivi_controller_layer*l,struct wl_output*o){(void)d;(void)l;(void)o;}
static void aa_layer_destroyed(void*d,struct ivi_controller_layer*l){(void)d;(void)l;}
static const struct ivi_controller_layer_listener aa_layer_events={aa_layer_int,aa_layer_int,aa_layer_rect,aa_layer_rect,aa_layer_size,aa_layer_int,aa_layer_screen,aa_layer_destroyed};
static int aa_screen_protocol(void){
    const struct wl_interface *c=&ivi_controller_interface,*s=&ivi_controller_screen_interface;
    return c->version==1&&c->method_count==3&&c->event_count==4&&
        !strcmp(c->methods[1].name,"layer_create")&&!strcmp(c->methods[1].signature,"uiin")&&
        s->version==1&&s->method_count==5&&!strcmp(s->methods[2].name,"add_layer")&&
        !strcmp(s->methods[2].signature,"o");
}
static void aa_screen_global(struct wl_registry*r,uint32_t n,const char*name){
    if(!strcmp(name,"wl_output")){
        aa_outputs++;if(aa_output){aa_screen_failed=1;return;}
        aa_output=(struct wl_output*)wl_proxy_marshal_constructor((struct wl_proxy*)r,0,&wl_output_interface,n,name,1u,NULL);
        if(!aa_output||wl_output_add_listener(aa_output,&aa_output_events,NULL))aa_screen_failed=1;
        return;
    }
    if(strcmp(name,"ivi_controller"))return;
    if(aa_controller||!aa_screen_protocol()){aa_screen_failed=1;return;}
    aa_controller=(struct ivi_controller*)wl_proxy_marshal_constructor((struct wl_proxy*)r,0,&ivi_controller_interface,n,name,1u,NULL);
    if(!aa_controller||ivi_controller_add_listener(aa_controller,&aa_controller_events,NULL))aa_screen_failed=1;
}
static int aa_screen_inventory(uint32_t lid,const char *phase){
    int n=0,found=0;uint32_t *ids=NULL,foreign_hash=2166136261u;
    int rc=ilm_getLayerIDsOnScreen(0,&n,&ids);
    if(rc||n<0||n>=4096){free(ids);return -1;}
    for(int i=0;i<n;i++)if(ids[i]==lid)found++;else foreign_hash=(foreign_hash^ids[i])*16777619u;
    /* Getter callback-list enumeration is NOT screen render order. */
    fprintf(stderr,"AA_SCREEN_MEMBERSHIP phase=%s count=%d own=%d foreign_enumeration_hash=%08x\n",phase,n,found,foreign_hash);
    free(ids);return found;
}
static int aa_screen_attach(uint32_t lid){
    fprintf(stderr,"AA_SCREEN_READY outputs=%u screens=%u controller=%d screen=%d failed=%u protocol=%d\n",aa_outputs,aa_screens,aa_controller!=NULL,aa_screen!=NULL,aa_screen_failed,aa_screen_protocol());
    if(aa_screen_failed||aa_screens!=1||!aa_screen||!aa_controller||aa_layer)return 0;
    if(aa_screen_inventory(lid,"before")!=0)return 0;
    /* The ID is ours and was created/validated by ilm_layerCreateWithDimension.
     * layer_create here obtains a controller handle to the existing layer. */
    aa_layer=ivi_controller_layer_create(aa_controller,lid,OW_WIDTH,OW_HEIGHT);
    if(!aa_layer||ivi_controller_layer_add_listener(aa_layer,&aa_layer_events,NULL))return 0;
    if(barrier()||!aa_order_read(&aa_order_before)||aa_order_before.flip!=1||
       aa_order_before.size/4>=MAX_LAYERS)return 0;
    for(unsigned i=0;i<aa_order_before.size/4;i++)if(aa_order_before.ids[i]==lid)return 0;
    aa_order_expected=aa_order_before;
    memmove(aa_order_expected.ids+1,aa_order_before.ids,aa_order_before.size);
    aa_order_expected.ids[0]=lid;aa_order_expected.size+=4;
    struct snapshot latest;
    if(!snapshot(aa_order_before.context,&latest)||!aa_order_equal(&aa_order_before,&latest))return 0;
    struct wl_array order={.size=aa_order_expected.size,.alloc=aa_order_expected.size,.data=aa_order_expected.ids};
    ivi_controller_screen_set_render_order(aa_screen,&order);
    ivi_controller_commit_changes(aa_controller);
    if(barrier()||!aa_order_verify())return 0;
    fprintf(stderr,"AA_SCREEN_FRONT layer=%u foreign_count=%u foreign_relative_order_verified=1 atomic=0\n",lid,aa_order_before.size/4);
    return 1;
}
static void aa_screen_close(void){
    aa_order_close();
    /* Actual own layer was already removed via ILM; release observer handles. */
    if(aa_layer)wl_proxy_destroy((struct wl_proxy*)aa_layer);
    if(aa_screen)ivi_controller_screen_destroy(aa_screen);
    if(aa_controller)ivi_controller_destroy(aa_controller);
    if(aa_output)wl_output_destroy(aa_output);
    aa_layer=NULL;aa_screen=NULL;aa_controller=NULL;
}
