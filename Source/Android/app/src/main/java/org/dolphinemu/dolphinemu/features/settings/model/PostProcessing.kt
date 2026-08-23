// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

object PostProcessing {
    @JvmStatic
    val shaderList: Array<String>
        external get

    /**
     * Downloads the shader pack with the given registry id (e.g. "libretro", "satpixie",
     * "retrocrisis") and installs it into the user Shaders directory, auto-fetching any missing
     * dependency packs first. [profile] selects a display profile for packs that ship several (only
     * "retrocrisis" uses it; pass "" for the others). Blocking; call off the UI thread. Returns the
     * number of installed presets, or -1 on failure.
     */
    @JvmStatic
    external fun downloadShaderPack(packId: String, profile: String): Int
}
