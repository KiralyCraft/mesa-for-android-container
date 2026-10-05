/* SPDX-License-Identifier: MIT
 * Opt-in C/D pipeline. Included by loader_dri3_helper.c: its DRI helpers and
 * drawable ABI must come from the same Mesa build. No native fence is passed
 * as an X11 shared-memory fence; Present is sent only after successful readiness.
 */
#include <sys/stat.h>

#define HDMI_PIPE_GENERATIONS 3
#define HDMI_PIPE_SLOTS 3
#define HDMI_PIPE_BYTES (512ULL * 1024 * 1024)
#define HDMI_PIPE_RETIRE 16

enum hdmi_pipe_phase { HDMI_FREE, HDMI_RESERVED, HDMI_PRODUCER, HDMI_COPY, HDMI_READY, HDMI_PRESENTED, HDMI_QUARANTINED };
struct hdmi_pipe_slot {
   xcb_pixmap_t pixmap;
   struct dri_image *image;
   struct loader_dri3_buffer *source;
   enum hdmi_pipe_phase phase;
   int fence_fd, interval;
   uint32_t serial;
   uint64_t order, sbc;
   bool completed, idle, integrated;
   int64_t started_ns;
};
struct hdmi_pipe_generation {
   unsigned width, height, fourcc;
   uint64_t identity, bytes;
   bool active;
   struct hdmi_pipe_slot slots[HDMI_PIPE_SLOTS];
};
struct hdmi_pipe {
   struct loader_dri3_drawable *draw;
   xcb_connection_t *conn;
   xcb_special_event_t *events;
   xcb_present_event_t eid;
   uint32_t stamp, serial;
   mtx_t lock;
   cnd_t changed;
   thrd_t worker, retire_worker;
   int wake_fd, stop;
   int failed;
   bool primed, threads_started;
   uint64_t msc, target_msc, generation, submitted, copied, resolved, completed, accepted, next_order;
   uint64_t producer_us, submit_us, gpu_us, mutex_us;
   struct hdmi_pipe_generation generations[HDMI_PIPE_GENERATIONS];
   struct loader_dri3_buffer *retire[HDMI_PIPE_RETIRE];
   unsigned retire_count;
   bool retire_stop;
};

static void dri3_destroy_render_buffer(struct loader_dri3_drawable *draw,
                                      struct loader_dri3_buffer *buffer);

static void
hdmi_pipe_wake(struct hdmi_pipe *p)
{
   uint64_t one = 1;
   if (write(p->wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
      mesa_loge("DRI3: HDMI pipeline wake failed");
}

/* An owner reference plus one reference for each outstanding transfer. Final
 * destruction belongs to a separate retirement worker: KGSL BO destruction
 * can wait for driver fences and must not stop presentation event processing. */
static void
hdmi_pipe_buffer_unref(struct loader_dri3_drawable *draw,
                       struct loader_dri3_buffer *buffer)
{
   struct hdmi_pipe *p = draw->hdmi_pipeline;
   if (p_atomic_dec_return(&buffer->pipeline_refs) != 0)
      return;
   mtx_lock(&p->lock);
   while (p->retire_count == HDMI_PIPE_RETIRE)
      cnd_wait(&p->changed, &p->lock);
   p->retire[p->retire_count++] = buffer;
   cnd_broadcast(&p->changed);
   mtx_unlock(&p->lock);
}

static int
hdmi_pipe_retire_thread(void *data)
{
   struct hdmi_pipe *p = data;
   for (;;) {
      mtx_lock(&p->lock);
      while (!p->retire_count && !p->retire_stop)
         cnd_wait(&p->changed, &p->lock);
      if (!p->retire_count && p->retire_stop) {
         mtx_unlock(&p->lock);
         return 0;
      }
      struct loader_dri3_buffer *buffer = p->retire[--p->retire_count];
      cnd_broadcast(&p->changed);
      mtx_unlock(&p->lock);
      dri3_destroy_render_buffer(p->draw, buffer);
   }
}

static bool
hdmi_pipe_generation_idle(struct hdmi_pipe_generation *g)
{
   for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++)
      if (g->slots[i].phase != HDMI_FREE)
         return false;
   return true;
}

static void
hdmi_pipe_destroy_generation(struct hdmi_pipe *p, struct hdmi_pipe_generation *g)
{
   for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++) {
      struct hdmi_pipe_slot *s = &g->slots[i];
      if (s->pixmap)
         xcb_free_pixmap(p->conn, s->pixmap);
      if (s->image)
         dri2_destroy_image(s->image);
   }
   memset(g, 0, sizeof(*g));
}

