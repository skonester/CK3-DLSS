package com.ck3dlss.installer

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CheckboxDefaults
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.WindowPosition
import androidx.compose.ui.window.WindowState
import androidx.compose.ui.window.application
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.awt.Dimension
import java.awt.EventQueue
import java.awt.Window as AwtWindow
import java.io.File
import java.time.Instant
import java.time.LocalDate
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import javax.swing.JFileChooser
import javax.swing.UIManager

// Palette: near-black neutrals with NVIDIA green as the single accent.
private val Canvas = Color(0xFF0D100F)
private val Panel = Color(0xFF151917)
private val PanelRaised = Color(0xFF1B201E)
private val Hairline = Color(0xFF2A312E)
private val TextMain = Color(0xFFE9EEEB)
private val TextMuted = Color(0xFF94A099)
private val Green = Color(0xFF76B900)
private val GreenText = Color(0xFFA6E04E)
private val Amber = Color(0xFFF0B429)
private val Red = Color(0xFFF2645A)
private val Blue = Color(0xFF5AA9F2)

private val InstallerColors: ColorScheme = darkColorScheme(
    primary = Green,
    onPrimary = Color(0xFF0E1600),
    secondary = GreenText,
    background = Canvas,
    onBackground = TextMain,
    surface = Panel,
    onSurface = TextMain,
    surfaceVariant = PanelRaised,
    onSurfaceVariant = TextMuted,
    outline = Hairline,
    error = Red,
)

private sealed interface Outcome {
    data object Idle : Outcome
    data class Running(val step: String) : Outcome
    data class Done(val ok: Boolean, val message: String) : Outcome
}

fun main(args: Array<String>) {
    runCatching { UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName()) }
    val explicitRoot = args.indexOf("--package-root")
        .takeIf { it >= 0 && it + 1 < args.size }
        ?.let { args[it + 1] }
    val backend = InstallerBackend(InstallerBackend.locatePackageRoot(explicitRoot))

    application {
        Window(
            onCloseRequest = ::exitApplication,
            title = "CK3 DLSS",
            state = WindowState(width = 1040.dp, height = 830.dp, position = WindowPosition(Alignment.Center)),
            icon = painterResource("icon.png"),
        ) {
            LaunchedEffect(Unit) { window.minimumSize = Dimension(920, 780) }
            MaterialTheme(colorScheme = InstallerColors) {
                InstallerScreen(backend, window)
            }
        }
    }
}

