# Ajusta o backend D3D12 do NVRHI para compilar com MinGW (clang ou gcc).
#
# Nos headers do D3D12, fora do MSVC, os métodos COM que devolvem struct (GetDesc,
# GetCPUDescriptorHandleForHeapStart, GetResourceAllocationInfo...) têm a forma com um
# ponteiro de saída como primeiro parâmetro (é o que o ABI do MinGW exige). O NVRHI usa a
# forma do MSVC; aqui as chamadas viram a forma com ponteiro. Só troca o texto original:
# rodar de novo não muda nada.
#
# Fora do MSVC, o __uuidof das interfaces do D3D12 vem do dxguids.h do DirectX-Headers
# (nvrhi-mingw/nvrhi_mingw_uuids.h), incluído em d3d12-backend.h, depois dos headers do D3D12.
#
# Uso: cmake -DNVRHI_DIR=<fonte do NVRHI> -P nvrhi-mingw.cmake

function(lkz_patch file)
    set(path "${NVRHI_DIR}/src/d3d12/${file}")
    file(READ "${path}" text)
    set(original "${text}")
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE 1 ${last} 2)
        math(EXPR j "${i} + 1")
        string(FIND "${text}" "${ARGV${j}}" done)
        if(done EQUAL -1) # já trocado numa execução anterior: nada a fazer
            string(REPLACE "${ARGV${i}}" "${ARGV${j}}" text "${text}")
        endif()
    endforeach()
    if(NOT text STREQUAL original)
        file(WRITE "${path}" "${text}")
        message(STATUS "NVRHI (MinGW): ${file} ajustado")
    endif()
endfunction()

set(alloc_from [=[D3D12_RESOURCE_ALLOCATION_INFO allocInfo = m_Context.device->GetResourceAllocationInfo(]=])
set(alloc_to [=[D3D12_RESOURCE_ALLOCATION_INFO allocInfo; m_Context.device->GetResourceAllocationInfo(&allocInfo, ]=])
set(alloc2_from [=[m_Context.device8->GetResourceAllocationInfo2(1, 1, ]=])
set(alloc2_to [=[D3D12_RESOURCE_ALLOCATION_INFO allocTotal; m_Context.device8->GetResourceAllocationInfo2(&allocTotal, 1, 1, ]=])

lkz_patch(d3d12-buffer.cpp
    "${alloc_from}" "${alloc_to}"
    "${alloc2_from}" "${alloc2_to}"
    [=[*reinterpret_cast<D3D12_RESOURCE_DESC*>(&buffer->resourceDesc) = pResource->GetDesc();]=]
    [=[pResource->GetDesc(reinterpret_cast<D3D12_RESOURCE_DESC*>(&buffer->resourceDesc));]=])

lkz_patch(d3d12-descriptor-heap.cpp
    [=[m_StartCpuHandleShaderVisible = m_ShaderVisibleHeap->GetCPUDescriptorHandleForHeapStart();]=]
    [=[m_ShaderVisibleHeap->GetCPUDescriptorHandleForHeapStart(&m_StartCpuHandleShaderVisible);]=]
    [=[m_StartGpuHandleShaderVisible = m_ShaderVisibleHeap->GetGPUDescriptorHandleForHeapStart();]=]
    [=[m_ShaderVisibleHeap->GetGPUDescriptorHandleForHeapStart(&m_StartGpuHandleShaderVisible);]=]
    [=[m_StartCpuHandle = m_Heap->GetCPUDescriptorHandleForHeapStart();]=]
    [=[m_Heap->GetCPUDescriptorHandleForHeapStart(&m_StartCpuHandle);]=]
    [=[oldHeap->GetCPUDescriptorHandleForHeapStart()]=]
    [=[[&] { D3D12_CPU_DESCRIPTOR_HANDLE start; oldHeap->GetCPUDescriptorHandleForHeapStart(&start); return start; }()]=])

lkz_patch(d3d12-texture.cpp
    "${alloc_from}" "${alloc_to}"
    "${alloc2_from}" "${alloc2_to}"
    [=[resourceDesc1 = resource2->GetDesc1();]=]
    [=[resource2->GetDesc1(&resourceDesc1);]=]
    [=[*reinterpret_cast<D3D12_RESOURCE_DESC*>(&resourceDesc1) = pResource->GetDesc();]=]
    [=[pResource->GetDesc(reinterpret_cast<D3D12_RESOURCE_DESC*>(&resourceDesc1));]=]
    [=[D3D12_RESOURCE_DESC1 rdFeedback = pResource->GetDesc1();]=]
    [=[D3D12_RESOURCE_DESC1 rdFeedback; pResource->GetDesc1(&rdFeedback);]=]
    [=[D3D12_RESOURCE_DESC resourceDesc = dest->resource->GetDesc();]=]
    [=[D3D12_RESOURCE_DESC resourceDesc; dest->resource->GetDesc(&resourceDesc);]=])

lkz_patch(d3d12-backend.h
    [=[#include <nvrhi/d3d12.h>
]=]
    [=[#include <nvrhi/d3d12.h>
#include <nvrhi_mingw_uuids.h>
]=])
