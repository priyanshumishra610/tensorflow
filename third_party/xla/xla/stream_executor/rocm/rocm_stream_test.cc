/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/stream_executor/rocm/rocm_stream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "xla/stream_executor/device_memory.h"
#include "xla/stream_executor/gpu/gpu_test_kernels.h"
#include "xla/stream_executor/kernel.h"
#include "xla/stream_executor/launch_dim.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/platform_manager.h"
#include "xla/stream_executor/rocm/rocm_event.h"
#include "xla/stream_executor/rocm/rocm_executor.h"
#include "xla/stream_executor/rocm/rocm_platform_id.h"
#include "xla/tsl/platform/status_matchers.h"
#include "xla/tsl/platform/statusor.h"
#include "xla/tsl/platform/test.h"

namespace stream_executor {
namespace gpu {
namespace {

using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::tsl::testing::IsOk;

class RocmStreamTest : public ::testing::Test {
 public:
  std::optional<RocmExecutor> executor_;

 private:
  void SetUp() override {
    TF_ASSERT_OK_AND_ASSIGN(Platform * platform,
                            stream_executor::PlatformManager::PlatformWithId(
                                stream_executor::rocm::kROCmPlatformId));
    executor_.emplace(platform, 0);
    ASSERT_THAT(executor_->Init(), absl_testing::IsOk());
  }
};

TEST_F(RocmStreamTest, Memset32) {
  constexpr int kBufferNumElements = 42;
  DeviceMemory<uint32_t> buffer =
      executor_->AllocateArray<uint32_t>(kBufferNumElements, 0);

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  // Should fail due to the invalid size parameter.
  EXPECT_THAT(stream->Memset32(&buffer, 0xDEADBEEF,
                               kBufferNumElements * sizeof(uint32_t) + 1),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));

  // Should fail due to the non-4-byte-aligned pointer.
  DeviceMemoryBase unaligned_pointer =
      buffer.GetByteSlice(/*offset_bytes=*/1, /*size_bytes=*/0);
  EXPECT_THAT(stream->Memset32(&unaligned_pointer, 0xDEADBEEF,
                               kBufferNumElements * sizeof(uint32_t) + 1),
              absl_testing::StatusIs(absl::StatusCode::kInvalidArgument));

  // Correct call. Should succeed.
  EXPECT_THAT(stream->Memset32(&buffer, 0xDEADBEEF,
                               kBufferNumElements * sizeof(uint32_t)),
              absl_testing::IsOk());

  std::array<uint32_t, kBufferNumElements> host_buffer;
  EXPECT_THAT(stream->MemcpyD2H(buffer, absl::MakeSpan(host_buffer)),
              absl_testing::IsOk());

  EXPECT_THAT(stream->BlockHostUntilDone(), absl_testing::IsOk());
  EXPECT_THAT(host_buffer, Each(0xDEADBEEF));
}

TEST_F(RocmStreamTest, MemZero) {
  constexpr int kBufferNumElements = 42;
  DeviceMemory<uint32_t> buffer =
      executor_->AllocateArray<uint32_t>(kBufferNumElements, 0);

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  EXPECT_THAT(stream->Memset32(&buffer, 0xDEADBEEF,
                               kBufferNumElements * sizeof(uint32_t)),
              absl_testing::IsOk());

  // We overwrite half the buffer with zeros.
  EXPECT_THAT(
      stream->MemZero(&buffer, kBufferNumElements / 2 * sizeof(uint32_t)),
      absl_testing::IsOk());

  std::array<uint32_t, kBufferNumElements> host_buffer;
  EXPECT_THAT(stream->MemcpyD2H(buffer, absl::MakeSpan(host_buffer)),
              absl_testing::IsOk());

  EXPECT_THAT(stream->BlockHostUntilDone(), absl_testing::IsOk());
  // We expect the first half of the buffer to be zeros.
  EXPECT_THAT(
      absl::MakeConstSpan(host_buffer).subspan(0, kBufferNumElements / 2),
      Each(0x0));

  // And it shouldn't have touched the second half.
  EXPECT_THAT(absl::MakeConstSpan(host_buffer).subspan(kBufferNumElements / 2),
              Each(0xDEADBEEF));
}

TEST_F(RocmStreamTest, MemcpyHostToDeviceAndBack) {
  constexpr int kBufferNumElements = 42;
  DeviceMemory<uint32_t> buffer =
      executor_->AllocateArray<uint32_t>(kBufferNumElements, 0);

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  std::array<uint32_t, kBufferNumElements> src_buffer;
  std::generate(src_buffer.begin(), src_buffer.end(),
                [i = 0]() mutable { return i++; });

  EXPECT_THAT(stream->MemcpyH2D(absl::MakeConstSpan(src_buffer), &buffer),
              absl_testing::IsOk());

  std::array<uint32_t, kBufferNumElements> host_buffer;
  EXPECT_THAT(stream->MemcpyD2H(buffer, absl::MakeSpan(host_buffer)),
              absl_testing::IsOk());

  EXPECT_THAT(stream->BlockHostUntilDone(), absl_testing::IsOk());
  EXPECT_THAT(host_buffer, ElementsAreArray(src_buffer));
}

TEST_F(RocmStreamTest, MemcpyDeviceToDevice) {
  constexpr int kBufferNumElements = 42;
  DeviceMemory<uint32_t> buffer1 =
      executor_->AllocateArray<uint32_t>(kBufferNumElements, 0);
  DeviceMemory<uint32_t> buffer2 =
      executor_->AllocateArray<uint32_t>(kBufferNumElements, 0);

  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  EXPECT_THAT(stream->Memset32(&buffer1, 0xDEADBEEF,
                               kBufferNumElements * sizeof(uint32_t)),
              absl_testing::IsOk());

  EXPECT_THAT(stream->MemcpyD2D(&buffer2, buffer1,
                                kBufferNumElements * sizeof(uint32_t)),
              absl_testing::IsOk());

  std::array<uint32_t, kBufferNumElements> host_buffer;
  EXPECT_THAT(stream->MemcpyD2H(buffer2, absl::MakeSpan(host_buffer)),
              absl_testing::IsOk());

  EXPECT_THAT(stream->BlockHostUntilDone(), absl_testing::IsOk());
  EXPECT_THAT(host_buffer, Each(0xDEADBEEF));
}

TEST_F(RocmStreamTest, DoHostCallback) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  bool callback_called = false;
  EXPECT_THAT(
      stream->DoHostCallback([&callback_called]() { callback_called = true; }),
      absl_testing::IsOk());

