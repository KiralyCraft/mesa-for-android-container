/*
 * Copyright 2026 KiralyCraft
 * SPDX-License-Identifier: MIT
 */

#include "loader_dri3_pacer.h"

#include <limits.h>
#include <string.h>

static void
pacer_observe(struct loader_dri3_pacer *pacer,
              enum loader_dri3_pacer_block_reason reason,
              uint64_t now_us)
{
   struct loader_dri3_pacer_observation *observation =
      &pacer->observations[pacer->observation_head];

   *observation = (struct loader_dri3_pacer_observation) {
      .at_us = now_us,
      .outstanding_frames = pacer->outstanding_frames,
      .retained_allocations = pacer->retained_allocations,
      .reason = reason,
   };
   pacer->observation_head =
      (pacer->observation_head + 1) % LOADER_DRI3_PACER_HISTORY_SIZE;
   if (pacer->observation_count < LOADER_DRI3_PACER_HISTORY_SIZE)
      pacer->observation_count++;
}

static struct loader_dri3_pacer_frame *
pacer_find_frame(struct loader_dri3_pacer *pacer, uint64_t serial)
{
   for (unsigned i = 0; i < LOADER_DRI3_PACER_FRAME_SLOTS; i++) {
      if (pacer->frames[i].reserved && pacer->frames[i].serial == serial)
         return &pacer->frames[i];
   }

   return NULL;
}

static struct loader_dri3_pacer_frame *
pacer_alloc_frame(struct loader_dri3_pacer *pacer)
{
   for (unsigned i = 0; i < LOADER_DRI3_PACER_FRAME_SLOTS; i++) {
      struct loader_dri3_pacer_frame *frame = &pacer->frames[i];

      if (!frame->reserved ||
          ((frame->completed || frame->cancelled) &&
           frame->storage_released))
         return frame;
   }

   /* Terminal frame retirement does not imply allocation reclamation.  If
    * consumer-release feedback is missing, stop accounting new paced work
    * instead of overwriting the only record of retained storage. */
   return NULL;
}

static void
pacer_add_sample(uint64_t *samples, uint32_t *head, uint32_t *count,
                 uint64_t sample)
{
   samples[*head] = sample;
   *head = (*head + 1) % LOADER_DRI3_PACER_HISTORY_SIZE;
   if (*count < LOADER_DRI3_PACER_HISTORY_SIZE)
      (*count)++;
}

static uint64_t
pacer_percentile(const uint64_t *samples, uint32_t count, unsigned percentile)
{
   uint64_t sorted[LOADER_DRI3_PACER_HISTORY_SIZE];
   unsigned index;

   if (!count)
      return 0;

   memcpy(sorted, samples, count * sizeof(*samples));
   for (unsigned i = 1; i < count; i++) {
      uint64_t value = sorted[i];
      unsigned j = i;

      while (j && sorted[j - 1] > value) {
         sorted[j] = sorted[j - 1];
         j--;
      }
      sorted[j] = value;
   }

   index = (count * percentile + 99) / 100;
   index = index ? index - 1 : 0;
   if (index >= count)
      index = count - 1;
   return sorted[index];
}

void
loader_dri3_pacer_init(struct loader_dri3_pacer *pacer, uint64_t now_us)
{
   memset(pacer, 0, sizeof(*pacer));
   pacer->generation = 1;
   pacer->current_admission_us = now_us;
   pacer->period_us = 16667;
   pacer->margin_us = 1000;
   pacer->stats.admitted = 1;
}

void
loader_dri3_pacer_reset_generation(struct loader_dri3_pacer *pacer,
                                   uint64_t now_us)
{
   /* Keep old-generation frame and allocation records until their real
    * completion/release events arrive.  Only the timing model and samples are
    * generation-local. */
   pacer->generation++;
   pacer->current_admission_us = now_us;
   pacer->period_us = 16667;
   pacer->last_complete_ust = 0;
   pacer->last_complete_msc = 0;
   pacer->last_complete_local_us = 0;
   pacer->production_head = 0;
   pacer->production_count = 0;
   pacer->residence_head = 0;
   pacer->residence_count = 0;
   pacer->timing_valid = false;
   pacer->timing_timed_out = false;
   pacer->generation_needs_current_completion = true;
   memset(pacer->production_us, 0, sizeof(pacer->production_us));
   memset(pacer->ready_residence_us, 0, sizeof(pacer->ready_residence_us));
   pacer->stats.admitted++;
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_ADMISSION, now_us);
}

bool
loader_dri3_pacer_submission_credit(const struct loader_dri3_pacer *pacer)
{
   return pacer->outstanding_frames == 0;
}

bool
loader_dri3_pacer_production_credit(const struct loader_dri3_pacer *pacer)
{
   unsigned active = 0;

   for (unsigned i = 0; i < LOADER_DRI3_PACER_FRAME_SLOTS; i++) {
      const struct loader_dri3_pacer_frame *frame = &pacer->frames[i];

      if (frame->reserved && !frame->completed && !frame->cancelled)
         active++;
   }

   /* One frame may be producing while its predecessor remains committed. */
   return active < 2;
}

