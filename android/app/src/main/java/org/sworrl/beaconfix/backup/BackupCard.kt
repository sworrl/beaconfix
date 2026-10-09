package org.sworrl.beaconfix.backup

import androidx.lifecycle.compose.collectAsStateWithLifecycle
import android.content.Context
import android.content.Intent
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.material3.Button
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.InfoCard
import org.sworrl.beaconfix.ui.ago
import org.sworrl.beaconfix.ui.theme.Slate

/** Open a screen of this app by its launch action (used when no navigation callback is given). */
private fun openAction(ctx: Context, action: String) {
    ctx.startActivity(Intent(ctx, MainActivity::class.java).putExtra("action", action).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_NEW_TASK))
}

/**
 * Settings → Backup: "Save backup…" (system file picker, no storage permission), "Send to the RV" (the paired desktop
 * merges it into its database; needs control access), "Restore…" (the Import screen reads the file back) and a pointer
 * to the identity export, which the backup deliberately leaves out. [onRestore] / [onIdentity] navigate when given;
 * without them the card opens those screens through MainActivity's launch actions.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun BackupCard(onRestore: (() -> Unit)? = null, onIdentity: (() -> Unit)? = null, vm: BackupViewModel = hiltViewModel()) {
    val ctx = LocalContext.current
    val busy by vm.busy.collectAsStateWithLifecycle()
    val message by vm.message.collectAsStateWithLifecycle()
    val last by vm.lastBackupAt.collectAsStateWithLifecycle()
    val targets by vm.targets.collectAsStateWithLifecycle()
    val save = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri -> uri?.let { vm.save(it) } }
    InfoCard(stringResource(R.string.backup_title)) {
        Text(stringResource(R.string.backup_body), color = Slate, style = MaterialTheme.typography.bodySmall)
        Text(if (last > 0) stringResource(R.string.backup_last, ago(last)) else stringResource(R.string.backup_never), style = MaterialTheme.typography.bodyMedium)
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Button(onClick = { save.launch(vm.suggestedFileName()) }, enabled = !busy, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.backup_save)) }
            if (targets.isEmpty()) OutlinedButton(onClick = {}, enabled = false, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.backup_send)) }
            else for (d in targets) OutlinedButton(onClick = { vm.sendTo(d) }, enabled = !busy, modifier = Modifier.heightIn(min = 48.dp)) {
                Text(if (targets.size == 1) stringResource(R.string.backup_send) else stringResource(R.string.backup_send_to, d.name.ifBlank { d.host }))
            }
            OutlinedButton(onClick = { onRestore?.invoke() ?: openAction(ctx, "import") }, modifier = Modifier.heightIn(min = 48.dp)) { Text(stringResource(R.string.backup_restore)) }
        }
        if (targets.isEmpty()) Text(stringResource(R.string.backup_send_needs_control), color = Slate, style = MaterialTheme.typography.bodySmall)
        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (message.isNotEmpty()) Text(message, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite })
        Text(stringResource(R.string.backup_identity_note), color = Slate, style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { onIdentity?.invoke() ?: openAction(ctx, "identity") }) { Text(stringResource(R.string.backup_identity_export)) }
    }
}
