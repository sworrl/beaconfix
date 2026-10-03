package org.sworrl.beaconfix.alpr

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.widget.Toast
import org.sworrl.beaconfix.MainActivity
import org.sworrl.beaconfix.alpr.core.Pairing
import org.sworrl.beaconfix.alpr.ui.AlprIncoming

/**
 * `falconeyez://pair?u=…&t=…&n=…` scanned with the system camera (or tapped): hands the payload to the ALPR screen,
 * which asks before pairing. Never pairs on its own.
 */
class AlprPairActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val p = intent?.dataString?.let { Pairing.parse(it) }
        if (p == null) Toast.makeText(this, "Not a FalconEyez pairing link", Toast.LENGTH_SHORT).show()
        else {
            AlprIncoming.payload.value = p
            startActivity(Intent(this, MainActivity::class.java).putExtra("action", "alpr").addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP))
        }
        finish()
    }
}