/* p->lock protects generations. XCB itself serializes access to the private
 * connection. No generation is recycled until both completion and idle arrive. */
static struct hdmi_pipe_generation *
hdmi_pipe_generation(struct hdmi_pipe *p, struct loader_dri3_buffer *buffer)
{
   int fourcc;
   if (!dri2_query_image(buffer->image, __DRI_IMAGE_ATTRIB_FOURCC, &fourcc))
      return NULL;
   if (buffer->cpp != 4 || !buffer->width || !buffer->height ||
       buffer->width > UINT16_MAX || buffer->height > UINT16_MAX ||
       (uint64_t)buffer->width * buffer->height * 4 * HDMI_PIPE_SLOTS > HDMI_PIPE_BYTES)
      return NULL;
   for (;;) {
      unsigned vacant = HDMI_PIPE_GENERATIONS;
      uint64_t bytes = 0;
      for (unsigned i = 0; i < HDMI_PIPE_GENERATIONS; i++) {
         struct hdmi_pipe_generation *g = &p->generations[i];
         if (g->identity && g->width == buffer->width && g->height == buffer->height && g->fourcc == (unsigned)fourcc) {
            for (unsigned j = 0; j < HDMI_PIPE_GENERATIONS; j++) p->generations[j].active = false;
            g->active = true;
            return g;
         }
      }
      /* The old active generation may itself be idle. Retire it before
       * backpressure, rather than waiting for an impossible active transition. */
      for (unsigned i = 0; i < HDMI_PIPE_GENERATIONS; i++) {
         struct hdmi_pipe_generation *g = &p->generations[i];
         g->active = false;
         if (g->identity && hdmi_pipe_generation_idle(g))
            hdmi_pipe_destroy_generation(p, g);
         if (!g->identity) vacant = i;
         else bytes += g->bytes;
      }
      if (vacant != HDMI_PIPE_GENERATIONS && bytes + (uint64_t)buffer->width * buffer->height * 4 * HDMI_PIPE_SLOTS <= HDMI_PIPE_BYTES) {
         struct hdmi_pipe_generation *g = &p->generations[vacant];
         g->width = buffer->width; g->height = buffer->height; g->fourcc = fourcc;
         g->identity = ++p->generation;
         for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++) {
            struct hdmi_pipe_slot *s = &g->slots[i];
            int width = 0, height = 0;
            s->fence_fd = -1;
            s->pixmap = xcb_generate_id(p->conn);
            xcb_generic_error_t *error = xcb_request_check(p->conn,
               xcb_create_pixmap_checked(p->conn, p->draw->depth, s->pixmap,
                                        p->draw->drawable, g->width, g->height));
            if (error) { free(error); hdmi_pipe_destroy_generation(p, g); return NULL; }
            s->image = loader_dri3_get_pixmap_buffer(p->conn, s->pixmap,
               p->draw->dri_screen_render_gpu, fourcc, p->draw->multiplanes_available,
               &width, &height, NULL);
            if (!s->image || width != g->width || height != g->height) {
               hdmi_pipe_destroy_generation(p, g);
               return NULL;
            }
            int dma_fd = -1, planes = 0, image_fourcc = 0;
            struct stat allocation;
            bool valid = dri2_query_image(s->image, __DRI_IMAGE_ATTRIB_NUM_PLANES, &planes) && planes == 1 &&
               dri2_query_image(s->image, __DRI_IMAGE_ATTRIB_FOURCC, &image_fourcc) && image_fourcc == fourcc &&
               dri2_query_image(s->image, __DRI_IMAGE_ATTRIB_FD, &dma_fd) && dma_fd >= 0 &&
               fstat(dma_fd, &allocation) == 0 && allocation.st_size > 0;
            if (dma_fd >= 0) close(dma_fd);
            if (!valid || (uint64_t)allocation.st_size > HDMI_PIPE_BYTES - bytes - g->bytes) {
               hdmi_pipe_destroy_generation(p, g);
               return NULL;
            }
            g->bytes += allocation.st_size;

         }
         for (unsigned i = 0; i < HDMI_PIPE_GENERATIONS; i++) p->generations[i].active = false;
         g->active = true;
         mesa_logi("DRI3: HDMI pipeline generation=%" PRIu64 " size=%ux%u Xorg-owned GPU buffers",
                   g->identity, g->width, g->height);
         return g;
      }
      if (p_atomic_read(&p->stop) || p_atomic_read(&p->failed)) return NULL;
      /* Bound resize storms by applying backpressure, while the worker keeps
       * servicing completion/idle events and transfer fences. */
      cnd_wait(&p->changed, &p->lock);
   }
}

