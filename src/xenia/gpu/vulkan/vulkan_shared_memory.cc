/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_shared_memory.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {

VulkanSharedMemory::VulkanSharedMemory(
    VulkanCommandProcessor& command_processor, Memory& memory,
    TraceWriter& trace_writer)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

VulkanSharedMemory::~VulkanSharedMemory() { Shutdown(true); }

bool VulkanSharedMemory::Initialize() {
  InitializeCommon();

  const ui::vulkan::VulkanProvider& provider =
      command_processor_.GetVulkanContext().GetVulkanProvider();
  const ui::vulkan::VulkanProvider::DeviceFunctions& dfn = provider.dfn();
  VkDevice device = provider.device();
  const VkPhysicalDeviceFeatures& device_features = provider.device_features();

  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = 0;
  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
      VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
  // TODO(Triang3l): Sparse binding.
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  VkResult buffer_create_result =
      dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_);
  if (buffer_create_result != VK_SUCCESS) {
    if (buffer_create_info.flags & sparse_flags) {
      buffer_create_info.flags &= ~sparse_flags;
      buffer_create_result =
          dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_);
    }
    if (buffer_create_result != VK_SUCCESS) {
      XELOGE("Shared memory: Failed to create the {} MB Vulkan buffer",
             kBufferSize >> 20);
      Shutdown();
      return false;
    }
  }
  VkMemoryRequirements buffer_memory_requirements;
  dfn.vkGetBufferMemoryRequirements(device, buffer_,
                                    &buffer_memory_requirements);
  // TODO(Triang3l): Determine sparse binding properties from memory
  // requirements.
  if (!xe::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                provider.memory_types_device_local(),
                            &buffer_memory_type_)) {
    XELOGE(
        "Shared memory: Failed to get a device-local Vulkan memory type for "
        "the buffer");
    Shutdown();
    return false;
  }
  if (!(buffer_create_info.flags & sparse_flags)) {
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize =
        buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr,
                             &buffer_memory) != VK_SUCCESS) {
      XELOGE(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) !=
        VK_SUCCESS) {
      XELOGE("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }
  }

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      provider, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      xe::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                size_t(1) << page_size_log2()));

  return true;
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);
  last_usage_ = Usage::kTransferDestination;

  const ui::vulkan::VulkanProvider& provider =
      command_processor_.GetVulkanContext().GetVulkanProvider();
  const ui::vulkan::VulkanProvider::DeviceFunctions& dfn = provider.dfn();
  VkDevice device = provider.device();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);

  buffer_memory_allocated_.clear();
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() { upload_buffer_pool_->FlushWrites(); }

void VulkanSharedMemory::Use(Usage usage,
                             std::pair<uint32_t, uint32_t> written_range) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second =
      std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags stage_mask_src, stage_mask_dst;
    VkBufferMemoryBarrier buffer_memory_barrier;
    GetBarrier(last_usage_, stage_mask_src,
               buffer_memory_barrier.srcAccessMask);
    GetBarrier(usage, stage_mask_dst, buffer_memory_barrier.dstAccessMask);
    buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    buffer_memory_barrier.pNext = nullptr;
    buffer_memory_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_memory_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    buffer_memory_barrier.buffer = buffer_;
    if (last_usage_ == usage) {
      // Committing the previous write.
      buffer_memory_barrier.offset = VkDeviceSize(last_written_range_.first);
      buffer_memory_barrier.size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      buffer_memory_barrier.offset = 0;
      buffer_memory_barrier.size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.deferred_command_buffer().CmdVkPipelineBarrier(
        stage_mask_src, stage_mask_dst, 0, 0, nullptr, 1,
        &buffer_memory_barrier, 0, nullptr);
  }
  last_written_range_ = written_range;
}

bool VulkanSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  // upload_page_ranges are sorted, use them to determine the range for the
  // ordering barrier.
  Use(Usage::kTransferDestination,
      std::make_pair(
          upload_page_ranges.front().first << page_size_log2(),
          (upload_page_ranges.back().first + upload_page_ranges.back().second -
           upload_page_ranges.front().first)
              << page_size_log2()));
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset,
          upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        XELOGE("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(),
                     uint32_t(upload_buffer_size), false);
      std::memcpy(
          upload_buffer_mapping,
          memory().TranslatePhysical(upload_range_start << page_size_log2()),
          upload_buffer_size);
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        assert_true(upload_buffer_previous != VK_NULL_HANDLE);
        command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                       uint32_t(upload_regions_.size()),
                                       upload_regions_.data());
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset =
          VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages =
          uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                   uint32_t(upload_regions_.size()),
                                   upload_regions_.data());
    upload_regions_.clear();
  }
  return successful;
}

void VulkanSharedMemory::GetBarrier(Usage usage,
                                    VkPipelineStageFlags& stage_mask,
                                    VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_READ_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
  }
  stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  const ui::vulkan::VulkanProvider& provider =
      command_processor_.GetVulkanContext().GetVulkanProvider();
  if (provider.device_features().tessellationShader) {
    stage_mask |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |=
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe