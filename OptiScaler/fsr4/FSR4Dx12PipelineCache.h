#pragma once

#include <d3d12.h>
#include <memory>
#include "Dx12PipelineCacheData.h"

namespace FSR4Dx12PipelineCache
{
struct Device;
using DevicePtr = std::shared_ptr<Device>;
using ComputeCreate = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID,
                                                  void**);
using StreamCreate = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID,
                                                 void**);

DevicePtr Prepare(ID3D12Device* device);
Dx12PipelineCacheData::Counters Statistics();

// Only synchronous calls made by the AMD DX12 upscaler provider participate. No GPU objects are retained.
class Scope
{
    Scope* previous;
    friend void RememberRootSignature(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
    friend HRESULT CreateCompute(ComputeCreate, ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID,
                                 void**);
    friend HRESULT CreateStream(StreamCreate, ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

  public:
    Device* device;
    Dx12PipelineCacheData::Counters counters;
    explicit Scope(const DevicePtr& device);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

void RememberRootSignature(ID3D12Device* device, UINT nodeMask, const void* bytes, SIZE_T size, REFIID iid,
                           void** output);
HRESULT CreateCompute(ComputeCreate original, ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                      REFIID iid, void** output);
HRESULT CreateStream(StreamCreate original, ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
                     REFIID iid, void** output);
} // namespace FSR4Dx12PipelineCache
