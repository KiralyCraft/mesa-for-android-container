/*
 * Copyright © 2021 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include "freedreno_autotune.h"
#include "freedreno_batch.h"
#include "freedreno_context.h"
#include "freedreno_resource.h"
#include "freedreno_util.h"

#include "util/u_math.h"

DEBUG_GET_ONCE_BOOL_OPTION(autotune_log, "FD_AUTOTUNE_LOG", false)

/**
 * Tracks, for a given batch key (which maps to a FBO/framebuffer state),
 *
 * ralloc parent is fd_autotune::ht
 */
struct fd_batch_history {
   struct fd_batch_key *key;

   /* Entry in fd_autotune::lru: */
   struct list_head node;

   unsigned num_results;

   /**
    * List of recent fd_batch_result's
    */
   struct list_head results;
#define MAX_RESULTS 5
};

/*
 * An address-independent description of work whose GMEM and SYSMEM timings
 * may be compared.  This is observation-only for now: the existing batch-key
 * history and samples-passed policy remain authoritative.
 *
 * Keep this as an explicitly versioned, zero-initialized fixed layout so that
 * compiler padding cannot make otherwise identical passes hash differently.
 */
#define FD_AUTOTUNE_SIGNATURE_VERSION 2

#define MAX_TIMING_HISTORIES       64
#define MAX_TIMING_RESULTS         16
#define MIN_COMPARE_RESULTS        8
#define TIMING_IMPROVEMENT_PERCENT 5

enum fd_autotune_timing_mode {
   FD_AUTOTUNE_TIMING_GMEM,
   FD_AUTOTUNE_TIMING_SYSMEM,
   FD_AUTOTUNE_TIMING_MODE_COUNT,
};

struct fd_autotune_mode_timings {
   uint64_t duration_ns[MAX_TIMING_RESULTS];
   uint64_t total_seen;
   uint8_t count;
   uint8_t next;
};

/* DEBUG: observation-only measured-history validation.
 *
 * A bounded, in-memory observation history for structurally comparable
 * render passes.  It is intentionally independent of fd_batch_history:
 * rotating presentation buffers may have different address-based batch keys
 * while still representing the same work.
 *
 * This history reports a candidate only for validation.  The legacy
 * samples-passed heuristic remains the sole selection policy.
 */
struct fd_autotune_timing_history {
   uint64_t structural_signature;
   struct list_head node;
   struct fd_autotune_mode_timings mode[FD_AUTOTUNE_TIMING_MODE_COUNT];
};

static void
log_timing_history(const struct fd_autotune_timing_history *history);

struct fd_autotune_attachment_signature {
   uint32_t pitch;
   uint32_t layer_stride;
   uint16_t view_format;
   uint16_t resource_format;
   uint16_t internal_format;
   uint16_t first_layer;
   uint16_t last_layer;
   uint16_t level;
   uint8_t target;
   uint8_t samples;
   uint8_t tile_mode;
   uint8_t ubwc;
   uint8_t cpp;
   uint8_t pitchalign;
   uint8_t plane;
   uint8_t present;
};

struct fd_autotune_structural_signature {
   uint32_t version;
   uint32_t width;
   uint32_t height;
   uint32_t invalidated;
   uint32_t cleared;
   uint32_t restore;
   uint32_t resolve;
   uint32_t gmem_reason;
   uint64_t shader_signature;
   uint16_t layers;
   uint8_t samples;
   uint8_t nr_cbufs;
   uint8_t draw_bucket;
   uint8_t cost_bucket;
   uint8_t subpass_bucket;
   uint8_t shader_program_bucket;
   uint8_t flags;
   uint8_t scissor_minx;
   uint8_t scissor_miny;
   uint8_t scissor_maxx;
   uint8_t scissor_maxy;
   uint8_t reserved[4];
   struct fd_autotune_attachment_signature cbuf[PIPE_MAX_COLOR_BUFS];
   struct fd_autotune_attachment_signature zsbuf;
   struct fd_autotune_attachment_signature resolve_resource;
};