@Composable
private fun InstallerScreen(backend: InstallerBackend, owner: AwtWindow) {
    val scope = rememberCoroutineScope()
    var gameRoot by remember { mutableStateOf(backend.defaultGameRoot().toString()) }
    val validation = backend.validateGameRoot(gameRoot)
    val root = validation.normalizedRoot.takeIf { validation.valid }

    var status by remember { mutableStateOf<InstallStatus?>(null) }
    var gameRunning by remember { mutableStateOf(false) }
    var uplift by remember { mutableStateOf<Boolean?>(null) }
    var selected by remember { mutableStateOf<InstallProfile?>(null) }
    var licensesAccepted by remember { mutableStateOf(false) }
    var outcome by remember { mutableStateOf<Outcome>(Outcome.Idle) }
    var log by remember { mutableStateOf("") }
    var showDetails by remember { mutableStateOf(false) }
    var confirmDisable by remember { mutableStateOf(false) }
    var confirmUninstall by remember { mutableStateOf(false) }
    var uninstalled by remember { mutableStateOf(false) }
    val busy = outcome is Outcome.Running

    // Keep the status tiles current: after operations, after the game closes, or a folder change.
    LaunchedEffect(root, busy) {
        while (true) {
            val (s, running) = withContext(Dispatchers.IO) {
                (root?.let(backend::readStatus)) to backend.isGameRunning()
            }
            status = s
            gameRunning = running
            uplift = withContext(Dispatchers.IO) { root?.let(backend::readNeuralUplift) }
            if (selected == null && s != null) selected = s.profile ?: InstallProfile.DLSS45
            delay(3000)
        }
    }

    fun run(firstStep: String, work: (onLine: (String) -> Unit) -> CommandResult) {
        if (busy) return
        outcome = Outcome.Running(firstStep)
        log += (if (log.isEmpty()) "" else "\n") + "> $firstStep\n"
        scope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    work { line ->
                        EventQueue.invokeLater {
                            log = (log + line + "\n").takeLast(120_000)
                            InstallerBackend.friendlyStep(line)?.let { outcome = Outcome.Running(it) }
                        }
                    }
                }.getOrElse { CommandResult(1, it.message ?: "Something went wrong.") }
            }
            outcome = Outcome.Done(result.succeeded, result.summary)
            status = withContext(Dispatchers.IO) { root?.let(backend::readStatus) }
        }
    }

    val profile = selected ?: InstallProfile.DLSS45
    val active = status?.profile
    val canAct = backend.packageReady() && root != null && !busy

    Surface(modifier = Modifier.fillMaxSize(), color = Canvas) {
        Column(
            modifier = Modifier.fillMaxSize().padding(horizontal = 32.dp, vertical = 26.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            TopBar(gameRunning)

            Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
                FolderTile(
                    validation = validation,
                    enabled = !busy,
                    onChange = { chooseGameFolder(owner, gameRoot)?.let { gameRoot = it.absolutePath } },
                    modifier = Modifier.weight(1.25f),
                )
                StatusTile(
                    label = "Active profile",
                    value = active?.title ?: "None",
                    detail = status?.feederBuild?.let { "Feeder $it" } ?: "DLSS isn't installed yet",
                    dot = if (active != null) GreenText else TextMuted,
                    modifier = Modifier.weight(1f),
                )
                StatusTile(
                    label = "Last game session",
                    value = status?.healthDetail ?: "Unknown",
                    detail = status?.lastSession?.let(::friendlyTime) ?: "Nothing to report yet",
                    dot = when (status?.health) {
                        SessionHealth.WORKING -> GreenText
                        SessionHealth.FAILED -> Red
                        else -> TextMuted
                    },
                    modifier = Modifier.weight(1f),
                )
            }

            if (!backend.packageReady() && !uninstalled) {
                Banner(Red, "Installer files are missing", "Open this app from the extracted CK3 DLSS package folder.")
            }

            Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                SectionTitle("Choose a profile")
                InstallProfile.entries.chunked(2).forEach { pair ->
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp), modifier = Modifier.fillMaxWidth()) {
                        pair.forEach { p ->
                            ProfileCard(
                                profile = p,
                                selected = p == profile,
                                active = p == active,
                                enabled = !busy,
                                onSelect = { selected = p },
                                modifier = Modifier.weight(1f),
                            )
                        }
                    }
                }
            }

            if (active != null) {
                InGameStrip(
                    neural = active.usesNeuralRendering,
                    uplift = uplift,
                    canToggle = root != null && !gameRunning && !busy,
                    gameRunning = gameRunning,
                    onToggle = { on ->
                        val target = root ?: return@InGameStrip
                        if (backend.setNeuralUplift(target, on)) uplift = on
                    },
                )
            }

            Spacer(Modifier.weight(1f))

            ActionPanel(
                outcome = outcome,
                showDetails = showDetails,
                onToggleDetails = { showDetails = !showDetails },
                log = log,
                licensesAccepted = licensesAccepted,
                onLicenses = { licensesAccepted = it },
                onViewNotices = {
                    if (!backend.openThirdPartyNotices()) outcome = Outcome.Done(false, "The license notices file couldn't be opened.")
                },
                busy = busy,
                primaryLabel = when (active) {
                    null -> "Install ${profile.title}"
                    profile -> "Reinstall ${profile.title}"
                    else -> "Switch to ${profile.title}"
                },
                primaryEnabled = canAct && licensesAccepted && !gameRunning,
                onPrimary = {
                    val target = root ?: return@ActionPanel
                    run("Installing ${profile.title}") { onLine -> backend.runAction(InstallerAction.INSTALL, profile, target, onLine) }
                },
                launchEnabled = canAct && active != null && !gameRunning,
                onLaunch = {
                    val target = root ?: return@ActionPanel
                    run("Checking the install before launch") { onLine -> backend.launchGame(target, onLine) }
                },
                checkEnabled = canAct && active != null,
                onCheck = {
                    val target = root ?: return@ActionPanel
                    run("Checking the install") { onLine -> backend.runAction(InstallerAction.VALIDATE, profile, target, onLine) }
                },
                offEnabled = canAct && active != null && !gameRunning,
                onOff = { confirmDisable = true },
                uninstallEnabled = root != null && !busy && !gameRunning && !uninstalled,
                onUninstall = { confirmUninstall = true },
                gameRunning = gameRunning,
            )
        }

        if (confirmUninstall) {
            val count = remember(root) { root?.let { runCatching { backend.uninstallTargets(it).size }.getOrDefault(0) } ?: 0 }
            AlertDialog(
                onDismissRequest = { confirmUninstall = false },
                containerColor = PanelRaised,
                title = { Text("Uninstall CK3 DLSS?") },
                text = {
                    Text(
                        "This puts CK3 back on the renderer it used before, then removes everything this package added " +
                            "($count items: DLSS files, ReShade, shaders, scripts and this installer). " +
                            "Your game files, saves and screenshots are not touched.",
                        color = TextMuted,
                    )
                },
                confirmButton = {
                    Button(
                        onClick = {
                            confirmUninstall = false
                            val target = root ?: return@Button
                            run("Uninstalling") { onLine ->
                                backend.uninstall(target, onLine).also { if (it.succeeded) uninstalled = true }
                            }
                        },
                        colors = ButtonDefaults.buttonColors(containerColor = Red, contentColor = Color.White),
                    ) { Text("Uninstall") }
                },
                dismissButton = { TextButton(onClick = { confirmUninstall = false }) { Text("Cancel", color = TextMain) } },
            )
        }

        if (confirmDisable) {
            AlertDialog(
                onDismissRequest = { confirmDisable = false },
                containerColor = PanelRaised,
                title = { Text("Turn off DLSS?") },
                text = {
                    Text(
                        "CK3 goes back to the renderer it used before DLSS was installed. Your profiles stay on disk, so you can switch back any time.",
                        color = TextMuted,
                    )
                },
                confirmButton = {
                    Button(
                        onClick = {
                            confirmDisable = false
                            val target = root ?: return@Button
                            run("Turning off DLSS") { onLine -> backend.runAction(InstallerAction.DISABLE, profile, target, onLine) }
                        },
                        colors = ButtonDefaults.buttonColors(containerColor = Red, contentColor = Color.White),
                    ) { Text("Turn off") }
                },
                dismissButton = { TextButton(onClick = { confirmDisable = false }) { Text("Cancel", color = TextMain) } },
            )
        }
    }
}

