package co.minilora.app.ui.screens

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.selectable
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import co.minilora.app.ui.model.PruneOption

/**
 * "Clean data store" popup — the app-side twin of the server's prune
 * buttons. Radio choice + Apply; the newest point always survives the
 * window options (cutoff is now − age, like the server).
 */
@Composable
fun PruneDialog(
    onApply: (PruneOption) -> Unit,
    onDismiss: () -> Unit,
) {
    var selected by remember { mutableStateOf(PruneOption.ALL) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Clean data store") },
        text = {
            Column {
                Text(
                    "Delete stored tracker data. The newest point always " +
                        "survives the window options.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(Modifier.height(8.dp))
                PruneOption.entries.forEach { opt ->
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .selectable(
                                selected = selected == opt,
                                role = Role.RadioButton,
                                onClick = { selected = opt },
                            )
                            .padding(vertical = 2.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(
                            selected = selected == opt,
                            onClick = null,   // row handles the click
                        )
                        Text(opt.label,
                            style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = {
                onApply(selected)
                onDismiss()
            }) { Text("Apply") }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}
