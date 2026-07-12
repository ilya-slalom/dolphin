// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "VideoCommon/PostProcessing/ShaderPackDownload.h"

using namespace VideoCommon;

namespace
{
void WriteZip(const std::string& zip_path,
              const std::vector<std::pair<std::string, std::string>>& entries)
{
  void* writer = mz_zip_writer_create();
  ASSERT_EQ(mz_zip_writer_open_file(writer, zip_path.c_str(), 0, 0), MZ_OK);
  for (const auto& [name, data] : entries)
  {
    mz_zip_file file_info = {};
    file_info.filename = name.c_str();
    file_info.flag = MZ_ZIP_FLAG_UTF8;
    ASSERT_EQ(mz_zip_writer_add_buffer(writer, const_cast<char*>(data.data()),
                                       static_cast<int32_t>(data.size()), &file_info),
              MZ_OK);
  }
  mz_zip_writer_close(writer);
  mz_zip_writer_delete(&writer);
}
}  // namespace

TEST(ShaderPackDownload, InstallsAllPresetsFromPackZip)
{
  const std::string tmp = File::CreateTempDir();
  ASSERT_FALSE(tmp.empty());
  const std::string zip = tmp + "/shaders_slang.zip";
  const std::string dest = tmp + "/Shaders";

  // Mirrors the buildbot pack shape: many presets across subdirs sharing an include/ tree.
  WriteZip(zip, {
                    {"crt/crt-royale.slangp", "shaders = \"1\"\nshader0 = \"../include/a.slang\"\n"},
                    {"crt/crt-geom.slangp", "shaders = \"1\"\nshader0 = \"../include/a.slang\"\n"},
                    {"include/a.slang",
                     "#pragma stage vertex\nvoid main(){}\n#pragma stage fragment\nvoid main(){}\n"},
                    {"README.md", "libretro slang shaders"},
                });

  const auto result = InstallShaderPackFromZip(zip, dest);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.preset_count, 2u);
  EXPECT_TRUE(File::Exists(dest + "/crt/crt-royale.slangp"));
  EXPECT_TRUE(File::Exists(dest + "/crt/crt-geom.slangp"));
  EXPECT_TRUE(File::Exists(dest + "/include/a.slang"));  // shared include preserved

  File::DeleteDirRecursively(tmp);
}

TEST(ShaderPackDownload, RejectsCorruptZip)
{
  const std::string tmp = File::CreateTempDir();
  ASSERT_FALSE(tmp.empty());
  const std::string zip = tmp + "/bad.zip";
  File::IOFile(zip, "wb").WriteBytes("not a zip", 9);
  const auto result = InstallShaderPackFromZip(zip, tmp + "/Shaders");
  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.empty());
  File::DeleteDirRecursively(tmp);
}
