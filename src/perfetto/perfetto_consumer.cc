// Copyright (C) 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "perfetto/perfetto_consumer.h"

#include <android-base/logging.h>
#include <utils/Printer.h>

#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

#include <inttypes.h>
#include <time.h>

namespace iorap::perfetto {

using State = PerfettoConsumer::State;
using Handle = PerfettoConsumer::Handle;
static constexpr Handle kInvalidHandle = PerfettoConsumer::kInvalidHandle;
using OnStateChangedCb = PerfettoConsumer::OnStateChangedCb;
using TraceBuffer = PerfettoConsumer::TraceBuffer;

enum class StateKind {
  kUncreated,
  kCreated,
  kStartedTracing,
  kReadTracing,
  kTimedOutDestroyed,  // same as kDestroyed but timed out.
  kDestroyed,          // calling kDestroyed before timing out.
};

std::ostream& operator<<(std::ostream& os, StateKind kind) {
  switch (kind) {
    case StateKind::kUncreated:
      os << "kUncreated";
      break;
    case StateKind::kCreated:
      os << "kCreated";
      break;
    case StateKind::kStartedTracing:
      os << "kStartedTracing";
      break;
    case StateKind::kReadTracing:
      os << "kReadTracing";
      break;
    case StateKind::kTimedOutDestroyed:
      os << "kTimedOutDestroyed";
      break;
    case StateKind::kDestroyed:
      os << "kDestroyed";
      break;
    default:
      os << "(invalid)";
      break;
  }
  return os;
}

std::string ToString(StateKind kind) {
  std::stringstream ss;
  ss << kind;
  return ss.str();
}

static uint64_t GetTimeNanoseconds() {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);

  uint64_t now_ns = (now.tv_sec * 1000000000LL + now.tv_nsec);
  return now_ns;
}

// Describe the state of our handle in detail for debugging/logging.
struct HandleDescription {
  Handle handle_;
  StateKind kind_;  // Our state. required for correctness.

  // For dumping to logs:
  State state_;  // perfetto state
  std::optional<uint64_t> started_tracing_ns_; // when StartedTracing last called.
  std::uint64_t last_transition_ns_;
};

// pimpl idiom to hide the implementation details from header
//
// Track and verify that our perfetto usage is sane.
struct PerfettoConsumerImpl::Impl {
  Impl() : raw_{new PerfettoConsumerRawImpl{}} {
  }

 private:
  std::unique_ptr<PerfettoConsumerRawImpl> raw_;
  std::map<Handle, HandleDescription> states_;

  // We need this to be a counter to avoid memory leaks.
  Handle last_created_{0};
  Handle last_destroyed_{0};

  std::mutex mutex_;

 public:
  Handle Create(const void* config_proto,
                size_t config_len,
                OnStateChangedCb callback,
                void* callback_arg) {
    LOG(VERBOSE) << "PerfettoConsumer::Create("
                 << "config_len=" << config_len << ")";
    Handle handle = raw_->Create(config_proto, config_len, callback, callback_arg);

    std::lock_guard<std::mutex> guard{mutex_};

    // Assume every Handle starts at 0 and then increments by 1 every Create.
    ++last_created_;
    CHECK_EQ(last_created_, handle) << "perfetto handle had unexpected behavior.";
    // Without this^ increment-by-1 behavior our detection of untracked state values is broken.
    // If we have to, we can go with Untracked=Uncreated|Destroyed but it's better to distinguish
    // the two if possible.

    HandleDescription handle_desc;
    handle_desc.handle_ = handle;
    UpdateHandleDescription(/*inout*/&handle_desc, StateKind::kCreated);

    // assume we never wrap around due to using int64
    CHECK(states_.find(handle) == states_.end()) << "perfetto handle was re-used: " << handle;
    states_[handle] = handle_desc;

    return handle;
  }

  void StartTracing(Handle handle) {
    LOG(DEBUG) << "PerfettoConsumer::StartTracing(handle=" << handle << ")";

    std::lock_guard<std::mutex> guard{mutex_};

    auto it = states_.find(handle);
    if (it == states_.end()) {
      LOG(ERROR) << "Cannot StartTracing(" << handle << "), untracked handle";
      return;
    }
    HandleDescription& handle_desc = it->second;

    raw_->StartTracing(handle);
    UpdateHandleDescription(/*inout*/&handle_desc, StateKind::kStartedTracing);

    // TODO: Use a looper here to add a timeout and immediately destroy the trace buffer.
  }

