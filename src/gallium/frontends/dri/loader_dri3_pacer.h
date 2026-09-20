/*
 * Copyright 2026 KiralyCraft
 * SPDX-License-Identifier: MIT
 */

#ifndef LOADER_DRI3_PACER_H
#define LOADER_DRI3_PACER_H

#include <stdbool.h>
#include <stdint.h>

#define LOADER_DRI3_PACER_HISTORY_SIZE 64
#define LOADER_DRI3_PACER_FRAME_SLOTS 8
#define LOADER_DRI3_PACER_MIN_SAMPLES 8
#define LOADER_DRI3_PACER_OBSERVATION_WINDOW_US 500000

enum loader_dri3_pacer_block_reason {
   LOADER_DRI3_PACER_BLOCK_COMMITMENT,
   LOADER_DRI3_PACER_BLOCK_ADMISSION,
   LOADER_DRI3_PACER_BLOCK_REASON_COUNT,
};

struct loader_dri3_pacer_frame {
   uint64_t generation;
   uint64_t serial;
   uint64_t target_msc;
   uint64_t admitted_us;
   uint64_t producer_ready_us;
   uint64_t submitted_us;
   bool reserved;
   bool producer_ready;
   bool submitted;
   bool residence_sampled;
   bool completed;
   bool storage_released;
   bool cancelled;
};

struct loader_dri3_pacer_observation {
   uint64_t at_us;
   uint32_t outstanding_frames;
   uint32_t retained_allocations;
};

struct loader_dri3_pacer_stats {
   uint64_t admitted;
   uint64_t producer_ready;
   uint64_t submitted;
   uint64_t completed;
   uint64_t storage_released;
   uint64_t ledger_overflows;
   uint64_t late_completions;
   uint64_t block_count[LOADER_DRI3_PACER_BLOCK_REASON_COUNT];
   uint64_t block_us[LOADER_DRI3_PACER_BLOCK_REASON_COUNT];
};

struct loader_dri3_pacer_snapshot {
   struct loader_dri3_pacer_stats stats;
   uint64_t period_us;
   uint64_t production_p95_us;
   uint64_t ready_residence_p95_us;
   uint32_t window_max_outstanding;
   uint32_t window_max_retained;
};

struct loader_dri3_pacer {
   struct loader_dri3_pacer_frame frames[LOADER_DRI3_PACER_FRAME_SLOTS];
   struct loader_dri3_pacer_observation
      observations[LOADER_DRI3_PACER_HISTORY_SIZE];
   uint64_t production_us[LOADER_DRI3_PACER_HISTORY_SIZE];
   uint64_t ready_residence_us[LOADER_DRI3_PACER_HISTORY_SIZE];
   struct loader_dri3_pacer_stats stats;
   uint64_t generation;
   uint64_t current_admission_us;
   uint64_t period_us;
   uint64_t margin_us;
   uint64_t last_complete_ust;
   uint64_t last_complete_msc;
   uint32_t production_head;
   uint32_t production_count;
   uint32_t residence_head;
   uint32_t residence_count;
   uint32_t observation_head;
   uint32_t observation_count;
   uint32_t outstanding_frames;
   uint32_t retained_allocations;
   bool timing_valid;
   bool timing_timed_out;
   bool generation_needs_current_completion;
};

void
loader_dri3_pacer_init(struct loader_dri3_pacer *pacer, uint64_t now_us);

void
loader_dri3_pacer_reset_generation(struct loader_dri3_pacer *pacer,
                                   uint64_t now_us);

bool
loader_dri3_pacer_production_credit(const struct loader_dri3_pacer *pacer);

bool
loader_dri3_pacer_reserve(struct loader_dri3_pacer *pacer,
                          uint64_t serial, uint64_t target_msc,
                          uint64_t admitted_us, uint64_t now_us);

void
loader_dri3_pacer_set_target(struct loader_dri3_pacer *pacer,
                             uint64_t serial, uint64_t target_msc);

void
loader_dri3_pacer_note_producer_ready(struct loader_dri3_pacer *pacer,
                                      uint64_t serial, uint64_t now_us);

void
loader_dri3_pacer_note_submitted(struct loader_dri3_pacer *pacer,
                                 uint64_t serial, uint64_t now_us);

bool
loader_dri3_pacer_note_complete(struct loader_dri3_pacer *pacer,
                                uint64_t serial, uint64_t ust,
                                uint64_t msc, uint64_t now_us);

void
loader_dri3_pacer_note_storage_released(struct loader_dri3_pacer *pacer,
                                        uint64_t serial, uint64_t now_us);

void
loader_dri3_pacer_cancel(struct loader_dri3_pacer *pacer,
                         uint64_t serial, uint64_t now_us);

void
loader_dri3_pacer_note_block(struct loader_dri3_pacer *pacer,
                             enum loader_dri3_pacer_block_reason reason,
                             uint64_t duration_us, uint64_t now_us);

void
loader_dri3_pacer_timing_timeout(struct loader_dri3_pacer *pacer,
                                 uint64_t now_us);

uint64_t
loader_dri3_pacer_next_admission(const struct loader_dri3_pacer *pacer,
                                 uint64_t submitted_target_msc,
                                 uint64_t now_us);

void
loader_dri3_pacer_admit_next(struct loader_dri3_pacer *pacer,
                             uint64_t now_us);

uint64_t
loader_dri3_pacer_production_p95(const struct loader_dri3_pacer *pacer);

void
loader_dri3_pacer_snapshot(const struct loader_dri3_pacer *pacer,
                           uint64_t now_us,
                           struct loader_dri3_pacer_snapshot *snapshot);

#endif