static uint8_t
bucket_u32(uint32_t value)
{
   return util_logbase2_ceil(value);
}

static uint8_t
quantize_coordinate(uint32_t coordinate, uint32_t extent)
{
   if (!extent)
      return 0;

   return MIN2((uint64_t)16, DIV_ROUND_UP((uint64_t)coordinate * 16, extent));
}

static void
attachment_signature(struct fd_autotune_attachment_signature *signature,
                     const struct pipe_surface *surface)
{
   if (!surface->texture)
      return;

   struct fd_resource *rsc = fd_resource(surface->texture);
   const struct fdl_layout *layout = &rsc->layout;

   signature->pitch = fdl_pitch(layout, surface->level);
   signature->layer_stride = fdl_layer_stride(layout, surface->level);
   signature->view_format = surface->format;
   signature->resource_format = layout->format;
   signature->internal_format = rsc->internal_format;
   signature->first_layer = surface->first_layer;
   signature->last_layer = surface->last_layer;
   signature->level = surface->level;
   signature->target = surface->texture->target;
   signature->samples = fd_resource_nr_samples(surface->texture);
   signature->tile_mode = fdl_tile_mode(layout, surface->level);
   signature->ubwc = fdl_ubwc_enabled(layout, surface->level);
   signature->cpp = layout->cpp;
   signature->pitchalign = layout->pitchalign;
   signature->plane = layout->plane;
   signature->present = true;
}

static void
resource_signature(struct fd_autotune_attachment_signature *signature,
                   struct pipe_resource *resource)
{
   if (!resource)
      return;

   struct pipe_surface surface = {
      .format = resource->format,
      .first_layer = 0,
      .last_layer = resource->array_size ? resource->array_size - 1 : 0,
      .level = 0,
      .texture = resource,
   };

   attachment_signature(signature, &surface);
}

static uint64_t
structural_signature(struct fd_batch *batch)
{
   const struct pipe_framebuffer_state *pfb = &batch->framebuffer;
   const struct pipe_scissor_state *scissor = &batch->max_scissor;
   struct fd_autotune_structural_signature signature;

   memset(&signature, 0, sizeof(signature));
   signature.version = FD_AUTOTUNE_SIGNATURE_VERSION;
   signature.width = pfb->width;
   signature.height = pfb->height;
   signature.invalidated = batch->invalidated;
   signature.cleared = batch->cleared;
   signature.restore = batch->restore;
   signature.resolve = batch->resolve;
   signature.gmem_reason = batch->gmem_reason;
   signature.shader_signature = batch->shader_signature;
   signature.layers = pfb->layers;
   signature.samples = pfb->samples;
   signature.nr_cbufs = pfb->nr_cbufs;
   signature.draw_bucket = bucket_u32(batch->num_draws);
   signature.cost_bucket = bucket_u32(batch->cost);
   signature.shader_program_bucket = bucket_u32(batch->num_shader_programs);
   signature.flags = (pfb->pls_enabled ? BITFIELD_BIT(0) : 0) |
                     (batch->tessellation ? BITFIELD_BIT(1) : 0);
   signature.scissor_minx = quantize_coordinate(scissor->minx, pfb->width);
   signature.scissor_miny = quantize_coordinate(scissor->miny, pfb->height);
   signature.scissor_maxx = quantize_coordinate(scissor->maxx + 1, pfb->width);
   signature.scissor_maxy = quantize_coordinate(scissor->maxy + 1, pfb->height);

   unsigned subpasses = 0;
   foreach_subpass(subpass, batch) subpasses++;
   signature.subpass_bucket = bucket_u32(subpasses);

   for (unsigned i = 0; i < pfb->nr_cbufs; i++)
      attachment_signature(&signature.cbuf[i], &pfb->cbufs[i]);
   attachment_signature(&signature.zsbuf, &pfb->zsbuf);
   resource_signature(&signature.resolve_resource, pfb->resolve);

   uint32_t low = _mesa_hash_data(&signature, sizeof(signature));
   uint32_t high =
      _mesa_hash_data_with_seed(&signature, sizeof(signature), 0x9e3779b9);

   return ((uint64_t)high << 32) | low;
}

