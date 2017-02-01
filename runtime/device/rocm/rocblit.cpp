//
// Copyright (c) 2015 Advanced Micro Devices, Inc. All rights reserved.
//
#include "platform/commandqueue.hpp"
#include "device/rocm/rocdevice.hpp"
#include "device/rocm/rocblit.hpp"
#include "device/rocm/rocmemory.hpp"
#include "device/rocm/rocvirtual.hpp"
#include "utils/debug.hpp"
#include <algorithm>

namespace roc {

DmaBlitManager::DmaBlitManager(VirtualGPU& gpu, Setup setup)
    : HostBlitManager(gpu, setup)
    , MinSizeForPinnedTransfer(dev().settings().pinnedMinXferSize_)
    , completeOperation_(false)
    , context_(NULL)
{
}

inline void
DmaBlitManager::synchronize() const
{
    // todo TS tracking isn't implemented
    gpu().releaseGpuMemoryFence();

    if (syncOperation_) {
//        gpu().waitAllEngines();
        gpu().releasePinnedMem();
    }
}

inline Memory&
DmaBlitManager::gpuMem(device::Memory& mem) const
{
    return static_cast<Memory&>(mem);
}

bool
DmaBlitManager::readMemoryStaged(
    Memory&     srcMemory,
    void*       dstHost,
    Memory&     xferBuf,
    size_t      origin,
    size_t&     offset,
    size_t&     totalSize,
    size_t      xferSize) const
{
    const_address src = srcMemory.getDeviceMemory();
    address staging = xferBuf.getDeviceMemory();

    // Copy data from device to host
    src += origin + offset;
    address dst = reinterpret_cast<address>(dstHost) + offset;
    bool ret = hsaCopyStaged(src, dst, totalSize, staging, false);

    return ret;
}

bool
DmaBlitManager::readBuffer(
    device::Memory&     srcMemory,
    void*       dstHost,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    bool        entire) const
{
    // Use host copy if memory has direct access
    if (setup_.disableReadBuffer_ || gpuMem(srcMemory).isHostMemDirectAccess()) {
        return HostBlitManager::readBuffer(
            srcMemory, dstHost, origin, size, entire);
    }
    else {
        size_t  srcSize =  size[0];
        size_t  offset = 0;
        size_t  pinSize = dev().settings().pinnedXferSize_;
        pinSize = std::min(pinSize, srcSize);

        // Check if a pinned transfer can be executed
        if (pinSize && (srcSize > MinSizeForPinnedTransfer)) {
            // Allign offset to 4K boundary (Vista/Win7 limitation)
            char* tmpHost = const_cast<char*>(
                amd::alignDown(reinterpret_cast<const char*>(dstHost),
                PinnedMemoryAlignment));

            // Find the partial size for unaligned copy
            size_t partial = reinterpret_cast<const char*>(dstHost) - tmpHost;

            amd::Memory* pinned = NULL;
            bool    first = true;
            size_t  tmpSize;
            size_t  pinAllocSize;

            // Copy memory, using pinning
            while (srcSize > 0) {
                // If it's the first iterarion, then readjust the copy size
                // to include alignment
                if (first) {
                    pinAllocSize = amd::alignUp(pinSize + partial,
                        PinnedMemoryAlignment);
                    tmpSize = std::min(pinAllocSize - partial, srcSize);
                    first = false;
                }
                else {
                    tmpSize = std::min(pinSize, srcSize);
                    pinAllocSize = amd::alignUp(tmpSize, PinnedMemoryAlignment);
                    partial = 0;
                }
                amd::Coord3D dst(partial, 0, 0);
                amd::Coord3D srcPin(origin[0] + offset, 0, 0);
                amd::Coord3D copySizePin(tmpSize, 0, 0);
                size_t partial2;

                // Allocate a GPU resource for pinning
                pinned = pinHostMemory(tmpHost, pinAllocSize, partial2);
                if (pinned != NULL) {
                    // Get device memory for this virtual device
                    Memory* dstMemory = dev().getRocMemory(pinned);

                    if (!hsaCopy(gpuMem(srcMemory), *dstMemory,
                        srcPin, dst, copySizePin)) {
                        LogWarning("DmaBlitManager::readBuffer failed a pinned copy!");
                        gpu().addPinnedMem(pinned);
                        break;
                    }
                    gpu().addPinnedMem(pinned);
                }
                else {
                    LogWarning("DmaBlitManager::readBuffer failed to pin a resource!");
                    break;
                }
                srcSize -= tmpSize;
                offset += tmpSize;
                tmpHost = reinterpret_cast<char*>(tmpHost) + tmpSize + partial;
            }
        }

        if (0 != srcSize) {
            Memory& xferBuf = dev().xferRead().acquire();

            // Read memory using a staging resource
            if (!readMemoryStaged(gpuMem(srcMemory), dstHost, xferBuf, origin[0],
                    offset, srcSize, srcSize)) {
                LogError("DmaBlitManager::readBuffer failed!");
                return false;
            }

            dev().xferRead().release(gpu(), xferBuf);
        }
    }

    return true;
}

bool
DmaBlitManager::readBufferRect(
    device::Memory&         srcMemory,
    void*                   dstHost,
    const amd::BufferRect&  bufRect,
    const amd::BufferRect&  hostRect,
    const amd::Coord3D&     size,
    bool                    entire) const
{
    // Use host copy if memory has direct access
    if (setup_.disableReadBufferRect_ || gpuMem(srcMemory).isHostMemDirectAccess()) {
        return HostBlitManager::readBufferRect(
            srcMemory, dstHost, bufRect, hostRect, size, entire);
    }
    else {
        Memory& xferBuf = dev().xferRead().acquire();
        address staging = xferBuf.getDeviceMemory();
        const_address src = gpuMem(srcMemory).getDeviceMemory();

        size_t srcOffset;
        size_t dstOffset;

        for (size_t z = 0; z < size[2]; ++z) {
            for (size_t y = 0; y < size[1]; ++y) {
                srcOffset = bufRect.offset(0, y, z);
                dstOffset = hostRect.offset(0, y, z);

                // Copy data from device to host - line by line
                address dst = reinterpret_cast<address>(dstHost) + dstOffset;
                src += srcOffset;
                bool retval = hsaCopyStaged(src, dst, size[0], staging, false);
                if (!retval) {
                    return retval;
                }
            }
        }
        dev().xferRead().release(gpu(), xferBuf);
    }

    return true;
}

bool
DmaBlitManager::readImage(
    device::Memory&     srcMemory,
    void*       dstHost,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    size_t      rowPitch,
    size_t      slicePitch,
    bool        entire) const
{
    if (setup_.disableReadImage_) {
        return HostBlitManager::readImage(srcMemory, dstHost,
            origin, size, rowPitch, slicePitch, entire);
    }
    else {
        //! @todo Add HW accelerated path
        return HostBlitManager::readImage(srcMemory, dstHost,
            origin, size, rowPitch, slicePitch, entire);
    }

    return true;
}

bool
DmaBlitManager::writeMemoryStaged(
    const void* srcHost,
    Memory&     dstMemory,
    Memory&     xferBuf,
    size_t      origin,
    size_t&     offset,
    size_t&     totalSize,
    size_t      xferSize) const
{
    address dst = dstMemory.getDeviceMemory();
    address staging = xferBuf.getDeviceMemory();

    // Copy data from host to device
    dst += origin + offset;
    const_address src = reinterpret_cast<const_address>(srcHost) + offset;
    bool retval = hsaCopyStaged(src, dst, totalSize, staging, true);

    return retval;
}

bool
DmaBlitManager::writeBuffer(
    const void* srcHost,
    device::Memory&     dstMemory,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    bool        entire) const
{
    // Use host copy if memory has direct access
    if (setup_.disableWriteBuffer_ ||
        gpuMem(dstMemory).isHostMemDirectAccess()) {
        return HostBlitManager::writeBuffer(
            srcHost, dstMemory, origin, size, entire);
    }
    else {
        size_t  dstSize = size[0];
        size_t  tmpSize = 0;
        size_t  offset = 0;
        size_t  pinSize = dev().settings().pinnedXferSize_;
        pinSize = std::min(pinSize, dstSize);

        // Check if a pinned transfer can be executed
        if (pinSize && (dstSize > MinSizeForPinnedTransfer)) {
            // Allign offset to 4K boundary (Vista/Win7 limitation)
            char* tmpHost = const_cast<char*>(
                amd::alignDown(reinterpret_cast<const char*>(srcHost),
                PinnedMemoryAlignment));

            // Find the partial size for unaligned copy
            size_t partial = reinterpret_cast<const char*>(srcHost) - tmpHost;

            amd::Memory* pinned = NULL;
            bool    first = true;
            size_t  tmpSize;
            size_t  pinAllocSize;

            // Copy memory, using pinning
            while (dstSize > 0) {
                // If it's the first iterarion, then readjust the copy size
                // to include alignment
                if (first) {
                    pinAllocSize = amd::alignUp(pinSize + partial,
                        PinnedMemoryAlignment);
                    tmpSize = std::min(pinAllocSize - partial, dstSize);
                    first = false;
                }
                else {
                    tmpSize = std::min(pinSize, dstSize);
                    pinAllocSize = amd::alignUp(tmpSize, PinnedMemoryAlignment);
                    partial = 0;
                }
                amd::Coord3D src(partial, 0, 0);
                amd::Coord3D dstPin(origin[0] + offset, 0, 0);
                amd::Coord3D copySizePin(tmpSize, 0, 0);
                size_t partial2;

                // Allocate a GPU resource for pinning
                pinned = pinHostMemory(tmpHost, pinAllocSize, partial2);

                if (pinned != NULL) {
                    // Get device memory for this virtual device
                    Memory* srcMemory = dev().getRocMemory(pinned);

                    if (!hsaCopy(*srcMemory, gpuMem(dstMemory), src, dstPin,
                            copySizePin)) {
                        LogWarning("DmaBlitManager::writeBuffer failed a pinned copy!");
                        gpu().addPinnedMem(pinned);
                        break;
                    }
                    gpu().addPinnedMem(pinned);
                }
                else {
                    LogWarning("DmaBlitManager::writeBuffer failed to pin a resource!");
                    break;
                }
                dstSize -= tmpSize;
                offset += tmpSize;
                tmpHost = reinterpret_cast<char*>(tmpHost) + tmpSize + partial;
            }
        }

        if (dstSize != 0) {
            Memory& xferBuf = dev().xferWrite().acquire();

            // Write memory using a staging resource
            if (!writeMemoryStaged(srcHost, gpuMem(dstMemory), xferBuf, origin[0],
                    offset, dstSize, dstSize)) {
                LogError("DmaBlitManager::writeBuffer failed!");
                return false;
            }

            gpu().addXferWrite(xferBuf);
        }
    }

    return true;
}

bool
DmaBlitManager::writeBufferRect(
    const void* srcHost,
    device::Memory&     dstMemory,
    const amd::BufferRect&   hostRect,
    const amd::BufferRect&   bufRect,
    const amd::Coord3D& size,
    bool        entire) const
{
    // Use host copy if memory has direct access
    if (setup_.disableWriteBufferRect_ || dstMemory.isHostMemDirectAccess()) {
        return HostBlitManager::writeBufferRect(
            srcHost, dstMemory, hostRect, bufRect, size, entire);
    }
    else {
        Memory& xferBuf = dev().xferWrite().acquire();
        address staging = xferBuf.getDeviceMemory();
        address dst = static_cast<roc::Memory&>(dstMemory).getDeviceMemory();

        size_t srcOffset;
        size_t dstOffset;

        for (size_t z = 0; z < size[2]; ++z) {
            for (size_t y = 0; y < size[1]; ++y) {
                srcOffset = hostRect.offset(0, y, z);
                dstOffset = bufRect.offset(0, y, z);

                // Copy data from host to device - line by line
                dst += dstOffset;
                const_address src = reinterpret_cast<const_address>(srcHost) + srcOffset;
                bool retval = hsaCopyStaged(src, dst, size[0], staging, true);
                if (!retval) {
                    return retval;
                }
            }
        }
        gpu().addXferWrite(xferBuf);
    }

    return true;
}

bool
DmaBlitManager::writeImage(
    const void* srcHost,
    device::Memory&     dstMemory,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    size_t      rowPitch,
    size_t      slicePitch,
    bool        entire) const
{
    if (setup_.disableWriteImage_) {
        return HostBlitManager::writeImage(
            srcHost, dstMemory, origin, size, rowPitch, slicePitch, entire);
    }
    else {
        //! @todo Add HW accelerated path
        return HostBlitManager::writeImage(
            srcHost, dstMemory, origin, size, rowPitch, slicePitch, entire);
    }

    return true;
}

bool
DmaBlitManager::copyBuffer(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire) const
{
    if (setup_.disableCopyBuffer_ ||
        (gpuMem(srcMemory).isHostMemDirectAccess() &&
         (dev().agent_profile() != HSA_PROFILE_FULL) &&
         gpuMem(dstMemory).isHostMemDirectAccess())) {
        return HostBlitManager::copyBuffer(
            srcMemory, dstMemory, srcOrigin, dstOrigin, size);
    }
    else {
        return hsaCopy(gpuMem(srcMemory), gpuMem(dstMemory),
            srcOrigin, dstOrigin, size);
    }

    return true;
}

bool
DmaBlitManager::copyBufferRect(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::BufferRect&   srcRect,
    const amd::BufferRect&   dstRect,
    const amd::Coord3D& size,
    bool        entire) const
{
    if (setup_.disableCopyBufferRect_ ||
        (gpuMem(srcMemory).isHostMemDirectAccess() &&
         gpuMem(dstMemory).isHostMemDirectAccess())) {
        return HostBlitManager::copyBufferRect(
            srcMemory, dstMemory, srcRect, dstRect, size, entire);
    }
    else {
        return false;
        void* src = gpuMem(srcMemory).getDeviceMemory();
        void* dst = gpuMem(dstMemory).getDeviceMemory();

        // Detect the agents for memory allocations
        const hsa_agent_t srcAgent = (srcMemory.isHostMemDirectAccess()) ?
            dev().getCpuAgent() : dev().getBackendDevice();
        const hsa_agent_t dstAgent = (dstMemory.isHostMemDirectAccess()) ?
            dev().getCpuAgent() : dev().getBackendDevice();

        const hsa_signal_value_t kInitVal = size[2] * size[1];
        hsa_signal_store_relaxed(completion_signal_, kInitVal);

        for (size_t z = 0; z < size[2]; ++z) {
            for (size_t y = 0; y < size[1]; ++y) {
                size_t srcOffset = srcRect.offset(0, y, z);
                size_t dstOffset = dstRect.offset(0, y, z);

                // Copy memory line by line
                hsa_status_t status = hsa_amd_memory_async_copy(
                    (reinterpret_cast<address>(dst) + dstOffset), dstAgent,
                    (reinterpret_cast<const_address>(src) + srcOffset),
                    srcAgent, size[0], 0, NULL, completion_signal_);
                if (status != HSA_STATUS_SUCCESS) {
                    LogPrintfError("DMA buffer failed with code %d", status);
                    return false;
                }
            }
        }

        hsa_signal_value_t val =
            hsa_signal_wait_acquire(completion_signal_, HSA_SIGNAL_CONDITION_EQ,
            0, uint64_t(-1), HSA_WAIT_STATE_ACTIVE);

        if (val != 0) {
            LogError("Async copy failed");
            return false;
        }
    }
    return true;
}

bool
DmaBlitManager::copyImageToBuffer(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire,
    size_t      rowPitch,
    size_t      slicePitch) const
{
    bool    result = false;

    if (setup_.disableCopyImageToBuffer_) {
        result = HostBlitManager::copyImageToBuffer(srcMemory, dstMemory,
            srcOrigin, dstOrigin, size, entire, rowPitch, slicePitch);
    }
    else {
        Image& srcImage = static_cast<roc::Image&>(srcMemory);
        Buffer& dstBuffer = static_cast<roc::Buffer&>(dstMemory);

        // Use ROC path for a transfer
        // Note: it doesn't support SDMA
        address dstHost = reinterpret_cast<address>(dstBuffer.getDeviceMemory()) +
            dstOrigin[0];

        // Use ROCm path for a transfer.
        // Note: it doesn't support SDMA
        hsa_ext_image_region_t image_region;
        image_region.offset.x = srcOrigin[0];
        image_region.offset.y = srcOrigin[1];
        image_region.offset.z = srcOrigin[2];
        image_region.range.x = size[0];
        image_region.range.y = size[1];
        image_region.range.z = size[2];

        hsa_status_t status = hsa_ext_image_export(gpu().gpu_device(),
             srcImage.getHsaImageObject(), dstHost, rowPitch,
             slicePitch, &image_region);
        result = (status == HSA_STATUS_SUCCESS) ? true : false;

        // Check if a HostBlit transfer is required
        if (completeOperation_ && !result) {
            result = HostBlitManager::copyImageToBuffer(srcMemory,
                dstMemory, srcOrigin, dstOrigin, size, entire, rowPitch, slicePitch);
        }
    }

    return result;
}

bool
DmaBlitManager::copyBufferToImage(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire,
    size_t      rowPitch,
    size_t      slicePitch) const
{
    bool    result = false;

    if (setup_.disableCopyBufferToImage_) {
        result = HostBlitManager::copyBufferToImage(srcMemory,
            dstMemory, srcOrigin, dstOrigin, size, entire, rowPitch, slicePitch);
    }
    else {
        Buffer& srcBuffer = static_cast<roc::Buffer&>(srcMemory);
        Image& dstImage = static_cast<roc::Image&>(dstMemory);

        // Use ROC path for a transfer
        // Note: it doesn't support SDMA
        address srcHost = reinterpret_cast<address>(srcBuffer.getDeviceMemory()) +
            srcOrigin[0];

        hsa_ext_image_region_t image_region;
        image_region.offset.x = dstOrigin[0];
        image_region.offset.y = dstOrigin[1];
        image_region.offset.z = dstOrigin[2];
        image_region.range.x = size[0];
        image_region.range.y = size[1];
        image_region.range.z = size[2];

        hsa_status_t status = hsa_ext_image_import(gpu().gpu_device(),
            srcHost, rowPitch, slicePitch,  dstImage.getHsaImageObject(), &image_region);
        result = (status == HSA_STATUS_SUCCESS) ? true : false;

        // Check if a HostBlit tran sfer is required
        if (completeOperation_ && !result) {
            result = HostBlitManager::copyBufferToImage(srcMemory,
                dstMemory, srcOrigin, dstOrigin, size, entire, rowPitch, slicePitch);
        }
    }

    return result;
}

bool
DmaBlitManager::copyImage(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire) const
{
    bool    result = false;

    if (setup_.disableCopyImage_) {
        return HostBlitManager::copyImage(srcMemory, dstMemory,
            srcOrigin, dstOrigin, size, entire);
    }
    else {
        //! @todo Add HW accelerated path
        return HostBlitManager::copyImage(srcMemory, dstMemory,
            srcOrigin, dstOrigin, size, entire);
    }

    return result;
}

bool DmaBlitManager::hsaCopy(
    const Memory&       srcMemory,
    const Memory&       dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool                enableCopyRect,
    bool                flushDMA) const
{
    address src = reinterpret_cast<address>(srcMemory.getDeviceMemory());
    address dst = reinterpret_cast<address>(dstMemory.getDeviceMemory());

    src += srcOrigin[0];
    dst += dstOrigin[0];

    // Just call copy function for full profile
    hsa_status_t status;
    if (dev().agent_profile() == HSA_PROFILE_FULL) {
        status = hsa_memory_copy(dst, src, size[0]);
        if (status != HSA_STATUS_SUCCESS) {
            LogPrintfError("Hsa copy of data failed with code %d", status);
        }
        return (status == HSA_STATUS_SUCCESS);
    }

    // Detect the agents for memory allocations
    const hsa_agent_t srcAgent = (srcMemory.isHostMemDirectAccess()) ?
        dev().getCpuAgent() : dev().getBackendDevice();
    const hsa_agent_t dstAgent = (dstMemory.isHostMemDirectAccess()) ?
        dev().getCpuAgent() : dev().getBackendDevice();

    const hsa_signal_value_t kInitVal = 1;
    hsa_signal_store_relaxed(completion_signal_, kInitVal);

    // Use SDMA to transfer the data
    status = hsa_amd_memory_async_copy(dst, dstAgent, src, srcAgent,
        size[0], 0, nullptr, completion_signal_);
    if (status == HSA_STATUS_SUCCESS) {
        hsa_signal_value_t val = hsa_signal_wait_acquire(
            completion_signal_, HSA_SIGNAL_CONDITION_EQ, 0,
            uint64_t(-1), HSA_WAIT_STATE_ACTIVE);
        if (val != (kInitVal - 1)) {
            LogError("Async copy failed");
            status = HSA_STATUS_ERROR;
        }
   }
   else {
        LogPrintfError("Hsa copy from host to device failed with code %d", status);
   }

    return (status == HSA_STATUS_SUCCESS);
}

bool DmaBlitManager::hsaCopyStaged(
    const_address hostSrc, address hostDst, size_t size, address staging, bool hostToDev) const
{
    // No allocation is necessary for Full Profile
    hsa_status_t status;
    if (dev().agent_profile() == HSA_PROFILE_FULL) {
        status = hsa_memory_copy(hostDst, hostSrc, size);
        if (status != HSA_STATUS_SUCCESS) {
            LogPrintfError("Hsa copy of data failed with code %d", status);
        }
        return (status == HSA_STATUS_SUCCESS);
    }

    size_t totalSize = size;
    size_t offset = 0;

    address hsaBuffer = staging;

    const hsa_signal_value_t kInitVal = 1;

    // Allocate requested size of memory
    while (totalSize > 0) {
        size = std::min(totalSize, dev().settings().stagedXferSize_);
        hsa_signal_store_relaxed(completion_signal_, kInitVal);

        // Copy data from Host to Device
        if (hostToDev) {
            memcpy(hsaBuffer, hostSrc + offset, size);
            status = hsa_amd_memory_async_copy(
                hostDst + offset, dev().getBackendDevice(), hsaBuffer,
                dev().getCpuAgent(), size, 0, NULL, completion_signal_);
            if (status == HSA_STATUS_SUCCESS) {
                hsa_signal_value_t val =
                hsa_signal_wait_acquire(completion_signal_,
                HSA_SIGNAL_CONDITION_EQ, 0,
                uint64_t(-1), HSA_WAIT_STATE_ACTIVE);

                if (val != (kInitVal - 1)) {
                    LogError("Async copy failed");
                    return false;
                }
            }
            else {
                LogPrintfError("Hsa copy from host to device failed with code %d", status);
                return false;
            }
            totalSize -= size;
            offset += size;
            continue;
        }

        // Copy data from Device to Host
        status = hsa_amd_memory_async_copy(hsaBuffer,
            dev().getCpuAgent(), hostSrc + offset, dev().getBackendDevice(),
            size, 0, NULL, completion_signal_);
        if (status == HSA_STATUS_SUCCESS) {
            hsa_signal_value_t val = hsa_signal_wait_acquire(
            completion_signal_, HSA_SIGNAL_CONDITION_EQ, 0, uint64_t(-1),
            HSA_WAIT_STATE_ACTIVE);

            if (val != (kInitVal - 1)) {
                LogError("Async copy failed");
                return false;
            }
            memcpy(hostDst + offset, hsaBuffer, size);
        }
        else {
            LogPrintfError("Hsa copy from device to host failed with code %d", status);
            return false;
        }
        totalSize -= size;
        offset += size;
    }

    return true;
}

KernelBlitManager::KernelBlitManager(
    VirtualGPU& gpu, Setup setup)
    : DmaBlitManager(gpu, setup)
    , program_(NULL)
    , constantBuffer_(NULL)
    , xferBufferSize_(0)
    , lockXferOps_(NULL)
{
    for (uint i = 0; i < BlitTotal; ++i) {
        kernels_[i] = NULL;
    }

    for (uint i = 0; i < MaxXferBuffers; ++i) {
        xferBuffers_[i] = NULL;
    }

    completeOperation_ = false;
}

KernelBlitManager::~KernelBlitManager()
{
    for (uint i = 0; i < BlitTotal; ++i) {
        if (NULL != kernels_[i]) {
            kernels_[i]->release();
        }
    }
    if (NULL != program_) {
        program_->release();
    }

    if (NULL != context_) {
        // Release a dummy context
        context_->release();
    }

    if (NULL != constantBuffer_) {
        constantBuffer_->release();
    }

    for (uint i = 0; i < MaxXferBuffers; ++i) {
        if (NULL != xferBuffers_[i]) {
            xferBuffers_[i]->release();
        }
    }

    delete lockXferOps_;
}

bool
KernelBlitManager::create(amd::Device& device)
{
    if (!DmaBlitManager::create(device)) {
        return false;
    }

    if (!createProgram(static_cast<Device&>(device))) {
        return false;
    }
    return true;
}

bool
KernelBlitManager::createProgram(Device& device)
{
    if (device.blitProgram() == nullptr) {
        return false;
    }

    std::vector<amd::Device*> devices;
    devices.push_back(&device);

    // Save context and program for this device
    context_ = device.blitProgram()->context_;
    context_->retain();
    program_ = device.blitProgram()->program_;
    program_->retain();

    bool result = false;
    do {
        // Create kernel objects for all blits
        for (uint i = 0; i < BlitTotal; ++i) {
            const amd::Symbol* symbol = program_->findSymbol(BlitName[i]);
            if (symbol == NULL) {
                break;
            }
            kernels_[i] = new amd::Kernel(*program_, *symbol, BlitName[i]);
            if (kernels_[i] == NULL) {
                break;
            }
            // Validate blit kernels for the scratch memory usage (pre SI)
            if (!device.validateKernel(*kernels_[i], &gpu())) {
                break;
            }
        }

        result = true;
    } while(!result);

    // Create an internal constant buffer
    constantBuffer_ = new (*context_)
        amd::Buffer(*context_, CL_MEM_ALLOC_HOST_PTR, 4 * Ki);

    if ((constantBuffer_ != NULL) && !constantBuffer_->create(NULL)) {
        constantBuffer_->release();
        constantBuffer_ = NULL;
        return false;
    }
    else if (constantBuffer_ == NULL) {
        return false;
    }

    // Assign the constant buffer to the current virtual GPU
    constantBuffer_->setVirtualDevice(&gpu());

    if (dev().settings().xferBufSize_ > 0) {
        xferBufferSize_ = dev().settings().xferBufSize_;
        for (uint i = 0; i < MaxXferBuffers; ++i) {
            // Create internal xfer buffers for image copy optimization
            xferBuffers_[i] = new (*context_)
                amd::Buffer(*context_, 0, xferBufferSize_);

            if ((xferBuffers_[i] != NULL) && !xferBuffers_[i]->create(NULL)) {
                xferBuffers_[i]->release();
                xferBuffers_[i] = NULL;
                return false;
            }
            else if (xferBuffers_[i] == NULL) {
                return false;
            }

            // Assign the xfer buffer to the current virtual GPU
            xferBuffers_[i]->setVirtualDevice(&gpu());
            //! @note Workaround for conformance allocation test.
            //! Force GPU mem alloc.
            //! Unaligned images require xfer optimization,
            //! but deferred memory allocation can cause
            //! virtual heap fragmentation for big allocations and
            //! then fail the following test with 32 bit ISA, because
            //! runtime runs out of 4GB space.
            dev().getRocMemory(xferBuffers_[i]);
        }
    }

    lockXferOps_ = new amd::Monitor("Transfer Ops Lock", true);
    if (NULL == lockXferOps_) {
        return false;
    }

    return result;
}

// The following data structures will be used for the view creations.
// Some formats has to be converted before a kernel blit operation
struct FormatConvertion {
    cl_uint clOldType_;
    cl_uint clNewType_;
};

// The list of rejected data formats and corresponding conversion
static const FormatConvertion RejectedData[] =
{
    { CL_UNORM_INT8,            CL_UNSIGNED_INT8  },
    { CL_UNORM_INT16,           CL_UNSIGNED_INT16 },
    { CL_SNORM_INT8,            CL_UNSIGNED_INT8  },
    { CL_SNORM_INT16,           CL_UNSIGNED_INT16 },
    { CL_HALF_FLOAT,            CL_UNSIGNED_INT16 },
    { CL_FLOAT,                 CL_UNSIGNED_INT32 },
    { CL_SIGNED_INT8,           CL_UNSIGNED_INT8  },
    { CL_SIGNED_INT16,          CL_UNSIGNED_INT16 },
    { CL_UNORM_INT_101010,      CL_UNSIGNED_INT8 },
    { CL_SIGNED_INT32,          CL_UNSIGNED_INT32 }
};

// The list of rejected channel's order and corresponding conversion
static const FormatConvertion RejectedOrder[] =
{
    { CL_A,                     CL_R  },
    { CL_RA,                    CL_RG },
    { CL_LUMINANCE,             CL_R  },
    { CL_INTENSITY,             CL_R },
    { CL_RGB,                   CL_RGBA },
    { CL_BGRA,                  CL_RGBA },
    { CL_ARGB,                  CL_RGBA },
    { CL_sRGB,                  CL_RGBA },
    { CL_sRGBx,                 CL_RGBA },
    { CL_sRGBA,                 CL_RGBA },
    { CL_sBGRA,                 CL_RGBA },
    { CL_DEPTH,                 CL_R }
};

const uint RejectedFormatDataTotal =
        sizeof(RejectedData) / sizeof(FormatConvertion);
const uint RejectedFormatChannelTotal =
        sizeof(RejectedOrder) / sizeof(FormatConvertion);

bool
KernelBlitManager::copyBufferToImage(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire,
    size_t      rowPitch,
    size_t      slicePitch) const
{
    amd::ScopedLock k(lockXferOps_);
    bool result = false;
    static const bool CopyRect = false;
    // Flush DMA for ASYNC copy
    static const bool FlushDMA = true;
    size_t imgRowPitch = size[0] * gpuMem(dstMemory).owner()->asImage()->getImageFormat().getElementSize();
    size_t imgSlicePitch = imgRowPitch * size[1];

    if (setup_.disableCopyBufferToImage_) {
        result = DmaBlitManager::copyBufferToImage(
            srcMemory, dstMemory, srcOrigin, dstOrigin, size,
            entire, rowPitch, slicePitch);
        synchronize();
        return result;
    }
    // Check if buffer is in system memory with direct access
    else if (gpuMem(srcMemory).isHostMemDirectAccess() &&
             (((rowPitch == 0) && (slicePitch == 0)) ||
              ((rowPitch == imgRowPitch) &&
               ((slicePitch == 0) || (slicePitch == imgSlicePitch))))) {
        // First attempt to do this all with DMA,
        // but there are restriciton with older hardware
        if (dev().settings().imageDMA_) {
            result = DmaBlitManager::copyBufferToImage(
                srcMemory, dstMemory, srcOrigin, dstOrigin, size,
                entire, rowPitch, slicePitch);
            if (result) {
                synchronize();
                return result;
            }
        }
    }

    if (!result) {
        result  = copyBufferToImageKernel(srcMemory,
            dstMemory, srcOrigin, dstOrigin, size, entire, rowPitch, slicePitch);
    }

    synchronize();

    return result;
}

void
CalcRowSlicePitches(
    cl_ulong* pitch, const cl_int* copySize,
    size_t rowPitch, size_t slicePitch, const Memory& mem)
{
    uint32_t memFmtSize = mem.owner()->asImage()->getImageFormat().getElementSize();
    bool img1Darray = (mem.owner()->getType() == CL_MEM_OBJECT_IMAGE1D_ARRAY) ? true : false;

    if (rowPitch == 0) {
        pitch[0] = copySize[0];
    }
    else {
        pitch[0] = rowPitch / memFmtSize;
    }
    if (slicePitch == 0) {
        pitch[1] = pitch[0] * (img1Darray ? 1 : copySize[1]);
    }
    else {
        pitch[1] = slicePitch / memFmtSize;
    }
    assert((pitch[0] <= pitch[1]) && "rowPitch must be <= slicePitch");

    if (img1Darray) {
        // For 1D array rowRitch = slicePitch
        pitch[0] = pitch[1];
    }
}

static void
setArgument(amd::Kernel* kernel, size_t index, size_t size, const void* value)
{
    kernel->parameters().set(index, size, value);
}

bool
KernelBlitManager::copyBufferToImageKernel(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire,
    size_t      rowPitch,
    size_t      slicePitch) const
{
    bool rejected = false;
    Memory* dstView = &gpuMem(dstMemory);
    bool    releaseView = false;
    bool    result = false;
    amd::Image::Format newFormat(gpuMem(dstMemory).owner()->asImage()->getImageFormat());

    // Find unsupported formats
    for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
        if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
            newFormat.image_channel_data_type = RejectedData[i].clNewType_;
            rejected = true;
            break;
        }
    }

    // Find unsupported channel's order
    for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
        if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
            newFormat.image_channel_order = RejectedOrder[i].clNewType_;
            rejected = true;
            break;
        }
    }

    // If the image format was rejected, then attempt to create a view
    if (rejected &&
        // todo ROC runtime has a problem with a view for this format
        (gpuMem(dstMemory).owner()->asImage()->
         getImageFormat().image_channel_data_type != CL_UNORM_INT_101010)) {
        dstView = createView(gpuMem(dstMemory), newFormat);
        if (dstView != NULL) {
            rejected = false;
            releaseView = true;
        }
    }

    // Fall into the host path if the image format was rejected
    if (rejected) {
        return DmaBlitManager::copyBufferToImage(
            srcMemory, dstMemory, srcOrigin, dstOrigin,
            size, entire, rowPitch, slicePitch);
    }

    // Use a common blit type with three dimensions by default
    uint    blitType = BlitCopyBufferToImage;
    size_t  dim = 0;
    size_t  globalWorkOffset[3] = { 0, 0, 0 };
    size_t  globalWorkSize[3];
    size_t  localWorkSize[3];

    // Program the kernels workload depending on the blit dimensions
    dim = 3;
    if (dstMemory.owner()->asImage()->getDims() == 1) {
        globalWorkSize[0] = amd::alignUp(size[0], 256);
        globalWorkSize[1] = amd::alignUp(size[1], 1);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = 256;
        localWorkSize[1] = localWorkSize[2] = 1;
    }
    else if (dstMemory.owner()->asImage()->getDims() == 2) {
        globalWorkSize[0] = amd::alignUp(size[0], 16);
        globalWorkSize[1] = amd::alignUp(size[1], 16);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = localWorkSize[1] = 16;
        localWorkSize[2] = 1;
    }
    else {
        globalWorkSize[0] = amd::alignUp(size[0], 8);
        globalWorkSize[1] = amd::alignUp(size[1], 8);
        globalWorkSize[2] = amd::alignUp(size[2], 4);
        localWorkSize[0] = localWorkSize[1] = 8;
        localWorkSize[2] = 4;
    }

    // Program kernels arguments for the blit operation
    cl_mem  mem = as_cl<amd::Memory>(srcMemory.owner());
    setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
    mem = as_cl<amd::Memory>(dstView->owner());
    setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);
    uint32_t memFmtSize = dstMemory.owner()->asImage()->getImageFormat().getElementSize();
    uint32_t components = dstMemory.owner()->asImage()->getImageFormat().getNumChannels();

    // 1 element granularity for writes by default
    cl_int  granularity = 1;
    if (memFmtSize == 2) {
        granularity = 2;
    }
    else if (memFmtSize >= 4) {
        granularity = 4;
    }
    CondLog(((srcOrigin[0] % granularity) != 0), "Unaligned offset in blit!");
    cl_ulong    srcOrg[4] = { srcOrigin[0] / granularity,
                              srcOrigin[1],
                              srcOrigin[2], 0 };
    setArgument(kernels_[blitType], 2, sizeof(srcOrg), srcOrg);

    cl_int  dstOrg[4] = { (cl_int)dstOrigin[0],
                          (cl_int)dstOrigin[1],
                          (cl_int)dstOrigin[2], 0 };
    cl_int  copySize[4] = { (cl_int)size[0],
                            (cl_int)size[1],
                            (cl_int)size[2], 0 };

    setArgument(kernels_[blitType], 3, sizeof(dstOrg), dstOrg);
    setArgument(kernels_[blitType], 4, sizeof(copySize), copySize);

    // Program memory format
    uint multiplier = memFmtSize / sizeof(uint32_t);
    multiplier = (multiplier == 0) ? 1 : multiplier;
    cl_uint format[4] = { components,
                          memFmtSize / components,
                          multiplier, 0 };
    setArgument(kernels_[blitType], 5, sizeof(format), format);

    // Program row and slice pitches
    cl_ulong  pitch[4] = { 0 };
    CalcRowSlicePitches(pitch, copySize, rowPitch, slicePitch, gpuMem(dstMemory));
    setArgument(kernels_[blitType], 6, sizeof(pitch), pitch);

    // Create ND range object for the kernel's execution
    amd::NDRangeContainer ndrange(dim,
        globalWorkOffset, globalWorkSize, localWorkSize);

    // Execute the blit
    address parameters = kernels_[blitType]->parameters().capture(dev());
    result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters, NULL);
    kernels_[blitType]->parameters().release(const_cast<address>(parameters), dev());

    if (releaseView) {
        // todo SRD programming could be changed to avoid a stall
        gpu().releaseGpuMemoryFence();
        dstView->owner()->release();
    }

    return result;
}

