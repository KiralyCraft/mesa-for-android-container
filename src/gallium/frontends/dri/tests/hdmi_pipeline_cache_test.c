#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <threads.h>
#include <sys/mman.h>
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#define ALIGN_POT(v,a) (((v)+(a)-1)&~((a)-1))
#define p_atomic_read(p) __atomic_load_n(p,__ATOMIC_SEQ_CST)
#define p_atomic_set(p,v) __atomic_store_n(p,v,__ATOMIC_SEQ_CST)
#define p_atomic_dec_return(p) __atomic_sub_fetch(p,1,__ATOMIC_SEQ_CST)
#define mesa_loge(...) ((void)0)
#define mesa_logi(...) ((void)0)
#define __DRI_IMAGE_ATTRIB_NUM_PLANES 1
#define __DRI_IMAGE_ATTRIB_FOURCC 2
#define __DRI_IMAGE_ATTRIB_FD 3
#define FORMAT 0x34325258
struct hdmi_pipe;
typedef struct { unsigned width_in_pixels,height_in_pixels; } xcb_screen_t;
struct dri_image { unsigned width,height; uint64_t bytes; };
struct loader_dri3_buffer { struct dri_image *image; unsigned width,height,cpp; int pipeline_refs; };
struct loader_dri3_drawable { struct hdmi_pipe *hdmi_pipeline; xcb_screen_t *screen; unsigned depth,drawable; void *dri_screen_render_gpu; bool multiplanes_available; };
typedef struct { int tag; } xcb_connection_t;
typedef void xcb_special_event_t;
typedef unsigned xcb_pixmap_t;
typedef unsigned xcb_xfixes_region_t;
typedef unsigned xcb_present_event_t;
typedef struct { int error_code; } xcb_generic_error_t;
static xcb_connection_t allocation={1}, events={2};
static mtx_t *event_lock;
static _Thread_local int owns_event;
static int checked_lock(mtx_t *m) { int r=mtx_lock(m); if(m==event_lock){assert(!owns_event);owns_event=1;} return r; }
static int checked_unlock(mtx_t *m) { if(m==event_lock){assert(owns_event);owns_event=0;} return mtx_unlock(m); }
static int checked_wait(cnd_t *c,mtx_t *m) { assert(m==event_lock&&owns_event);owns_event=0;int r=cnd_wait(c,m);owns_event=1;return r; }
#define mtx_lock checked_lock
#define mtx_unlock checked_unlock
#define cnd_wait checked_wait
static int64_t os_time_get(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*INT64_C(1000000)+t.tv_nsec/1000; }
static unsigned next_id, live_pixmaps, live_images, live_regions, created, destroyed;
static int fail_create, fail_import, fail_region, create_pending_error;
static uint64_t padded_bytes;
static struct { unsigned w,h;bool live; } pixmaps[4096];
static bool regions[4096];
static void check_external(xcb_connection_t *c) { assert(c==&allocation);assert(!owns_event); }
static unsigned xcb_generate_id(xcb_connection_t *c) {check_external(c);assert(++next_id<4096);return next_id;}
static int xcb_create_pixmap_checked(xcb_connection_t *c,unsigned depth,unsigned id,unsigned win,unsigned w,unsigned h) {
 (void)depth;(void)win;check_external(c);
 if(fail_create){fail_create=0;create_pending_error=1;return 0;}
 pixmaps[id].w=w;pixmaps[id].h=h;pixmaps[id].live=true;live_pixmaps++;created++;return 0;
}
static xcb_generic_error_t *xcb_request_check(xcb_connection_t *c,int cookie) {
 (void)cookie;check_external(c);if(!create_pending_error)return NULL;create_pending_error=0;return calloc(1,sizeof(xcb_generic_error_t));
}
static void xcb_free_pixmap(xcb_connection_t *c,unsigned id) {check_external(c);assert(pixmaps[id].live);pixmaps[id].live=false;live_pixmaps--;destroyed++;}
static int xcb_xfixes_create_region_checked(xcb_connection_t *c,unsigned id,unsigned count,void *rects) {
 check_external(c);assert(!count&&!rects);
 if(fail_region){fail_region=0;create_pending_error=1;return 0;}
 assert(!regions[id]);regions[id]=true;live_regions++;return 0;
}
static void xcb_xfixes_destroy_region(xcb_connection_t *c,unsigned id) {
 check_external(c);assert(regions[id]);regions[id]=false;live_regions--;
}
static void xcb_flush(xcb_connection_t *c) {check_external(c);}
static struct dri_image *loader_dri3_get_pixmap_buffer(xcb_connection_t *c,unsigned id,void *screen,unsigned fourcc,bool multi,int *w,int *h,void *unused) {
 (void)screen;(void)multi;(void)unused;check_external(c);assert(fourcc==FORMAT);
 if(fail_import){fail_import=0;return NULL;}assert(pixmaps[id].live);
 struct dri_image *image=calloc(1,sizeof(*image));assert(image);
 *w=image->width=pixmaps[id].w;*h=image->height=pixmaps[id].h;
 image->bytes=padded_bytes?padded_bytes:(uint64_t)*w**h*4;live_images++;return image;
}
static bool dri2_query_image(struct dri_image *image,int attr,int *result) {
 if(attr==__DRI_IMAGE_ATTRIB_FOURCC){*result=FORMAT;return true;}
 assert(!owns_event);
 if(attr==__DRI_IMAGE_ATTRIB_NUM_PLANES){*result=1;return true;}
 assert(attr==__DRI_IMAGE_ATTRIB_FD);*result=memfd_create("hdmi-cache-test",MFD_CLOEXEC);assert(*result>=0);assert(!ftruncate(*result,image->bytes));return true;
}
static void dri2_destroy_image(struct dri_image *image) {assert(!owns_event);assert(live_images);live_images--;free(image);}
/* PRODUCTION */
static void dri3_destroy_render_buffer(struct loader_dri3_drawable *draw,struct loader_dri3_buffer *buffer) {(void)draw;(void)buffer;assert(!owns_event);}
static void init(struct hdmi_pipe *p,struct loader_dri3_drawable *d) {
 memset(p,0,sizeof(*p));p->draw=d;d->hdmi_pipeline=p;p->conn=&events;p->allocation_conn=&allocation;
 assert(mtx_init(&p->lock,mtx_plain)==thrd_success);assert(mtx_init(&p->admission_lock,mtx_plain)==thrd_success);assert(cnd_init(&p->changed)==thrd_success);
 event_lock=&p->lock;assert(thrd_create(&p->retire_worker,hdmi_pipe_retire_thread,p)==thrd_success);
}
static void fini(struct hdmi_pipe *p) {
 mtx_lock(&p->lock);p->retire_stop=true;cnd_broadcast(&p->changed);mtx_unlock(&p->lock);thrd_join(p->retire_worker,NULL);
 for(unsigned i=0;i<HDMI_PIPE_GENERATIONS;i++)hdmi_pipe_destroy_generation(p,&p->generations[i]);
 cnd_destroy(&p->changed);mtx_destroy(&p->lock);mtx_destroy(&p->admission_lock);event_lock=NULL;assert(!live_images&&!live_pixmaps&&!live_regions);
}
static struct hdmi_pipe_generation *admit(struct hdmi_pipe *p,unsigned w,unsigned h) {
 struct dri_image image={.width=w,.height=h};struct loader_dri3_buffer b={.image=&image,.width=w,.height=h,.cpp=4};
 mtx_lock(&p->admission_lock);mtx_lock(&p->lock);struct hdmi_pipe_generation *g=hdmi_pipe_generation(p,&b);mtx_unlock(&p->lock);mtx_unlock(&p->admission_lock);return g;
}
struct admission {struct hdmi_pipe *p;struct hdmi_pipe_generation *result;int done;};
static int async_admit(void *data) {struct admission *a=data;a->result=admit(a->p,40,30);p_atomic_set(&a->done,1);return 0;}
static void wait_test(bool stop) {
 struct hdmi_pipe p;struct loader_dri3_drawable d={0};init(&p,&d);
 for(unsigned i=1;i<=3;i++)admit(&p,i*10,30)->slots[0].phase=HDMI_PRESENTED;
 struct admission a={.p=&p};thrd_t thread;assert(thrd_create(&thread,async_admit,&a)==thrd_success);
 struct timespec delay={.tv_nsec=20000000};thrd_sleep(&delay,NULL);assert(!p_atomic_read(&a.done));
 mtx_lock(&p.lock);if(stop)p_atomic_set(&p.stop,1);else p.generations[0].slots[0].phase=HDMI_FREE;cnd_broadcast(&p.changed);mtx_unlock(&p.lock);
 thrd_join(thread,NULL);assert(stop?!a.result:!!a.result);fini(&p);
}
int main(void) {
 struct hdmi_pipe p;struct loader_dri3_drawable d={0};init(&p,&d);
 uint64_t ids[3];for(unsigned i=0;i<3;i++)ids[i]=admit(&p,800+i,600)->identity;
 for(unsigned repeat=0;repeat<10;repeat++)for(unsigned i=0;i<3;i++)assert(admit(&p,800+i,600)->identity==ids[i]);
 assert(p.generation==3&&p.cache_hits==30&&live_pixmaps==9&&p.cache_evictions==0);
 /* All nonfree states must pin the generation. */
 for(int phase=HDMI_RESERVED;phase<=HDMI_QUARANTINED;phase++){p.generations[0].slots[1].phase=phase;assert(!hdmi_pipe_generation_idle(&p.generations[0]));}p.generations[0].slots[1].phase=HDMI_FREE;
 struct hdmi_pipe_generation *pinned=admit(&p,800,600);pinned->slots[0].phase=HDMI_PRESENTED;
 assert(admit(&p,900,600));assert(p.cache_evictions==1&&pinned->identity==ids[0]);assert(live_pixmaps==9);
 fini(&p);
 /* Real backing size controls admission, not the width*height estimate. */
 init(&p,&d);padded_bytes=60ULL*1024*1024;
 assert(admit(&p,10,10));assert(admit(&p,20,10));assert(admit(&p,30,10));
 assert(p.cache_evictions==1&&p.generation==3&&live_pixmaps==6);padded_bytes=0;fini(&p);
 init(&p,&d);fail_create=1;assert(!admit(&p,10,10));assert(!live_pixmaps&&!live_images);
 fail_import=1;assert(!admit(&p,10,10));assert(!live_pixmaps&&!live_images);assert(admit(&p,10,10));fini(&p);
 wait_test(false);wait_test(true);
 /* Logical sizes within a capacity share storage, including while a previous
  * frame pins a slot. Their image geometry stays immutable. */
 init(&p,&d);p.resize_capacity=true;
 struct hdmi_pipe_generation *capacity=admit(&p,641,479);
 assert(capacity&&capacity->width==768&&capacity->height==512&&live_regions==3);
 capacity->slots[0].phase=HDMI_PRESENTED;
 uint64_t identity=capacity->identity;
 for(unsigned i=0;i<100;i++) {
  struct hdmi_pipe_generation *g=admit(&p,641+i,479+i%30);
  assert(g==capacity&&g->identity==identity&&g->width==768&&g->height==512);
 }
 assert(p.generation==1&&p.capacity_reuses==100&&p.cache_evictions==0);
 assert(admit(&p,769,513)->identity!=identity);
 assert(capacity->identity==identity&&capacity->slots[0].phase==HDMI_PRESENTED);
 fini(&p);
 /* Preserve an exact fullscreen allocation instead of reusing a padded
  * windowed capacity that would prevent scanout eligibility. */
 xcb_screen_t screen={3840,2160};d.screen=&screen;
 init(&p,&d);p.resize_capacity=true;
 struct hdmi_pipe_generation *windowed=admit(&p,3839,2159);
 identity=windowed->identity;assert(windowed->width==3840&&windowed->height==2176);
 struct hdmi_pipe_generation *fullscreen=admit(&p,3840,2160);
 assert(fullscreen->identity!=identity&&fullscreen->width==3840&&fullscreen->height==2160);
 assert(admit(&p,3838,2158)->identity==identity);fini(&p);d.screen=NULL;
 init(&p,&d);p.resize_capacity=true;fail_region=1;
 assert(!admit(&p,641,479));assert(!live_pixmaps&&!live_images&&!live_regions);
 assert(admit(&p,641,479));fini(&p);
 assert(created==destroyed);puts("PASS: production cache reuse, bounded padded allocations, busy pinning, event-lock freedom, retirement, failure cleanup, wait and stop; logical resize capacity, exact fullscreen and region lifetime");
}
