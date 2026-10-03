package org.sworrl.beaconfix.ui.vm

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.sworrl.beaconfix.data.api.LinkedDevice
import org.sworrl.beaconfix.data.db.AppDatabase
import org.sworrl.beaconfix.identity.IdentityStore
import org.sworrl.beaconfix.net.Bfs3
import org.sworrl.beaconfix.net.Bfs3Exception
import org.sworrl.beaconfix.net.HubClient
import org.sworrl.beaconfix.net.HubErrors
import org.sworrl.beaconfix.net.HubLive
import org.sworrl.beaconfix.net.HubStatus
import org.sworrl.beaconfix.net.HubStore
import org.sworrl.beaconfix.sync.SyncRepository
import javax.inject.Inject

/** The Hub screen (enrolment, status) and the map's hub devices layer. */
@HiltViewModel
class HubViewModel @Inject constructor(
    private val store: HubStore, private val hub: HubClient, private val hubLive: HubLive,
    private val sync: SyncRepository, private val identity: IdentityStore, db: AppDatabase,
) : ViewModel() {
    val status: StateFlow<HubStatus> = store.status
    val devices: StateFlow<List<LinkedDevice>> = hubLive.devices
    val streaming: StateFlow<Boolean> = hubLive.streaming
    val queued: StateFlow<Int> = db.observations().unsyncedCount().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), 0)
    /** An invite read from a QR or pasted, waiting for the user to compare its fingerprint. */
    val invite = MutableStateFlow<Bfs3.Invite?>(null)
    val busy = MutableStateFlow(false)
    val message = MutableStateFlow("")
    val deviceName: String get() = identity.deviceName
    private var watching = false

    fun readInvite(text: String) {
        try { invite.value = Bfs3.parseInvite(text); message.value = "" }
        catch (e: Bfs3Exception) { invite.value = null; message.value = e.message ?: "not a BFS3 invite" }
    }
    fun cancelInvite() { invite.value = null; message.value = "" }

    /** The user compared the fingerprints: enrol, then sync at once. */
    fun enroll(url: String, name: String) {
        val inv = invite.value ?: return
        viewModelScope.launch {
            busy.value = true
            try {
                val cfg = hub.enroll(inv, url, name)
                invite.value = null
                message.value = "Enrolled as ${cfg.name} (${cfg.deviceId}). Syncing…"
                val r = withContext(Dispatchers.IO) { sync.syncAll() }.firstOrNull()
                message.value = "Enrolled as ${cfg.name}. " + (r?.let { if (it.ok) "First sync: ${it.message}" else "First sync failed: ${it.message}" } ?: "")
            } catch (e: CancellationException) { throw e } catch (e: Exception) {
                message.value = e.message ?: e.toString()
            } finally { busy.value = false }
        }
    }

    fun syncNow() = viewModelScope.launch {
        busy.value = true
        try { val r = withContext(Dispatchers.IO) { sync.syncAll() }.firstOrNull(); message.value = r?.let { if (it.ok) "Synced: ${it.message}" else "Sync failed: ${it.message}" } ?: "" }
        finally { busy.value = false }
    }

    /** One sealed round trip (`GET /hello` over BFS3): proves the URL, TLS, WireGuard, the keys and the clock. */
    fun test() = viewModelScope.launch {
        busy.value = true
        try {
            val api = hub.api() ?: run { message.value = "not enrolled"; return@launch }
            val r = withContext(Dispatchers.IO) { api.hello() }
            message.value = if (r.isSuccessful) r.body()!!.let { "Hub answered over BFS3: ${it.hostname.ifEmpty { it.name }} ${it.version}".trim() }.also { withContext(Dispatchers.IO) { store.noteContact() } }
                            else HubErrors.ofStatus(r.code(), hub.clockSkewS)
        } catch (e: CancellationException) { throw e } catch (e: Exception) {
            message.value = HubErrors.describe(e)
            withContext(Dispatchers.IO) { store.noteError(message.value, HubErrors.isUnreachable(e)) }
        } finally { busy.value = false }
    }

    fun setUrl(url: String) = viewModelScope.launch {
        try { withContext(Dispatchers.IO) { store.setUrl(url) }; message.value = "Hub address saved" }
        catch (e: IllegalArgumentException) { message.value = e.message ?: "invalid address" }
        catch (e: Exception) { message.value = e.message ?: e.toString() }
    }

    fun forget() = viewModelScope.launch {
        withContext(Dispatchers.IO) { store.forget(); hub.reset() }
        message.value = "Hub forgotten on this phone — revoke it at the hub too (beaconfix --server --revoke <deviceId>)"
    }

    /** The map (or the Hub screen) shows every node's live position while visible. */
    fun live(on: Boolean) {
        if (on == watching) return
        watching = on
        if (on) hubLive.start() else hubLive.stop()
    }
    override fun onCleared() { live(false) }
}