static void
hdmi_pipe_events(struct hdmi_pipe *p)
{
   xcb_generic_event_t *event;
   while ((event = xcb_poll_for_special_event(p->conn, p->events))) {
      xcb_present_generic_event_t *ge = (void *)event;
      uint64_t completed_sbc=0, completed_msc=0, completed_ust=0;
      mtx_lock(&p->lock);
      for (unsigned g = 0; g < HDMI_PIPE_GENERATIONS; g++) {
         for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++) {
            struct hdmi_pipe_slot *s = &p->generations[g].slots[i];
            if (s->phase != HDMI_PRESENTED) continue;
            if (ge->evtype == XCB_PRESENT_EVENT_COMPLETE_NOTIFY) {
               xcb_present_complete_notify_event_t *ce = (void *)ge;
               if (ce->kind == XCB_PRESENT_COMPLETE_KIND_PIXMAP && ce->serial == s->serial) {
                  if (!s->completed) {
                     s->completed = true; p->completed++; p->msc = ce->msc;
                     completed_sbc=s->sbc; completed_msc=ce->msc; completed_ust=ce->ust;
                  }
                  if (!p->primed || p->target_msc <= ce->msc) p->target_msc = ce->msc + 2;
                  p->primed = true;
               }
            } else if (ge->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
               xcb_present_idle_notify_event_t *ie = (void *)ge;
               if (ie->pixmap == s->pixmap && ie->serial == s->serial) s->idle = true;
            }
            if (s->completed && s->idle) s->phase = HDMI_FREE;
         }
      }
      cnd_broadcast(&p->changed);
      mtx_unlock(&p->lock);
      if (completed_sbc) {
         mtx_lock(&p->draw->mtx);
         if (completed_sbc > p->draw->recv_sbc) {
            p->draw->recv_sbc=completed_sbc;
            p->draw->msc=completed_msc; p->draw->ust=completed_ust;
         }
         cnd_broadcast(&p->draw->event_cnd);
         mtx_unlock(&p->draw->mtx);
      }
      free(event);
   }
   while ((event = xcb_poll_for_event(p->conn))) {
      if (!(event->response_type & 0x7f)) {
         mesa_loge("DRI3: HDMI pipeline X error %u", ((xcb_generic_error_t *)event)->error_code);
         p_atomic_set(&p->failed, true);
      }
      free(event);
   }
   if (xcb_connection_has_error(p->conn)) p_atomic_set(&p->failed, true);
}

static int
hdmi_pipe_native_fence(struct dri_context *ctx)
{
   void *fence = dri_create_fence_fd(ctx, -1);
   if (!fence) return -1;
   int fd = dri_get_fence_fd(ctx->screen, fence);
   dri_destroy_fence(ctx->screen, fence);
   return fd;
}

static void
hdmi_pipe_release_source(struct hdmi_pipe *p, struct hdmi_pipe_slot *s)
{
   struct loader_dri3_buffer *source = s->source;
   s->source = NULL;
   if (!source) return;
   mtx_unlock(&p->lock);
   mtx_lock(&p->draw->mtx);
   source->busy = false;
   cnd_broadcast(&p->draw->event_cnd);
   mtx_unlock(&p->draw->mtx);
   hdmi_pipe_buffer_unref(p->draw, source);
   mtx_lock(&p->lock);
}

/* Only native sync_file success authorizes Present. An unknown outcome is
 * quarantined; it never becomes a ready image or a reusable application BO. */
static int
hdmi_pipe_fence_status(int fd)
{
   struct pollfd ready = {.fd=fd, .events=POLLIN};
   struct sync_file_info info = {0};
   int ret = poll(&ready, 1, 0);
   if (!ret) return 0;
   if (ret < 0 && errno == EINTR) return 0;
   if (ret < 0 || !(ready.revents & POLLIN) ||
       ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0 || !info.status)
      return -2;
   return info.status > 0 ? 1 : -1;
}