static struct fd_autotune_timing_history *
get_timing_history(struct fd_autotune *at, uint64_t signature)
{
   if (!at->timing_ht)
      return NULL;

   struct fd_autotune_timing_history *history =
      _mesa_hash_table_u64_search(at->timing_ht, signature);

   if (history)
      goto found;

   history = rzalloc(at->timing_ht, struct fd_autotune_timing_history);
   if (!history)
      return NULL;

   history->structural_signature = signature;
   list_inithead(&history->node);

   if (_mesa_hash_table_u64_num_entries(at->timing_ht) >=
       MAX_TIMING_HISTORIES) {
      struct fd_autotune_timing_history *last = list_last_entry(
         &at->timing_lru, struct fd_autotune_timing_history, node);

      if (unlikely(at->log))
         log_timing_history(last);

      _mesa_hash_table_u64_remove(at->timing_ht, last->structural_signature);
      list_del(&last->node);
      ralloc_free(last);
   }

   _mesa_hash_table_u64_insert(at->timing_ht, signature, history);

found:
   list_delinit(&history->node);
   list_add(&history->node, &at->timing_lru);

   return history;
}

static void
record_timing_result(struct fd_autotune *at,
                     const struct fd_batch_result *result)
{
   if (!result->timestamped || !result->duration_ns)
      return;

   struct fd_autotune_timing_history *history =
      get_timing_history(at, result->structural_signature);
   if (!history)
      return;

   enum fd_autotune_timing_mode mode =
      result->use_bypass ? FD_AUTOTUNE_TIMING_SYSMEM : FD_AUTOTUNE_TIMING_GMEM;
   struct fd_autotune_mode_timings *timings = &history->mode[mode];

   timings->duration_ns[timings->next] = result->duration_ns;
   timings->next = (timings->next + 1) % MAX_TIMING_RESULTS;
   timings->count = MIN2(timings->count + 1, MAX_TIMING_RESULTS);
   timings->total_seen++;
}

struct fd_autotune_timing_summary {
   uint64_t mean_ns;
   uint64_t p95_ns;
};

static struct fd_autotune_timing_summary
summarize_timings(const struct fd_autotune_mode_timings *timings)
{
   uint64_t sorted[MAX_TIMING_RESULTS];
   uint64_t sum = 0;

   for (unsigned i = 0; i < timings->count; i++) {
      sorted[i] = timings->duration_ns[i];
      sum += sorted[i];
   }

   for (unsigned i = 1; i < timings->count; i++) {
      uint64_t value = sorted[i];
      unsigned j = i;

      while (j && sorted[j - 1] > value) {
         sorted[j] = sorted[j - 1];
         j--;
      }
      sorted[j] = value;
   }

   if (!timings->count)
      return (struct fd_autotune_timing_summary){0};

   unsigned p95 = DIV_ROUND_UP(95 * timings->count, 100) - 1;

   return (struct fd_autotune_timing_summary){
      .mean_ns = sum / timings->count,
      .p95_ns = sorted[p95],
   };
}

