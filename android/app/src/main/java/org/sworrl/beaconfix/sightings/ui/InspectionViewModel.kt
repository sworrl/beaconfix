// SPDX-License-Identifier: Apache-2.0
package org.sworrl.beaconfix.sightings.ui

import android.content.Context
import android.content.Intent
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.data.Prefs
import org.sworrl.beaconfix.sightings.InspectionManager
import org.sworrl.beaconfix.sightings.InspectionPlan
import javax.inject.Inject

@HiltViewModel
class InspectionViewModel @Inject constructor(
    val manager: InspectionManager,
    val prefs: Prefs,
) : ViewModel() {
    val activePlan: StateFlow<InspectionPlan?> = manager.currentPlan
    val privateMode: StateFlow<Boolean> = prefs.privateInspectionActive.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), false)
    val liveGuardAlert: StateFlow<String?> = manager.liveGuardAlert

    private val _loading = MutableStateFlow(false)
    val loading: StateFlow<Boolean> = _loading.asStateFlow()

    private val _inspectedCameraPlan = MutableStateFlow<InspectionPlan?>(null)
    val inspectedCameraPlan: StateFlow<InspectionPlan?> = _inspectedCameraPlan.asStateFlow()

    fun inspectCamera(
        camId: String,
        lat: Double,
        lon: Double,
        op: String,
        model: String,
        dir: String,
        fromLat: Double,
        fromLon: Double,
        profile: String = "foot",
    ) {
        viewModelScope.launch {
            _loading.value = true
            val plan = manager.fetchInspectionPlan(camId, lat, lon, op, model, dir, fromLat, fromLon, profile)
            _inspectedCameraPlan.value = plan
            _loading.value = false
        }
    }

    fun startPrivateInspection(plan: InspectionPlan) {
        manager.startInspection(plan)
    }

    fun stopInspection() {
        manager.stopInspection()
        _inspectedCameraPlan.value = null
    }

    fun closePlanSheet() {
        _inspectedCameraPlan.value = null
    }

    fun openInExternalNavigation(context: Context, plan: InspectionPlan) {
        val uri = manager.exportGpxUri(plan) ?: return
        val intent = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, "application/gpx+xml")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        runCatching {
            val chooser = Intent.createChooser(intent, "Open in OsmAnd / Organic Maps")
            chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            context.startActivity(chooser)
        }
    }
}