static int
hdmi_pipe_thread(void *data)
{
   struct hdmi_pipe *p = data;
   for (;;) {
      struct pollfd fds[2 + HDMI_PIPE_GENERATIONS * HDMI_PIPE_SLOTS];
      unsigned count = 2;
      bool transfers = false;
      hdmi_pipe_events(p);
      mtx_lock(&p->lock);
      fds[0] = (struct pollfd){.fd=xcb_get_file_descriptor(p->conn), .events=POLLIN};
      fds[1] = (struct pollfd){.fd=p->wake_fd, .events=POLLIN};
      for (unsigned g = 0; g < HDMI_PIPE_GENERATIONS; g++) {
         struct hdmi_pipe_generation *gen = &p->generations[g];
         for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++) {
            struct hdmi_pipe_slot *s = &gen->slots[i];
            if (s->phase == HDMI_RESERVED) { transfers = true; continue; }
            if (s->phase != HDMI_PRODUCER && s->phase != HDMI_COPY) continue;
            int status = hdmi_pipe_fence_status(s->fence_fd);
            if (!status) { transfers = true; fds[count++] = (struct pollfd){.fd=s->fence_fd,.events=POLLIN}; continue; }
            close(s->fence_fd); s->fence_fd = -1;
            if (status < 0) {
               p_atomic_set(&p->failed,true);
               s->phase = HDMI_QUARANTINED;
               continue;
            }
            int64_t now = os_time_get();
            if (s->phase == HDMI_PRODUCER && !s->integrated)
               p->producer_us += now - s->started_ns;
            else p->gpu_us += now - s->started_ns;
            if (s->phase == HDMI_PRODUCER && !s->integrated && !p_atomic_read(&p->failed) && !p_atomic_read(&p->stop)) {
               struct dri_image *destination = s->image;
               struct loader_dri3_buffer *source = s->source;
               mtx_unlock(&p->lock);
               int64_t start = os_time_get();
               struct dri_context *ctx = loader_dri3_blit_context_get(p->draw);
               int64_t acquired = os_time_get();
               int fd = -1;
               if (ctx) {
                  dri2_blit_image(ctx,destination,source->image,0,0,gen->width,gen->height,0,0,gen->width,gen->height,0);
                  fd = hdmi_pipe_native_fence(ctx);
                  if (fd < 0) {
                     /* Error recovery only: retain ordering and drain the
                      * submitted operation before releasing source ownership. */
                     dri2_blit_image(ctx,destination,source->image,0,0,gen->width,gen->height,0,0,gen->width,gen->height,__BLIT_FLAG_FINISH);
                  }
               }
               loader_dri3_blit_context_put();
               mtx_lock(&p->lock);
               p->mutex_us += acquired - start; p->submit_us += os_time_get() - acquired;
               if (fd < 0) { p_atomic_set(&p->failed,true); hdmi_pipe_release_source(p,s); s->phase=HDMI_FREE; continue; }
               s->fence_fd=fd; s->phase=HDMI_COPY; s->started_ns=os_time_get(); p->copied++;
               transfers=true; fds[count++] = (struct pollfd){.fd=fd,.events=POLLIN};
               continue;
            }
            hdmi_pipe_release_source(p,s);
            s->phase=HDMI_READY;
         }
      }
      /* Preserve accepted-frame FIFO across slots and resize generations.
       * Initial/re-prime MSC lead remains the measured bridge's +2. */
      for (;;) {
         struct hdmi_pipe_slot *next=NULL;
         for (unsigned g=0;g<HDMI_PIPE_GENERATIONS;g++) for (unsigned i=0;i<HDMI_PIPE_SLOTS;i++) {
            struct hdmi_pipe_slot *s=&p->generations[g].slots[i];
            if (s->phase != HDMI_FREE && s->phase != HDMI_PRESENTED && s->order==p->next_order) next=s;
         }
         if (!next || next->phase != HDMI_READY) break;
         if (p_atomic_read(&p->failed) || p_atomic_read(&p->stop)) { next->phase=HDMI_FREE; p->next_order++; continue; }
         if (p->submitted && !p->primed && next->interval > 0) break;
         uint64_t target=p->primed ? p->target_msc : 0;
         if (p->primed) p->target_msc += MAX2(abs(next->interval),1);
         next->serial=++p->serial; next->completed=next->idle=false;
         next->phase=HDMI_PRESENTED; p->submitted++; p->next_order++;
         uint32_t options=next->interval <= 0 ? XCB_PRESENT_OPTION_ASYNC : XCB_PRESENT_OPTION_NONE;
         xcb_present_pixmap(p->conn,p->draw->drawable,next->pixmap,next->serial,0,0,0,0,XCB_NONE,XCB_NONE,XCB_NONE,options,target,0,0,0,NULL);
         xcb_flush(p->conn);
      }
      cnd_broadcast(&p->changed);
      bool done=p_atomic_read(&p->stop) && !transfers;
      mtx_unlock(&p->lock);
      /* Never nest draw->mtx under the generation lock: resize holds draw's
       * lock while retiring buffers. Faults must wake back-buffer admission. */
      mtx_lock(&p->draw->mtx); cnd_broadcast(&p->draw->event_cnd); mtx_unlock(&p->draw->mtx);
      if (done) break;
      int ret;
      do { ret=poll(fds,count,100); } while (ret<0 && errno==EINTR);
      if (ret<0) p_atomic_set(&p->failed,true);
      if (fds[1].revents) { uint64_t value; while(read(p->wake_fd,&value,sizeof(value))==sizeof(value)) {} }
   }
   return 0;
}

