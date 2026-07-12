// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

object PostProcessing {
    @JvmStatic
    val shaderList: Array<String>
        external get

    /**
     * Downloads the RetroArch slang shader pack from the libretro buildbot and installs it into
     * the user Shaders directory. Blocking; call off the UI thread. Returns the number of
     * installed presets, or -1 on failure.
     */
    @JvmStatic
    external fun downloadShaderPack(): Int
}
