package org.sworrl.beaconfix.car

import android.content.Intent
import androidx.car.app.CarAppService
import androidx.car.app.Session
import androidx.car.app.validation.HostValidator

class BeaconFixCarAppService : CarAppService() {
    override fun createHostValidator(): HostValidator {
        // Allows all hosts so that development, debug, and sideloaded apps run seamlessly on Android Auto
        return HostValidator.ALLOW_ALL_HOSTS_VALIDATOR
    }

    override fun onCreateSession(): Session {
        return BeaconFixCarSession()
    }
}