  EXPECT_THAT(stream->BlockHostUntilDone(), absl_testing::IsOk());
  EXPECT_TRUE(callback_called);
}

TEST_F(RocmStreamTest, LaunchKernel) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  TF_ASSERT_OK_AND_ASSIGN(auto add, LoadAddI32TestKernel(&executor_.value()));

  constexpr int64_t kLength = 4;
  constexpr int64_t kByteLength = sizeof(int32_t) * kLength;

  // Prepare arguments: a=1, b=2, c=0
  DeviceMemory<int32_t> a = executor_->AllocateArray<int32_t>(kLength, 0);
  DeviceMemory<int32_t> b = executor_->AllocateArray<int32_t>(kLength, 0);
  DeviceMemory<int32_t> c = executor_->AllocateArray<int32_t>(kLength, 0);

  EXPECT_THAT(stream->Memset32(&a, 1, kByteLength), absl_testing::IsOk());
  EXPECT_THAT(stream->Memset32(&b, 2, kByteLength), absl_testing::IsOk());
  EXPECT_THAT(stream->MemZero(&c, kByteLength), absl_testing::IsOk());
  EXPECT_THAT(add.Launch(ThreadDim(), BlockDim(kLength), stream.get(), a, b, c),
              absl_testing::IsOk());

  EXPECT_THAT(stream->BlockHostUntilDone(), absl_testing::IsOk());

  std::array<int32_t, kLength> host_buffer;
  EXPECT_THAT(stream->MemcpyD2H(c, absl::MakeSpan(host_buffer)),
              absl_testing::IsOk());
  EXPECT_THAT(host_buffer, Each(3));
}

TEST_F(RocmStreamTest, SetName) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  constexpr absl::string_view kStreamName = "Test stream";
  stream->SetName(std::string(kStreamName));
  EXPECT_EQ(stream->GetName(), kStreamName);
}

TEST_F(RocmStreamTest, WaitForEvent) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  TF_ASSERT_OK_AND_ASSIGN(
      RocmEvent event,
      RocmEvent::Create(&executor_.value(), /*allow_timing=*/false));

  EXPECT_THAT(stream->WaitFor(&event), absl_testing::IsOk());

  bool callback_called = false;
  EXPECT_THAT(
      stream->DoHostCallback([&callback_called]() { callback_called = true; }),
      absl_testing::IsOk());

  EXPECT_THAT(stream->RecordEvent(&event), absl_testing::IsOk());
  EXPECT_THAT(stream->BlockHostUntilDone(), absl_testing::IsOk());
  EXPECT_TRUE(callback_called);
}

TEST_F(RocmStreamTest, WaitForOtherStream) {
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream1,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));
  TF_ASSERT_OK_AND_ASSIGN(std::unique_ptr<RocmStream> stream2,
                          RocmStream::Create(&executor_.value(),
                                             /*priority=*/std::nullopt));

  TF_ASSERT_OK_AND_ASSIGN(
      RocmEvent event,
      RocmEvent::Create(&executor_.value(), /*allow_timing=*/false));

  enum class ExecutionStage {
    kBeforeWaitForEvent,
    kAfterWaitForEvent,
    kAfterWaitForStream
  };

  std::vector<ExecutionStage> execution_order;

  // - stream1 waits for the event to be recorded and
  // - stream2 waits for stream1 to be done.
  // - Afterwards stream2 invokes the host callback.
  EXPECT_THAT(stream1->DoHostCallback([&execution_order]() {
    execution_order.push_back(ExecutionStage::kBeforeWaitForEvent);
  }),
              absl_testing::IsOk());
  EXPECT_THAT(stream1->WaitFor(&event), absl_testing::IsOk());
  EXPECT_THAT(stream1->DoHostCallback([&execution_order]() {
    execution_order.push_back(ExecutionStage::kAfterWaitForEvent);
  }),
              absl_testing::IsOk());
  EXPECT_THAT(stream2->WaitFor(stream1.get()), absl_testing::IsOk());
  EXPECT_THAT(stream2->DoHostCallback([&execution_order]() {
    execution_order.push_back(ExecutionStage::kAfterWaitForStream);
  }),
              absl_testing::IsOk());

  EXPECT_THAT(stream1->RecordEvent(&event), absl_testing::IsOk());
  EXPECT_THAT(stream2->BlockHostUntilDone(), absl_testing::IsOk());
  EXPECT_THAT(execution_order,
              ElementsAre(ExecutionStage::kBeforeWaitForEvent,
                          ExecutionStage::kAfterWaitForEvent,
                          ExecutionStage::kAfterWaitForStream));
}

}  // namespace
}  // namespace gpu
}  // namespace stream_executor