bool
KernelBlitManager::copyImageToBuffer(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire,
    size_t      rowPitch,
    size_t      slicePitch) const
{
    amd::ScopedLock k(lockXferOps_);
    bool result = false;
    static const bool CopyRect = false;
    // Flush DMA for ASYNC copy
    static const bool FlushDMA = true;
    size_t imgRowPitch = size[0] * gpuMem(srcMemory).owner()->asImage()->getImageFormat().getElementSize();
    size_t imgSlicePitch = imgRowPitch * size[1];

    if (setup_.disableCopyImageToBuffer_) {
        result = HostBlitManager::copyImageToBuffer(
            srcMemory, dstMemory, srcOrigin, dstOrigin,
            size, entire, rowPitch, slicePitch);
        synchronize();
        return result;
    }
    // Check if buffer is in system memory with direct access
    else if (gpuMem(dstMemory).isHostMemDirectAccess() &&
             (((rowPitch == 0) && (slicePitch == 0)) ||
              ((rowPitch == imgRowPitch) &&
                ((slicePitch == 0) || (slicePitch == imgSlicePitch))))) {
        // First attempt to do this all with DMA,
        // but there are restriciton with older hardware
        // If the dest buffer is external physical(SDI), copy two step as
        // single step SDMA is causing corruption and the cause is under investigation
        if (dev().settings().imageDMA_) {
            result = DmaBlitManager::copyImageToBuffer(
                srcMemory, dstMemory, srcOrigin, dstOrigin,
                size, entire, rowPitch, slicePitch);
            if (result) {
                synchronize();
                return result;
            }
        }
    }

    if (!result) {
        result = copyImageToBufferKernel(srcMemory,
            dstMemory, srcOrigin, dstOrigin, size, entire, rowPitch, slicePitch);
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::copyImageToBufferKernel(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire,
    size_t      rowPitch,
    size_t      slicePitch) const
{
    bool rejected = false;
    Memory* srcView = &gpuMem(srcMemory);
    bool    releaseView = false;
    bool    result = false;
    amd::Image::Format newFormat(gpuMem(srcMemory).owner()->asImage()->getImageFormat());

    // Find unsupported formats
    for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
        if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
            newFormat.image_channel_data_type = RejectedData[i].clNewType_;
            rejected = true;
            break;
        }
    }

    // Find unsupported channel's order
    for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
        if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
            newFormat.image_channel_order = RejectedOrder[i].clNewType_;
            rejected = true;
            break;
        }
    }

    // If the image format was rejected, then attempt to create a view
    if (rejected &&
        // todo ROC runtime has a problem with a view for this format
        (gpuMem(srcMemory).owner()->asImage()->
         getImageFormat().image_channel_data_type != CL_UNORM_INT_101010)) {
        srcView = createView(gpuMem(srcMemory), newFormat);
        if (srcView != NULL) {
            rejected = false;
            releaseView = true;
        }
    }

    // Fall into the host path if the image format was rejected
    if (rejected) {
        return DmaBlitManager::copyImageToBuffer(
            srcMemory, dstMemory, srcOrigin, dstOrigin,
            size, entire, rowPitch, slicePitch);
    }

    uint    blitType = BlitCopyImageToBuffer;
    size_t  dim = 0;
    size_t  globalWorkOffset[3] = { 0, 0, 0 };
    size_t  globalWorkSize[3];
    size_t  localWorkSize[3];

    // Program the kernels workload depending on the blit dimensions
    dim = 3;
    // Find the current blit type
    if (srcMemory.owner()->asImage()->getDims() == 1) {
        globalWorkSize[0] = amd::alignUp(size[0], 256);
        globalWorkSize[1] = amd::alignUp(size[1], 1);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = 256;
        localWorkSize[1] = localWorkSize[2] = 1;
    }
    else if (srcMemory.owner()->asImage()->getDims() == 2) {
        globalWorkSize[0] = amd::alignUp(size[0], 16);
        globalWorkSize[1] = amd::alignUp(size[1], 16);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = localWorkSize[1] = 16;
        localWorkSize[2] = 1;
    }
    else {
        globalWorkSize[0] = amd::alignUp(size[0], 8);
        globalWorkSize[1] = amd::alignUp(size[1], 8);
        globalWorkSize[2] = amd::alignUp(size[2], 4);
        localWorkSize[0] = localWorkSize[1] = 8;
        localWorkSize[2] = 4;
    }

    // Program kernels arguments for the blit operation
    cl_mem  mem = as_cl<amd::Memory>(srcView->owner());
    setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
    mem = as_cl<amd::Memory>(dstMemory.owner());
    setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);

    // Update extra paramters for USHORT and UBYTE pointers.
    // Only then compiler can optimize the kernel to use
    // UAV Raw for other writes
    setArgument(kernels_[blitType], 2, sizeof(cl_mem), &mem);
    setArgument(kernels_[blitType], 3, sizeof(cl_mem), &mem);

    cl_int  srcOrg[4] = { (cl_int)srcOrigin[0],
                          (cl_int)srcOrigin[1],
                          (cl_int)srcOrigin[2], 0 };
    cl_int  copySize[4] = { (cl_int)size[0],
                            (cl_int)size[1],
                            (cl_int)size[2], 0 };
    setArgument(kernels_[blitType], 4, sizeof(srcOrg), srcOrg);
    uint32_t memFmtSize = srcMemory.owner()->asImage()->getImageFormat().getElementSize();
    uint32_t components = srcMemory.owner()->asImage()->getImageFormat().getNumChannels();

    // 1 element granularity for writes by default
    cl_int  granularity = 1;
    if (memFmtSize == 2) {
        granularity = 2;
    }
    else if (memFmtSize >= 4) {
        granularity = 4;
    }
    CondLog(((dstOrigin[0] % granularity) != 0), "Unaligned offset in blit!");
    cl_ulong    dstOrg[4] = { dstOrigin[0] / granularity,
                              dstOrigin[1],
                              dstOrigin[2], 0 };
    setArgument(kernels_[blitType], 5, sizeof(dstOrg), dstOrg);
    setArgument(kernels_[blitType], 6, sizeof(copySize), copySize);

    // Program memory format
    uint multiplier = memFmtSize / sizeof(uint32_t);
    multiplier = (multiplier == 0) ? 1 : multiplier;
    cl_uint format[4] = { components,
                          memFmtSize / components,
                          multiplier, 0 };
    setArgument(kernels_[blitType], 7, sizeof(format), format);

    // Program row and slice pitches
    cl_ulong    pitch[4] = { 0 };
    CalcRowSlicePitches(pitch, copySize, rowPitch, slicePitch, gpuMem(srcMemory));
    setArgument(kernels_[blitType], 8, sizeof(pitch), pitch);

    // Create ND range object for the kernel's execution
    amd::NDRangeContainer ndrange(dim,
        globalWorkOffset, globalWorkSize, localWorkSize);

    // Execute the blit
    address parameters = kernels_[blitType]->parameters().capture(dev());
    result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters, NULL);
    kernels_[blitType]->parameters().release(const_cast<address>(parameters), dev());
    if (releaseView) {
        // todo SRD programming could be changed to avoid a stall
        gpu().releaseGpuMemoryFence();
        srcView->owner()->release();
    }

    return result;
}

