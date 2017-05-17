//
// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//

#pragma once

#ifndef WITHOUT_HSA_BACKEND

#include "prodriver.hpp"
#include "thread/monitor.hpp"
#include <map>

/*! \addtogroup HSA
 *  @{
 */

//! HSA Device Implementation
namespace roc {

class ProDevice : public IProDevice {
public:
  ProDevice()
    : file_desc_(0)
    , major_ver_(0)
    , minor_ver_(0)
    , cp_ver_(0)
    , alloc_ops_(nullptr) {}
  virtual ~ProDevice() override;

  bool Create(uint32_t bus, uint32_t device, uint32_t func);

  virtual void* AllocDmaBuffer(
      hsa_agent_t agent, size_t size, void** host_ptr) const override;
  virtual void FreeDmaBuffer(void* ptr) const override;

private:
  int32_t               file_desc_;   //!< File descriptor for the device
  uint32_t              major_ver_;   //!< Major driver version
  uint32_t              minor_ver_;   //!< Minor driver version
  uint32_t              cp_ver_;      //!< CP ucode version
  amdgpu_device_handle  dev_handle_;  //!< AMD gpu device handle
  amdgpu_gpu_info       gpu_info_;    //!< GPU info structure
  amdgpu_heap_info      heap_info_;   //!< Information about memory
  mutable std::map<void*, std::pair<amdgpu_bo_handle, uint32_t>> allocs_; //!< Alloced memory mapping
  amd::Monitor*         alloc_ops_;   //!< Serializes memory allocations/destructions
};

}  // namespace roc

/**
 * @}
 */
#endif /*WITHOUT_HSA_BACKEND*/