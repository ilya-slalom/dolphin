// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.utils

import android.content.Context
import android.net.Uri
import org.dolphinemu.dolphinemu.R
import org.dolphinemu.dolphinemu.features.settings.model.PostProcessing
import java.io.File
import java.io.IOException

object ShaderImportHelper {
    sealed class Result {
        data class Success(val shaderName: String, val gpuCompiled: Boolean) : Result()
        data class Failure(val message: String) : Result()
    }

    /**
     * Reads the shader at [uri], validates it natively, and — only on success —
     * copies it into the user Shaders directory as <name>.glsl. Nothing is
     * written on failure.
     */
    fun importShader(context: Context, uri: Uri): Result {
        val displayName = ContentHandler.getDisplayName(uri)
            ?: return Result.Failure(context.getString(R.string.shader_import_read_failed))

        val shaderName = stripGlslExtension(displayName)
        if (shaderName.isEmpty()) {
            return Result.Failure(context.getString(R.string.shader_import_read_failed))
        }

        val source = try {
            context.contentResolver.openInputStream(uri)?.use { it.readBytes().toString(Charsets.UTF_8) }
        } catch (e: IOException) {
            null
        } ?: return Result.Failure(context.getString(R.string.shader_import_read_failed))

        val validation = PostProcessing.validateShaderSource(source)
        if (!validation.valid) {
            return Result.Failure(
                context.getString(R.string.shader_import_failed, validation.errorMessage)
            )
        }

        val shaderDir = File(PostProcessing.getUserShaderDirectory())
        if (!shaderDir.exists() && !shaderDir.mkdirs()) {
            return Result.Failure(context.getString(R.string.shader_import_write_failed))
        }

        val target = File(shaderDir, "$shaderName.glsl")
        if (target.exists()) {
            return Result.Failure(
                context.getString(R.string.shader_import_name_conflict, shaderName)
            )
        }

        return try {
            target.writeText(source)
            Result.Success(shaderName, validation.gpuCompiled)
        } catch (e: IOException) {
            if (target.exists()) target.delete()
            Result.Failure(context.getString(R.string.shader_import_write_failed))
        }
    }

    private fun stripGlslExtension(fileName: String): String {
        val ext = FileBrowserHelper.getExtension(fileName, false)
        return if (ext != null && ext.equals("glsl", ignoreCase = true)) {
            fileName.substring(0, fileName.length - ext.length - 1)
        } else {
            fileName
        }
    }
}
