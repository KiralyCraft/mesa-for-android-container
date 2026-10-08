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
#define HDMI_PIPE_CAPACITY_ALIGN 128
/* Private capability advertised only by a fence-gated KGSL Xorg exporter. */
#define HDMI_PRESENT_CAP_FENCE_GATED_EXPORT 0x08000000u

enum hdmi_pipe_phase { HDMI_FREE, HDMI_RESERVED, HDMI_PRODUCER, HDMI_COPY, HDMI_READY, HDMI_PRESENTED, HDMI_QUARANTINED };
struct hdmi_pipe_slot {
   xcb_pixmap_t pixmap;
   xcb_xfixes_region_t valid;
   struct dri_image *image;
   struct loader_dri3_buffer *source;
   enum hdmi_pipe_phase phase;
   int fence_fd, interval;
   uint32_t serial;
   unsigned width, height;
   uint64_t order, sbc;
   bool completed, idle, integrated, low_latency, paced_latency;
   int64_t started_ns;
};
struct hdmi_pipe_generation {
   unsigned width, height, fourcc;
   uint64_t identity, bytes, last_used;
   bool active;
   struct hdmi_pipe_slot slots[HDMI_PIPE_SLOTS];
};
struct hdmi_pipe {
   struct loader_dri3_drawable *draw;
   xcb_connection_t *conn, *allocation_conn;
   xcb_special_event_t *events;
   xcb_present_event_t eid;
   uint32_t stamp, serial;
   mtx_t lock, admission_lock;
   cnd_t changed;
   thrd_t worker, retire_worker;
   int wake_fd, stop;
   int failed;
   bool primed, threads_started, low_latency, resize_capacity, binding_diagnostic;
   bool logged_binding;
   uint64_t binding_checks, binding_mismatches;
   unsigned paced_queue, paced_lead;
   uint64_t msc, target_msc, generation, submitted, copied, resolved, completed, accepted, next_order;
   uint64_t producer_us, submit_us, gpu_us, mutex_us;
   uint64_t cache_clock, cache_hits, cache_evictions, capacity_reuses;
   /* Mutated under lock; log2-microsecond histograms are emitted at teardown. */
   struct { uint64_t count, total_us, max_us, buckets[32]; } timing[8];
   struct hdmi_pipe_generation generations[HDMI_PIPE_GENERATIONS];
   struct hdmi_pipe_generation *retire_generation;
   struct loader_dri3_buffer *retire[HDMI_PIPE_RETIRE];
   unsigned retire_count;
   bool retire_stop;
};

enum hdmi_pipe_timing { HDMI_LOCK, HDMI_GENERATION_WAIT, HDMI_ALLOCATE,
   HDMI_RETIRE, HDMI_SLOT_WAIT, HDMI_RESOLVE_SUBMIT, HDMI_FENCE_EXPORT, HDMI_DRAWABLE_PREPARE };

/* Caller owns p->lock. Buckets give upper bounds, not exact percentiles. */
static void
hdmi_pipe_time(struct hdmi_pipe *p, enum hdmi_pipe_timing stage, int64_t us)
{
   uint64_t duration = MAX2(us, 0);
   unsigned bucket = 0;
   while (bucket < 31 && duration > (UINT64_C(1) << bucket)) bucket++;
   p->timing[stage].count++;
   p->timing[stage].total_us += duration;
   p->timing[stage].max_us = MAX2(p->timing[stage].max_us, duration);
   p->timing[stage].buckets[bucket]++;
}

static void dri3_destroy_render_buffer(struct loader_dri3_drawable *draw,
                                      struct loader_dri3_buffer *buffer);
