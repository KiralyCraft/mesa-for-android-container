/*
 * Copyright 2026 KiralyCraft
 * SPDX-License-Identifier: MIT
 */

#include "loader_dri3_pacer.h"

#include <assert.h>
#include <stdio.h>

static void
finish_frame(struct loader_dri3_pacer *pacer, uint64_t serial,
             uint64_t admitted_us, uint64_t production_us,
             uint64_t ust, uint64_t msc)
{
   assert(loader_dri3_pacer_submission_credit(pacer));
   assert(loader_dri3_pacer_reserve(pacer, serial, msc, admitted_us,
                                    admitted_us));
   loader_dri3_pacer_note_producer_ready(pacer, serial,
                                         admitted_us + production_us);
   loader_dri3_pacer_note_submitted(pacer, serial,
                                    admitted_us + production_us + 100, true);
   assert(pacer->submission_slots_used == 1);
   assert(loader_dri3_pacer_submission_credit(pacer));
   loader_dri3_pacer_note_backend_released(
      pacer, (uint32_t) serial, true,
      admitted_us + production_us + 500);
   assert(loader_dri3_pacer_submission_credit(pacer));
   loader_dri3_pacer_note_complete(pacer, serial, ust, msc,
                                   admitted_us + production_us + 1000);
   loader_dri3_pacer_note_storage_released(pacer, serial,
                                           admitted_us + production_us + 2000);
}

static void
test_independent_lifetimes(void)
{
   struct loader_dri3_pacer pacer;
   struct loader_dri3_pacer_snapshot snapshot;

   loader_dri3_pacer_init(&pacer, 1000);
   assert(loader_dri3_pacer_reserve(&pacer, 1, 11, 1000, 1100));
   loader_dri3_pacer_note_submitted(&pacer, 1, 1200, true);
   loader_dri3_pacer_note_backend_released(&pacer, 1, true, 1250);
   loader_dri3_pacer_note_complete(&pacer, 1, 10000, 11, 1300);
   loader_dri3_pacer_snapshot(&pacer, 1300, &snapshot);
   assert(snapshot.outstanding_frames == 0);
   assert(snapshot.retained_allocations == 1);
   assert(snapshot.submission_completion_p95_us == 100);

   loader_dri3_pacer_note_storage_released(&pacer, 1, 1400);
   loader_dri3_pacer_snapshot(&pacer, 1400, &snapshot);
   assert(snapshot.retained_allocations == 0);
   assert(snapshot.storage_retention_p95_us == 200);
   assert(snapshot.stats.completed == 1);
   assert(snapshot.stats.storage_released == 1);
}

static void
test_production_and_submission_credits_are_independent(void)
{
   struct loader_dri3_pacer pacer;

   loader_dri3_pacer_init(&pacer, 1000);
   assert(loader_dri3_pacer_reserve(&pacer, 1, 11, 1000, 1000));
   loader_dri3_pacer_note_submitted(&pacer, 1, 1100, true);
   assert(loader_dri3_pacer_submission_credit(&pacer));

   assert(loader_dri3_pacer_production_credit(&pacer));
   assert(loader_dri3_pacer_reserve(&pacer, 2, 0, 1100, 1100));
   loader_dri3_pacer_set_target(&pacer, 2, 12);
   assert(pacer.frames[1].target_msc == 12);
   loader_dri3_pacer_note_submitted(&pacer, 2, 1200, true);
   assert(!loader_dri3_pacer_submission_credit(&pacer));
   assert(!loader_dri3_pacer_production_credit(&pacer));
   assert(!loader_dri3_pacer_reserve(&pacer, 3, 13, 1200, 1200));

   loader_dri3_pacer_note_complete(&pacer, 1, 10000, 11, 1300);
   assert(!loader_dri3_pacer_submission_credit(&pacer));
   assert(loader_dri3_pacer_production_credit(&pacer));
   loader_dri3_pacer_note_backend_released(&pacer, 1, true, 1350);
   assert(loader_dri3_pacer_submission_credit(&pacer));
   assert(pacer.submission_slots_used == 1);
}

