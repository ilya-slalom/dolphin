add_dolphin_test(VertexLoaderTest VertexLoaderTest.cpp)
add_dolphin_test(VideoConfigTest VideoConfigTest.cpp)
add_dolphin_test(SlangPresetTest PostProcessing/SlangPresetTest.cpp)
add_dolphin_test(SlangShaderTest PostProcessing/SlangShaderTest.cpp)
add_dolphin_test(PassSizingTest PostProcessing/PassSizingTest.cpp)
add_dolphin_test(SlangTranslatorTest PostProcessing/SlangTranslatorTest.cpp)
add_dolphin_test(SlangSamplersTest PostProcessing/SlangSamplersTest.cpp)
add_dolphin_test(MipGenTest PostProcessing/MipGenTest.cpp)
add_dolphin_test(PassGraphTest PostProcessing/PassGraphTest.cpp)
add_dolphin_test(PresetArchiveTest PostProcessing/PresetArchiveTest.cpp)
add_dolphin_test(ShaderPackDownloadTest PostProcessing/ShaderPackDownloadTest.cpp)
add_dolphin_test(ShaderPackSourceTest PostProcessing/ShaderPackSourceTest.cpp)
add_dolphin_test(PresetNameTest PostProcessing/PresetNameTest.cpp)
add_dolphin_test(SlangCompileTest PostProcessing/SlangCompileTest.cpp)
# SlangCompileTest cross-compiles its SPIR-V to HLSL/MSL, so it needs what the D3D and Metal
# backends need. glslang alone cannot see a sampler-dimension mismatch: `sampler2D` is valid GLSL.
# glslang-default-resource-limits is for the low-GLSL-version rows, which drive glslang's front end
# directly instead of going through SPIRV::Compile* (that path cannot target below 310 es).
target_link_libraries(SlangCompileTest PRIVATE spirv_cross glslang-default-resource-limits)
add_dolphin_test(RetroCrisisInstallTest PostProcessing/RetroCrisisInstallTest.cpp)
add_dolphin_test(ShaderChainSpecTest PostProcessing/ShaderChainSpecTest.cpp)
add_dolphin_test(SlangSourceDownscaleTest PostProcessing/SlangSourceDownscaleTest.cpp)
add_dolphin_test(ChainOutputPolicyTest PostProcessing/ChainOutputPolicyTest.cpp)