bool
KernelBlitManager::copyImage(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& size,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool rejected = false;
    Memory* srcView = &gpuMem(srcMemory);
    Memory* dstView = &gpuMem(dstMemory);
    bool    releaseView = false;
    bool    result = false;
    amd::Image::Format newFormat(gpuMem(srcMemory).owner()->asImage()->getImageFormat());

    // Find unsupported formats
    for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
        if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
            newFormat.image_channel_data_type = RejectedData[i].clNewType_;
            rejected = true;
            break;
        }
    }

    // Search for the rejected channel's order only if the format was rejected
    // Note: Image blit is independent from the channel order
    if (rejected) {
        for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
            if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
                newFormat.image_channel_order = RejectedOrder[i].clNewType_;
                rejected = true;
                break;
            }
        }
    }

    // Attempt to create a view if the format was rejected
    if (rejected) {
        srcView = createView(gpuMem(srcMemory), newFormat);
        if (srcView != NULL) {
            dstView = createView(gpuMem(dstMemory), newFormat);
            if (dstView != NULL) {
                rejected = false;
                releaseView = true;
            }
            else {
                delete srcView;
            }
        }
    }

    // Fall into the host path for the entire 2D copy or
    // if the image format was rejected
    if (rejected) {
        result = HostBlitManager::copyImage(srcMemory, dstMemory,
            srcOrigin, dstOrigin, size, entire);
        synchronize();
        return result;
    }

    uint    blitType = BlitCopyImage;
    size_t  dim = 0;
    size_t  globalWorkOffset[3] = { 0, 0, 0 };
    size_t  globalWorkSize[3];
    size_t  localWorkSize[3];

    // Program the kernels workload depending on the blit dimensions
    dim = 3;
    // Find the current blit type
    if ((srcMemory.owner()->asImage()->getDims() == 1) ||
        (dstMemory.owner()->asImage()->getDims() == 1)) {
        globalWorkSize[0] = amd::alignUp(size[0], 256);
        globalWorkSize[1] = amd::alignUp(size[1], 1);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = 256;
        localWorkSize[1] = localWorkSize[2] = 1;
    }
    else if ((srcMemory.owner()->asImage()->getDims() == 2) ||
             (dstMemory.owner()->asImage()->getDims() == 2)) {
        globalWorkSize[0] = amd::alignUp(size[0], 16);
        globalWorkSize[1] = amd::alignUp(size[1], 16);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = localWorkSize[1] = 16;
        localWorkSize[2] = 1;
    }
    else {
        globalWorkSize[0] = amd::alignUp(size[0], 8);
        globalWorkSize[1] = amd::alignUp(size[1], 8);
        globalWorkSize[2] = amd::alignUp(size[2], 4);
        localWorkSize[0] = localWorkSize[1] = 8;
        localWorkSize[2] = 4;
    }

    // The current OpenCL spec allows "copy images from a 1D image
    // array object to a 1D image array object" only.
    if ((gpuMem(srcMemory).owner()->getType() == CL_MEM_OBJECT_IMAGE1D_ARRAY) ||
        (gpuMem(dstMemory).owner()->getType() == CL_MEM_OBJECT_IMAGE1D_ARRAY)) {
        blitType = BlitCopyImage1DA;
    }

    // Program kernels arguments for the blit operation
    cl_mem  mem = as_cl<amd::Memory>(srcView->owner());
    setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
    mem = as_cl<amd::Memory>(dstView->owner());
    setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);

    // Program source origin
    cl_int  srcOrg[4] = { (cl_int)srcOrigin[0],
                          (cl_int)srcOrigin[1],
                          (cl_int)srcOrigin[2], 0 };
    setArgument(kernels_[blitType], 2, sizeof(srcOrg), srcOrg);

    // Program destinaiton origin
    cl_int  dstOrg[4] = { (cl_int)dstOrigin[0],
                          (cl_int)dstOrigin[1],
                          (cl_int)dstOrigin[2], 0 };
    setArgument(kernels_[blitType], 3, sizeof(dstOrg), dstOrg);

    cl_int  copySize[4] = { (cl_int)size[0],
                            (cl_int)size[1],
                            (cl_int)size[2], 0 };
    setArgument(kernels_[blitType], 4, sizeof(copySize), copySize);

    // Create ND range object for the kernel's execution
    amd::NDRangeContainer ndrange(dim,
        globalWorkOffset, globalWorkSize, localWorkSize);

    // Execute the blit
    address parameters = kernels_[blitType]->parameters().capture(dev());
    result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters, NULL);
    kernels_[blitType]->parameters().release(const_cast<address>(parameters), dev());
    if (releaseView) {
        // todo SRD programming could be changed to avoid a stall
        gpu().releaseGpuMemoryFence();
        srcView->owner()->release();
        dstView->owner()->release();
    }

    synchronize();

    return result;
}