static void
test_legacy_completion_releases_submission_slot(void)
{
   struct loader_dri3_pacer pacer;

   loader_dri3_pacer_init(&pacer, 1000);
   assert(loader_dri3_pacer_reserve(&pacer, 1, 11, 1000, 1000));
   loader_dri3_pacer_note_submitted(&pacer, 1, 1100, false);
   assert(loader_dri3_pacer_submission_credit(&pacer));
   assert(loader_dri3_pacer_reserve(&pacer, 2, 12, 1100, 1100));
   loader_dri3_pacer_note_submitted(&pacer, 2, 1150, false);
   assert(!loader_dri3_pacer_submission_credit(&pacer));
   loader_dri3_pacer_note_complete(&pacer, 1, 10000, 11, 1200);
   assert(loader_dri3_pacer_submission_credit(&pacer));
   assert(pacer.submission_slots_used == 1);
   assert(pacer.stats.backend_released == 1);
}

static void
test_ready_residence_covers_both_event_orders(void)
{
   struct loader_dri3_pacer pacer;

   loader_dri3_pacer_init(&pacer, 1000);

   assert(loader_dri3_pacer_reserve(&pacer, 1, 11, 1000, 1000));
   loader_dri3_pacer_note_producer_ready(&pacer, 1, 1200);
   loader_dri3_pacer_note_submitted(&pacer, 1, 1500, true);
   assert(pacer.residence_count == 1);
   assert(pacer.ready_residence_us[0] == 300);
   assert(pacer.frames[0].residence_sampled);
   loader_dri3_pacer_note_complete(&pacer, 1, 10000, 11, 1600);
   loader_dri3_pacer_note_backend_released(&pacer, 1, true, 1650);
   loader_dri3_pacer_note_storage_released(&pacer, 1, 1700);

   assert(loader_dri3_pacer_reserve(&pacer, 2, 12, 1800, 1800));
   loader_dri3_pacer_note_submitted(&pacer, 2, 1900, true);
   assert(pacer.residence_count == 1);
   loader_dri3_pacer_note_producer_ready(&pacer, 2, 2200);
   assert(pacer.residence_count == 2);
   assert(pacer.ready_residence_us[1] == 0);
   assert(pacer.frames[0].serial == 2);
   assert(pacer.frames[0].residence_sampled);

   /* Repeated notifications must not contribute duplicate samples. */
   loader_dri3_pacer_note_producer_ready(&pacer, 2, 2300);
   loader_dri3_pacer_note_submitted(&pacer, 2, 2300, true);
   assert(pacer.residence_count == 2);
}

static void
test_future_submissions_get_distinct_targets(void)
{
   struct loader_dri3_pacer pacer;

   loader_dri3_pacer_init(&pacer, 1000);
   assert(loader_dri3_pacer_reserve(&pacer, 1, 0, 1000, 1000));
   loader_dri3_pacer_set_target(
      &pacer, 1, loader_dri3_pacer_next_target_msc(&pacer, 10));
   assert(pacer.frames[0].target_msc == 11);
   loader_dri3_pacer_note_submitted(&pacer, 1, 1100, true);

   assert(loader_dri3_pacer_reserve(&pacer, 2, 0, 1100, 1100));
   loader_dri3_pacer_set_target(
      &pacer, 2, loader_dri3_pacer_next_target_msc(&pacer, 10));
   assert(pacer.frames[1].target_msc == 12);

   loader_dri3_pacer_note_complete(&pacer, 1, 10000, 11, 1200);
   assert(loader_dri3_pacer_next_target_msc(&pacer, 11) == 12);
}

