// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <jni.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "VideoCommon/PostProcessing.h"
#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"

extern "C" {

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_settings_model_PostProcessing_getShaderList(JNIEnv* env,
                                                                                    jclass)
{
  return SpanToJStringArray(env, VideoCommon::PostProcessing::GetShaderList());
}

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_settings_model_PostProcessing_getAnaglyphShaderList(
    JNIEnv* env, jclass)
{
  return SpanToJStringArray(env, VideoCommon::PostProcessing::GetAnaglyphShaderList());
}

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_settings_model_PostProcessing_getPassiveShaderList(
    JNIEnv* env, jclass)
{
  return SpanToJStringArray(env, VideoCommon::PostProcessing::GetPassiveShaderList());
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_features_settings_model_PostProcessing_getUserShaderDirectory(
    JNIEnv* env, jclass)
{
  return ToJString(env, File::GetUserPath(D_SHADERS_IDX));
}

JNIEXPORT jobject JNICALL
Java_org_dolphinemu_dolphinemu_features_settings_model_PostProcessing_validateShaderSource(
    JNIEnv* env, jclass, jstring code)
{
  const VideoCommon::PostProcessing::ShaderValidationResult result =
      VideoCommon::PostProcessing::ValidateShaderSource(GetJString(env, code));

  const jclass result_class = IDCache::GetPostProcessingShaderValidationResultClass();
  const jmethodID ctor = IDCache::GetPostProcessingShaderValidationResultConstructor();
  return env->NewObject(result_class, ctor, static_cast<jboolean>(result.valid),
                        static_cast<jboolean>(result.gpu_compiled),
                        ToJString(env, result.error_message));
}
}
