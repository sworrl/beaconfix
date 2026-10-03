package org.sworrl.beaconfix.ui

import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Explore
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.MoreHoriz
import androidx.compose.material.icons.filled.Place
import androidx.compose.material.icons.filled.Radar
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material.icons.filled.RssFeed
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import androidx.window.core.layout.WindowWidthSizeClass
import dagger.hilt.android.EntryPointAccessors
import kotlinx.coroutines.flow.first
import org.sworrl.beaconfix.BuildConfig
import org.sworrl.beaconfix.LaunchArgs
import org.sworrl.beaconfix.share.ShareViewModel
import org.sworrl.beaconfix.ui.screens.BeaconsScreen
import org.sworrl.beaconfix.ui.screens.EventsScreen
import org.sworrl.beaconfix.ui.screens.HelpScreen
import org.sworrl.beaconfix.ui.screens.HomeScreen
import org.sworrl.beaconfix.ui.screens.IdentityScreen
import org.sworrl.beaconfix.ui.screens.MapScreen
import org.sworrl.beaconfix.ui.screens.MoreScreen
import org.sworrl.beaconfix.ui.screens.NearbyScreen
import org.sworrl.beaconfix.ui.screens.OnboardingScreen
import org.sworrl.beaconfix.ui.screens.LinkScreen
import org.sworrl.beaconfix.ui.screens.SettingsScreen
import org.sworrl.beaconfix.ui.screens.SurveyScreen
import org.sworrl.beaconfix.ui.screens.SyncScreen
import org.sworrl.beaconfix.ui.screens.TripScreen
import org.sworrl.beaconfix.ui.screens.WidgetGalleryScreen
import org.sworrl.beaconfix.ui.screens.ImportScreen
import org.sworrl.beaconfix.ui.vm.IdentityViewModel
import org.sworrl.beaconfix.ui.vm.RootViewModel
import org.sworrl.beaconfix.widget.WidgetEntryPoint

data class Dest(val route: String, val label: String, val icon: ImageVector, val phoneTab: Boolean)
val DESTS = listOf(
    Dest("home", "Home", Icons.Default.Home, true), Dest("map", "Map", Icons.Default.Map, true), Dest("beacons", "Beacons", Icons.Default.Wifi, true),
    Dest("nearby", "Places", Icons.Default.Place, true), Dest("more", "More", Icons.Default.MoreHoriz, true),
    Dest("detector", "Detector", Icons.Default.Radar, false),
    Dest("trip", "Trip", Icons.Default.Explore, false), Dest("events", "Events", Icons.Default.RssFeed, false), Dest("survey", "Survey", Icons.Default.Radar, false),
    Dest("sync", "Sync", Icons.Default.Sync, false), Dest("settings", "Settings", Icons.Default.Settings, false),
)