static void
test_production_history_and_deadline(void)
{
   struct loader_dri3_pacer pacer;
   uint64_t admit = 1000000;
   uint64_t ust = 1000000;

   loader_dri3_pacer_init(&pacer, admit);
   for (uint64_t i = 1; i <= 8; i++) {
      finish_frame(&pacer, i, admit, 4000 + i * 1000, ust, i);
      admit += 16667;
      ust += 16667;
      loader_dri3_pacer_admit_next(&pacer, admit);
   }

   assert(loader_dri3_pacer_production_p95(&pacer) == 12000);
   assert(pacer.timing_valid);

   /* Last completion is MSC 8 at UST 1,116,669.  A submitted frame for MSC 9
    * makes MSC 10 the next production target. */
   assert(loader_dri3_pacer_next_admission(&pacer, 9, 1120000) ==
          1137003);

   pacer.production_us[0] = 40000;
   assert(loader_dri3_pacer_next_admission(&pacer, 9, 1120000) == 0);
}

static void
test_timeout_and_recovery(void)
{
   struct loader_dri3_pacer pacer;

   loader_dri3_pacer_init(&pacer, 1000);
   finish_frame(&pacer, 1, 1000, 5000, 100000, 10);
   finish_frame(&pacer, 2, 17000, 5000, 116667, 11);
   assert(pacer.timing_valid);

   loader_dri3_pacer_timing_timeout(&pacer, 120000);
   assert(pacer.timing_timed_out);
   assert(!pacer.timing_valid);
   assert(!loader_dri3_pacer_note_complete(&pacer, 99, 116667, 11,
                                           120100));
   assert(pacer.timing_timed_out);
   assert(loader_dri3_pacer_note_complete(&pacer, 100, 133334, 12,
                                          133500));
   assert(!pacer.timing_timed_out);
   assert(pacer.timing_valid);
}

static void
test_choreographer_deadline_drives_admission(void)
{
   struct loader_dri3_pacer pacer;
   struct loader_dri3_pacer_snapshot snapshot;
   uint64_t admit = 1000000;
   uint64_t ust = 1000000;

   loader_dri3_pacer_init(&pacer, admit);
   for (uint64_t i = 1; i <= 8; i++) {
      finish_frame(&pacer, i, admit, 4000 + i * 1000, ust, i);
      admit += 16667;
      ust += 16667;
      loader_dri3_pacer_admit_next(&pacer, admit);
   }

   /* The renderer consumed MSC 8 after X selected its contents at 1,100,000.
    * Android's later readiness deadline is preserved as telemetry but is not
    * the X selection boundary.  A frame already committed to MSC 9 means the
    * next producer is aimed at MSC 10: two periods after that measured
    * opportunity, less the 12 ms p95 and 1 ms guard. */
   loader_dri3_pacer_note_timeline(&pacer, 1119000, 1120000,
                                   1100000, 8, 1117000);
   assert(loader_dri3_pacer_next_admission(&pacer, 9, 1120000) ==
          1120334);

   loader_dri3_pacer_snapshot(&pacer, 1120000, &snapshot);
   assert(snapshot.timeline_valid);
   assert(snapshot.timeline_msc == 8);
   assert(snapshot.timeline_opportunity_us == 1100000);
   assert(snapshot.timeline_deadline_us == 1119000);
   assert(snapshot.timeline_expected_us == 1120000);
   assert(snapshot.stats.timeline_updates == 1);

   /* Timing metadata cannot establish any ownership transition. */
   assert(snapshot.outstanding_frames == 0);
   assert(snapshot.retained_allocations == 0);
   assert(snapshot.submission_slots_used == 0);

   loader_dri3_pacer_note_timeline(&pacer, 1200000, 1190000,
                                   1180000, 9, 1180000);
   assert(pacer.stats.timeline_invalid == 1);
   assert(pacer.last_timeline_msc == 8);
}