  TraceBuffer ReadTrace(Handle handle) {
    LOG(DEBUG) << "PerfettoConsumer::ReadTrace(handle=" << handle << ")";

    std::lock_guard<std::mutex> guard{mutex_};

    auto it = states_.find(handle);
    if (it == states_.end()) {
      LOG(ERROR) << "Cannot ReadTrace(" << handle << "), untracked handle";
      return TraceBuffer{};
    }

    HandleDescription& handle_desc = it->second;

    TraceBuffer trace_buffer = raw_->ReadTrace(handle);
    UpdateHandleDescription(/*inout*/&handle_desc, StateKind::kReadTracing);

    return trace_buffer;
  }

  void Destroy(Handle handle) {
    LOG(VERBOSE) << "PerfettoConsumer::Destroy(handle=" << handle << ")";

    std::lock_guard<std::mutex> guard{mutex_};

    auto it = states_.find(handle);
    if (it == states_.end()) {
      // Leniency for calling Destroy multiple times. It's not a mistake.
      LOG(ERROR) << "Cannot Destroy(" << handle << "), untracked handle";
      return;
    }

    HandleDescription& handle_desc = it->second;

    raw_->Destroy(handle);
    UpdateHandleDescription(/*inout*/&handle_desc, StateKind::kDestroyed);

    // No longer track this handle to avoid memory leaks.
    last_destroyed_ = handle;
    states_.erase(it);
  }

  State PollState(Handle handle) {
    // Just pass-through the call, we never use it directly anyway.
    return raw_->PollState(handle);
  }

  // Either fetch or infer the current handle state from a handle.
  // Meant for debugging/logging only.
  HandleDescription GetOrInferHandleDescription(Handle handle) {
    std::lock_guard<std::mutex> guard{mutex_};

    auto it = states_.find(handle);
    if (it == states_.end()) {
      HandleDescription state;
      // If it's untracked it hasn't been created yet, or it was already destroyed.
      if (IsDestroyed(handle)) {
        UpdateHandleDescription(/*inout*/&state, StateKind::kDestroyed);
      } else {
        if (!IsUncreated(handle)) {
          LOG(WARNING) << "bad state detection";
        }
      }
      return state;
    }
    return it->second;
  }

 private:
  void UpdateHandleDescription(/*inout*/HandleDescription* handle_desc, StateKind kind) {
    CHECK(handle_desc != nullptr);
    handle_desc->kind_ = kind;
    handle_desc->state_ = raw_->PollState(handle_desc->handle_);

    handle_desc->last_transition_ns_ = GetTimeNanoseconds();
    if (kind == StateKind::kStartedTracing) {
      handle_desc->started_tracing_ns_ = handle_desc->last_transition_ns_;
    }
  }

  // The following state detection is for debugging only.
  // We figure out if something is destroyed, uncreated, or live.

  // Does not distinguish between kTimedOutDestroyed and kDestroyed.
  bool IsDestroyed(Handle handle) const {
    auto it = states_.find(handle);
    if (it != states_.end()) {
      // Tracked values are not destroyed yet.
      return false;
    }

    if (handle == kInvalidHandle) {
      return false;
    }

    // The following assumes handles are incrementally generated:
    if (it == states_.end()) {
      // value is in range of [0, last_destroyed]  => destroyed.
      return handle <= last_destroyed_;
    }

    auto min_it = states_.begin();
    if (handle < min_it->first) {
      // value smaller than anything tracked: it was destroyed and we stopped tracking it.
      return true;
    }

    auto max_it = states_.rbegin();
    if (handle > max_it->first) {
      // value too big: it's uncreated;
      return false;
    }

    // else it was a value that was previously tracked within [min,max] but no longer
    return true;
  }

