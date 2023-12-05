// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/task/thread_pool/thread_group.h"

#include <utility>

#include "base/check.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/task/task_features.h"
#include "base/task/thread_pool/task_tracker.h"
#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/base/attributes.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/com_init_check_hook.h"
#include "base/win/scoped_winrt_initializer.h"
#endif

namespace base {
namespace internal {

namespace {

constexpr size_t kMaxNumberOfWorkers = 256;

// In a background thread group:
// - Blocking calls take more time than in a foreground thread group.
// - We want to minimize impact on foreground work, not maximize execution
//   throughput.
// For these reasons, the timeout to increase the maximum number of concurrent
// tasks when there is a MAY_BLOCK ScopedBlockingCall is *long*. It is not
// infinite because execution throughput should not be reduced forever if a task
// blocks forever.
//
// TODO(fdoray): On platforms without background thread groups, blocking in a
// BEST_EFFORT task should:
// 1. Increment the maximum number of concurrent tasks after a *short* timeout,
//    to allow scheduling of USER_VISIBLE/USER_BLOCKING tasks.
// 2. Increment the maximum number of concurrent BEST_EFFORT tasks after a
//    *long* timeout, because we only want to allow more BEST_EFFORT tasks to be
//    be scheduled concurrently when we believe that a BEST_EFFORT task is
//    blocked forever.
// Currently, only 1. is true as the configuration is per thread group.
// TODO(https://crbug.com/927755): Fix racy condition when MayBlockThreshold ==
// BlockedWorkersPoll.
constexpr TimeDelta kForegroundMayBlockThreshold = Milliseconds(1000);
constexpr TimeDelta kForegroundBlockedWorkersPoll = Milliseconds(1200);
constexpr TimeDelta kBackgroundMayBlockThreshold = Seconds(10);
constexpr TimeDelta kBackgroundBlockedWorkersPoll = Seconds(12);

// ThreadGroup that owns the current thread, if any.
ABSL_CONST_INIT thread_local const ThreadGroup* current_thread_group = nullptr;

}  // namespace

constexpr ThreadGroup::YieldSortKey ThreadGroup::kMaxYieldSortKey;

void ThreadGroup::BaseScopedCommandsExecutor::ScheduleReleaseTaskSource(
    RegisteredTaskSource task_source) {
  task_sources_to_release_.push_back(std::move(task_source));
}

ThreadGroup::BaseScopedCommandsExecutor::BaseScopedCommandsExecutor() = default;

ThreadGroup::BaseScopedCommandsExecutor::~BaseScopedCommandsExecutor() {
  CheckedLock::AssertNoLockHeldOnCurrentThread();
}

ThreadGroup::ScopedReenqueueExecutor::ScopedReenqueueExecutor() = default;

ThreadGroup::ScopedReenqueueExecutor::~ScopedReenqueueExecutor() {
  if (destination_thread_group_) {
    destination_thread_group_->PushTaskSourceAndWakeUpWorkers(
        std::move(transaction_with_task_source_.value()));
  }
}

void ThreadGroup::ScopedReenqueueExecutor::
    SchedulePushTaskSourceAndWakeUpWorkers(
        RegisteredTaskSourceAndTransaction transaction_with_task_source,
        ThreadGroup* destination_thread_group) {
  DCHECK(destination_thread_group);
  DCHECK(!destination_thread_group_);
  DCHECK(!transaction_with_task_source_);
  transaction_with_task_source_.emplace(
      std::move(transaction_with_task_source));
  destination_thread_group_ = destination_thread_group;
}

ThreadGroup::ThreadGroup(StringPiece histogram_label,
                         StringPiece thread_group_label,
                         ThreadType thread_type_hint,
                         TrackedRef<TaskTracker> task_tracker,
                         TrackedRef<Delegate> delegate)
    : task_tracker_(std::move(task_tracker)),
      delegate_(std::move(delegate)),
      histogram_label_(histogram_label),
      thread_group_label_(thread_group_label),
      thread_type_hint_(thread_type_hint),
      idle_workers_set_cv_for_testing_(lock_.CreateConditionVariable()) {
  DCHECK(!thread_group_label_.empty());
}

void ThreadGroup::Start(
    size_t max_tasks,
    size_t max_best_effort_tasks,
    TimeDelta suggested_reclaim_time,
    scoped_refptr<SingleThreadTaskRunner> service_thread_task_runner,
    WorkerThreadObserver* worker_thread_observer,
    WorkerEnvironment worker_environment,
    absl::optional<TimeDelta> may_block_threshold) {
  DCHECK(!replacement_thread_group_);

  in_start().no_worker_reclaim = FeatureList::IsEnabled(kNoWorkerThreadReclaim);
  in_start().may_block_threshold =
      may_block_threshold ? may_block_threshold.value()
                          : (thread_type_hint_ != ThreadType::kBackground
                                 ? kForegroundMayBlockThreshold
                                 : kBackgroundMayBlockThreshold);
  in_start().blocked_workers_poll_period =
      thread_type_hint_ != ThreadType::kBackground
          ? kForegroundBlockedWorkersPoll
          : kBackgroundBlockedWorkersPoll;
  in_start().ensure_enough_workers_at_end_of_get_work =
      base::FeatureList::IsEnabled(kUseNewJobImplementation);

  CheckedAutoLock auto_lock(lock_);

  max_tasks_ = max_tasks;
  DCHECK_GE(max_tasks_, 1U);
  in_start().initial_max_tasks = max_tasks_;
  DCHECK_LE(in_start().initial_max_tasks, kMaxNumberOfWorkers);
  max_best_effort_tasks_ = max_best_effort_tasks;
  in_start().suggested_reclaim_time = suggested_reclaim_time;
  in_start().worker_environment = worker_environment;
  in_start().service_thread_task_runner = std::move(service_thread_task_runner);
  in_start().worker_thread_observer = worker_thread_observer;

#if DCHECK_IS_ON()
  in_start().initialized = true;
#endif
}

ThreadGroup::~ThreadGroup() = default;

void ThreadGroup::BindToCurrentThread() {
  DCHECK(!CurrentThreadHasGroup());
  current_thread_group = this;
}

void ThreadGroup::UnbindFromCurrentThread() {
  DCHECK(IsBoundToCurrentThread());
  current_thread_group = nullptr;
}

bool ThreadGroup::IsBoundToCurrentThread() const {
  return current_thread_group == this;
}

size_t
ThreadGroup::GetNumAdditionalWorkersForBestEffortTaskSourcesLockRequired()
    const {
  // For simplicity, only 1 worker is assigned to each task source regardless of
  // its max concurrency, with the exception of the top task source.
  const size_t num_queued =
      priority_queue_.GetNumTaskSourcesWithPriority(TaskPriority::BEST_EFFORT);
  if (num_queued == 0 ||
      !task_tracker_->CanRunPriority(TaskPriority::BEST_EFFORT)) {
    return 0U;
  }
  if (priority_queue_.PeekSortKey().priority() == TaskPriority::BEST_EFFORT) {
    // Assign the correct number of workers for the top TaskSource (-1 for the
    // worker that is already accounted for in |num_queued|).
    return std::max<size_t>(
        1, num_queued +
               priority_queue_.PeekTaskSource()->GetRemainingConcurrency() - 1);
  }
  return num_queued;
}

size_t
ThreadGroup::GetNumAdditionalWorkersForForegroundTaskSourcesLockRequired()
    const {
  // For simplicity, only 1 worker is assigned to each task source regardless of
  // its max concurrency, with the exception of the top task source.
  const size_t num_queued = priority_queue_.GetNumTaskSourcesWithPriority(
                                TaskPriority::USER_VISIBLE) +
                            priority_queue_.GetNumTaskSourcesWithPriority(
                                TaskPriority::USER_BLOCKING);
  if (num_queued == 0 ||
      !task_tracker_->CanRunPriority(TaskPriority::HIGHEST)) {
    return 0U;
  }
  auto priority = priority_queue_.PeekSortKey().priority();
  if (priority == TaskPriority::USER_VISIBLE ||
      priority == TaskPriority::USER_BLOCKING) {
    // Assign the correct number of workers for the top TaskSource (-1 for the
    // worker that is already accounted for in |num_queued|).
    return std::max<size_t>(
        1, num_queued +
               priority_queue_.PeekTaskSource()->GetRemainingConcurrency() - 1);
  }
  return num_queued;
}

RegisteredTaskSource ThreadGroup::RemoveTaskSource(
    const TaskSource& task_source) {
  CheckedAutoLock auto_lock(lock_);
  return priority_queue_.RemoveTaskSource(task_source);
}

void ThreadGroup::ReEnqueueTaskSourceLockRequired(
    BaseScopedCommandsExecutor* workers_executor,
    ScopedReenqueueExecutor* reenqueue_executor,
    RegisteredTaskSourceAndTransaction transaction_with_task_source) {
  // Decide in which thread group the TaskSource should be reenqueued.
  ThreadGroup* destination_thread_group = delegate_->GetThreadGroupForTraits(
      transaction_with_task_source.transaction.traits());

  bool push_to_immediate_queue =
      transaction_with_task_source.task_source.WillReEnqueue(
          TimeTicks::Now(), &transaction_with_task_source.transaction);

  if (destination_thread_group == this) {
    // Another worker that was running a task from this task source may have
    // reenqueued it already, in which case its heap_handle will be valid. It
    // shouldn't be queued twice so the task source registration is released.
    if (transaction_with_task_source.task_source->immediate_heap_handle()
            .IsValid()) {
      workers_executor->ScheduleReleaseTaskSource(
          std::move(transaction_with_task_source.task_source));
    } else {
      // If the TaskSource should be reenqueued in the current thread group,
      // reenqueue it inside the scope of the lock.
      if (push_to_immediate_queue) {
        auto sort_key = transaction_with_task_source.task_source->GetSortKey();
        // When moving |task_source| into |priority_queue_|, it may be destroyed
        // on another thread as soon as |lock_| is released, since we're no
        // longer holding a reference to it. To prevent UAF, release
        // |transaction| before moving |task_source|. Ref. crbug.com/1412008
        transaction_with_task_source.transaction.Release();
        priority_queue_.Push(
            std::move(transaction_with_task_source.task_source), sort_key);
      }
    }
    // This is called unconditionally to ensure there are always workers to run
    // task sources in the queue. Some ThreadGroup implementations only invoke
    // TakeRegisteredTaskSource() once per wake up and hence this is required to
    // avoid races that could leave a task source stranded in the queue with no
    // active workers.
    EnsureEnoughWorkersLockRequired(workers_executor);
  } else {
    // Otherwise, schedule a reenqueue after releasing the lock.
    reenqueue_executor->SchedulePushTaskSourceAndWakeUpWorkers(
        std::move(transaction_with_task_source), destination_thread_group);
  }
}

RegisteredTaskSource ThreadGroup::TakeRegisteredTaskSource(
    BaseScopedCommandsExecutor* executor) {
  DCHECK(!priority_queue_.IsEmpty());

  auto run_status = priority_queue_.PeekTaskSource().WillRunTask();

  if (run_status == TaskSource::RunStatus::kDisallowed) {
    executor->ScheduleReleaseTaskSource(priority_queue_.PopTaskSource());
    return nullptr;
  }

  if (run_status == TaskSource::RunStatus::kAllowedSaturated)
    return priority_queue_.PopTaskSource();

  // If the TaskSource isn't saturated, check whether TaskTracker allows it to
  // remain in the PriorityQueue.
  // The canonical way of doing this is to pop the task source to return, call
  // RegisterTaskSource() to get an additional RegisteredTaskSource, and
  // reenqueue that task source if valid. Instead, it is cheaper and equivalent
  // to peek the task source, call RegisterTaskSource() to get an additional
  // RegisteredTaskSource to replace if valid, and only pop |priority_queue_|
  // otherwise.
  RegisteredTaskSource task_source =
      task_tracker_->RegisterTaskSource(priority_queue_.PeekTaskSource().get());
  if (!task_source)
    return priority_queue_.PopTaskSource();
  // Replace the top task_source and then update the queue.
  std::swap(priority_queue_.PeekTaskSource(), task_source);
  priority_queue_.UpdateSortKey(*task_source.get(), task_source->GetSortKey());
  return task_source;
}

void ThreadGroup::UpdateSortKeyImpl(BaseScopedCommandsExecutor* executor,
                                    TaskSource::Transaction transaction) {
  CheckedAutoLock auto_lock(lock_);
  priority_queue_.UpdateSortKey(*transaction.task_source(),
                                transaction.task_source()->GetSortKey());
  EnsureEnoughWorkersLockRequired(executor);
}

void ThreadGroup::PushTaskSourceAndWakeUpWorkersImpl(
    BaseScopedCommandsExecutor* executor,
    RegisteredTaskSourceAndTransaction transaction_with_task_source) {
  CheckedAutoLock auto_lock(lock_);
  DCHECK(!replacement_thread_group_);
  DCHECK_EQ(delegate_->GetThreadGroupForTraits(
                transaction_with_task_source.transaction.traits()),
            this);
  if (transaction_with_task_source.task_source->immediate_heap_handle()
          .IsValid()) {
    // If the task source changed group, it is possible that multiple concurrent
    // workers try to enqueue it. Only the first enqueue should succeed.
    executor->ScheduleReleaseTaskSource(
        std::move(transaction_with_task_source.task_source));
    return;
  }
  auto sort_key = transaction_with_task_source.task_source->GetSortKey();
  // When moving |task_source| into |priority_queue_|, it may be destroyed
  // on another thread as soon as |lock_| is released, since we're no longer
  // holding a reference to it. To prevent UAF, release |transaction| before
  // moving |task_source|. Ref. crbug.com/1412008
  transaction_with_task_source.transaction.Release();
  priority_queue_.Push(std::move(transaction_with_task_source.task_source),
                       sort_key);
  EnsureEnoughWorkersLockRequired(executor);
}

void ThreadGroup::HandoffNonUserBlockingTaskSourcesToOtherThreadGroup(
    ThreadGroup* destination_thread_group) {
  PriorityQueue new_priority_queue;
  TaskSourceSortKey top_sort_key;
  {
    // This works because all USER_BLOCKING tasks are at the front of the queue.
    CheckedAutoLock current_thread_group_lock(lock_);
    while (!priority_queue_.IsEmpty() &&
           (top_sort_key = priority_queue_.PeekSortKey()).priority() ==
               TaskPriority::USER_BLOCKING) {
      new_priority_queue.Push(priority_queue_.PopTaskSource(), top_sort_key);
    }
    new_priority_queue.swap(priority_queue_);
  }
  {
    CheckedAutoLock destination_thread_group_lock(
        destination_thread_group->lock_);
    while (!new_priority_queue.IsEmpty()) {
      top_sort_key = new_priority_queue.PeekSortKey();
      destination_thread_group->priority_queue_.Push(
          new_priority_queue.PopTaskSource(), top_sort_key);
    }
  }
}

bool ThreadGroup::ShouldYield(TaskSourceSortKey sort_key) {
  DCHECK(TS_UNCHECKED_READ(max_allowed_sort_key_).is_lock_free());

  if (!task_tracker_->CanRunPriority(sort_key.priority()))
    return true;
  // It is safe to read |max_allowed_sort_key_| without a lock since this
  // variable is atomic, keeping in mind that threads may not immediately see
  // the new value when it is updated.
  auto max_allowed_sort_key =
      TS_UNCHECKED_READ(max_allowed_sort_key_).load(std::memory_order_relaxed);

  // To reduce unnecessary yielding, a task will never yield to a BEST_EFFORT
  // task regardless of its worker_count.
  if (sort_key.priority() > max_allowed_sort_key.priority ||
      max_allowed_sort_key.priority == TaskPriority::BEST_EFFORT) {
    return false;
  }
  // Otherwise, a task only yields to a task of equal priority if its
  // worker_count would be greater still after yielding, e.g. a job with 1
  // worker doesn't yield to a job with 0 workers.
  if (sort_key.priority() == max_allowed_sort_key.priority &&
      sort_key.worker_count() <= max_allowed_sort_key.worker_count + 1) {
    return false;
  }

  // Reset |max_allowed_sort_key_| so that only one thread should yield at a
  // time for a given task.
  max_allowed_sort_key =
      TS_UNCHECKED_READ(max_allowed_sort_key_)
          .exchange(kMaxYieldSortKey, std::memory_order_relaxed);
  // Another thread might have decided to yield and racily reset
  // |max_allowed_sort_key_|, in which case this thread doesn't yield.
  return max_allowed_sort_key.priority != TaskPriority::BEST_EFFORT;
}

#if BUILDFLAG(IS_WIN)
// static
std::unique_ptr<win::ScopedWindowsThreadEnvironment>
ThreadGroup::GetScopedWindowsThreadEnvironment(WorkerEnvironment environment) {
  std::unique_ptr<win::ScopedWindowsThreadEnvironment> scoped_environment;
  if (environment == WorkerEnvironment::COM_MTA) {
    scoped_environment = std::make_unique<win::ScopedWinrtInitializer>();

    // TODO(crbug.com/1498668): rollback the change or replace it with a CHECK
    // before closing the bug.
    DUMP_WILL_BE_CHECK(scoped_environment->Succeeded());
  }

  DCHECK(!scoped_environment || scoped_environment->Succeeded());
  return scoped_environment;
}
#endif

// static
bool ThreadGroup::CurrentThreadHasGroup() {
  return current_thread_group != nullptr;
}

size_t ThreadGroup::GetMaxTasksForTesting() const {
  CheckedAutoLock auto_lock(lock_);
  return max_tasks_;
}

size_t ThreadGroup::GetMaxBestEffortTasksForTesting() const {
  CheckedAutoLock auto_lock(lock_);
  return max_best_effort_tasks_;
}

size_t ThreadGroup::GetMaxConcurrentNonBlockedTasksDeprecated() const {
#if DCHECK_IS_ON()
  CheckedAutoLock auto_lock(lock_);
  DCHECK_NE(after_start().initial_max_tasks, 0U)
      << "GetMaxConcurrentTasksDeprecated() should only be called after the "
      << "thread group has started.";
#endif
  return after_start().initial_max_tasks;
}

size_t ThreadGroup::GetDesiredNumAwakeWorkersLockRequired() const {
  // Number of BEST_EFFORT task sources that are running or queued and allowed
  // to run by the CanRunPolicy.
  const size_t num_running_or_queued_can_run_best_effort_task_sources =
      num_running_best_effort_tasks_ +
      GetNumAdditionalWorkersForBestEffortTaskSourcesLockRequired();

  const size_t workers_for_best_effort_task_sources =
      std::max(std::min(num_running_or_queued_can_run_best_effort_task_sources,
                        max_best_effort_tasks_),
               num_running_best_effort_tasks_);

  // Number of USER_{VISIBLE|BLOCKING} task sources that are running or queued.
  const size_t num_running_or_queued_foreground_task_sources =
      (num_running_tasks_ - num_running_best_effort_tasks_) +
      GetNumAdditionalWorkersForForegroundTaskSourcesLockRequired();

  const size_t workers_for_foreground_task_sources =
      num_running_or_queued_foreground_task_sources;

  return std::min({workers_for_best_effort_task_sources +
                       workers_for_foreground_task_sources,
                   max_tasks_, kMaxNumberOfWorkers});
}

void ThreadGroup::ScheduleAdjustMaxTasks() {
  // |adjust_max_tasks_posted_| can't change before the task posted below runs.
  // Skip check on NaCl to avoid unsafe reference acquisition warning.
#if !BUILDFLAG(IS_NACL)
  DCHECK(TS_UNCHECKED_READ(adjust_max_tasks_posted_));
#endif

  after_start().service_thread_task_runner->PostDelayedTask(
      FROM_HERE, BindOnce(&ThreadGroup::AdjustMaxTasks, Unretained(this)),
      after_start().blocked_workers_poll_period);
}

bool ThreadGroup::ShouldPeriodicallyAdjustMaxTasksLockRequired() {
  // AdjustMaxTasks() should be scheduled to periodically adjust |max_tasks_|
  // and |max_best_effort_tasks_| when (1) the concurrency limits are not large
  // enough to accommodate all queued and running task sources and an idle
  // worker and (2) there are unresolved MAY_BLOCK ScopedBlockingCalls.
  // - When (1) is false: No worker would be created or woken up if the
  //   concurrency limits were increased, so there is no hurry to increase them.
  // - When (2) is false: The concurrency limits could not be increased by
  //   AdjustMaxTasks().

  const size_t num_running_or_queued_best_effort_task_sources =
      num_running_best_effort_tasks_ +
      GetNumAdditionalWorkersForBestEffortTaskSourcesLockRequired();
  if (num_running_or_queued_best_effort_task_sources > max_best_effort_tasks_ &&
      num_unresolved_best_effort_may_block_ > 0) {
    return true;
  }

  const size_t num_running_or_queued_task_sources =
      num_running_tasks_ +
      GetNumAdditionalWorkersForBestEffortTaskSourcesLockRequired() +
      GetNumAdditionalWorkersForForegroundTaskSourcesLockRequired();
  constexpr size_t kIdleWorker = 1;
  return num_running_or_queued_task_sources + kIdleWorker > max_tasks_ &&
         num_unresolved_may_block_ > 0;
}

void ThreadGroup::UpdateMinAllowedPriorityLockRequired() {
  if (priority_queue_.IsEmpty() || num_running_tasks_ < max_tasks_) {
    max_allowed_sort_key_.store(kMaxYieldSortKey, std::memory_order_relaxed);
  } else {
    max_allowed_sort_key_.store({priority_queue_.PeekSortKey().priority(),
                                 priority_queue_.PeekSortKey().worker_count()},
                                std::memory_order_relaxed);
  }
}

void ThreadGroup::DecrementTasksRunningLockRequired(TaskPriority priority) {
  DCHECK_GT(num_running_tasks_, 0U);
  --num_running_tasks_;
  if (priority == TaskPriority::BEST_EFFORT) {
    DCHECK_GT(num_running_best_effort_tasks_, 0U);
    --num_running_best_effort_tasks_;
  }
  UpdateMinAllowedPriorityLockRequired();
}

void ThreadGroup::IncrementTasksRunningLockRequired(TaskPriority priority) {
  ++num_running_tasks_;
  DCHECK_LE(num_running_tasks_, max_tasks_);
  DCHECK_LE(num_running_tasks_, kMaxNumberOfWorkers);
  if (priority == TaskPriority::BEST_EFFORT) {
    ++num_running_best_effort_tasks_;
    DCHECK_LE(num_running_best_effort_tasks_, num_running_tasks_);
    DCHECK_LE(num_running_best_effort_tasks_, max_best_effort_tasks_);
  }
  UpdateMinAllowedPriorityLockRequired();
}

void ThreadGroup::DecrementMaxTasksLockRequired() {
  DCHECK_GT(num_running_tasks_, 0U);
  DCHECK_GT(max_tasks_, 0U);
  --max_tasks_;
  UpdateMinAllowedPriorityLockRequired();
}

void ThreadGroup::IncrementMaxTasksLockRequired() {
  DCHECK_GT(num_running_tasks_, 0U);
  ++max_tasks_;
  UpdateMinAllowedPriorityLockRequired();
}

void ThreadGroup::DecrementMaxBestEffortTasksLockRequired() {
  DCHECK_GT(num_running_tasks_, 0U);
  DCHECK_GT(max_best_effort_tasks_, 0U);
  --max_best_effort_tasks_;
  UpdateMinAllowedPriorityLockRequired();
}

void ThreadGroup::IncrementMaxBestEffortTasksLockRequired() {
  DCHECK_GT(num_running_tasks_, 0U);
  ++max_best_effort_tasks_;
  UpdateMinAllowedPriorityLockRequired();
}

ThreadGroup::InitializedInStart::InitializedInStart() = default;
ThreadGroup::InitializedInStart::~InitializedInStart() = default;

}  // namespace internal
}  // namespace base