static void
test_lost_timing_preserves_dependencies(void)
{
   struct loader_dri3_pacer pacer;
   struct loader_dri3_pacer_snapshot snapshot;

   loader_dri3_pacer_init(&pacer, 1000);
   assert(loader_dri3_pacer_reserve(&pacer, 1, 10, 1000, 1000));
   loader_dri3_pacer_note_producer_ready(&pacer, 1, 4000);
   loader_dri3_pacer_note_submitted(&pacer, 1, 4100, true);

   loader_dri3_pacer_timing_timeout(&pacer, 104100);
   loader_dri3_pacer_snapshot(&pacer, 104100, &snapshot);
   assert(snapshot.timing_timed_out);
   assert(snapshot.outstanding_frames == 1);
   assert(snapshot.retained_allocations == 1);
   assert(snapshot.submission_slots_used == 1);

   /* A timing timeout cancels only scheduling waits.  Backend capacity,
    * logical completion, and consumer release remain independent. */
   loader_dri3_pacer_note_complete(&pacer, 1, 166670, 10, 105000);
   loader_dri3_pacer_snapshot(&pacer, 105000, &snapshot);
   assert(snapshot.outstanding_frames == 0);
   assert(snapshot.retained_allocations == 1);
   assert(snapshot.submission_slots_used == 1);

   loader_dri3_pacer_note_backend_released(&pacer, 1, true, 105500);
   assert(loader_dri3_pacer_submission_credit(&pacer));
   assert(pacer.submission_slots_used == 0);

   loader_dri3_pacer_note_storage_released(&pacer, 1, 106000);
   loader_dri3_pacer_snapshot(&pacer, 106000, &snapshot);
   assert(snapshot.retained_allocations == 0);
}

static void
test_generation_reset_and_cancel(void)
{
   struct loader_dri3_pacer pacer;
   uint64_t generation;

   loader_dri3_pacer_init(&pacer, 1000);
   generation = pacer.generation;
   assert(loader_dri3_pacer_reserve(&pacer, 1, 2, 1000, 1100));
   loader_dri3_pacer_cancel(&pacer, 1, 1200);
   assert(pacer.stats.cancelled == 1);
   assert(!pacer.frames[0].storage_released);
   loader_dri3_pacer_note_producer_ready(&pacer, 1, 1300);
   assert(pacer.frames[0].storage_released);

   loader_dri3_pacer_reset_generation(&pacer, 2000);
   assert(pacer.generation == generation + 1);
   assert(pacer.outstanding_frames == 0);
   assert(pacer.retained_allocations == 0);
   assert(pacer.current_admission_us == 2000);
   assert(pacer.stats.cancelled == 1);
   assert(pacer.generation_needs_current_completion);
}

static void
test_old_generation_feedback_does_not_seed_timeline(void)
{
   struct loader_dri3_pacer pacer;

   loader_dri3_pacer_init(&pacer, 1000);
   assert(loader_dri3_pacer_reserve(&pacer, 1, 2, 1000, 1100));
   loader_dri3_pacer_note_submitted(&pacer, 1, 1200, true);
   loader_dri3_pacer_reset_generation(&pacer, 1300);
   loader_dri3_pacer_note_producer_ready(&pacer, 1, 1350);
   assert(pacer.production_count == 0);
   assert(pacer.residence_count == 0);
   loader_dri3_pacer_note_complete(&pacer, 1, 10000, 2, 1400);
   loader_dri3_pacer_note_backend_released(&pacer, 1, true, 1450);
   assert(pacer.last_complete_ust == 0);
   assert(pacer.generation_needs_current_completion);
   assert(pacer.completion_count == 0);

   loader_dri3_pacer_note_storage_released(&pacer, 1, 1500);
   assert(pacer.retention_count == 0);
   assert(loader_dri3_pacer_reserve(&pacer, 2, 3, 1500, 1500));
   loader_dri3_pacer_note_submitted(&pacer, 2, 1600, true);
   loader_dri3_pacer_note_backend_released(&pacer, 2, true, 1650);
   loader_dri3_pacer_note_complete(&pacer, 2, 20000, 3, 1700);
   assert(pacer.last_complete_ust == 20000);
   assert(!pacer.generation_needs_current_completion);
   assert(!pacer.timing_valid);
}