static const char *
timing_candidate(const struct fd_autotune_timing_history *history,
                 const struct fd_autotune_timing_summary *gmem,
                 const struct fd_autotune_timing_summary *sysmem)
{
   if (history->mode[FD_AUTOTUNE_TIMING_GMEM].count < MIN_COMPARE_RESULTS ||
       history->mode[FD_AUTOTUNE_TIMING_SYSMEM].count < MIN_COMPARE_RESULTS)
      return "insufficient";

   const double threshold = (100.0 - TIMING_IMPROVEMENT_PERCENT) / 100.0;

   if (gmem->mean_ns <= sysmem->mean_ns * threshold &&
       gmem->p95_ns <= sysmem->p95_ns * threshold)
      return "gmem";

   if (sysmem->mean_ns <= gmem->mean_ns * threshold &&
       sysmem->p95_ns <= gmem->p95_ns * threshold)
      return "sysmem";

   return "inconclusive";
}

static void
log_timing_history(const struct fd_autotune_timing_history *history)
{
   const struct fd_autotune_mode_timings *gmem_timings =
      &history->mode[FD_AUTOTUNE_TIMING_GMEM];
   const struct fd_autotune_mode_timings *sysmem_timings =
      &history->mode[FD_AUTOTUNE_TIMING_SYSMEM];
   struct fd_autotune_timing_summary gmem = summarize_timings(gmem_timings);
   struct fd_autotune_timing_summary sysmem = summarize_timings(sysmem_timings);

   mesa_logi("freedreno autotune observation: signature=%016" PRIx64
             " gmem=%u/%" PRIu64 " mean_ns=%" PRIu64 " p95_ns=%" PRIu64
             " sysmem=%u/%" PRIu64 " mean_ns=%" PRIu64 " p95_ns=%" PRIu64
             " candidate=%s policy=unchanged",
             history->structural_signature, gmem_timings->count,
             gmem_timings->total_seen, gmem.mean_ns, gmem.p95_ns,
             sysmem_timings->count, sysmem_timings->total_seen, sysmem.mean_ns,
             sysmem.p95_ns, timing_candidate(history, &gmem, &sysmem));
}

static struct fd_batch_history *
get_history(struct fd_autotune *at, struct fd_batch *batch)
{
   struct fd_batch_history *history;

   /* draw batches should still have their key at this point. */
   assert(batch->key || batch->nondraw);
   if (!batch->key)
      return NULL;

   struct hash_entry *entry =
      _mesa_hash_table_search_pre_hashed(at->ht, batch->hash, batch->key);

   if (entry) {
      history = entry->data;
      goto found;
   }

   history = rzalloc_size(at->ht, sizeof(*history));

   history->key = fd_batch_key_clone(history, batch->key);
   list_inithead(&history->node);
   list_inithead(&history->results);

   /* Note: We cap # of cached GMEM states at 20.. so assuming double-
    * buffering, 40 should be a good place to cap cached autotune state
    */
   if (at->ht->entries >= 40) {
      struct fd_batch_history *last =
         list_last_entry(&at->lru, struct fd_batch_history, node);
      _mesa_hash_table_remove_key(at->ht, last->key);
      list_del(&last->node);
      ralloc_free(last);
   }

   _mesa_hash_table_insert_pre_hashed(at->ht, batch->hash, history->key,
                                      history);

found:
   /* Move to the head of the LRU: */
   list_delinit(&history->node);
   list_add(&history->node, &at->lru);

   return history;
}

static void
result_destructor(void *r)
{
   struct fd_batch_result *result = r;

   if (result->pending)
      result->at->num_pending--;

   /* Just in case we manage to somehow still be on the pending_results list: */
   list_del(&result->node);
}

static struct fd_batch_result *
get_result(struct fd_autotune *at, struct fd_batch_history *history)
{
   if (at->num_pending >= ARRAY_SIZE(at->results->result))
      return NULL;

   struct fd_batch_result *result = rzalloc_size(history, sizeof(*result));
   if (!result)
      return NULL;

   result->fence =
      ++at->fence_counter; /* pre-increment so zero isn't valid fence */
   result->idx = at->idx_counter++;
   result->at = at;
   result->pending = true;
   at->num_pending++;

   if (at->idx_counter >= ARRAY_SIZE(at->results->result))
      at->idx_counter = 0;

   /* This field is also the KGSL completion token.  Reset it before the
    * slot is submitted so stale data from an earlier use cannot make a new
    * result appear ready.
    */
   p_atomic_set(&at->results->result[result->idx].timestamp_end, UINT64_MAX);

   result->history = history;
   list_addtail(&result->node, &at->pending_results);

   ralloc_set_destructor(result, result_destructor);

   return result;
}

