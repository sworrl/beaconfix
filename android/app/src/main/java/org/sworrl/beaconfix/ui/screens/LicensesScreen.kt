// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.ui.screens

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.mikepenz.aboutlibraries.ui.compose.m3.LibrariesContainer
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.ui.theme.Slate

/**
 * Settings → Open-source licenses (docs/LICENSING.md): every library in the build with its license, collected at build
 * time by AboutLibraries from the dependencies' metadata, plus the ALPR models and ONNX Runtime (app/config/libraries).
 */
@Composable
fun LicensesScreen(onBack: () -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            TextButton(onClick = onBack) { Text("‹ Back") }
            Text("Open-source licenses", style = MaterialTheme.typography.headlineSmall)
        }
        Text("BeaconFix Android ${BuildConfig.VERSION_NAME} is licensed under the Apache License 2.0. It is built with the components below, " +
            "each under its own license; tap one for its license text. Map data © OpenStreetMap contributors (ODbL).",
            color = Slate, style = MaterialTheme.typography.bodySmall, modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp))
        LibrariesContainer(Modifier.fillMaxSize())
    }
}