static bool
hdmi_pipe_init(struct loader_dri3_drawable *draw)
{
   if (draw->hdmi_pipeline) return true;
   struct hdmi_pipe *p = calloc(1,sizeof(*p));
   if (!p) return false;
   p->draw = draw; p->next_order=1;
   p->wake_fd = eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
   if (p->wake_fd < 0) { free(p); return false; }
   if (mtx_init(&p->lock,mtx_plain) != thrd_success) { close(p->wake_fd); free(p); return false; }
   if (cnd_init(&p->changed) != thrd_success) { mtx_destroy(&p->lock); close(p->wake_fd); free(p); return false; }
   p->conn = xcb_connect(NULL,NULL);
   if (!p->conn || xcb_connection_has_error(p->conn)) goto fail;
   p->eid = xcb_generate_id(p->conn);
   xcb_generic_error_t *error = xcb_request_check(p->conn,
      xcb_present_select_input_checked(p->conn,p->eid,draw->drawable,
         XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY|XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY));
   if (error) { free(error); goto fail; }
   p->events = xcb_register_for_special_xge(p->conn,&xcb_present_id,p->eid,&p->stamp);
   if (!p->events) goto fail;
   draw->hdmi_pipeline = p;
   if (thrd_create(&p->retire_worker,hdmi_pipe_retire_thread,p) != thrd_success) goto fail;
   if (thrd_create(&p->worker,hdmi_pipe_thread,p) != thrd_success) {
      mtx_lock(&p->lock); p->retire_stop = true; cnd_broadcast(&p->changed); mtx_unlock(&p->lock);
      thrd_join(p->retire_worker,NULL); goto fail;
   }
   p->threads_started = true;
   mesa_logi("DRI3: HDMI_LOS_MESA_PIPELINE_ABI=1 persistent GPU pipeline; resolve=%s",
             draw->hdmi_pipeline_resolve ? "application-context" : "worker-context");
   return true;
fail:
   draw->hdmi_pipeline = NULL;
   if (p->events) xcb_unregister_for_special_event(p->conn,p->events);
   if (p->conn) xcb_disconnect(p->conn);
   cnd_destroy(&p->changed); mtx_destroy(&p->lock); close(p->wake_fd); free(p);
   return false;
}