@Composable
fun BeaconFixRoot(launch: LaunchArgs = LaunchArgs()) {
    val idVm: IdentityViewModel = hiltViewModel()
    val rootVm: RootViewModel = hiltViewModel()
    val hasIdentity by rootVm.hasIdentity.collectAsState()
    val ctx = LocalContext.current
    val prefs = remember { EntryPointAccessors.fromApplication(ctx.applicationContext, WidgetEntryPoint::class.java).prefs() }
    LaunchedEffect(launch.seq) {
        // MainActivity is exported, so any app can send these extras: identity import and wipe are adb automation only
        // (a debug build, or Settings → Developer automation on).
        val dev = BuildConfig.DEBUG || prefs.devAutomation.first()
        if (dev && !launch.importHost.isNullOrBlank() && !launch.importCode.isNullOrBlank()) { if (launch.importDryRun) idVm.checkImportFromCode(launch.importHost, launch.importCode, launch.importPass ?: "") else idVm.importFromCodeAndPass(launch.importHost, launch.importCode, launch.importPass ?: "") }
        if (dev && launch.action == "forget_identity") idVm.forgetAll()
        if (!launch.linkPayload.isNullOrBlank() && launch.linkPayload.startsWith(org.sworrl.beaconfix.identity.IdentityOps.PREFIX)) idVm.handleScanned(launch.linkPayload)
    }
    // No identity yet → onboarding takes over the whole screen (any app: create or import)
    // "Link a PC" on the first screen: the identity is made, then the Link screen opens
    var linkAfterOnboarding by androidx.compose.runtime.saveable.rememberSaveable { androidx.compose.runtime.mutableStateOf(false) }
    if (hasIdentity == false) { OnboardingScreen(initialName = launch.identityName, onImportHistory = {}, onLinkPc = { linkAfterOnboarding = true }, vm = idVm); return }
    if (hasIdentity == null) return
    val nav = rememberNavController()
    val shareVm: ShareViewModel = hiltViewModel()
    LaunchedEffect(linkAfterOnboarding) { if (linkAfterOnboarding) { linkAfterOnboarding = false; nav.navigate("link") } }
    LaunchedEffect(launch.seq) {
        // A pairing request from outside (a tapped beaconfix://pair link, any app's pair_host extra) only fills in the
        // address and asks: a paired desktop receives this phone's location history and feeds the Help screen, widget,
        // tile and Call button. Only adb automation (a debug build, or Developer automation on) still pairs at once.
        if (!launch.pairHost.isNullOrBlank()) {
            val dev = BuildConfig.DEBUG || prefs.devAutomation.first()
            org.sworrl.beaconfix.nav.PairRequest.route(launch.pairHost, launch.pairPort, dev) { android.net.Uri.encode(it) }?.let { nav.navigate(it) }
        }
        // A link offer / statement from outside (a tapped link, another app) is never acted on unasked: it waits on the
        // Identity screen for the user's yes (linking lets that device sign in as us and receive our history).
        if (!launch.linkPayload.isNullOrBlank()) { if (!launch.linkPayload.startsWith(org.sworrl.beaconfix.identity.IdentityOps.PREFIX)) idVm.offerIncoming(launch.linkPayload); nav.navigate("identity") }
        if (launch.file != null) nav.navigate("import")
        if (launch.importDryRun) nav.navigate("identity")
        when (launch.action) {
            "sync" -> rootVm.syncNow(); "scan" -> rootVm.scanOnce(); "collector_on" -> rootVm.collector(true); "collector_off" -> rootVm.collector(false)
            "help" -> nav.navigate("help?focus="); "help_peds" -> nav.navigate("help?focus=peds"); "find_rv" -> nav.navigate("home") { popUpTo("home"); launchSingleTop = true }
            "share_location" -> shareVm.shareLocation(ctx)
            "sighting" -> launch.sightingUid?.let { nav.navigate("sighting/" + android.net.Uri.encode(it)) } ?: nav.navigate("sightings")
            "identity", "widgets", "import", "map", "beacons", "survey", "settings", "trip", "events", "nearby", "more", "sync_tab", "anchors", "detector", "hub", "alpr", "link", "sightings", "licenses" -> nav.navigate(if (launch.action == "sync_tab") "sync" else launch.action)
        }
    }
    val wide = currentWindowAdaptiveInfo().windowSizeClass.windowWidthSizeClass != WindowWidthSizeClass.COMPACT
    val entry by nav.currentBackStackEntryAsState()
    val dest = entry?.destination
    fun go(route: String) = nav.navigate(route) { popUpTo(nav.graph.startDestinationId) { saveState = true }; launchSingleTop = true; restoreState = true }
    val selected: (Dest) -> Boolean = { t -> dest?.hierarchy?.any { it.route == t.route } == true }
    if (wide) {
        Row(Modifier.fillMaxSize()) {
            NavigationRail { for (t in DESTS.filter { it.route != "more" }) NavigationRailItem(selected = selected(t), onClick = { go(t.route) }, icon = { Icon(t.icon, contentDescription = t.label) }, label = { Text(t.label) }) }
            Graph(nav, idVm, Modifier.fillMaxSize(), launch.file)
        }
    } else Scaffold(bottomBar = { NavigationBar { for (t in DESTS.filter { it.phoneTab }) NavigationBarItem(selected = selected(t), onClick = { go(t.route) }, icon = { Icon(t.icon, contentDescription = t.label) }, label = { Text(t.label) }) } }) { pad -> Graph(nav, idVm, Modifier.padding(pad), launch.file) }
}