static void hdmi_pipe_destroy_generation(struct hdmi_pipe *p,
                                        struct hdmi_pipe_generation *g);

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
      while (!p->retire_count && !p->retire_generation && !p->retire_stop)
         cnd_wait(&p->changed, &p->lock);
      if (!p->retire_count && !p->retire_generation && p->retire_stop) {
         mtx_unlock(&p->lock);
         return 0;
      }
      if (p->retire_generation) {
         struct hdmi_pipe_generation *g = p->retire_generation;
         mtx_unlock(&p->lock);
         int64_t start = os_time_get();
         hdmi_pipe_destroy_generation(p, g);
         free(g);
         mtx_lock(&p->lock);
         hdmi_pipe_time(p, HDMI_RETIRE, os_time_get() - start);
         p->retire_generation = NULL;
         cnd_broadcast(&p->changed);
         mtx_unlock(&p->lock);
         continue;
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
         xcb_free_pixmap(p->allocation_conn, s->pixmap);
      if (s->valid)
         xcb_xfixes_destroy_region(p->allocation_conn, s->valid);
      if (s->image)
         dri2_destroy_image(s->image);
   }
   memset(g, 0, sizeof(*g));
   xcb_flush(p->allocation_conn);
}

/* Only admission owns allocation_conn. During retirement it hands ownership
 * to retire_worker and waits without p->lock. Present/event traffic uses conn,
 * which the event worker alone reads after initialization. */
static bool
hdmi_pipe_allocate_generation(struct hdmi_pipe *p, struct hdmi_pipe_generation *g,
                               uint64_t budget, uint64_t *required)
{
   *required = 0;
   for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++) {
      struct hdmi_pipe_slot *s = &g->slots[i];
      int width = 0, height = 0;
      s->fence_fd = -1;
      s->pixmap = xcb_generate_id(p->allocation_conn);
      xcb_generic_error_t *error = xcb_request_check(p->allocation_conn,
         xcb_create_pixmap_checked(p->allocation_conn, p->draw->depth, s->pixmap,
                                  p->draw->drawable, g->width, g->height));
      if (error) { free(error); s->pixmap = 0; return false; }
      if (p->resize_capacity) {
         s->valid = xcb_generate_id(p->allocation_conn);
         error = xcb_request_check(p->allocation_conn,
            xcb_xfixes_create_region_checked(p->allocation_conn, s->valid, 0, NULL));
         if (error) { free(error); s->valid = 0; return false; }
      }
      s->image = loader_dri3_get_pixmap_buffer(p->allocation_conn, s->pixmap,
         p->draw->dri_screen_render_gpu, g->fourcc, p->draw->multiplanes_available,
         &width, &height, NULL);
      if (!s->image || width != g->width || height != g->height) return false;
      /* Diagnostic control only: this readback drains Xorg's initial
       * make-exportable copy before this process writes the shared image.
       * It is never enabled in a normal accelerated runtime. */
      if (debug_get_bool_option("MESA_KGSL_HDMI_ALLOC_READBACK_CONTROL", false)) {
         xcb_get_image_reply_t *reply = xcb_get_image_reply(p->allocation_conn,
            xcb_get_image(p->allocation_conn, XCB_IMAGE_FORMAT_Z_PIXMAP,
                          s->pixmap, 0, 0, 1, 1, UINT32_MAX), NULL);
         if (!reply) return false;
         free(reply);
      }
      int dma_fd = -1, planes = 0, image_fourcc = 0;
      struct stat allocation;
      bool valid = dri2_query_image(s->image, __DRI_IMAGE_ATTRIB_NUM_PLANES, &planes) && planes == 1 &&
         dri2_query_image(s->image, __DRI_IMAGE_ATTRIB_FOURCC, &image_fourcc) && image_fourcc == (int)g->fourcc &&
         dri2_query_image(s->image, __DRI_IMAGE_ATTRIB_FD, &dma_fd) && dma_fd >= 0 &&
         fstat(dma_fd, &allocation) == 0 && allocation.st_size > 0;
      if (dma_fd >= 0) close(dma_fd);
      if (!valid) return false;
      if ((uint64_t)allocation.st_size > budget - g->bytes) {
         /* Ask admission to evict another idle cache entry and retry. Actual
          * allocation bytes, including padding, control the budget. */
         *required = g->bytes + allocation.st_size;
         return false;
      }
      g->bytes += allocation.st_size;
   }
   return true;
}

