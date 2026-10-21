# Embed presentation HLSL and vendored FidelityFX: no runtime file/include IO.
file(READ "${PROJECT_SOURCE_DIR}/externals/FidelityFX-FSR/ffx-fsr/ffx_a.h"
     FFX_A_SOURCE)
file(READ "${PROJECT_SOURCE_DIR}/externals/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h"
     FFX_FSR_SOURCE)
# DXC -HV 2021 requires explicit select for vector conditional expressions.
# Patch only the embedded copy, keeping the vendored AMD source intact.
foreach(width 2 3 4)
  string(
    REPLACE
      "(AU${width}_AF${width}(x)!=AU${width}_(0))?AU${width}_(0):AU${width}_AF${width}(y)"
      "select(AU${width}_AF${width}(x)!=AU${width}_(0),AU${width}_(0),AU${width}_AF${width}(y))"
      FFX_A_SOURCE
      "${FFX_A_SOURCE}")
endforeach()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/present.hlsl" PRESENT_HLSL)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/frame_generation.hlsl" FRAME_GENERATION_HLSL)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/temporal_upscaler.hlsl" TEMPORAL_UPSCALER_HLSL)
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS
           "${PROJECT_SOURCE_DIR}/externals/FidelityFX-FSR/ffx-fsr/ffx_a.h"
           "${PROJECT_SOURCE_DIR}/externals/FidelityFX-FSR/ffx-fsr/ffx_fsr1.h"
           "${CMAKE_CURRENT_SOURCE_DIR}/present.hlsl"
           "${CMAKE_CURRENT_SOURCE_DIR}/frame_generation.hlsl"
           "${CMAKE_CURRENT_SOURCE_DIR}/temporal_upscaler.hlsl")
# Keep literal tokens below MSVC's size limit; concatenate at startup.
set(PRESENT_SOURCE
    "#define A_GPU 1\n#define A_HLSL 1\n${FFX_A_SOURCE}\n#define FSR_EASU_F 1\n#define FSR_RCAS_F 1\n#define FSR_RCAS_PASSTHROUGH_ALPHA 1\n${FFX_FSR_SOURCE}\n${PRESENT_HLSL}"
)
string(LENGTH "${PRESENT_SOURCE}" PRESENT_SOURCE_LENGTH)
set(SHADER_SOURCE_PARTS "")
set(offset 0)
while(offset LESS PRESENT_SOURCE_LENGTH)
  string(SUBSTRING "${PRESENT_SOURCE}" ${offset} 12000 part)
  string(APPEND SHADER_SOURCE_PARTS
         "    source.append(R\"NXHLSL(${part})NXHLSL\");\n")
  math(EXPR offset "${offset} + 12000")
endwhile()
configure_file(shader_source.h.in d3d12_present_shader_source.h @ONLY)
target_include_directories(video_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_sources(video_core PRIVATE present.cpp present.h present_math.h
               frame_generation.cpp frame_generation.h frame_generation_policy.h
               frame_pacing_policy.h temporal_upscaler.cpp temporal_upscaler.h)