@Composable
private fun Graph(nav: NavHostController, idVm: IdentityViewModel, modifier: Modifier, incomingFile: android.net.Uri? = null) {
    NavHost(nav, startDestination = "home", modifier = modifier) {
        composable("home") { HomeScreen(onPair = { nav.navigate("link") }, onIdentity = { nav.navigate("identity") }, onHelp = { nav.navigate("help?focus=") }, onMap = { nav.navigate("map") }) }
        composable("map") { MapScreen() }
        composable("beacons") { BeaconsScreen() }
        composable("nearby") { NearbyScreen(onHelp = { nav.navigate("help?focus=") }, onMap = { nav.navigate("map") }) }
        composable("more") { MoreScreen(onGo = { nav.navigate(it) }) }
        composable("trip") { TripScreen(onMap = { nav.navigate("map") }) }
        composable("help?focus={focus}") { e -> HelpScreen(focus = e.arguments?.getString("focus"), onBack = { nav.popBackStack() }, onMap = { nav.navigate("map") }) }
        composable("events") { EventsScreen() }
        composable("survey") { SurveyScreen() }
        composable("sync") { SyncScreen(onPair = { nav.navigate("link") }, onHub = { nav.navigate("hub") }) }
        composable("hub") { org.sworrl.beaconfix.ui.screens.HubScreen(onBack = { nav.popBackStack() }, onLink = { nav.navigate("link") }) }
        composable("link") { LinkScreen(onBack = { nav.popBackStack() }) }
        composable("alpr") { org.sworrl.beaconfix.alpr.ui.AlprScreen(onBack = { nav.popBackStack() }) }
        composable("detector") { org.sworrl.beaconfix.ui.screens.DetectorScreen(onAlpr = { nav.navigate("alpr") }) }
        composable("settings") { SettingsScreen(onPair = { nav.navigate("link") }, onIdentity = { nav.navigate("identity") }, onWidgets = { nav.navigate("widgets") }, onImport = { nav.navigate("import") }, onLicenses = { nav.navigate("licenses") }) }
        composable("licenses") { org.sworrl.beaconfix.ui.screens.LicensesScreen(onBack = { nav.popBackStack() }) }
        composable("sightings") { org.sworrl.beaconfix.sightings.ui.SightingsScreen(onOpen = { nav.navigate("sighting/" + android.net.Uri.encode(it)) }, onBack = { nav.popBackStack() }) }
        composable("sighting/{uid}") { e ->
            org.sworrl.beaconfix.sightings.ui.SightingDetailScreen(uid = e.arguments?.getString("uid").orEmpty(), onBack = { nav.popBackStack() }, onMap = { nav.navigate("map") })
        }
        composable("pair?host={host}&port={port}&auto={auto}") { entry ->
            // a beaconfix://pair link (or adb automation): the address becomes a row on the Link screen
            LinkScreen(onBack = { nav.popBackStack() }, hintHost = entry.arguments?.getString("host"),
                hintPort = entry.arguments?.getString("port")?.toIntOrNull() ?: 47822, autoLink = entry.arguments?.getString("auto") == "true")
        }
        composable("identity") { IdentityScreen(onBack = { nav.popBackStack() }, onLink = { nav.navigate("link") }, vm = idVm) }
        composable("anchors") { org.sworrl.beaconfix.ui.screens.AnchorsScreen(onBack = { nav.popBackStack() }, onMap = { nav.navigate("map") }) }
        composable("widgets") { WidgetGalleryScreen(onBack = { nav.popBackStack() }) }
        composable("import") { ImportScreen(onBack = { nav.popBackStack() }, incoming = incomingFile) }
    }
}
