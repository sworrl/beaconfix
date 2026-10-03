package org.sworrl.beaconfix.car

import android.content.Intent
import androidx.car.app.Screen
import androidx.car.app.Session

class BeaconFixCarSession : Session() {
    override fun onCreateScreen(intent: Intent): Screen {
        return BeaconFixCarScreen(carContext)
    }
}