/* Enter/leave with p->lock held. admission_lock serializes cache publication
 * and pins the chosen generation until its first slot has been reserved.
 * Neither X replies, imports nor resource destruction hold the event lock. */
static struct hdmi_pipe_generation *
hdmi_pipe_generation(struct hdmi_pipe *p, struct loader_dri3_buffer *buffer)
{
   int fourcc;
   if (!dri2_query_image(buffer->image, __DRI_IMAGE_ATTRIB_FOURCC, &fourcc)) return NULL;
   if (buffer->cpp != 4 || !buffer->width || !buffer->height ||
       buffer->width > UINT16_MAX || buffer->height > UINT16_MAX)
      return NULL;
   unsigned width = buffer->width, height = buffer->height;
   /* Capacity belongs to the shared resolve destination, never the application's
    * render target or viewport. Keep an exact screen-sized allocation eligible
    * for fullscreen Present flips instead of rounding its height upward. */
   if (p->resize_capacity &&
       !(p->draw->screen && width == p->draw->screen->width_in_pixels &&
         height == p->draw->screen->height_in_pixels)) {
      width = MIN2(ALIGN_POT(width, HDMI_PIPE_CAPACITY_ALIGN), UINT16_MAX);
      height = MIN2(ALIGN_POT(height, HDMI_PIPE_CAPACITY_ALIGN), UINT16_MAX);
   }
   uint64_t minimum = (uint64_t)width * height * 4 * HDMI_PIPE_SLOTS;
   if (minimum > HDMI_PIPE_BYTES) return NULL;
   for (;;) {
      if (p_atomic_read(&p->stop) || p_atomic_read(&p->failed)) return NULL;
      unsigned vacant = HDMI_PIPE_GENERATIONS, victim = HDMI_PIPE_GENERATIONS;
      uint64_t bytes = 0;
      for (unsigned i = 0; i < HDMI_PIPE_GENERATIONS; i++) {
         struct hdmi_pipe_generation *g = &p->generations[i];
         if (g->identity && g->width == width && g->height == height && g->fourcc == (unsigned)fourcc) {
            for (unsigned j = 0; j < HDMI_PIPE_GENERATIONS; j++) p->generations[j].active = false;
            g->active = true; g->last_used = ++p->cache_clock; p->cache_hits++;
            if (g->width != buffer->width || g->height != buffer->height) p->capacity_reuses++;
            return g;
         }
         bytes += g->bytes;
         if (!g->identity) vacant = i;
         else if (hdmi_pipe_generation_idle(g) &&
                  (victim == HDMI_PIPE_GENERATIONS || g->last_used < p->generations[victim].last_used))
            victim = i;
      }
      if (vacant != HDMI_PIPE_GENERATIONS && minimum <= HDMI_PIPE_BYTES - bytes) {
         struct hdmi_pipe_generation created = {
            .width = width, .height = height, .fourcc = fourcc,
         };
         uint64_t required;
         mtx_unlock(&p->lock);
         int64_t start = os_time_get();
         bool ok = hdmi_pipe_allocate_generation(p, &created, HDMI_PIPE_BYTES - bytes, &required);
         if (!ok) hdmi_pipe_destroy_generation(p, &created);
         mtx_lock(&p->lock);
         hdmi_pipe_time(p, HDMI_ALLOCATE, os_time_get() - start);
         if (!ok) {
            if (!required || required > HDMI_PIPE_BYTES) return NULL;
            minimum = MAX2(minimum, required);
            continue;
         }
         struct hdmi_pipe_generation *g = &p->generations[vacant];
         *g = created; g->identity = ++p->generation;
         g->last_used = ++p->cache_clock;
         for (unsigned i = 0; i < HDMI_PIPE_GENERATIONS; i++) p->generations[i].active = false;
         g->active = true;
         mesa_logi("DRI3: HDMI pipeline generation=%" PRIu64 " size=%ux%u Xorg-owned GPU buffers",
                   g->identity, g->width, g->height);
         return g;
      }
      if (victim != HDMI_PIPE_GENERATIONS) {
         struct hdmi_pipe_generation *retired = malloc(sizeof(*retired));
         if (!retired) return NULL;
         assert(!p->retire_generation);
         *retired = p->generations[victim];
         memset(&p->generations[victim], 0, sizeof(p->generations[victim]));
         p->retire_generation = retired; p->cache_evictions++;
         cnd_broadcast(&p->changed);
         int64_t start = os_time_get();
         /* Keep retired storage inside the three-generation/byte budget until
          * destruction completes. The event worker runs throughout this wait. */
         while (p->retire_generation) cnd_wait(&p->changed, &p->lock);
         hdmi_pipe_time(p, HDMI_GENERATION_WAIT, os_time_get() - start);
         continue;
      }
      int64_t start = os_time_get();
      cnd_wait(&p->changed, &p->lock);
      hdmi_pipe_time(p, HDMI_GENERATION_WAIT, os_time_get() - start);
   }
}