static void
test_retained_allocations_are_not_recycled(void)
{
   struct loader_dri3_pacer pacer;

   loader_dri3_pacer_init(&pacer, 1000);
   for (uint64_t serial = 1; serial <= LOADER_DRI3_PACER_FRAME_SLOTS;
        serial++) {
      assert(loader_dri3_pacer_reserve(&pacer, serial, serial,
                                       serial * 1000, serial * 1000));
      loader_dri3_pacer_note_submitted(&pacer, serial,
                                       serial * 1000 + 1, true);
      loader_dri3_pacer_note_backend_released(
         &pacer, (uint32_t) serial, true, serial * 1000 + 1);
      loader_dri3_pacer_note_complete(&pacer, serial, serial * 16667,
                                      serial, serial * 1000 + 2);
   }

   assert(pacer.retained_allocations == LOADER_DRI3_PACER_FRAME_SLOTS);
   assert(!loader_dri3_pacer_reserve(&pacer, 99, 99, 99000, 99000));
   assert(pacer.stats.ledger_overflows == 1);

   loader_dri3_pacer_note_storage_released(&pacer, 1, 100000);
   assert(loader_dri3_pacer_reserve(&pacer, 99, 99, 100000, 100000));
}

static void
test_observation_window(void)
{
   struct loader_dri3_pacer pacer;
   struct loader_dri3_pacer_snapshot snapshot;

   loader_dri3_pacer_init(&pacer, 1);
   assert(loader_dri3_pacer_reserve(&pacer, 1, 2, 1, 2));
   loader_dri3_pacer_note_submitted(&pacer, 1, 3, false);
   loader_dri3_pacer_note_block(&pacer,
                                LOADER_DRI3_PACER_BLOCK_COMMITMENT,
                                100, 4);
   loader_dri3_pacer_snapshot(&pacer, 5, &snapshot);
   assert(snapshot.window_max_outstanding == 1);
   assert(snapshot.window_max_retained == 1);
   assert(snapshot.stats.block_count[LOADER_DRI3_PACER_BLOCK_COMMITMENT] == 1);
   assert(snapshot.stats.block_us[LOADER_DRI3_PACER_BLOCK_COMMITMENT] == 100);
}

static void
test_actual_feedback_is_timing_only_and_bounded(void)
{
   struct loader_dri3_pacer pacer;
   struct loader_dri3_pacer_snapshot snapshot;

   loader_dri3_pacer_init(&pacer, 1000);
   loader_dri3_pacer_expect_actual(&pacer, 1, 1000);
   loader_dri3_pacer_note_actual(&pacer, 1, true, 5000, 6000);
   loader_dri3_pacer_expect_actual(&pacer, 2, 7000);
   loader_dri3_pacer_note_actual(&pacer, 2, false, 0, 8000);

   assert(pacer.stats.actual_expected == 2);
   assert(pacer.stats.actual_presented == 1);
   assert(pacer.stats.actual_unknown == 1);
   assert(pacer.outstanding_frames == 0);
   assert(pacer.retained_allocations == 0);

   loader_dri3_pacer_expect_actual(&pacer, 3, 9000);
   loader_dri3_pacer_expect_actual(
      &pacer, 4, 9000 + LOADER_DRI3_PACER_ACTUAL_TIMEOUT_US);
   assert(pacer.stats.actual_timeouts == 1);
   loader_dri3_pacer_note_actual(&pacer, 3, true, 10000,
                                 10000 + LOADER_DRI3_PACER_ACTUAL_TIMEOUT_US);
   assert(pacer.stats.actual_unmatched == 1);

   loader_dri3_pacer_snapshot(&pacer, 2010000, &snapshot);
   assert(snapshot.submission_actual_p95_us == 4000);
   assert(snapshot.actual_pending == 1);
}

int
main(void)
{
   test_independent_lifetimes();
   test_production_and_submission_credits_are_independent();
   test_legacy_completion_releases_submission_slot();
   test_ready_residence_covers_both_event_orders();
   test_future_submissions_get_distinct_targets();
   test_production_history_and_deadline();
   test_choreographer_deadline_drives_admission();
   test_timeout_and_recovery();
   test_lost_timing_preserves_dependencies();
   test_generation_reset_and_cancel();
   test_old_generation_feedback_does_not_seed_timeline();
   test_retained_allocations_are_not_recycled();
   test_observation_window();
   test_actual_feedback_is_timing_only_and_bounded();
   puts("loader_dri3_pacer_test: pass");
   return 0;
}