bool
loader_dri3_pacer_reserve(struct loader_dri3_pacer *pacer,
                          uint64_t serial, uint64_t target_msc,
                          uint64_t admitted_us, uint64_t now_us)
{
   struct loader_dri3_pacer_frame *frame;

   if (!loader_dri3_pacer_production_credit(pacer) ||
       pacer_find_frame(pacer, serial))
      return false;

   frame = pacer_alloc_frame(pacer);
   if (!frame) {
      pacer->stats.ledger_overflows++;
      return false;
   }

   *frame = (struct loader_dri3_pacer_frame) {
      .generation = pacer->generation,
      .serial = serial,
      .target_msc = target_msc,
      .admitted_us = admitted_us,
      .reserved = true,
   };
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_COMMITMENT, now_us);
   return true;
}

void
loader_dri3_pacer_set_target(struct loader_dri3_pacer *pacer,
                             uint64_t serial, uint64_t target_msc)
{
   struct loader_dri3_pacer_frame *frame = pacer_find_frame(pacer, serial);

   if (frame && !frame->submitted)
      frame->target_msc = target_msc;
}

void
loader_dri3_pacer_note_producer_ready(struct loader_dri3_pacer *pacer,
                                      uint64_t serial, uint64_t now_us)
{
   struct loader_dri3_pacer_frame *frame = pacer_find_frame(pacer, serial);

   if (!frame || frame->producer_ready)
      return;

   frame->producer_ready = true;
   frame->producer_ready_us = now_us;
   pacer->stats.producer_ready++;
   if (frame->cancelled && !frame->submitted) {
      frame->storage_released = true;
      frame->storage_released_us = now_us;
      pacer->stats.storage_released++;
   }
   if (frame->admitted_us && now_us >= frame->admitted_us &&
       now_us - frame->admitted_us <=
          (pacer->period_us * 4 > 100000 ? pacer->period_us * 4 : 100000))
      pacer_add_sample(pacer->production_us, &pacer->production_head,
                       &pacer->production_count,
                       now_us - frame->admitted_us);

   /* Completed-candidate residence ends at backend submission.  When the
    * dependency becomes ready only after submission, the frame never resided
    * in the completed-but-uncommitted state and contributes a zero sample. */
   if (frame->submitted && !frame->residence_sampled) {
      pacer_add_sample(pacer->ready_residence_us, &pacer->residence_head,
                       &pacer->residence_count,
                       frame->submitted_us >= now_us ?
                          frame->submitted_us - now_us : 0);
      frame->residence_sampled = true;
   }
}

void
loader_dri3_pacer_note_submitted(struct loader_dri3_pacer *pacer,
                                 uint64_t serial, uint64_t now_us)
{
   struct loader_dri3_pacer_frame *frame = pacer_find_frame(pacer, serial);

   if (!frame || frame->submitted)
      return;

   frame->submitted = true;
   frame->submitted_us = now_us;
   pacer->outstanding_frames++;
   pacer->retained_allocations++;
   pacer->stats.submitted++;
   if (frame->producer_ready && !frame->residence_sampled) {
      pacer_add_sample(pacer->ready_residence_us, &pacer->residence_head,
                       &pacer->residence_count,
                       now_us >= frame->producer_ready_us ?
                          now_us - frame->producer_ready_us : 0);
      frame->residence_sampled = true;
   }
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_COMMITMENT, now_us);
}

bool
loader_dri3_pacer_note_complete(struct loader_dri3_pacer *pacer,
                                uint64_t serial, uint64_t ust,
                                uint64_t msc, uint64_t now_us)
{
   struct loader_dri3_pacer_frame *frame = pacer_find_frame(pacer, serial);
   bool recovered = false;
   bool current_generation =
      frame && frame->generation == pacer->generation;

   if ((!pacer->generation_needs_current_completion || current_generation) &&
       pacer->last_complete_ust && ust > pacer->last_complete_ust &&
       msc > pacer->last_complete_msc) {
      uint64_t sample =
         (ust - pacer->last_complete_ust) / (msc - pacer->last_complete_msc);

      if (sample >= 4000 && sample <= 100000) {
         pacer->period_us = (pacer->period_us * 7 + sample) / 8;
         recovered = pacer->timing_timed_out;
         pacer->timing_valid = true;
         pacer->timing_timed_out = false;
      }
   }

   if (!pacer->generation_needs_current_completion || current_generation) {
      pacer->last_complete_ust = ust;
      pacer->last_complete_msc = msc;
      pacer->last_complete_local_us = now_us;
      pacer->generation_needs_current_completion = false;
   }

   if (!frame || frame->completed)
      return recovered;

   frame->completed = true;
   frame->completed_us = now_us;
   frame->completed_ust = ust;
   frame->completed_msc = msc;
   if (frame->submitted && pacer->outstanding_frames)
      pacer->outstanding_frames--;
   if (msc > frame->target_msc)
      pacer->stats.late_completions++;
   pacer->stats.completed++;
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_COMMITMENT, now_us);
   return recovered;
}

