# Compila um ponto de entrada HLSL para DXIL (D3D12) e SPIR-V (Vulkan) e grava os dois
# binários num header C++ (arrays <NAME>_dxil e <NAME>_spirv).
#
# Variáveis: DXC, SOURCE, ENTRY, PROFILE, NAME, OUTPUT.
#
# Vulkan: os registradores do HLSL viram bindings com o deslocamento padrão do NVRHI
# (VulkanBindingOffsets: t = 0, s = 128, b = 256, u = 384), no espaço 0.

get_filename_component(directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${directory}")
set(dxil "${OUTPUT}.dxil")
set(spirv "${OUTPUT}.spv")

execute_process(
    COMMAND "${DXC}" -nologo -T ${PROFILE} -E ${ENTRY} -O3 -Fo "${dxil}" "${SOURCE}"
    RESULT_VARIABLE result ERROR_VARIABLE errors OUTPUT_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "DXC (DXIL) falhou em ${SOURCE}:${ENTRY}\n${errors}${output}")
endif()
execute_process(
    COMMAND "${DXC}" -nologo -spirv -fspv-target-env=vulkan1.3 -T ${PROFILE} -E ${ENTRY} -O3
            -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 0 -fvk-u-shift 384 0
            -Fo "${spirv}" "${SOURCE}"
    RESULT_VARIABLE result ERROR_VARIABLE errors OUTPUT_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "DXC (SPIR-V) falhou em ${SOURCE}:${ENTRY}\n${errors}${output}")
endif()

function(lkz_bytes file out)
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
    string(REGEX REPLACE "(0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..,)" "\\1\n    " bytes "${bytes}")
    set(${out} "${bytes}" PARENT_SCOPE)
endfunction()
lkz_bytes("${dxil}" dxil_bytes)
lkz_bytes("${spirv}" spirv_bytes)
file(WRITE "${OUTPUT}" "// Gerado por CompileShader.cmake a partir de ${SOURCE} (${ENTRY}, ${PROFILE}). Não editar.
#pragma once
inline constexpr unsigned char ${NAME}_dxil[] = {
    ${dxil_bytes}
};
inline constexpr unsigned char ${NAME}_spirv[] = {
    ${spirv_bytes}
};
")
