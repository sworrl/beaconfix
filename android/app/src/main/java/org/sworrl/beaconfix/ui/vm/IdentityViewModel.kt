package org.sworrl.beaconfix.ui.vm

import android.content.Context
import android.net.Uri
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.sworrl.beaconfix.data.DesktopStore
import org.sworrl.beaconfix.data.db.DesktopEntity
import org.sworrl.beaconfix.data.db.PendingLinkEntity
import org.sworrl.beaconfix.identity.Crypto
import org.sworrl.beaconfix.identity.EffWords
import org.sworrl.beaconfix.identity.IdentityOps
import org.sworrl.beaconfix.identity.IdentityRecord
import org.sworrl.beaconfix.identity.IdentityRepository
import org.sworrl.beaconfix.identity.IdentityStore
import org.sworrl.beaconfix.identity.LinkOffer
import org.sworrl.beaconfix.identity.LinkStatement
import org.sworrl.beaconfix.identity.identityJson
import org.sworrl.beaconfix.widget.WidgetUpdater
import javax.inject.Inject

/** Everything the onboarding and Settings → Identity screens do. */
@HiltViewModel
class IdentityViewModel @Inject constructor(
    private val store: IdentityStore, private val repo: IdentityRepository, private val desktops: DesktopStore,
    private val widgets: WidgetUpdater, @ApplicationContext private val ctx: Context,
    private val peers: org.sworrl.beaconfix.data.PeerDiscovery, private val sync: org.sworrl.beaconfix.sync.SyncRepository,
) : ViewModel() {
    /** Find the BeaconFix that owns [identityId]: paired desktops first, then what discovery has seen, then a subnet scan. */
    private suspend fun desktopFor(identityId: String): DesktopEntity? {
        for (d in desktops.paired()) if (repo.remoteIdentity(d)?.id == identityId) return d
        var found: org.sworrl.beaconfix.data.Peer? = peers.peers.value.values.firstOrNull { it.identityId == identityId }
        if (found == null) { message.value = "finding that BeaconFix on the network…"; peers.scanSubnet(); found = peers.peers.value.values.firstOrNull { it.identityId == identityId } }
        val known: org.sworrl.beaconfix.data.Peer = found ?: return null
        val existing = desktops.get(known.key)
        if (existing != null) return existing
        val fresh = DesktopEntity(id = known.key, host = known.host, port = known.port, name = known.hostname.ifEmpty { known.host }, hostname = known.hostname, version = known.version)
        desktops.upsert(fresh); return fresh
    }
    val identity: StateFlow<IdentityRecord?> = store.current.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), null)
    val pending: StateFlow<List<PendingLinkEntity>> = store.pending.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val desktopsFlow: StateFlow<List<DesktopEntity>> = desktops.all().stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())
    val busy = MutableStateFlow(false)
    val message = MutableStateFlow("")
    val exportText = MutableStateFlow("")       // BFID1:… once exported (shown as QR + text)
    val exportCode = MutableStateFlow("")       // the 6-word code when one was generated
    val offerText = MutableStateFlow("")        // our link QR (BFLINK1:…)
    val statementText = MutableStateFlow("")    // a completed statement to show back as QR (BFLINKS1:…)
    /** pending import: bundle text waiting for its passphrase */
    val importBundle = MutableStateFlow("")

    private fun run(block: suspend () -> Unit) = viewModelScope.launch { busy.value = true; message.value = ""; try { block() } catch (e: Exception) { message.value = e.message ?: e.toString() } finally { busy.value = false } }

    fun create(name: String) = run { store.create(name); message.value = "identity created"; widgets.touch("identity") }
    fun stageImport(text: String) { importBundle.value = text.trim(); message.value = if (text.isBlank()) "" else "bundle received — enter its passphrase" }
    fun importWithPassphrase(passphrase: String) = run { val r = store.import(importBundle.value, passphrase); importBundle.value = ""; message.value = "imported ${r.name} (${Crypto.grouped(r.id)})"; widgets.touch("identity") }
    fun importFromFile(uri: Uri) = run { val text = ctx.contentResolver.openInputStream(uri)?.bufferedReader()?.readText() ?: error("cannot read that file"); stageImport(text) }
    fun importFromCode(host: String, port: Int, code: String) = run { stageImport(repo.fetchBundle(host, port, false, code)) }
    /** Automation: fetch by code and import in one go (the passphrase comes from the launch intent). */
    fun checkImportFromCode(host: String, code: String, pass: String) = run {
        val b = repo.fetchBundle(host, 47822, false, code); val p = IdentityOps.open(b, pass)
        val ok = Crypto.idOf(Crypto.publicKey(Crypto.unb64(p.seed))) == p.record.id
        message.value = if (ok) "hand-off check OK: the bundle from $host decrypts to ${p.record.name} (${Crypto.grouped(p.record.id)}) — nothing imported" else "hand-off check FAILED: key does not match the id"
        android.util.Log.i("BeaconFixId", message.value)
    }
    fun importFromCodeAndPass(host: String, code: String, pass: String) = run { val b = repo.fetchBundle(host, 47822, false, code); val r = store.import(b, pass); message.value = "imported ${r.name}"; widgets.touch("identity") }

    fun export(passphrase: String?) = run {
        val pass = passphrase?.takeIf { it.isNotBlank() } ?: EffWords.code(ctx).also { exportCode.value = it }
        require(pass.length >= 8) { "use at least 8 characters" }
        exportText.value = store.export(pass)
    }
    fun clearExport() { exportText.value = ""; exportCode.value = "" }
    fun writeExport(uri: Uri) = run { ctx.contentResolver.openOutputStream(uri)?.use { it.write(exportText.value.toByteArray()) } ?: error("cannot write there"); message.value = "saved" }
    fun rename(name: String) = run { store.rename(name); widgets.touch("identity") }
    fun forgetDevice(name: String) = run { store.forgetDevice(name) }
    fun forgetAll() = run { store.forgetAll(); desktops.all().first().forEach { desktops.forgetToken(it.id) }; widgets.touch("identity") }

    // ── linking ──────────────────────────────────────────────────────────────
    fun showOffer() = run { offerText.value = store.offer()?.let { IdentityOps.encodeOffer(it) } ?: error("no identity") }
    /** Something was scanned/pasted: an identity bundle, a link offer, or a completed statement. */
    fun handleScanned(text: String) = run {
        val t = text.trim()
        when {
            t.startsWith(IdentityOps.PREFIX) || t.startsWith("{") -> stageImport(t)
            t.startsWith(IdentityOps.OFFER_PREFIX) -> {
                val offer = IdentityOps.decodeOffer(t) ?: error("unreadable link QR")
                val me = store.currentNow() ?: error("no identity")
                if (offer.id == me.id || offer.id in store.linkedIds()) {
                    // same owner already: nothing to link — just find that BeaconFix and sign in (no code, no prompt)
                    val d = desktopFor(offer.id) ?: error("${if (offer.id == me.id) "that is this identity" else "already linked"} — but ${offer.name.ifEmpty { "that BeaconFix" }} is not reachable on this network")
                    message.value = (if (offer.id == me.id) "that is this identity" else "already linked with ${offer.name}") + " · signing in to ${d.name}…"
                    when (val r = repo.login(d)) {
                        is org.sworrl.beaconfix.identity.LoginResult.Ok -> { message.value = message.value.substringBefore(" · ") + " · signed in to ${d.name} (paired, no code needed)"; runCatching { sync.syncAll() } }
                        is org.sworrl.beaconfix.identity.LoginResult.UnknownIdentity -> message.value = r.message
                        is org.sworrl.beaconfix.identity.LoginResult.Failed -> message.value = "sign-in failed: ${r.message}"
                    }
                    widgets.touch("identity"); return@run
                }
                val half = store.complete(offer)          // ts stays exactly as the QR carried it: the desktop only co-signs a nonce it displayed
                // the desktop that owns that identity co-signs over the LAN (no token needed); then we sign in with the now-linked identity
                val d = desktopFor(offer.id)
                val finished = d?.let { repo.sendLink(it, half, Crypto.unb64(offer.pub)).onFailure { e -> message.value = e.message ?: "link refused" }.getOrNull() }
                if (finished != null && d != null) {
                    statementText.value = ""
                    message.value = "linked with ${offer.name.ifEmpty { Crypto.grouped(offer.id) }}"
                    when (val r = repo.login(d)) {
                        is org.sworrl.beaconfix.identity.LoginResult.Ok -> { message.value += " · signed in to ${d.name} (paired, no code needed)"; runCatching { sync.syncAll() } }
                        is org.sworrl.beaconfix.identity.LoginResult.UnknownIdentity -> message.value += " · ${r.message}"
                        is org.sworrl.beaconfix.identity.LoginResult.Failed -> message.value += " · sign-in later: ${r.message}"
                    }
                    widgets.touch("identity")
                } else {
                    statementText.value = IdentityOps.encodeStatement(half)
                    if (d == null) message.value = "could not reach ${offer.name.ifEmpty { "that BeaconFix" }} on this network — show this QR to it so it can co-sign, or scan again when you are on the same Wi-Fi"
                }
            }
            t.startsWith(IdentityOps.STMT_PREFIX) -> {
                val st = IdentityOps.decodeStatement(t) ?: error("unreadable statement")
                val rec = store.currentNow() ?: error("no identity")
                if (st.a != rec.id && st.b != rec.id) error("that statement is not about this identity")
                val ourSig = if (st.a == rec.id) st.sigA else st.sigB
                val other = st.other(rec.id)
                // the other side's key: from the statement, a pending entry, or a paired desktop that owns that id
                var theirPub: ByteArray? = (if (st.a == rec.id) st.pubB else st.pubA)?.let { runCatching { Crypto.unb64(it) }.getOrNull() }
                    ?: store.pending.first().firstOrNull { it.id == other }?.pub?.let { Crypto.unb64(it) }
                if (theirPub == null) for (d in desktops.paired()) { val ri = repo.remoteIdentity(d); if (ri != null && ri.id == other && ri.pub.isNotEmpty()) { theirPub = Crypto.unb64(ri.pub); break } }
                if (theirPub == null) error("unknown identity $other — scan its link payload first")
                if (Crypto.idOf(theirPub) != other) error("that statement's key does not match its id")
                val full = if (ourSig.isEmpty()) store.countersign(st) else st
                val ok = store.addLink(full, if (full.a == rec.id) Crypto.unb64(rec.pub) else theirPub, if (full.a == rec.id) theirPub else Crypto.unb64(rec.pub))
                if (!ok) error("the statement did not verify")
                statementText.value = if (ourSig.isEmpty()) IdentityOps.encodeStatement(full) else ""
                message.value = if (ourSig.isEmpty()) "linked — show this QR back so the other side stores it too" else "linked"
                widgets.touch("identity")
            }
            else -> error("not a BeaconFix code")
        }
    }
    fun dismissPending(id: String) = run { store.removePending(id) }
    fun clearLinkUi() { offerText.value = ""; statementText.value = "" }
    fun statementFor(p: PendingLinkEntity): String = p.statementJson.takeIf { it.isNotEmpty() }?.let { IdentityOps.encodeStatement(identityJson.decodeFromString(LinkStatement.serializer(), it)) } ?: ""
}
