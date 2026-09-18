/*
 * Copyright © 2021 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include "freedreno_autotune.h"
#include "freedreno_batch.h"
#include "freedreno_context.h"
#include "freedreno_util.h"

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
         mesa_logi("freedreno autotune: key=%08x draws=%u mode=%s "
                   "samples=%" PRIu64 " duration_ns=%" PRIu64,
                   result->batch_hash, result->num_draws,
                   result->use_bypass ? "sysmem" : "gmem",
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

   at->results_mem = fd_bo_new(
      ctx->screen->dev, sizeof(struct fd_autotune_results), 0, "autotune");
   at->results = fd_bo_map(at->results_mem);
   at->ts_to_ns = ctx->ts_to_ns;
   at->log = debug_get_option_autotune_log();
   at->use_timestamp_completion = ctx->screen->is_kgsl;

   if (at->log)
      mesa_logi("freedreno autotune: whole-pass timing log enabled");

   list_inithead(&at->pending_results);
}

void
fd_autotune_fini(struct fd_autotune *at)
{
   _mesa_hash_table_destroy(at->ht, NULL);
   fd_bo_del(at->results_mem);
}