void
loader_dri3_pacer_note_storage_released(struct loader_dri3_pacer *pacer,
                                        uint64_t serial, uint64_t now_us)
{
   struct loader_dri3_pacer_frame *frame = pacer_find_frame(pacer, serial);

   if (!frame || frame->storage_released)
      return;

   frame->storage_released = true;
   frame->storage_released_us = now_us;
   if (frame->submitted && pacer->retained_allocations)
      pacer->retained_allocations--;
   pacer->stats.storage_released++;
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_BUFFER, now_us);
}

void
loader_dri3_pacer_cancel(struct loader_dri3_pacer *pacer,
                         uint64_t serial, uint64_t now_us)
{
   struct loader_dri3_pacer_frame *frame = pacer_find_frame(pacer, serial);

   if (!frame || frame->cancelled || frame->submitted)
      return;

   frame->cancelled = true;
   frame->storage_released = frame->producer_ready;
   frame->storage_released_us = frame->producer_ready ? now_us : 0;
   pacer->stats.cancelled++;
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_COMMITMENT, now_us);
}

void
loader_dri3_pacer_note_block(struct loader_dri3_pacer *pacer,
                             enum loader_dri3_pacer_block_reason reason,
                             uint64_t duration_us, uint64_t now_us)
{
   if (reason >= LOADER_DRI3_PACER_BLOCK_REASON_COUNT)
      return;

   pacer->stats.block_count[reason]++;
   pacer->stats.block_us[reason] += duration_us;
   pacer_observe(pacer, reason, now_us);
}

void
loader_dri3_pacer_timing_timeout(struct loader_dri3_pacer *pacer,
                                 uint64_t now_us)
{
   pacer->timing_timed_out = true;
   pacer->timing_valid = false;
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_STALE_TIMELINE, now_us);
}

uint64_t
loader_dri3_pacer_production_p95(const struct loader_dri3_pacer *pacer)
{
   return pacer_percentile(pacer->production_us, pacer->production_count, 95);
}

uint64_t
loader_dri3_pacer_next_admission(const struct loader_dri3_pacer *pacer,
                                 uint64_t submitted_target_msc,
                                 uint64_t now_us)
{
   uint64_t production_us, next_target_msc, msc_delta, next_target_ust;
   uint64_t lead_us, deadline_us;

   if (!pacer->timing_valid || pacer->timing_timed_out ||
       pacer->production_count < LOADER_DRI3_PACER_MIN_SAMPLES ||
       !pacer->last_complete_ust ||
       submitted_target_msc < pacer->last_complete_msc)
      return 0;

   production_us = loader_dri3_pacer_production_p95(pacer);
   next_target_msc = submitted_target_msc + 1;
   msc_delta = next_target_msc - pacer->last_complete_msc;
   if (!msc_delta || msc_delta > 3 ||
       msc_delta > UINT64_MAX / pacer->period_us)
      return 0;

   next_target_ust = pacer->last_complete_ust + msc_delta * pacer->period_us;
   lead_us = production_us + pacer->margin_us;
   if (next_target_ust <= lead_us)
      return 0;

   deadline_us = next_target_ust - lead_us;
   if (deadline_us <= now_us)
      return 0;

   /* A deadline more than two refresh periods away is not a useful admission
    * point for this bounded two-frame pipeline and probably indicates a clock
    * or generation mismatch. */
   if (deadline_us - now_us > pacer->period_us * 2)
      return 0;

   return deadline_us;
}

void
loader_dri3_pacer_admit_next(struct loader_dri3_pacer *pacer,
                             uint64_t now_us)
{
   pacer->current_admission_us = now_us;
   pacer->stats.admitted++;
   pacer_observe(pacer, LOADER_DRI3_PACER_BLOCK_ADMISSION, now_us);
}

void
loader_dri3_pacer_snapshot(const struct loader_dri3_pacer *pacer,
                           uint64_t now_us,
                           struct loader_dri3_pacer_snapshot *snapshot)
{
   uint64_t window_start = now_us > LOADER_DRI3_PACER_OBSERVATION_WINDOW_US ?
      now_us - LOADER_DRI3_PACER_OBSERVATION_WINDOW_US : 0;

   memset(snapshot, 0, sizeof(*snapshot));
   snapshot->stats = pacer->stats;
   snapshot->period_us = pacer->period_us;
   snapshot->production_p95_us = loader_dri3_pacer_production_p95(pacer);
   snapshot->ready_residence_p95_us =
      pacer_percentile(pacer->ready_residence_us, pacer->residence_count, 95);
   snapshot->outstanding_frames = pacer->outstanding_frames;
   snapshot->retained_allocations = pacer->retained_allocations;
   snapshot->timing_valid = pacer->timing_valid;
   snapshot->timing_timed_out = pacer->timing_timed_out;

   for (unsigned i = 0; i < pacer->observation_count; i++) {
      const struct loader_dri3_pacer_observation *observation =
         &pacer->observations[i];

      if (observation->at_us < window_start)
         continue;
      if (observation->outstanding_frames > snapshot->window_max_outstanding)
         snapshot->window_max_outstanding = observation->outstanding_frames;
      if (observation->retained_allocations > snapshot->window_max_retained)
         snapshot->window_max_retained = observation->retained_allocations;
   }
}
