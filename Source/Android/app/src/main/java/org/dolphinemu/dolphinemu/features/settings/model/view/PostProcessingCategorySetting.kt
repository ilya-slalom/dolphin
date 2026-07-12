// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu.features.settings.model.view

import android.content.Context
import org.dolphinemu.dolphinemu.features.settings.model.Settings

/**
 * A non-persistent single-choice dropdown used to pick a post-processing shader *category*
 * (the top-level folder of the buildbot pack: crt, bezel, border, blurs, ...). Selecting a
 * category updates an in-memory value and reloads the settings list so the shader dropdown can
 * be filtered to that category, making the ~2500-preset list navigable.
 */
class PostProcessingCategorySetting(
    context: Context,
    titleId: Int,
    descriptionId: Int,
    categories: Array<String>,
    private var current: String,
    private val onCategoryChanged: (String) -> Unit
) : StringSingleChoiceSetting(context, null, titleId, descriptionId, categories, categories) {
    override val selectedValue: String
        get() = current

    override val selectedChoice: String
        get() = current

    override fun setSelectedValue(settings: Settings, selection: String) {
        current = selection
        onCategoryChanged(selection)
    }
}