static void
hdmi_pipe_complete_msc(struct hdmi_pipe *p, const struct hdmi_pipe_slot *s, uint64_t msc)
{
   if (!p->primed || p->target_msc <= msc)
      p->target_msc = msc + (s->paced_latency ? p->paced_lead : 2);
   p->primed = true;
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
                  hdmi_pipe_complete_msc(p, s, ce->msc);
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
   /* The special-event poll above is the only socket reader. A later general
    * poll_for_event can read a newly arrived Present notification into XCB's
    * special queue, leaving the socket empty just before the worker sleeps.
    * Drain queued general errors without consuming new socket data here. */
   while ((event = xcb_poll_for_queued_event(p->conn))) {
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

/* Admission latency is independent of the resize cache's storage budget.
 * Count even completed-but-not-idle slots: the consumer still owns them. */
static unsigned
hdmi_pipe_outstanding(const struct hdmi_pipe *p)
{
   unsigned count = 0;
   for (unsigned g = 0; g < HDMI_PIPE_GENERATIONS; g++)
      for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++)
         count += p->generations[g].slots[i].phase != HDMI_FREE;
   return count;
}

/* Presentation latency and storage retirement have different lifetimes. A
 * completed fullscreen flip keeps its buffer until a replacement flips in.
 * It must not prevent producing that replacement; the nonfree slot still pins
 * storage and generation eviction until the real IDLE notification arrives.
 */
static unsigned
hdmi_pipe_pending(const struct hdmi_pipe *p)
{
   unsigned count = 0;
   for (unsigned g = 0; g < HDMI_PIPE_GENERATIONS; g++)
      for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++) {
         const struct hdmi_pipe_slot *s = &p->generations[g].slots[i];
         count += s->phase != HDMI_FREE &&
                  !(s->phase == HDMI_PRESENTED && s->completed);
      }
   return count;
}

static bool
hdmi_pipe_can_submit(const struct hdmi_pipe *p, const struct hdmi_pipe_slot *next)
{
   if (!next->low_latency)
      return !(p->submitted && !p->primed && next->interval > 0);
   /* A target-zero request must not overtake a previously scheduled request
    * when an application changes interval or mixes swap APIs. */
   for (unsigned g = 0; g < HDMI_PIPE_GENERATIONS; g++)
      for (unsigned i = 0; i < HDMI_PIPE_SLOTS; i++) {
         const struct hdmi_pipe_slot *s = &p->generations[g].slots[i];
         if (s->phase == HDMI_PRESENTED && !s->low_latency && !s->completed)
            return false;
      }
   return true;
}

/* Count consumer-owned storage across every resize generation. Scene rendering
 * can overlap the previous presentation; only its resolve/admission is bounded.
 * Keep nonordinary OML calls and interval values other than zero/one unchanged.
 */
