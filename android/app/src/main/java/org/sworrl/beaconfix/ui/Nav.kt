package org.sworrl.beaconfix.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Map
import androidx.compose.material.icons.filled.Radar
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Sync
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.navigation.NavDestination.Companion.hierarchy
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import org.sworrl.beaconfix.R
import org.sworrl.beaconfix.ui.screens.BeaconsScreen
import org.sworrl.beaconfix.ui.screens.HomeScreen
import org.sworrl.beaconfix.ui.screens.MapScreen
import org.sworrl.beaconfix.ui.screens.PairScreen
import org.sworrl.beaconfix.ui.screens.SettingsScreen
import org.sworrl.beaconfix.ui.screens.SurveyScreen
import org.sworrl.beaconfix.ui.screens.SyncScreen

enum class Tab(val route: String, val label: Int, val icon: ImageVector) {
    Home("home", R.string.nav_home, Icons.Default.Home),
    Map("map", R.string.nav_map, Icons.Default.Map),
    Beacons("beacons", R.string.nav_beacons, Icons.Default.Wifi),
    Survey("survey", R.string.nav_survey, Icons.Default.Radar),
    Sync("sync", R.string.nav_sync, Icons.Default.Sync),
    Settings("settings", R.string.nav_settings, Icons.Default.Settings),
}

@Composable
fun BeaconFixRoot() {
    val nav = rememberNavController()
    val entry by nav.currentBackStackEntryAsState()
    val dest = entry?.destination
    Scaffold(bottomBar = {
        NavigationBar {
            Tab.entries.forEach { t ->
                NavigationBarItem(
                    selected = dest?.hierarchy?.any { it.route == t.route } == true,
                    onClick = { nav.navigate(t.route) { popUpTo(nav.graph.startDestinationId) { saveState = true }; launchSingleTop = true; restoreState = true } },
                    icon = { Icon(t.icon, contentDescription = stringResource(t.label)) },
                    label = { Text(stringResource(t.label)) },
                )
            }
        }
    }) { pad ->
        NavHost(nav, startDestination = Tab.Home.route, modifier = Modifier.padding(pad)) {
            composable(Tab.Home.route) { HomeScreen(onPair = { nav.navigate("pair") }) }
            composable(Tab.Map.route) { MapScreen() }
            composable(Tab.Beacons.route) { BeaconsScreen() }
            composable(Tab.Survey.route) { SurveyScreen() }
            composable(Tab.Sync.route) { SyncScreen(onPair = { nav.navigate("pair") }) }
            composable(Tab.Settings.route) { SettingsScreen(onPair = { nav.navigate("pair") }) }
            composable("pair") { PairScreen(onDone = { nav.popBackStack() }) }
        }
    }
}
