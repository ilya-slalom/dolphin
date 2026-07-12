// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/PostProcessing/ShaderPackDownload.h"

#include <chrono>
#include <string>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "VideoCommon/PostProcessing/PresetArchive.h"

namespace VideoCommon
{
ShaderPackDownloadResult InstallShaderPackFromZip(const std::string& local_zip_path,
                                                  const std::string& dest_root)
{
  std::string error;
  const std::vector<std::string> presets =
      ExtractSanitizedArchive(local_zip_path, dest_root, &error);
  if (presets.empty() && !error.empty())
    return {false, 0, error};

  return {true, static_cast<u32>(presets.size()), ""};
}

ShaderPackDownloadResult DownloadAndInstallShaderPack(const std::string& url,
                                                      const std::string& dest_root,
                                                      DownloadProgress progress)
{
  INFO_LOG_FMT(VIDEO, "Downloading slang shader pack from {}", url);

  Common::HttpRequest::ProgressCallback curl_progress = nullptr;
  if (progress)
  {
    curl_progress = [progress](s64 dltotal, s64 dlnow, s64, s64) -> bool {
      return progress(dlnow, dltotal);
    };
  }

  Common::HttpRequest http(std::chrono::seconds(60), std::move(curl_progress));
  http.FollowRedirects(10);  // the buildbot may 30x.

  const Common::HttpRequest::Response response = http.Get(url);
  if (!response)
  {
    const std::string error =
        "download failed (HTTP " + std::to_string(http.GetLastResponseCode()) + ")";
    ERROR_LOG_FMT(VIDEO, "Slang shader pack {}", error);
    return {false, 0, error};
  }

  const std::string tmp_dir = File::CreateTempDir();
  if (tmp_dir.empty())
    return {false, 0, "could not create temp directory"};
  Common::ScopeGuard tmp_guard{[&] { File::DeleteDirRecursively(tmp_dir); }};

  const std::string zip_path = tmp_dir + DIR_SEP "shaders_slang.zip";
  {
    File::IOFile zip(zip_path, "wb");
    if (!zip || !zip.WriteBytes(response->data(), response->size()))
      return {false, 0, "failed to write downloaded archive"};
  }

  INFO_LOG_FMT(VIDEO, "Downloaded {} bytes; extracting shader pack into {}", response->size(),
               dest_root);
  return InstallShaderPackFromZip(zip_path, dest_root);
}
}  // namespace VideoCommon
