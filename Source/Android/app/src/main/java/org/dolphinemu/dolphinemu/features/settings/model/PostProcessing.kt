// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model

import androidx.annotation.Keep

object PostProcessing {
    @JvmStatic
    val shaderList: Array<String>
        external get

    @JvmStatic
    val anaglyphShaderList: Array<String>
        external get

    @JvmStatic
    val passiveShaderList: Array<String>
        external get

    /**
     * Result of [validateShaderSource]. Mirrors the native ShaderValidationResult.
     *
     * @Keep is required: this class and its constructor are only referenced from
     * native code (IDCache looks up the (ZZLjava/lang/String;)V constructor in
     * JNI_OnLoad), which R8 cannot see. Without it, minified release builds strip
     * the constructor and crash at startup with NoSuchMethodError.
     */
    @Keep
    data class ShaderValidationResult(
        @JvmField val valid: Boolean,
        @JvmField val gpuCompiled: Boolean,
        @JvmField val errorMessage: String
    )

    /** Validates raw shader source. Never touches the filesystem. */
    @JvmStatic
    external fun validateShaderSource(code: String): ShaderValidationResult

    /** Absolute path of the user Shaders directory (ends with a separator). */
    @JvmStatic
    external fun getUserShaderDirectory(): String
}