static unsigned
hdmi_pipe_admission_budget(const struct hdmi_pipe *p, bool ordinary, int interval)
{
   if (!p->low_latency || !ordinary) return 0;
   if (interval == 0) return 2;
   return interval == 1 ? p->paced_queue : 0;
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
                  dri2_blit_image(ctx,destination,source->image,0,0,s->width,s->height,0,0,s->width,s->height,__BLIT_FLAG_FLUSH);
                  fd = hdmi_pipe_native_fence(ctx);
                  if (fd < 0) {
                     /* Error recovery only: retain ordering and drain the
                      * submitted operation before releasing source ownership. */
                     dri2_blit_image(ctx,destination,source->image,0,0,s->width,s->height,0,0,s->width,s->height,__BLIT_FLAG_FINISH);
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
         if (!hdmi_pipe_can_submit(p, next)) break;
         uint64_t target=next->low_latency ? 0 : (p->primed ? p->target_msc : 0);
         if (!next->low_latency && p->primed) p->target_msc += MAX2(abs(next->interval),1);
         next->serial=++p->serial; next->completed=next->idle=false;
         next->phase=HDMI_PRESENTED; p->submitted++; p->next_order++;
         uint32_t options=next->interval <= 0 ? XCB_PRESENT_OPTION_ASYNC : XCB_PRESENT_OPTION_NONE;
         if (next->valid) {
            /* Regions are snapshotted by the server when Present is processed.
             * This slot is reused only after COMPLETE plus IDLE; padding is
             * never declared valid or copied into a subsequently larger window. */
            xcb_rectangle_t rect = {0, 0, next->width, next->height};
            xcb_xfixes_set_region(p->conn, next->valid, 1, &rect);
         }
         xcb_present_pixmap(p->conn,p->draw->drawable,next->pixmap,next->serial,next->valid,next->valid,0,0,XCB_NONE,XCB_NONE,XCB_NONE,options,target,0,0,0,NULL);
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
   const char *queue = getenv("MESA_KGSL_HDMI_QUEUE");
   if (queue && strcmp(queue, "fifo") && strcmp(queue, "low-latency")) {
      mesa_loge("DRI3: invalid MESA_KGSL_HDMI_QUEUE"); free(p); return false;
   }
   p->low_latency = queue && !strcmp(queue, "low-latency");
   int paced_queue = debug_get_num_option("MESA_KGSL_HDMI_PACED_QUEUE", p->low_latency ? 1 : 0);
   int paced_lead = debug_get_num_option("MESA_KGSL_HDMI_PACED_LEAD", p->low_latency ? 1 : 2);
   if (paced_queue < 0 || paced_queue > HDMI_PIPE_SLOTS || paced_lead < 1 || paced_lead > 2) {
      mesa_loge("DRI3: invalid HDMI paced queue/lead"); free(p); return false;
   }
   p->paced_queue = paced_queue; p->paced_lead = paced_lead;
   p->resize_capacity = debug_get_bool_option("MESA_KGSL_HDMI_RESIZE_CAPACITY", true);
   p->binding_diagnostic = debug_get_bool_option("MESA_KGSL_HDMI_BINDING_DIAGNOSTIC", false);
   p->draw = draw; p->next_order=1;
   p->wake_fd = eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);
   if (p->wake_fd < 0) { free(p); return false; }
   if (mtx_init(&p->lock,mtx_plain) != thrd_success) { close(p->wake_fd); free(p); return false; }
   if (mtx_init(&p->admission_lock,mtx_plain) != thrd_success) { mtx_destroy(&p->lock); close(p->wake_fd); free(p); return false; }
   if (cnd_init(&p->changed) != thrd_success) { mtx_destroy(&p->admission_lock); mtx_destroy(&p->lock); close(p->wake_fd); free(p); return false; }
   p->conn = xcb_connect(NULL,NULL);
   if (!p->conn || xcb_connection_has_error(p->conn)) goto fail;
   p->allocation_conn = xcb_connect(NULL,NULL);
   if (!p->allocation_conn || xcb_connection_has_error(p->allocation_conn)) goto fail;
   xcb_present_query_capabilities_reply_t *capabilities = xcb_present_query_capabilities_reply(
      p->conn, xcb_present_query_capabilities(p->conn, draw->drawable), NULL);
   bool export_ready = capabilities &&
      (capabilities->capabilities & HDMI_PRESENT_CAP_FENCE_GATED_EXPORT);
   free(capabilities);
   if (!export_ready &&
       !debug_get_bool_option("MESA_KGSL_HDMI_ALLOC_READBACK_CONTROL", false)) {
      mesa_loge("DRI3: HDMI pipeline requires fence-gated Xorg pixmap exports; refusing an unsafe buffer handoff");
      goto fail;
   }
   if (p->resize_capacity) {
      xcb_connection_t *connections[] = {p->conn, p->allocation_conn};
      for (unsigned i = 0; i < ARRAY_SIZE(connections); i++) {
         xcb_xfixes_query_version_reply_t *version = xcb_xfixes_query_version_reply(
            connections[i], xcb_xfixes_query_version(connections[i], 5, 0), NULL);
         bool supported = version && version->major_version >= 2;
         free(version);
         if (!supported) {
            mesa_loge("DRI3: resize capacity requires XFixes regions on both private connections");
            goto fail;
         }
      }
   }
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
   mesa_logi("DRI3: HDMI_LOS_MESA_RESIZE_ABI=1 bounded size cache; separate allocation connection");
   mesa_logi("DRI3: HDMI_LOS_MESA_CAPACITY_ABI=1 shared resize capacity=%u; logical valid/update regions",
             p->resize_capacity ? HDMI_PIPE_CAPACITY_ALIGN : 0);
   mesa_logi("DRI3: HDMI_LOS_MESA_QUEUE_ABI=1 queue=%s", p->low_latency ? "low-latency" : "fifo");
   mesa_logi("DRI3: HDMI_LOS_MESA_PACED_QUEUE_ABI=1 max_pending=%u re_prime_lead=%u",
             p->paced_queue, p->paced_lead);
   return true;
fail:
   draw->hdmi_pipeline = NULL;
   if (p->events) xcb_unregister_for_special_event(p->conn,p->events);
   if (p->conn) xcb_disconnect(p->conn);
   if (p->allocation_conn) xcb_disconnect(p->allocation_conn);
   cnd_destroy(&p->changed); mtx_destroy(&p->admission_lock); mtx_destroy(&p->lock); close(p->wake_fd); free(p);
   return false;
}