void
FindPinSize(
    size_t& pinSize, const amd::Coord3D& size,
    size_t& rowPitch, size_t& slicePitch, const Memory& mem)
{
    pinSize = size[0] * mem.owner()->asImage()->getImageFormat().getElementSize();
    if ((rowPitch == 0) || (rowPitch == pinSize)) {
        rowPitch = 0;
    }
    else {
        pinSize = rowPitch;
    }

    // Calculate the pin size, which should be equal to the copy size
    for (uint i = 1; i < mem.owner()->asImage()->getDims(); ++i) {
        pinSize *= size[i];
        if (i == 1) {
            if ((slicePitch == 0) || (slicePitch == pinSize)) {
                slicePitch = 0;
            }
            else {
                if (mem.owner()->getType() != CL_MEM_OBJECT_IMAGE1D_ARRAY) {
                    pinSize = slicePitch;
                }
                else {
                    pinSize = slicePitch * size[i];
                }
            }
        }
    }
}

bool
KernelBlitManager::readImage(
    device::Memory&     srcMemory,
    void*       dstHost,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    size_t      rowPitch,
    size_t      slicePitch,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool result = false;

    // Use host copy if memory has direct access
    if (setup_.disableReadImage_ ||
        (gpuMem(srcMemory).isHostMemDirectAccess())) {
        result = HostBlitManager::readImage(srcMemory, dstHost,
            origin, size, rowPitch, slicePitch, entire);
        synchronize();
        return result;
    }
    else {
        size_t  pinSize;
        FindPinSize(pinSize, size, rowPitch, slicePitch, gpuMem(srcMemory));

        size_t  partial;
        amd::Memory* amdMemory = pinHostMemory(dstHost, pinSize, partial);

        if (amdMemory == NULL) {
            // Force SW copy
            result = HostBlitManager::readImage(srcMemory, dstHost,
                origin, size, rowPitch, slicePitch, entire);
            synchronize();
            return result;
        }

        // Readjust destination offset
        const amd::Coord3D dstOrigin(partial);

        // Get device memory for this virtual device
        Memory* dstMemory = dev().getRocMemory(amdMemory);

        // Copy image to buffer
        result = copyImageToBuffer(srcMemory, *dstMemory,
            origin, dstOrigin, size, entire, rowPitch, slicePitch);

        // Add pinned memory for a later release
        gpu().addPinnedMem(amdMemory);
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::writeImage(
    const void* srcHost,
    device::Memory&     dstMemory,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    size_t      rowPitch,
    size_t      slicePitch,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool result = false;

    // Use host copy if memory has direct access
    if (setup_.disableWriteImage_|| gpuMem(dstMemory).isHostMemDirectAccess()) {
        result = HostBlitManager::writeImage(
            srcHost, dstMemory, origin, size, rowPitch, slicePitch, entire);
        synchronize();
        return result;
    }
    else {
        size_t  pinSize;
        FindPinSize(pinSize, size, rowPitch, slicePitch, gpuMem(dstMemory));

        size_t  partial;
        amd::Memory* amdMemory = pinHostMemory(srcHost, pinSize, partial);

        if (amdMemory == NULL) {
            // Force SW copy
            result = HostBlitManager::writeImage(
                srcHost, dstMemory, origin, size, rowPitch, slicePitch, entire);
            synchronize();
            return result;
        }

        // Readjust destination offset
        const amd::Coord3D srcOrigin(partial);

        // Get device memory for this virtual device
        Memory* srcMemory = dev().getRocMemory(amdMemory);

        // Copy image to buffer
        result = copyBufferToImage(*srcMemory, dstMemory,
            srcOrigin, origin, size, entire, rowPitch, slicePitch);

        // Add pinned memory for a later release
        gpu().addPinnedMem(amdMemory);
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::copyBufferRect(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::BufferRect&  srcRectIn,
    const amd::BufferRect&  dstRectIn,
    const amd::Coord3D& sizeIn,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool    result = false;
    bool    rejected = false;

    // Fall into the ROC path for rejected transfers
    if (setup_.disableCopyBufferRect_ ||
        gpuMem(srcMemory).isHostMemDirectAccess() || gpuMem(dstMemory).isHostMemDirectAccess()) {
        result = DmaBlitManager::copyBufferRect(srcMemory, dstMemory,
            srcRectIn, dstRectIn, sizeIn, entire);

        if (result) {
            synchronize();
            return result;
        }
    }

    uint    blitType = BlitCopyBufferRect;
    size_t  dim = 3;
    size_t  globalWorkOffset[3] = { 0, 0, 0 };
    size_t  globalWorkSize[3];
    size_t  localWorkSize[3];

    const static uint CopyRectAlignment[3] = { 16, 4, 1 };

    bool aligned;
    uint i;
    for (i = 0; i < sizeof(CopyRectAlignment) / sizeof(uint); i++) {
        // Check source alignments
        aligned = ((srcRectIn.rowPitch_ % CopyRectAlignment[i]) == 0);
        aligned &= ((srcRectIn.slicePitch_ % CopyRectAlignment[i]) == 0);
        aligned &= ((srcRectIn.start_ % CopyRectAlignment[i]) == 0);

        // Check destination alignments
        aligned &= ((dstRectIn.rowPitch_ % CopyRectAlignment[i]) == 0);
        aligned &= ((dstRectIn.slicePitch_ % CopyRectAlignment[i]) == 0);
        aligned &= ((dstRectIn.start_ % CopyRectAlignment[i]) == 0);

        // Check copy size alignment in the first dimension
        aligned &= ((sizeIn[0] % CopyRectAlignment[i]) == 0);

        if (aligned) {
            if (CopyRectAlignment[i] != 1) {
                blitType = BlitCopyBufferRectAligned;
            }
            break;
        }
    }

    amd::BufferRect srcRect;
    amd::BufferRect dstRect;
    amd::Coord3D    size(sizeIn[0], sizeIn[1], sizeIn[2]);

    srcRect.rowPitch_      = srcRectIn.rowPitch_ / CopyRectAlignment[i];
    srcRect.slicePitch_    = srcRectIn.slicePitch_ / CopyRectAlignment[i];
    srcRect.start_         = srcRectIn.start_ / CopyRectAlignment[i];
    srcRect.end_           = srcRectIn.end_ / CopyRectAlignment[i];

    dstRect.rowPitch_      = dstRectIn.rowPitch_ / CopyRectAlignment[i];
    dstRect.slicePitch_    = dstRectIn.slicePitch_ / CopyRectAlignment[i];
    dstRect.start_         = dstRectIn.start_ / CopyRectAlignment[i];
    dstRect.end_           = dstRectIn.end_ / CopyRectAlignment[i];

    size.c[0] /= CopyRectAlignment[i];

    // Program the kernel's workload depending on the transfer dimensions
    if ((size[1] == 1) && (size[2] == 1)) {
        globalWorkSize[0] = amd::alignUp(size[0], 256);
        globalWorkSize[1] = 1;
        globalWorkSize[2] = 1;
        localWorkSize[0] = 256;
        localWorkSize[1] = 1;
        localWorkSize[2] = 1;
    }
    else if (size[2] == 1) {
        globalWorkSize[0] = amd::alignUp(size[0], 16);
        globalWorkSize[1] = amd::alignUp(size[1], 16);
        globalWorkSize[2] = 1;
        localWorkSize[0] = localWorkSize[1] = 16;
        localWorkSize[2] = 1;
    }
    else {
        globalWorkSize[0] = amd::alignUp(size[0], 8);
        globalWorkSize[1] = amd::alignUp(size[1], 8);
        globalWorkSize[2] = amd::alignUp(size[2], 4);
        localWorkSize[0] = localWorkSize[1] = 8;
        localWorkSize[2] = 4;
    }


    // Program kernels arguments for the blit operation
    cl_mem  mem = as_cl<amd::Memory>(srcMemory.owner());
    setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
    mem = as_cl<amd::Memory>(dstMemory.owner());
    setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);
    cl_ulong    src[4] = { srcRect.rowPitch_,
                           srcRect.slicePitch_,
                           srcRect.start_, 0 };
    setArgument(kernels_[blitType], 2, sizeof(src), src);
    cl_ulong    dst[4] = {  dstRect.rowPitch_,
                            dstRect.slicePitch_,
                            dstRect.start_, 0 };
    setArgument(kernels_[blitType], 3, sizeof(dst), dst);
    cl_ulong    copySize[4] = { size[0], size[1], size[2], CopyRectAlignment[i] };
    setArgument(kernels_[blitType], 4, sizeof(copySize), copySize);

    // Create ND range object for the kernel's execution
    amd::NDRangeContainer ndrange(dim,
        globalWorkOffset, globalWorkSize, localWorkSize);

    // Execute the blit
    address parameters = kernels_[blitType]->parameters().capture(dev());
    result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters, NULL);
    kernels_[blitType]->parameters().release(const_cast<address>(parameters), dev());

    synchronize();

    return result;
}

bool
KernelBlitManager::readBuffer(
    device::Memory&     srcMemory,
    void*       dstHost,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool    result = false;
    // Use host copy if memory has direct access
    if (setup_.disableReadBuffer_ ||
        (gpuMem(srcMemory).isHostMemDirectAccess())) {
        result = HostBlitManager::readBuffer(
            srcMemory, dstHost, origin, size, entire);
        synchronize();
        return result;
    }
    else {
        size_t  pinSize =  size[0];
        // Check if a pinned transfer can be executed with a single pin
        if ((pinSize <= dev().settings().pinnedXferSize_) &&
            (pinSize > MinSizeForPinnedTransfer)) {
            size_t  partial;
            amd::Memory* amdMemory = pinHostMemory(dstHost, pinSize, partial);

            if (amdMemory == NULL) {
                // Force SW copy
                result = HostBlitManager::readBuffer(
                    srcMemory, dstHost, origin, size, entire);
                synchronize();
                return result;
            }

            // Readjust host mem offset
            amd::Coord3D    dstOrigin(partial);

            // Get device memory for this virtual device
            Memory* dstMemory = dev().getRocMemory(amdMemory);

            // Copy image to buffer
            result = copyBuffer(srcMemory, *dstMemory,
                origin, dstOrigin, size, entire);

            // Add pinned memory for a later release
            gpu().addPinnedMem(amdMemory);
        }
        else {
            result = DmaBlitManager::readBuffer(
                srcMemory, dstHost, origin, size, entire);
        }
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::readBufferRect(
    device::Memory&     srcMemory,
    void*       dstHost,
    const amd::BufferRect&   bufRect,
    const amd::BufferRect&   hostRect,
    const amd::Coord3D& size,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool    result = false;

    // Use host copy if memory has direct access
    if (setup_.disableReadBufferRect_ || gpuMem(srcMemory).isHostMemDirectAccess()) {
        result = HostBlitManager::readBufferRect(
            srcMemory, dstHost, bufRect, hostRect, size, entire);
        synchronize();
        return result;
    }
    else {
        size_t  pinSize = hostRect.start_ + hostRect.end_;
        size_t  partial;
        amd::Memory* amdMemory = pinHostMemory(dstHost, pinSize, partial);

        if (amdMemory == NULL) {
            // Force SW copy
            result = HostBlitManager::readBufferRect(
                srcMemory, dstHost, bufRect, hostRect, size, entire);
            synchronize();
            return result;
        }

        // Readjust host mem offset
        amd::BufferRect rect;
        rect.rowPitch_      = hostRect.rowPitch_;
        rect.slicePitch_    = hostRect.slicePitch_;
        rect.start_         = hostRect.start_ + partial;
        rect.end_           = hostRect.end_;

        // Get device memory for this virtual device
        Memory* dstMemory = dev().getRocMemory(amdMemory);

        // Copy image to buffer
        result = copyBufferRect(srcMemory, *dstMemory,
            bufRect, rect, size, entire);

        // Add pinned memory for a later release
        gpu().addPinnedMem(amdMemory);
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::writeBuffer(
    const void* srcHost,
    device::Memory&     dstMemory,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool    result = false;

    // Use host copy if memory has direct access
    if (setup_.disableWriteBuffer_ || gpuMem(dstMemory).isHostMemDirectAccess()) {
        result = HostBlitManager::writeBuffer(
            srcHost, dstMemory, origin, size, entire);
        synchronize();
        return result;
    }
    else {
        size_t  pinSize = size[0];

        // Check if a pinned transfer can be executed with a single pin
        if ((pinSize <= dev().settings().pinnedXferSize_) &&
            (pinSize > MinSizeForPinnedTransfer)) {
            size_t  partial;
            amd::Memory* amdMemory = pinHostMemory(srcHost, pinSize, partial);

            if (amdMemory == NULL) {
                // Force SW copy
                result = HostBlitManager::writeBuffer(
                    srcHost, dstMemory, origin, size, entire);
                synchronize();
                return result;
            }

            // Readjust destination offset
            const amd::Coord3D srcOrigin(partial);

            // Get device memory for this virtual device
            Memory* srcMemory = dev().getRocMemory(amdMemory);

            // Copy buffer rect
            result = copyBuffer(*srcMemory, dstMemory,
                srcOrigin, origin, size, entire);

            // Add pinned memory for a later release
            gpu().addPinnedMem(amdMemory);
        }
        else {
            result =  DmaBlitManager::writeBuffer(
                srcHost, dstMemory, origin, size, entire);
        }
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::writeBufferRect(
    const void* srcHost,
    device::Memory&     dstMemory,
    const amd::BufferRect&   hostRect,
    const amd::BufferRect&   bufRect,
    const amd::Coord3D& size,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool result = false;

    // Use host copy if memory has direct access
    if (setup_.disableWriteBufferRect_ ||
        gpuMem(dstMemory).isHostMemDirectAccess()) {
        result = HostBlitManager::writeBufferRect(
            srcHost, dstMemory, hostRect, bufRect, size, entire);
        synchronize();
        return result;
    }
    else {
        size_t  pinSize = hostRect.start_ + hostRect.end_;
        size_t  partial;
        amd::Memory* amdMemory = pinHostMemory(srcHost, pinSize, partial);

        if (amdMemory == NULL) {
            // Force DMA copy with staging
            result = DmaBlitManager::writeBufferRect(
                srcHost, dstMemory, hostRect, bufRect, size, entire);
            synchronize();
            return result;
        }

        // Readjust destination offset
        const amd::Coord3D srcOrigin(partial);

        // Get device memory for this virtual device
        Memory* srcMemory = dev().getRocMemory(amdMemory);

        // Readjust host mem offset
        amd::BufferRect rect;
        rect.rowPitch_      = hostRect.rowPitch_;
        rect.slicePitch_    = hostRect.slicePitch_;
        rect.start_         = hostRect.start_ + partial;
        rect.end_           = hostRect.end_;

        // Copy buffer rect
        result = copyBufferRect(*srcMemory, dstMemory,
            rect, bufRect, size, entire);

       // Add pinned memory for a later release
       gpu().addPinnedMem(amdMemory);
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::fillBuffer(
    device::Memory&     memory,
    const void* pattern,
    size_t      patternSize,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    bool        entire
    ) const
{
    amd::ScopedLock k(lockXferOps_);
    bool result = false;

    // Use host fill if memory has direct access
    if (setup_.disableFillBuffer_ ||
        gpuMem(memory).isHostMemDirectAccess()) {
        result = HostBlitManager::fillBuffer(
            memory, pattern, patternSize, origin, size, entire);
        synchronize();
        return result;
    }
    else {
        uint    fillType = FillBuffer;
        size_t  globalWorkOffset[3] = { 0, 0, 0 };
        cl_ulong  fillSize = size[0] / patternSize;
        size_t  globalWorkSize = amd::alignUp(fillSize, 256);
        size_t  localWorkSize = 256;
        bool    dwordAligned =
            ((patternSize % sizeof(uint32_t)) == 0) ? true : false;

        // Program kernels arguments for the fill operation
        cl_mem  mem = as_cl<amd::Memory>(memory.owner());
        if (dwordAligned) {
            setArgument(kernels_[fillType], 0, sizeof(cl_mem), NULL);
            setArgument(kernels_[fillType], 1, sizeof(cl_mem), &mem);
        }
        else {
            setArgument(kernels_[fillType], 0, sizeof(cl_mem), &mem);
            setArgument(kernels_[fillType], 1, sizeof(cl_mem), NULL);
        }
        Memory* gpuCB = dev().getRocMemory(constantBuffer_);
        if (gpuCB == NULL) {
            return false;
        }
        void* constBuf = constantBuffer_->getHostMem();
        memcpy(constBuf, pattern, patternSize);

        mem = as_cl<amd::Memory>(gpuCB->owner());
        setArgument(kernels_[fillType], 2, sizeof(cl_mem), &mem);
        cl_ulong    offset = origin[0];
        if (dwordAligned) {
            patternSize /= sizeof(uint32_t);
            offset /= sizeof(uint32_t);
        }
        setArgument(kernels_[fillType], 3, sizeof(cl_uint), &patternSize);
        setArgument(kernels_[fillType], 4, sizeof(offset), &offset);
        setArgument(kernels_[fillType], 5, sizeof(fillSize), &fillSize);

        // Create ND range object for the kernel's execution
        amd::NDRangeContainer ndrange(1,
            globalWorkOffset, &globalWorkSize, &localWorkSize);

        // Execute the blit
        address parameters = kernels_[fillType]->parameters().capture(dev());
        result = gpu().submitKernelInternal(ndrange, *kernels_[fillType], parameters, NULL);
        kernels_[fillType]->parameters().release(const_cast<address>(parameters), dev());
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::copyBuffer(
    device::Memory&     srcMemory,
    device::Memory&     dstMemory,
    const amd::Coord3D& srcOrigin,
    const amd::Coord3D& dstOrigin,
    const amd::Coord3D& sizeIn,
    bool        entire) const
{
    amd::ScopedLock k(lockXferOps_);
    bool    result = false;

    if (!gpuMem(srcMemory).isHostMemDirectAccess() &&
        !gpuMem(dstMemory).isHostMemDirectAccess()) {
        uint    blitType = BlitCopyBuffer;
        size_t  dim = 1;
        size_t  globalWorkOffset[3] = { 0, 0, 0 };
        size_t  globalWorkSize = 0;
        size_t  localWorkSize = 0;

        // todo LC shows much better performance with the unaligned version
        const static uint CopyBuffAlignment[3] = { 1/*16*/, 1/*4*/, 1 };
        amd::Coord3D    size(sizeIn[0], sizeIn[1], sizeIn[2]);

        bool aligned = false;
        uint i;
        for (i = 0; i < sizeof(CopyBuffAlignment) / sizeof(uint); i++) {
            // Check source alignments
            aligned = ((srcOrigin[0] % CopyBuffAlignment[i]) == 0);
            // Check destination alignments
            aligned &= ((dstOrigin[0] % CopyBuffAlignment[i]) == 0);
            // Check copy size alignment in the first dimension
            aligned &= ((sizeIn[0] % CopyBuffAlignment[i]) == 0);

            if (aligned) {
                if (CopyBuffAlignment[i] != 1) {
                    blitType = BlitCopyBufferAligned;
                }
                break;
            }
        }

        cl_uint remain;
        if (blitType == BlitCopyBufferAligned) {
            size.c[0] /= CopyBuffAlignment[i];
        }
        else {
            remain = size[0] % 4;
            size.c[0] /= 4;
            size.c[0] += 1;
        }

        // Program the dispatch dimensions
        localWorkSize = 256;
        globalWorkSize = amd::alignUp(size[0] , 256);

        // Program kernels arguments for the blit operation
        cl_mem mem = as_cl<amd::Memory>(srcMemory.owner());
        setArgument(kernels_[blitType], 0, sizeof(cl_mem), &mem);
        mem = as_cl<amd::Memory>(dstMemory.owner());
        setArgument(kernels_[blitType], 1, sizeof(cl_mem), &mem);
        // Program source origin
        cl_ulong    srcOffset = srcOrigin[0] / CopyBuffAlignment[i];;
        setArgument(kernels_[blitType], 2, sizeof(srcOffset), &srcOffset);

        // Program destinaiton origin
        cl_ulong    dstOffset = dstOrigin[0] / CopyBuffAlignment[i];;
        setArgument(kernels_[blitType], 3, sizeof(dstOffset), &dstOffset);

        cl_ulong    copySize = size[0];
        setArgument(kernels_[blitType], 4, sizeof(copySize), &copySize);

        if (blitType == BlitCopyBufferAligned) {
            cl_int  alignment = CopyBuffAlignment[i];
            setArgument(kernels_[blitType], 5, sizeof(alignment), &alignment);
        }
        else {
            setArgument(kernels_[blitType], 5, sizeof(remain), &remain);
        }

        // Create ND range object for the kernel's execution
        amd::NDRangeContainer ndrange(1,
            globalWorkOffset, &globalWorkSize, &localWorkSize);

        // Execute the blit
        address parameters = kernels_[blitType]->parameters().capture(dev());
        result = gpu().submitKernelInternal(ndrange, *kernels_[blitType], parameters, NULL);
        kernels_[blitType]->parameters().release(const_cast<address>(parameters), dev());
    }
    else {
        result = DmaBlitManager::copyBuffer(
            srcMemory, dstMemory, srcOrigin, dstOrigin, sizeIn, entire);
    }

    synchronize();

    return result;
}

bool
KernelBlitManager::fillImage(
    device::Memory&     memory,
    const void* pattern,
    const amd::Coord3D& origin,
    const amd::Coord3D& size,
    bool        entire
    ) const
{
    amd::ScopedLock k(lockXferOps_);
    bool    result = false;

    // Use host fill if memory has direct access
    if (setup_.disableFillImage_ ||
        gpuMem(memory).isHostMemDirectAccess()) {
        result = HostBlitManager::fillImage(
            memory, pattern, origin, size, entire);
        synchronize();
        return result;
    }

    uint    fillType;
    size_t  dim = 0;
    size_t  globalWorkOffset[3] = { 0, 0, 0 };
    size_t  globalWorkSize[3];
    size_t  localWorkSize[3];
    Memory* memView = &gpuMem(memory);
    amd::Image::Format newFormat(gpuMem(memory).owner()->asImage()->getImageFormat());

    // Program the kernels workload depending on the fill dimensions
    fillType = FillImage;
    dim = 3;

    void *newpattern = const_cast<void *>(pattern);
    cl_uint4  iFillColor;

    bool rejected = false;
    bool    releaseView = false;

    // For depth, we need to create a view
    if (newFormat.image_channel_order == CL_sRGBA) {
        // Find unsupported data type
        for (uint i = 0; i < RejectedFormatDataTotal; ++i) {
            if (RejectedData[i].clOldType_ == newFormat.image_channel_data_type) {
                newFormat.image_channel_data_type = RejectedData[i].clNewType_;
                rejected = true;
                break;
            }
        }

        if (newFormat.image_channel_order == CL_sRGBA) {
            // Converting a linear RGB floating-point color value to a 8-bit unsigned integer sRGB value because hw is not support write_imagef for sRGB.
            float *fColor = static_cast<float *>(newpattern);
            iFillColor.s[0] = sRGBmap(fColor[0]);
            iFillColor.s[1] = sRGBmap(fColor[1]);
            iFillColor.s[2] = sRGBmap(fColor[2]);
            iFillColor.s[3] = (cl_uint)(fColor[3]*255.0f);
            newpattern = static_cast<void*>(&iFillColor);
            for (uint i = 0; i < RejectedFormatChannelTotal; ++i) {
                if (RejectedOrder[i].clOldType_ == newFormat.image_channel_order) {
                    newFormat.image_channel_order = RejectedOrder[i].clNewType_;
                    rejected = true;
                    break;
                }
            }
        }
    }
    // If the image format was rejected, then attempt to create a view
    if (rejected) {
        memView = createView(gpuMem(memory), newFormat);
        if (memView != NULL) {
            rejected = false;
            releaseView = true;
        }
    }

    if (rejected) {
        return DmaBlitManager::fillImage(memory, pattern, origin, size, entire);
    }

    // Perform workload split to allow multiple operations in a single thread
    globalWorkSize[0] = (size[0] + TransferSplitSize - 1) / TransferSplitSize;
    // Find the current blit type
    if (memView->owner()->asImage()->getDims() == 1) {
        globalWorkSize[0] = amd::alignUp(globalWorkSize[0], 256);
        globalWorkSize[1] = amd::alignUp(size[1], 1);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = 256;
        localWorkSize[1] = localWorkSize[2] = 1;
    }
    else if (memView->owner()->asImage()->getDims()== 2) {
        globalWorkSize[0] = amd::alignUp(globalWorkSize[0], 16);
        globalWorkSize[1] = amd::alignUp(size[1], 16);
        globalWorkSize[2] = amd::alignUp(size[2], 1);
        localWorkSize[0] = localWorkSize[1] = 16;
        localWorkSize[2] = 1;
    }
    else {
        globalWorkSize[0] = amd::alignUp(globalWorkSize[0], 8);
        globalWorkSize[1] = amd::alignUp(size[1], 8);
        globalWorkSize[2] = amd::alignUp(size[2], 4);
        localWorkSize[0] = localWorkSize[1] = 8;
        localWorkSize[2] = 4;
    }

    // Program kernels arguments for the blit operation
    cl_mem  mem = as_cl<amd::Memory>(memView->owner());
    setArgument(kernels_[fillType], 0, sizeof(cl_mem), &mem);
    setArgument(kernels_[fillType], 1, sizeof(cl_float4), newpattern);
    setArgument(kernels_[fillType], 2, sizeof(cl_int4), newpattern);
    setArgument(kernels_[fillType], 3, sizeof(cl_uint4), newpattern);

    cl_int fillOrigin[4] = { (cl_int)origin[0],
                             (cl_int)origin[1],
                             (cl_int)origin[2], 0 };
    cl_int   fillSize[4] = { (cl_int)size[0],
                             (cl_int)size[1],
                             (cl_int)size[2], 0 };
    setArgument(kernels_[fillType], 4, sizeof(fillOrigin), fillOrigin);
    setArgument(kernels_[fillType], 5, sizeof(fillSize), fillSize);

    // Find the type of image
    uint32_t    type = 0;
    switch (newFormat.image_channel_data_type) {
        case CL_SNORM_INT8:
        case CL_SNORM_INT16:
        case CL_UNORM_INT8:
        case CL_UNORM_INT16:
        case CL_UNORM_SHORT_565:
        case CL_UNORM_SHORT_555:
        case CL_UNORM_INT_101010:
        case CL_HALF_FLOAT:
        case CL_FLOAT:
            type = 0;
            break;
        case CL_SIGNED_INT8:
        case CL_SIGNED_INT16:
        case CL_SIGNED_INT32:
            type = 1;
            break;
        case CL_UNSIGNED_INT8:
        case CL_UNSIGNED_INT16:
        case CL_UNSIGNED_INT32:
            type = 2;
            break;
    }
    setArgument(kernels_[fillType], 6, sizeof(type), &type);

    // Create ND range object for the kernel's execution
    amd::NDRangeContainer ndrange(dim,
        globalWorkOffset, globalWorkSize, localWorkSize);

    // Execute the blit
    address parameters = kernels_[fillType]->parameters().capture(dev());
    result = gpu().submitKernelInternal(ndrange, *kernels_[fillType], parameters, NULL);
    kernels_[fillType]->parameters().release(const_cast<address>(parameters), dev());
    if (releaseView) {
        // todo SRD programming could be changed to avoid a stall
        gpu().releaseGpuMemoryFence();
        memView->owner()->release();
    }

    synchronize();

    return result;
}

amd::Memory*
DmaBlitManager::pinHostMemory(
    const void* hostMem,
    size_t      pinSize,
    size_t&     partial) const
{
    size_t  pinAllocSize;
    const static bool SysMem = true;
    amd::Memory* amdMemory;

    // Allign offset to 4K boundary (Vista/Win7 limitation)
    char* tmpHost = const_cast<char*>(
        amd::alignDown(reinterpret_cast<const char*>(hostMem),
        PinnedMemoryAlignment));

    // Find the partial size for unaligned copy
    partial = reinterpret_cast<const char*>(hostMem) - tmpHost;

    // Recalculate pin memory size
    pinAllocSize = amd::alignUp(pinSize + partial, PinnedMemoryAlignment);

    amdMemory = gpu().findPinnedMem(tmpHost, pinAllocSize);

    if (NULL != amdMemory) {
        return amdMemory;
    }

    amdMemory = new(*context_)
        amd::Buffer(*context_, CL_MEM_USE_HOST_PTR, pinAllocSize);

    if ((amdMemory != NULL) && !amdMemory->create(tmpHost, SysMem)) {
        amdMemory->release();
        return NULL;
    }

    // Get device memory for this virtual device
    // @note: This will force real memory pinning
    amdMemory->setVirtualDevice(&gpu());
    Memory* srcMemory = dev().getRocMemory(amdMemory);

    if (srcMemory == NULL) {
        // Release all pinned memory and attempt pinning again
        gpu().releasePinnedMem();
        srcMemory = dev().getRocMemory(amdMemory);
        if (srcMemory == NULL) {
            // Release memory
            amdMemory->release();
            amdMemory = NULL;
        }
    }

    return amdMemory;
}

Memory*
KernelBlitManager::createView(
    const Memory&   parent,
    const cl_image_format   format) const
{
    assert((parent.owner()->asBuffer() == nullptr) && "View supports images only");
    amd::Image *image =
        parent.owner()->asImage()->createView(parent.owner()->getContext(), format, &gpu());

    if (image == NULL) {
        LogError("[OCL] Fail to allocate view of image object");
        return NULL;
    }

    Image* devImage = new roc::Image(dev(), *image);
    if (devImage == NULL) {
        LogError("[OCL] Fail to allocate device mem object for the view");
        image->release();
        return NULL;
    }

    if (!devImage->createView(parent)) {
        LogError("[OCL] Fail to create device mem object for the view");
        delete devImage;
        image->release();
        return NULL;
    }

    image->replaceDeviceMemory(&dev_, devImage);

    return devImage;
}

} // namespace pal
