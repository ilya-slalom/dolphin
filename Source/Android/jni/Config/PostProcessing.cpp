// Copyright 2021 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <vector>

#include <jni.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "VideoCommon/PostProcessing/MultipassPostProcessing.h"
#include "VideoCommon/PostProcessing/ShaderPackDownload.h"
#include "jni/AndroidCommon/AndroidCommon.h"

extern "C" {

JNIEXPORT jobjectArray JNICALL
Java_org_dolphinemu_dolphinemu_features_settings_model_PostProcessing_getShaderList(JNIEnv* env,
                                                                                    jclass)
{
  return SpanToJStringArray(env, VideoCommon::MultipassPostProcessing::GetPresetList());
}

// Downloads the RetroArch slang shader pack from the libretro buildbot and installs it into the
// user Shaders dir. Blocking; call off the UI thread. Returns the number of installed .slangp
// presets, or -1 on failure.
JNIEXPORT jint JNICALL
Java_org_dolphinemu_dolphinemu_features_settings_model_PostProcessing_downloadShaderPack(JNIEnv*,
                                                                                         jclass)
{
  const VideoCommon::ShaderPackDownloadResult result = VideoCommon::DownloadAndInstallShaderPack(
      VideoCommon::SLANG_SHADER_PACK_URL, File::GetUserPath(D_SHADERS_IDX));
  return result.ok ? static_cast<jint>(result.preset_count) : -1;
}
}