@Composable
private fun TopBar(gameRunning: Boolean) {
    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.fillMaxWidth()) {
        Image(
            painter = painterResource("icon.png"),
            contentDescription = null,
            modifier = Modifier.size(42.dp).clip(RoundedCornerShape(10.dp)),
        )
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Text("CK3 DLSS", fontSize = 24.sp, fontWeight = FontWeight.Bold, color = TextMain)
            Text("Set up NVIDIA DLSS for Crusader Kings III", fontSize = 13.sp, color = TextMuted)
        }
        if (gameRunning) Chip("Game running: close it to make changes", Amber)
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, fontSize = 15.sp, fontWeight = FontWeight.SemiBold, color = TextMain)
}

@Composable
private fun Tile(modifier: Modifier, content: @Composable () -> Unit) {
    Surface(
        modifier = modifier.height(108.dp),
        color = Panel,
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(1.dp, Hairline),
    ) {
        Box(Modifier.padding(horizontal = 16.dp, vertical = 13.dp)) { content() }
    }
}

@Composable
private fun TileLabel(text: String) {
    Text(text.uppercase(), fontSize = 11.sp, letterSpacing = 0.8.sp, fontWeight = FontWeight.SemiBold, color = TextMuted)
}

@Composable
private fun FolderTile(validation: GameRootValidation, enabled: Boolean, onChange: () -> Unit, modifier: Modifier) {
    Tile(modifier) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                TileLabel("Game folder")
                Spacer(Modifier.weight(1f))
                Text(
                    "Change",
                    color = if (enabled) GreenText else TextMuted,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.clip(RoundedCornerShape(6.dp)).clickable(enabled = enabled, onClick = onChange)
                        .padding(horizontal = 6.dp, vertical = 2.dp),
                )
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Dot(if (validation.valid) GreenText else Red)
                Spacer(Modifier.width(8.dp))
                Text(
                    validation.message,
                    color = if (validation.valid) TextMain else Red,
                    fontWeight = FontWeight.SemiBold,
                    fontSize = 15.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            Text(
                validation.normalizedRoot?.toString() ?: "",
                color = TextMuted,
                fontSize = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun StatusTile(label: String, value: String, detail: String, dot: Color, modifier: Modifier) {
    Tile(modifier) {
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            TileLabel(label)
            Row(verticalAlignment = Alignment.CenterVertically) {
                Dot(dot)
                Spacer(Modifier.width(8.dp))
                Text(value, color = TextMain, fontWeight = FontWeight.SemiBold, fontSize = 15.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Text(detail, color = TextMuted, fontSize = 12.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
    }
}

@Composable
private fun Dot(color: Color) {
    Box(Modifier.size(8.dp).background(color, CircleShape))
}

@Composable
private fun Chip(text: String, color: Color) {
    Surface(
        color = color.copy(alpha = 0.12f),
        shape = RoundedCornerShape(999.dp),
        border = BorderStroke(1.dp, color.copy(alpha = 0.45f)),
    ) {
        Text(
            text,
            color = color,
            fontSize = 11.sp,
            fontWeight = FontWeight.SemiBold,
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
        )
    }
}

@Composable
private fun ProfileCard(
    profile: InstallProfile,
    selected: Boolean,
    active: Boolean,
    enabled: Boolean,
    onSelect: () -> Unit,
    modifier: Modifier,
) {
    val tagColor = when (profile.tag) {
        ProfileTag.RECOMMENDED -> GreenText
        ProfileTag.PREVIEW -> Blue
        ProfileTag.EXPERIMENTAL -> Amber
    }
    Surface(
        modifier = modifier
            .height(104.dp)
            .clip(RoundedCornerShape(14.dp))
            .clickable(enabled = enabled, onClick = onSelect),
        color = if (selected) Color(0xFF1A2216) else Panel,
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(if (selected) 2.dp else 1.dp, if (selected) Green else Hairline),
    ) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.Top) {
            Box(
                Modifier
                    .padding(top = 2.dp)
                    .size(18.dp)
                    .border(2.dp, if (selected) Green else TextMuted, CircleShape),
                contentAlignment = Alignment.Center,
            ) {
                if (selected) Box(Modifier.size(8.dp).background(Green, CircleShape))
            }
            Spacer(Modifier.width(12.dp))
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(profile.title, fontSize = 16.sp, fontWeight = FontWeight.Bold, color = TextMain)
                    Chip(profile.tag.label, tagColor)
                    if (active) Chip("Active", GreenText)
                }
                Text(profile.summary, fontSize = 13.sp, color = TextMuted, lineHeight = 18.sp)
            }
        }
    }
}

@Composable
private fun Key(label: String) {
    Surface(color = PanelRaised, shape = RoundedCornerShape(6.dp), border = BorderStroke(1.dp, Hairline)) {
        Text(label, color = TextMain, fontSize = 12.sp, fontWeight = FontWeight.Bold, modifier = Modifier.padding(horizontal = 8.dp, vertical = 2.dp))
    }
}

/** How to use it once the game is running, plus the DLSS 5 on/off switch that F6 also controls. */
@Composable
private fun InGameStrip(neural: Boolean, uplift: Boolean?, canToggle: Boolean, gameRunning: Boolean, onToggle: (Boolean) -> Unit) {
    Surface(
        color = Panel,
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(1.dp, Hairline),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("In the game", fontSize = 13.sp, fontWeight = FontWeight.SemiBold, color = TextMain)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (neural) {
                        Key("F6")
                        Text("turns DLSS 5 on or off", fontSize = 13.sp, color = TextMuted)
                        Spacer(Modifier.width(10.dp))
                    }
                    Key("Home")
                    Text("opens the ReShade menu", fontSize = 13.sp, color = TextMuted)
                }
            }
            if (neural && uplift != null) {
                Column(horizontalAlignment = Alignment.End) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text(
                            if (uplift) "DLSS 5 is on" else "DLSS 5 is off",
                            color = if (uplift) GreenText else Amber,
                            fontSize = 13.sp,
                            fontWeight = FontWeight.SemiBold,
                        )
                        Spacer(Modifier.width(10.dp))
                        Switch(
                            checked = uplift,
                            onCheckedChange = onToggle,
                            enabled = canToggle,
                            colors = SwitchDefaults.colors(checkedTrackColor = Green, checkedThumbColor = Color.White),
                        )
                    }
                    Text(
                        if (gameRunning) "Use F6 while playing" else "F6 remembers your choice for next time",
                        fontSize = 11.sp,
                        color = TextMuted,
                    )
                }
            }
        }
    }
}

