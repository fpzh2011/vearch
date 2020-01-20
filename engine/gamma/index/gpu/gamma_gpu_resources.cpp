/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 *
 * The works below are modified based on faiss:
 * 1. Add memory slice manager
 *
 * Modified works copyright 2019 The Gamma Authors.
 *
 * The modified codes are licensed under the Apache License, Version 2.0 license
 * found in the LICENSE file in the root directory of this source tree.
 *
 */

#include "gamma_gpu_resources.h"
#include <faiss/gpu/utils/MemorySpace.h>
#include <faiss/impl/FaissAssert.h>
#include <limits>
#include "log.h"

namespace faiss {
namespace gpu {

namespace {

// How many streams per device we allocate by default (for multi-streaming)
constexpr int kNumStreams = 2;

// Use 256 MiB of pinned memory for async CPU <-> GPU copies by default
constexpr size_t kDefaultPinnedMemoryAllocation = (size_t)256 * 1024 * 1024;

// Default temporary memory allocation for <= 4 GiB memory GPUs
constexpr size_t k4GiBTempMem = (size_t)512 * 1024 * 1024;

// Default temporary memory allocation for <= 8 GiB memory GPUs
constexpr size_t k8GiBTempMem = (size_t)1024 * 1024 * 1024;

// Maximum temporary memory allocation for all GPUs
constexpr size_t kMaxTempMem = (size_t)1536 * 1024 * 1024;

}  // namespace

GammaGpuResources::GammaGpuResources()
    : pinnedMemAlloc_(nullptr),
      pinnedMemAllocSize_(0),
      // let the adjustment function determine the memory size for us by
      // passing in a huge value that will then be adjusted
      tempMemSize_(
          getDefaultTempMemForGPU(-1, std::numeric_limits<size_t>::max())),
      pinnedMemSize_(kDefaultPinnedMemoryAllocation),
      cudaMallocWarning_(true) {}

GammaGpuResources::~GammaGpuResources() {
  for (auto& entry : defaultStreams_) {
    DeviceScope scope(entry.first);

    auto it = userDefaultStreams_.find(entry.first);
    if (it == userDefaultStreams_.end()) {
      // The user did not specify this stream, thus we are the ones
      // who have created it
      CUDA_VERIFY(cudaStreamDestroy(entry.second));
    }
  }

  for (auto& entry : alternateStreams_) {
    DeviceScope scope(entry.first);

    for (auto stream : entry.second) {
      CUDA_VERIFY(cudaStreamDestroy(stream));
    }
  }

  for (auto& entry : asyncCopyStreams_) {
    DeviceScope scope(entry.first);

    CUDA_VERIFY(cudaStreamDestroy(entry.second));
  }

  for (auto& entry : blasHandles_) {
    DeviceScope scope(entry.first);

    auto blasStatus = cublasDestroy(entry.second);
    FAISS_ASSERT(blasStatus == CUBLAS_STATUS_SUCCESS);
  }

  if (pinnedMemAlloc_) {
    freeMemorySpace(MemorySpace::HostPinned, pinnedMemAlloc_);
  }
}

size_t GammaGpuResources::getDefaultTempMemForGPU(int device,
                                                  size_t requested) {
  auto totalMem = device != -1 ? getDeviceProperties(device).totalGlobalMem
                               : std::numeric_limits<size_t>::max();

  if (totalMem <= (size_t)4 * 1024 * 1024 * 1024) {
    // If the GPU has <= 4 GiB of memory, reserve 512 MiB

    if (requested > k4GiBTempMem) {
      return k4GiBTempMem;
    }
  } else if (totalMem <= (size_t)8 * 1024 * 1024 * 1024) {
    // If the GPU has <= 8 GiB of memory, reserve 1 GiB

    if (requested > k8GiBTempMem) {
      return k8GiBTempMem;
    }
  } else {
    // Never use more than 1.5 GiB
    if (requested > kMaxTempMem) {
      return kMaxTempMem;
    }
  }

  // use whatever lower limit the user requested
  return requested;
}

void GammaGpuResources::noTempMemory() {
  setTempMemory(0);
  setCudaMallocWarning(false);
}

void GammaGpuResources::setTempMemory(size_t size) {}

void GammaGpuResources::setPinnedMemory(size_t size) {
  // Should not call this after devices have been initialized
  FAISS_ASSERT(defaultStreams_.size() == 0);
  FAISS_ASSERT(!pinnedMemAlloc_);

  pinnedMemSize_ = size;
}

void GammaGpuResources::setDefaultStream(int device, cudaStream_t stream) {
  auto it = defaultStreams_.find(device);
  if (it != defaultStreams_.end()) {
    // Replace this stream with the user stream
    CUDA_VERIFY(cudaStreamDestroy(it->second));
    it->second = stream;
  }

  userDefaultStreams_[device] = stream;
}

void GammaGpuResources::setDefaultNullStreamAllDevices() {
  for (int dev = 0; dev < getNumDevices(); ++dev) {
    setDefaultStream(dev, nullptr);
  }
}

void GammaGpuResources::setCudaMallocWarning(bool b) {
  cudaMallocWarning_ = b;
}

bool GammaGpuResources::isInitialized(int device) const {
  // Use default streams as a marker for whether or not a certain
  // device has been initialized
  return defaultStreams_.count(device) != 0;
}

void GammaGpuResources::initializeForDevice(int device) {
  if (isInitialized(device)) {
    return;
  }

  // If this is the first device that we're initializing, create our
  // pinned memory allocation
  if (defaultStreams_.empty() && pinnedMemSize_ > 0) {
    allocMemorySpace(MemorySpace::HostPinned, &pinnedMemAlloc_, pinnedMemSize_);
    pinnedMemAllocSize_ = pinnedMemSize_;
  }

  FAISS_ASSERT(device < getNumDevices());
  DeviceScope scope(device);

  // Make sure that device properties for all devices are cached
  auto& prop = getDeviceProperties(device);

  // Also check to make sure we meet our minimum compute capability (3.0)
  FAISS_ASSERT_FMT(prop.major >= 3,
                   "Device id %d with CC %d.%d not supported, "
                   "need 3.0+ compute capability",
                   device, prop.major, prop.minor);

  // Create streams
  cudaStream_t defaultStream = 0;
  auto it = userDefaultStreams_.find(device);
  if (it != userDefaultStreams_.end()) {
    // We already have a stream provided by the user
    defaultStream = it->second;
  } else {
    CUDA_VERIFY(
        cudaStreamCreateWithFlags(&defaultStream, cudaStreamNonBlocking));
  }

  defaultStreams_[device] = defaultStream;

  cudaStream_t asyncCopyStream = 0;
  CUDA_VERIFY(
      cudaStreamCreateWithFlags(&asyncCopyStream, cudaStreamNonBlocking));

  asyncCopyStreams_[device] = asyncCopyStream;

  std::vector<cudaStream_t> deviceStreams;
  for (int j = 0; j < kNumStreams; ++j) {
    cudaStream_t stream = 0;
    CUDA_VERIFY(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));

    deviceStreams.push_back(stream);
  }