struct hdmi_pipe_resolve_args {
   struct dri_context *ctx;
   struct dri_image *dst, *src;
   struct dri_drawable *drawable;
   bool diagnostic, source_matches, render_matches;
   unsigned actual_width, actual_height;
   unsigned width, height;
   int64_t started_us, finished_us;
};

static void
hdmi_pipe_resolve_before_flush(void *data)
{
   struct hdmi_pipe_resolve_args *args = data;
   args->started_us = os_time_get();
   if (args->diagnostic) {
      struct pipe_resource *actual = args->drawable->textures[ST_ATTACHMENT_BACK_LEFT];
      args->source_matches = actual == args->src->texture;
      struct gl_framebuffer *fb = args->ctx->st->ctx->DrawBuffer;
      struct pipe_resource *render = fb && fb->_ColorDrawBuffers[0] ?
         fb->_ColorDrawBuffers[0]->texture : NULL;
      args->render_matches = render == args->src->texture ||
         render == args->drawable->msaa_textures[ST_ATTACHMENT_BACK_LEFT];
      args->actual_width = actual ? actual->width0 : 0;
      args->actual_height = actual ? actual->height0 : 0;
   }
   /* dri_flush's callback runs after vertices/bitmap/MSAA/HUD work. Do not
    * recursively flush here: the enclosing end-of-frame flush exports one
    * fence covering rendering and the final resolve into shared storage. */
   dri2_blit_image(args->ctx, args->dst, args->src,
                   0, 0, args->width, args->height,
                   0, 0, args->width, args->height, 0);
   args->ctx->st->pipe->flush_resource(args->ctx->st->pipe, args->dst->texture);
   args->finished_us = os_time_get();
}