  bool IsUncreated(Handle handle) const {
    auto it = states_.find(handle);
    if (it != states_.end()) {
      // Tracked values are not uncreated.
      return false;
    }

    if (handle == kInvalidHandle) {
      // Strangely enough, an invalid handle can never be created.
      return true;
    }

    // The following assumes handles are incrementally generated:
    if (it == states_.end()) {
      // value is in range of (last_destroyed, inf)  => uncreated.
      return handle > last_destroyed_;
    }

    auto min_it = states_.begin();
    if (handle < min_it->first) {
      // value smaller than anything tracked: it was destroyed and we stopped tracking it.
      return false;
    }

    auto max_it = states_.rbegin();
    if (handle > max_it->first) {
      // value too big: it's uncreated;
      return true;
    }

    // else it was a value that was previously tracked within [min,max] but no longer
    return false;
  }

 public:
  void Dump(::android::Printer& printer) {
    // Locking can fail if we dump during a deadlock, so just do a best-effort lock here.
    bool is_it_locked = mutex_.try_lock();

    printer.printFormatLine("Perfetto consumer state:");
    printer.printFormatLine("  Last destroyed handle: %" PRId64, last_destroyed_);
    printer.printFormatLine("  Last created handle: %" PRId64, last_created_);
    printer.printFormatLine("");
    printer.printFormatLine("  In-flight handles:");

    for (auto it = states_.begin(); it != states_.end(); ++it) {
      HandleDescription& handle_desc = it->second;
      uint64_t started_tracing =
          handle_desc.started_tracing_ns_ ? *handle_desc.started_tracing_ns_ : 0;
      printer.printFormatLine("    Handle %" PRId64, handle_desc.handle_);
      printer.printFormatLine("      Kind: %s", ToString(handle_desc.kind_).c_str());
      printer.printFormatLine("      Perfetto State: %d", static_cast<int>(handle_desc.state_));
      printer.printFormatLine("      Started tracing at: %" PRIu64, started_tracing);
      printer.printFormatLine("      Last transition at: %" PRIu64,
                              handle_desc.last_transition_ns_);
    }
    if (states_.empty()) {
      printer.printFormatLine("    (None)");
    }

    printer.printFormatLine("");

    if (is_it_locked) {  // u.b. if calling unlock on an unlocked mutex.
      mutex_.unlock();
    }
  }

  static PerfettoConsumerImpl::Impl* GetImplSingleton() {
    static PerfettoConsumerImpl::Impl impl;
    return &impl;
  }
};

// Use a singleton because fruit instantiates a new PerfettoConsumer object for every
// new rx chain in RxProducerFactory. However, we want to track all perfetto transitions globally
// through 1 impl object.
//
// TODO: Avoiding a singleton would mean a more significant refactoring to remove the fruit/perfetto
// usage.


//
// Forward all calls to PerfettoConsumerImpl::Impl
//

PerfettoConsumerImpl::~PerfettoConsumerImpl() {
  // delete impl_;  // TODO: no singleton
}

void PerfettoConsumerImpl::Initialize() {
  // impl_ = new PerfettoConsumerImpl::Impl();  // TODO: no singleton
  impl_ = PerfettoConsumerImpl::Impl::GetImplSingleton();
}

void PerfettoConsumerImpl::Dump(::android::Printer& printer) {
  PerfettoConsumerImpl::Impl::GetImplSingleton()->Dump(/*borrow*/printer);
}

PerfettoConsumer::Handle PerfettoConsumerImpl::Create(const void* config_proto,
                                    size_t config_len,
                                    PerfettoConsumer::OnStateChangedCb callback,
                                    void* callback_arg) {
  return impl_->Create(config_proto,
                       config_len,
                       callback,
                       callback_arg);
}

void PerfettoConsumerImpl::StartTracing(PerfettoConsumer::Handle handle) {
  impl_->StartTracing(handle);
}

PerfettoConsumer::TraceBuffer PerfettoConsumerImpl::ReadTrace(PerfettoConsumer::Handle handle) {
  return impl_->ReadTrace(handle);
}

void PerfettoConsumerImpl::Destroy(PerfettoConsumer::Handle handle) {
  impl_->Destroy(handle);
}

PerfettoConsumer::State PerfettoConsumerImpl::PollState(PerfettoConsumer::Handle handle) {
  return impl_->PollState(handle);
}

}  // namespace iorap::perfetto