static bool
hdmi_pipe_present(struct loader_dri3_drawable *draw, struct loader_dri3_buffer *buffer,
                  int producer_fd, unsigned flush_flags)
{
   if (!hdmi_pipe_init(draw)) { if (producer_fd >= 0) close(producer_fd); return false; }
   struct hdmi_pipe *p = draw->hdmi_pipeline;
   mtx_lock(&p->lock);
   struct hdmi_pipe_generation *g = hdmi_pipe_generation(p,buffer);
   struct hdmi_pipe_slot *s = NULL;
   while (g && !p_atomic_read(&p->failed) && !p_atomic_read(&p->stop)) {
      for (unsigned i=0;i<HDMI_PIPE_SLOTS;i++) if (g->slots[i].phase == HDMI_FREE) { s=&g->slots[i]; break; }
      if (s) break;
      cnd_wait(&p->changed,&p->lock);
   }
   if (!s) { mtx_unlock(&p->lock); if (producer_fd >= 0) close(producer_fd); return false; }
   s->source=buffer; s->interval=draw->swap_interval; s->integrated=draw->hdmi_pipeline_resolve;
   p_atomic_inc(&buffer->pipeline_refs);
   /* Reserve before releasing the generation lock for application submission. */
   s->phase=HDMI_RESERVED; s->fence_fd=-1; s->order=++p->accepted; s->started_ns=os_time_get();
   mtx_unlock(&p->lock);
   mtx_lock(&draw->mtx); buffer->busy=true; mtx_unlock(&draw->mtx);
   int fd=producer_fd;
   if (s->integrated) {
      struct dri_context *ctx=draw->vtable->get_dri_context(draw);
      if (producer_fd >= 0) close(producer_fd);
      fd=-1;
      if (ctx && draw->vtable->in_current_context(draw) && draw->vtable->flush_drawable_with_fence_fd) {
         dri2_blit_image(ctx,s->image,buffer->image,0,0,g->width,g->height,0,0,g->width,g->height,0);
         fd=draw->vtable->flush_drawable_with_fence_fd(draw,flush_flags);
         if (fd < 0)
            dri2_blit_image(ctx,s->image,buffer->image,0,0,g->width,g->height,0,0,g->width,g->height,__BLIT_FLAG_FINISH);
      }
   }
   if (fd >= 0) {
      mtx_lock(&draw->mtx);
      s->sbc=++draw->send_sbc; buffer->last_swap=s->sbc;
      if (draw->stamp) ++(*draw->stamp);
      draw->cur_back=(draw->cur_back+1)%draw->max_num_back;
      mtx_unlock(&draw->mtx);
   }
   mtx_lock(&p->lock);
   s->fence_fd=fd; s->phase=HDMI_PRODUCER; s->started_ns=os_time_get();
   if (fd < 0) { p_atomic_set(&p->failed,true); hdmi_pipe_release_source(p,s); s->phase=HDMI_FREE; }
   else if (s->integrated) p->resolved++;
   cnd_broadcast(&p->changed); mtx_unlock(&p->lock); hdmi_pipe_wake(p);
   return fd >= 0;
}

static void
hdmi_pipe_fini(struct loader_dri3_drawable *draw)
{
   struct hdmi_pipe *p=draw->hdmi_pipeline;
   if (!p) return;
   p_atomic_set(&p->stop,true); hdmi_pipe_wake(p);
   mtx_lock(&p->lock); cnd_broadcast(&p->changed); mtx_unlock(&p->lock);
   thrd_join(p->worker,NULL); /* Teardown retains images until GPU work ends. */
   mtx_lock(&p->lock);
   for (unsigned g=0;g<HDMI_PIPE_GENERATIONS;g++) for(unsigned i=0;i<HDMI_PIPE_SLOTS;i++) {
      struct hdmi_pipe_slot *s=&p->generations[g].slots[i];
      if (s->source) {
         /* Quarantined work cannot be reused. On final drawable destruction,
          * driver BO retirement still waits its actual GPU dependencies. */
         struct loader_dri3_buffer *source=s->source; s->source=NULL;
         mtx_unlock(&p->lock); hdmi_pipe_buffer_unref(draw,source); mtx_lock(&p->lock);
      }
   }
   mtx_unlock(&p->lock);
   mtx_lock(&p->lock); p->retire_stop=true; cnd_broadcast(&p->changed); mtx_unlock(&p->lock);
   thrd_join(p->retire_worker,NULL);
   for (unsigned g=0;g<HDMI_PIPE_GENERATIONS;g++) hdmi_pipe_destroy_generation(p,&p->generations[g]);
   xcb_unregister_for_special_event(p->conn,p->events); xcb_disconnect(p->conn);
   cnd_destroy(&p->changed); mtx_destroy(&p->lock); close(p->wake_fd);
   mesa_logi("DRI3: HDMI pipeline submitted=%" PRIu64 " completed=%" PRIu64 " worker_copies=%" PRIu64 " integrated_resolves=%" PRIu64,
             p->submitted,p->completed,p->copied,p->resolved);
   mesa_logi("DRI3: HDMI pipeline wait_us producer=%" PRIu64 " mutex=%" PRIu64 " submit_export=%" PRIu64 " completion=%" PRIu64,
             p->producer_us,p->mutex_us,p->submit_us,p->gpu_us);
   free(p); draw->hdmi_pipeline=NULL;
}