  alternateStreams_[device] = std::move(deviceStreams);

  // Create cuBLAS handle
  cublasHandle_t blasHandle = 0;
  auto blasStatus = cublasCreate(&blasHandle);
  FAISS_ASSERT(blasStatus == CUBLAS_STATUS_SUCCESS);
  blasHandles_[device] = blasHandle;
}

cublasHandle_t GammaGpuResources::getBlasHandle(int device) {
  initializeForDevice(device);
  return blasHandles_[device];
}

cudaStream_t GammaGpuResources::getDefaultStream(int device) {
  initializeForDevice(device);
  return defaultStreams_[device];
}

std::vector<cudaStream_t> GammaGpuResources::getAlternateStreams(int device) {
  initializeForDevice(device);
  return alternateStreams_[device];
}

DeviceMemory& GammaGpuResources::getMemoryManager(int device) {
  initializeForDevice(device);

  tig_gamma::gamma_gpu::DeviceMemoryManager* manager = manager_.GetManager(device);
  StackDeviceMemory* mem = manager->Get();
  return *mem;
}

std::pair<void*, size_t> GammaGpuResources::getPinnedMemory() {
  return std::make_pair(pinnedMemAlloc_, pinnedMemAllocSize_);
}

cudaStream_t GammaGpuResources::getAsyncCopyStream(int device) {
  initializeForDevice(device);
  return asyncCopyStreams_[device];
}

}  // namespace gpu
}  // namespace faiss