static void
process_results(struct fd_autotune *at)
{
   uint32_t current_fence = p_atomic_read(&at->results->fence);

   list_for_each_entry_safe (struct fd_batch_result, result,
                             &at->pending_results, node) {
      uint64_t timestamp_end = UINT64_MAX;

      /* The CACHE_CLEAN user fence used by the DRM path does not update the
       * mapped results BO on KGSL.  The preceding RB_DONE timestamp does, so
       * use that write as the asynchronous completion token there.  It also
       * carries the whole-pass end time we want to measure.
       */
      if (at->use_timestamp_completion && result->timestamped) {
         timestamp_end =
            p_atomic_read(&at->results->result[result->idx].timestamp_end);
         if (timestamp_end == UINT64_MAX)
            break;
      } else if (result->fence > current_fence) {
         break;
      }

      struct fd_batch_history *history = result->history;

      result->samples_passed = at->results->result[result->idx].samples_end -
                               at->results->result[result->idx].samples_start;

      if (result->timestamped) {
         uint64_t start =
            p_atomic_read(&at->results->result[result->idx].timestamp_start);
         uint64_t end = timestamp_end;

         if (!at->use_timestamp_completion) {
            end =
               p_atomic_read(&at->results->result[result->idx].timestamp_end);
         }

         result->duration_ns = at->ts_to_ns(end - start);
      }

      if (unlikely(at->log)) {
         record_timing_result(at, result);

         mesa_logi("freedreno autotune: key=%08x signature=%016" PRIx64
                   " draws=%u mode=%s "
                   "samples=%" PRIu64 " duration_ns=%" PRIu64,
                   result->batch_hash, result->structural_signature,
                   result->num_draws, result->use_bypass ? "sysmem" : "gmem",
                   result->samples_passed, result->duration_ns);
      }

      result->pending = false;
      at->num_pending--;
      list_delinit(&result->node);
      list_add(&result->node, &history->results);

      if (history->num_results < MAX_RESULTS) {
         history->num_results++;
      } else {
         /* Once above a limit, start popping old results off the
          * tail of the list:
          */
         struct fd_batch_result *old_result =
            list_last_entry(&history->results, struct fd_batch_result, node);
         list_delinit(&old_result->node);
         ralloc_free(old_result);
      }
   }
}

static bool
fallback_use_bypass(struct fd_batch *batch)
{
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   /* Fallback logic if we have no historical data about the rendertarget: */
   if (batch->cleared || batch->gmem_reason ||
       (batch->num_draws > 5) || (pfb->samples > 1)) {
      return false;
   }

   return true;
}

/**
 * A magic 8-ball that tells the gmem code whether we should do bypass mode
 * for moar fps.
 */
bool
fd_autotune_use_bypass(struct fd_autotune *at, struct fd_batch *batch)
{
   struct pipe_framebuffer_state *pfb = &batch->framebuffer;

   process_results(at);

   /* Only enable on gen's that opt-in (and actually have sample-passed
    * collection wired up:
    */
   if (!batch->ctx->screen->gmem_reason_mask)
      return fallback_use_bypass(batch);

   if (batch->gmem_reason & ~batch->ctx->screen->gmem_reason_mask)
      return fallback_use_bypass(batch);

   for (unsigned i = 0; i < pfb->nr_cbufs; i++) {
      /* If ms-rtt is involved, force GMEM, as we don't currently
       * implement a temporary render target that we can MSAA resolve
       * from
       */
      if (pfb->cbufs[i].texture && pfb->cbufs[i].nr_samples)
         return fallback_use_bypass(batch);
   }

   struct fd_batch_history *history = get_history(at, batch);
   if (!history)
      return fallback_use_bypass(batch);

   batch->autotune_result = get_result(at, history);
   if (!batch->autotune_result)
      return fallback_use_bypass(batch);

   batch->autotune_result->cost = batch->cost;
   batch->autotune_result->batch_hash = batch->hash;
   batch->autotune_result->structural_signature = structural_signature(batch);
   batch->autotune_result->num_draws = batch->num_draws;

   bool use_bypass = fallback_use_bypass(batch);

   if (use_bypass)
      return true;

   if (history->num_results > 0) {
      uint32_t total_samples = 0;

      // TODO we should account for clears somehow
      // TODO should we try to notice if there is a drastic change from
      // frame to frame?
      list_for_each_entry (struct fd_batch_result, result, &history->results,
                           node) {
         total_samples += result->samples_passed;
      }

      float avg_samples = (float)total_samples / (float)history->num_results;

      /* Low sample count could mean there was only a clear.. or there was
       * a clear plus draws that touch no or few samples
       */
      if (avg_samples < 500.0f)
         return true;

      /* Cost-per-sample is an estimate for the average number of reads+
       * writes for a given passed sample.
       */
      float sample_cost = batch->cost;
      sample_cost /= batch->num_draws;

      float total_draw_cost = (avg_samples * sample_cost) / batch->num_draws;
      DBG("%08x:%u\ttotal_samples=%u, avg_samples=%f, sample_cost=%f, "
          "total_draw_cost=%f\n",
          batch->hash, batch->num_draws, total_samples, avg_samples,
          sample_cost, total_draw_cost);

      if (total_draw_cost < 3000.0f)
         return true;
   }

   return use_bypass;
}

void
fd_autotune_begin(struct fd_autotune *at, struct fd_batch *batch,
                  bool use_bypass)
{
   struct fd_batch_result *result = batch->autotune_result;
   struct fd_context *ctx = batch->ctx;

   if (!result)
      return;

   result->use_bypass = use_bypass;
   result->timestamped = ctx->record_timestamp && at->ts_to_ns;

   if (!result->timestamped)
      return;

   ctx->record_timestamp(batch->gmem,
                         results_ptr(at, result[result->idx].timestamp_start));
}

void
fd_autotune_init(struct fd_autotune *at, struct fd_context *ctx)
{
   STATIC_ASSERT(sizeof(struct fd_autotune_results) == 4096);

   at->ht =
      _mesa_hash_table_create(NULL, fd_batch_key_hash, fd_batch_key_equals);
   list_inithead(&at->lru);

   at->log = debug_get_option_autotune_log();

   /* DEBUG: do not allocate or update observation history in normal runs. */
   at->timing_ht = at->log ? _mesa_hash_table_u64_create(NULL) : NULL;
   list_inithead(&at->timing_lru);

   at->results_mem = fd_bo_new(
      ctx->screen->dev, sizeof(struct fd_autotune_results), 0, "autotune");
   at->results = fd_bo_map(at->results_mem);
   at->ts_to_ns = ctx->ts_to_ns;
   at->use_timestamp_completion = ctx->screen->is_kgsl;

   if (at->log)
      mesa_logi("freedreno autotune: whole-pass timing log enabled");

   list_inithead(&at->pending_results);
}

void
fd_autotune_fini(struct fd_autotune *at)
{
   if (unlikely(at->log) && at->timing_ht) {
      hash_table_u64_foreach (at->timing_ht, entry) {
         log_timing_history(entry.data);
      }
   }

   _mesa_hash_table_destroy(at->ht, NULL);
   _mesa_hash_table_u64_destroy(at->timing_ht);
   fd_bo_del(at->results_mem);
}
