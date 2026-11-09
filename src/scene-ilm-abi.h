/* Reconstructed used ABI from original HU 002_156_007 libilmControl ARM accesses.
 * No modern ILM header used. Unused tail is deliberately opaque, not invented. */
struct ilm_used_properties {
 double opacity;
 uint32_t source_x,source_y,source_w,source_h,original_w,original_h;
 uint32_t dest_x,dest_y,dest_w,dest_h;
 int32_t orientation;
 uint32_t visibility;
};
struct ilm_layer_properties {struct ilm_used_properties used;unsigned char opaque_tail[24];};
_Static_assert(sizeof(struct ilm_used_properties)==56,"used ILM prefix");
_Static_assert(sizeof(struct ilm_layer_properties)==80,"ELF layer memcpy size");
_Static_assert(offsetof(struct ilm_used_properties,visibility)==52,"callback visibility access");
typedef void (*ilm_notify)(uint32_t,struct ilm_used_properties *,uint32_t);
extern int ilm_initWithNativedisplay(void *),ilm_destroy(void),ilm_commitChanges(void);
extern int ilm_surfaceCreate(void *,uint32_t,uint32_t,int,uint32_t *),ilm_surfaceRemove(uint32_t);
extern int ilm_layerCreateWithDimension(uint32_t *,int,int),ilm_layerRemove(uint32_t);
extern int ilm_layerAddNotification(uint32_t,ilm_notify),ilm_surfaceAddNotification(uint32_t,ilm_notify);
extern int ilm_layerRemoveNotification(uint32_t),ilm_surfaceRemoveNotification(uint32_t);
extern int ilm_layerSetVisibility(uint32_t,uint32_t),ilm_surfaceSetVisibility(uint32_t,uint32_t);
extern int ilm_layerSetOpacity(uint32_t,double),ilm_surfaceSetOpacity(uint32_t,double);
extern int ilm_layerSetSourceRectangle(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern int ilm_surfaceSetSourceRectangle(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern int ilm_layerSetDestinationRectangle(uint32_t,int32_t,int32_t,uint32_t,uint32_t);
extern int ilm_surfaceSetDestinationRectangle(uint32_t,int32_t,int32_t,uint32_t,uint32_t);
extern int ilm_layerAddSurface(uint32_t,uint32_t),ilm_layerRemoveSurface(uint32_t,uint32_t);
extern int ilm_layerSetRenderOrder(uint32_t,uint32_t *,int),ilm_displaySetRenderOrder(uint32_t,uint32_t *,uint32_t);
extern int ilm_getScreenIDs(int *,uint32_t **),ilm_getLayerIDs(int *,uint32_t **),ilm_getSurfaceIDs(int *,uint32_t **);
extern int ilm_getLayerIDsOnScreen(uint32_t,int *,uint32_t **),ilm_getSurfaceIDsOnLayer(uint32_t,int *,uint32_t **);
extern int ilm_getPropertiesOfLayer(uint32_t,struct ilm_layer_properties *);
extern int ilm_layerGetVisibility(uint32_t,uint32_t *),ilm_surfaceGetVisibility(uint32_t,uint32_t *);
extern int ilm_layerGetOpacity(uint32_t,double *),ilm_surfaceGetOpacity(uint32_t,double *);
extern int ilm_surfaceGetDimension(uint32_t,uint32_t *);
extern int ilm_getScreenResolution(uint32_t,uint32_t *,uint32_t *);
/* 20-byte memcpy at libilmControl 0x3C44..0x3C58. Width/height offsets
 * confirmed by output mode listener 0x25C4. Unused word remains opaque. */
struct ilm_screen_properties {uint32_t count;uint32_t *ids;uint32_t opaque,width,height;};
_Static_assert(sizeof(struct ilm_screen_properties)==20,"screen properties copy");
extern int ilm_getPropertiesOfScreen(uint32_t,struct ilm_screen_properties *);