@Composable
private fun Banner(color: Color, title: String, body: String) {
    Surface(
        color = color.copy(alpha = 0.10f),
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, color.copy(alpha = 0.4f)),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Text(title, color = color, fontWeight = FontWeight.SemiBold)
            Text(body, color = TextMuted, fontSize = 13.sp)
        }
    }
}

@Composable
private fun ActionPanel(
    outcome: Outcome,
    showDetails: Boolean,
    onToggleDetails: () -> Unit,
    log: String,
    licensesAccepted: Boolean,
    onLicenses: (Boolean) -> Unit,
    onViewNotices: () -> Unit,
    busy: Boolean,
    primaryLabel: String,
    primaryEnabled: Boolean,
    onPrimary: () -> Unit,
    launchEnabled: Boolean,
    onLaunch: () -> Unit,
    checkEnabled: Boolean,
    onCheck: () -> Unit,
    offEnabled: Boolean,
    onOff: () -> Unit,
    uninstallEnabled: Boolean,
    onUninstall: () -> Unit,
    gameRunning: Boolean,
) {
    Surface(
        color = Panel,
        shape = RoundedCornerShape(16.dp),
        border = BorderStroke(1.dp, Hairline),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(horizontal = 18.dp, vertical = 14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            // Progress / result line
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.heightIn(min = 40.dp)) {
                when (outcome) {
                    is Outcome.Idle -> Text(
                        if (gameRunning) "Close Crusader Kings III to install or switch profiles."
                        else "Pick a profile, then install. Switching keeps your settings for each profile.",
                        color = TextMuted,
                        fontSize = 13.sp,
                        modifier = Modifier.weight(1f),
                    )
                    is Outcome.Running -> Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text(outcome.step, color = TextMain, fontSize = 13.sp, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        LinearProgressIndicator(
                            modifier = Modifier.fillMaxWidth().height(4.dp).clip(RoundedCornerShape(2.dp)),
                            color = Green,
                            trackColor = Hairline,
                        )
                    }
                    is Outcome.Done -> Row(Modifier.weight(1f), verticalAlignment = Alignment.CenterVertically) {
                        Dot(if (outcome.ok) GreenText else Red)
                        Spacer(Modifier.width(10.dp))
                        SelectionContainer {
                            Text(
                                outcome.message,
                                color = if (outcome.ok) GreenText else Red,
                                fontWeight = FontWeight.SemiBold,
                                fontSize = 14.sp,
                                maxLines = 3,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
                if (log.isNotEmpty()) {
                    Spacer(Modifier.width(12.dp))
                    TextButton(onClick = { copyToClipboard(log) }) {
                        Text("Copy log", color = TextMuted, fontSize = 13.sp)
                    }
                    TextButton(onClick = onToggleDetails) {
                        Text(if (showDetails) "Hide details" else "Details", color = TextMuted, fontSize = 13.sp)
                    }
                }
            }

            AnimatedVisibility(visible = showDetails && log.isNotEmpty()) {
                val scroll = rememberScrollState()
                LaunchedEffect(log) { scroll.scrollTo(scroll.maxValue) }
                Surface(
                    color = Canvas,
                    shape = RoundedCornerShape(10.dp),
                    border = BorderStroke(1.dp, Hairline),
                    modifier = Modifier.fillMaxWidth().heightIn(max = 170.dp),
                ) {
                    SelectionContainer(Modifier.verticalScroll(scroll).padding(12.dp)) {
                        Text(
                            log,
                            color = Color(0xFFB7C4BC),
                            fontFamily = FontFamily.Monospace,
                            fontSize = 11.sp,
                            lineHeight = 15.sp,
                        )
                    }
                }
            }

            Row(verticalAlignment = Alignment.CenterVertically) {
                Checkbox(
                    checked = licensesAccepted,
                    onCheckedChange = onLicenses,
                    enabled = !busy,
                    colors = CheckboxDefaults.colors(checkedColor = Green, checkmarkColor = Color(0xFF0E1600)),
                )
                Text(
                    "I accept the NVIDIA and third-party license terms",
                    fontSize = 13.sp,
                    color = TextMain,
                    modifier = Modifier.clickable(enabled = !busy) { onLicenses(!licensesAccepted) },
                )
                TextButton(onClick = onViewNotices) { Text("Read them", color = GreenText, fontSize = 13.sp) }
            }

            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                TextButton(onClick = onUninstall, enabled = uninstallEnabled) {
                    Text("Uninstall", color = if (uninstallEnabled) Red else TextMuted, fontSize = 13.sp)
                }
                TextButton(onClick = onOff, enabled = offEnabled) {
                    Text("Turn off DLSS", color = if (offEnabled) TextMain else TextMuted, fontSize = 13.sp)
                }
                Spacer(Modifier.weight(1f))
                OutlinedButton(onClick = onCheck, enabled = checkEnabled, border = BorderStroke(1.dp, Hairline), modifier = Modifier.height(42.dp)) {
                    Text("Check install", color = if (checkEnabled) TextMain else TextMuted)
                }
                OutlinedButton(onClick = onLaunch, enabled = launchEnabled, border = BorderStroke(1.dp, Hairline), modifier = Modifier.height(42.dp)) {
                    Text("Launch CK3", color = if (launchEnabled) TextMain else TextMuted)
                }
                Button(
                    onClick = onPrimary,
                    enabled = primaryEnabled,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Green,
                        contentColor = Color(0xFF0E1600),
                        disabledContainerColor = PanelRaised,
                        disabledContentColor = TextMuted,
                    ),
                    modifier = Modifier.height(42.dp),
                ) {
                    Text(primaryLabel, fontWeight = FontWeight.Bold)
                }
            }
        }
    }
}

private fun copyToClipboard(text: String) {
    runCatching {
        java.awt.Toolkit.getDefaultToolkit().systemClipboard.setContents(java.awt.datatransfer.StringSelection(text), null)
    }
}

private fun friendlyTime(instant: Instant): String {
    val local = instant.atZone(ZoneId.systemDefault())
    val today = LocalDate.now()
    val time = local.format(DateTimeFormatter.ofPattern("h:mm a"))
    return when (local.toLocalDate()) {
        today -> "Today, $time"
        today.minusDays(1) -> "Yesterday, $time"
        else -> local.format(DateTimeFormatter.ofPattern("MMM d, h:mm a"))
    }
}

private fun chooseGameFolder(owner: AwtWindow, currentPath: String): File? {
    val current = File(currentPath).takeIf(File::exists)
    val chooser = JFileChooser(current).apply {
        dialogTitle = "Select your Crusader Kings III folder"
        fileSelectionMode = JFileChooser.DIRECTORIES_ONLY
        isAcceptAllFileFilterUsed = false
        selectedFile = current
    }
    return if (chooser.showOpenDialog(owner) == JFileChooser.APPROVE_OPTION) chooser.selectedFile else null
}