static bool
hdmi_pipe_present(struct loader_dri3_drawable *draw, struct loader_dri3_buffer *buffer,
                  int producer_fd, unsigned flush_flags, bool ordinary_swap)
{
   if (!hdmi_pipe_init(draw)) { if (producer_fd >= 0) close(producer_fd); return false; }
   struct hdmi_pipe *p = draw->hdmi_pipeline;
   int64_t lock_started = os_time_get();
   mtx_lock(&p->admission_lock);
   mtx_lock(&p->lock);
   hdmi_pipe_time(p, HDMI_LOCK, os_time_get() - lock_started);
   bool low_latency = p->low_latency && ordinary_swap && draw->swap_interval == 0;
   unsigned admission_budget = hdmi_pipe_admission_budget(p, ordinary_swap, draw->swap_interval);
   while (admission_budget &&
          (low_latency ? hdmi_pipe_outstanding(p) : hdmi_pipe_pending(p)) >= admission_budget &&
          !p_atomic_read(&p->failed) && !p_atomic_read(&p->stop)) {
      int64_t waited = os_time_get();
      cnd_wait(&p->changed, &p->lock);
      hdmi_pipe_time(p, HDMI_SLOT_WAIT, os_time_get() - waited);
   }
   struct hdmi_pipe_generation *g = hdmi_pipe_generation(p,buffer);
   struct hdmi_pipe_slot *s = NULL;
   while (g && !p_atomic_read(&p->failed) && !p_atomic_read(&p->stop)) {
      for (unsigned i=0;i<HDMI_PIPE_SLOTS;i++) if (g->slots[i].phase == HDMI_FREE) { s=&g->slots[i]; break; }
      if (s) break;
      int64_t waited = os_time_get();
      cnd_wait(&p->changed,&p->lock);
      hdmi_pipe_time(p, HDMI_SLOT_WAIT, os_time_get() - waited);
   }
   if (!s) { mtx_unlock(&p->lock); mtx_unlock(&p->admission_lock); if (producer_fd >= 0) close(producer_fd); return false; }
   s->source=buffer; s->interval=draw->swap_interval; s->integrated=draw->hdmi_pipeline_resolve;
   s->width=buffer->width; s->height=buffer->height;
   s->low_latency=low_latency;
   s->paced_latency=p->low_latency && ordinary_swap && draw->swap_interval == 1;
   p_atomic_inc(&buffer->pipeline_refs);
   /* Reserve before releasing the generation lock for application submission. */
   s->phase=HDMI_RESERVED; s->fence_fd=-1; s->order=++p->accepted; s->started_ns=os_time_get();
   mtx_unlock(&p->lock);
   mtx_lock(&draw->mtx); buffer->busy=true; mtx_unlock(&draw->mtx);
   int fd=producer_fd;
   int64_t submit_started=os_time_get(), integrated_submit_us=0, resolve_us=0, export_us=0, prepare_us=0;
   if (s->integrated) {
      struct dri_context *ctx=draw->vtable->get_dri_context(draw);
      if (producer_fd >= 0) close(producer_fd);
      fd=-1;
      if (ctx && draw->vtable->in_current_context(draw) && draw->vtable->flush_drawable_with_fence_fd) {
         struct hdmi_pipe_resolve_args args = {
            .ctx=ctx, .dst=s->image, .src=buffer->image,
            .drawable=draw->dri_drawable, .diagnostic=p->binding_diagnostic,
            .width=s->width, .height=s->height,
         };
         fd=dri_flush_with_fence_fd_and_callback(ctx,draw->dri_drawable,
               flush_flags,__DRI2_THROTTLE_SWAPBUFFER,
               hdmi_pipe_resolve_before_flush,&args);
         if (args.finished_us && args.diagnostic) {
            p->binding_checks++;
            p->binding_mismatches += !args.source_matches || !args.render_matches;
            if (!p->logged_binding || ((!args.source_matches || !args.render_matches) && p->binding_mismatches <= 4)) {
               mesa_logi("DRI3: HDMI resolve binding frame=%" PRIu64 " matches=%u render_matches=%u source=%ux%u actual=%ux%u",
                         s->order, args.source_matches, args.render_matches, s->width, s->height,
                         args.actual_width, args.actual_height);
               p->logged_binding = true;
            }
         }
         if (args.finished_us) {
            prepare_us = args.started_us - submit_started;
            resolve_us = args.finished_us - args.started_us;
            export_us = os_time_get() - args.finished_us;
         } else if (fd >= 0) {
            close(fd); fd=-1;
         }
      }
   }
   if (s->integrated) integrated_submit_us=os_time_get()-submit_started;
   if (fd >= 0) {
      mtx_lock(&draw->mtx);
      s->sbc=++draw->send_sbc; buffer->last_swap=s->sbc;
      if (draw->stamp) ++(*draw->stamp);
      draw->cur_back=(draw->cur_back+1)%draw->max_num_back;
      mtx_unlock(&draw->mtx);
   }
   mtx_lock(&p->lock);
   p->submit_us+=integrated_submit_us;
   if (s->integrated) {
      hdmi_pipe_time(p, HDMI_RESOLVE_SUBMIT, resolve_us);
      hdmi_pipe_time(p, HDMI_FENCE_EXPORT, export_us);
      hdmi_pipe_time(p, HDMI_DRAWABLE_PREPARE, prepare_us);
   }
   s->fence_fd=fd; s->phase=HDMI_PRODUCER; s->started_ns=os_time_get();
   if (fd < 0) { p_atomic_set(&p->failed,true); hdmi_pipe_release_source(p,s); s->phase=HDMI_FREE; }
   else if (s->integrated) p->resolved++;
   cnd_broadcast(&p->changed); mtx_unlock(&p->lock); hdmi_pipe_wake(p);
   mtx_unlock(&p->admission_lock);
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
   xcb_disconnect(p->allocation_conn);
   cnd_destroy(&p->changed); mtx_destroy(&p->admission_lock); mtx_destroy(&p->lock); close(p->wake_fd);
   mesa_logi("DRI3: HDMI pipeline submitted=%" PRIu64 " completed=%" PRIu64 " worker_copies=%" PRIu64 " integrated_resolves=%" PRIu64,
             p->submitted,p->completed,p->copied,p->resolved);
   mesa_logi("DRI3: HDMI pipeline wait_us producer=%" PRIu64 " mutex=%" PRIu64 " submit_export=%" PRIu64 " completion=%" PRIu64,
             p->producer_us,p->mutex_us,p->submit_us,p->gpu_us);
   mesa_logi("DRI3: HDMI resize generations=%" PRIu64 " cache_hits=%" PRIu64 " evictions=%" PRIu64,
             p->generation, p->cache_hits, p->cache_evictions);
   mesa_logi("DRI3: HDMI capacity reuses=%" PRIu64, p->capacity_reuses);
   static const char *names[] = {"admission_lock", "generation_wait", "allocate_import",
      "generation_retire", "slot_wait", "resolve_submit", "fence_export", "drawable_prepare"};
   for (unsigned i=0; i<ARRAY_SIZE(names); i++) {
      uint64_t cumulative=0, percentile=0, threshold=(p->timing[i].count*95+99)/100;
      for (unsigned b=0; b<32 && threshold; b++) {
         cumulative += p->timing[i].buckets[b];
         if (cumulative >= threshold) { percentile=UINT64_C(1)<<b; break; }
      }
      mesa_logi("DRI3: HDMI stage=%s count=%" PRIu64 " total_us=%" PRIu64
                " max_us=%" PRIu64 " p95_bucket_us=%" PRIu64,
                names[i], p->timing[i].count, p->timing[i].total_us,
                p->timing[i].max_us, percentile);
   }
   free(p); draw->hdmi_pipeline=NULL;
}
