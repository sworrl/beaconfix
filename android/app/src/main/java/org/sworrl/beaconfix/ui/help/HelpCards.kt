package org.sworrl.beaconfix.ui.help

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/**
 * One-glance nearest-help cards for Places (header) and Home. [onOpen] opens the Help screen.
 *
 * STUB (A0): the signatures are frozen; A2 fills in the bodies (children's ER · distance · ETA, plus a call button).
 */
@Composable
fun HelpSummaryCard(onOpen: () -> Unit, modifier: Modifier = Modifier) {
    OutlinedCard(onClick = onOpen, modifier = modifier.fillMaxWidth()) {
        Text("Nearest help", Modifier.padding(16.dp), style = MaterialTheme.typography.titleSmall)
    }
}

@Composable
fun HomeHelpCard(onOpen: () -> Unit) {
    HelpSummaryCard(onOpen = onOpen)
}
