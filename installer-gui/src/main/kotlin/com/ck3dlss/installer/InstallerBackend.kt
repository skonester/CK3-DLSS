package com.ck3dlss.installer

import java.awt.Desktop
import java.nio.charset.Charset
import java.nio.file.Files
import java.nio.file.InvalidPathException
import java.nio.file.Path
import java.time.Instant
import kotlin.io.path.absolute
import kotlin.io.path.exists
import kotlin.io.path.getLastModifiedTime
import kotlin.io.path.isDirectory
import kotlin.io.path.isRegularFile
import kotlin.io.path.name

enum class ProfileTag(val label: String) { RECOMMENDED("Recommended"), PREVIEW("Preview"), EXPERIMENTAL("Experimental") }

enum class InstallProfile(
    val scriptValue: String,
    val title: String,
    val tag: ProfileTag,
    val summary: String,
    val usesNeuralRendering: Boolean,
) {
    DLSS45(
        scriptValue = "DLSS45",
        title = "DLSS 4.5",
        tag = ProfileTag.RECOMMENDED,
        summary = "Model M anti-aliasing at native resolution. Stable on RTX 20, 30 and 40 cards.",
        usesNeuralRendering = false,
    ),
    DLSS5(
        scriptValue = "DLSS5",
        title = "DLSS 5",
        tag = ProfileTag.PREVIEW,
        summary = "NVIDIA's signed DLSS 5 neural rendering, for cards the preview supports.",
        usesNeuralRendering = true,
    ),
    DLSS5_EXTENDED(
        scriptValue = "DLSS5Extended",
        title = "DLSS 5 Extended",
        tag = ProfileTag.EXPERIMENTAL,
        summary = "DLSS 5 on more cards through the ShortFuse runtime. Used for portrait mode.",
        usesNeuralRendering = true,
    ),
    NATIVE_STREAMLINE(
        scriptValue = "NativeStreamline",
        title = "Native Streamline",
        tag = ProfileTag.EXPERIMENTAL,
        summary = "Routes Vulkan through NVIDIA Streamline. Still being tested.",
        usesNeuralRendering = true,
    ),
    ;

    companion object {
        fun fromScriptValue(value: String): InstallProfile? =
            entries.firstOrNull { it.scriptValue.equals(value.trim(), ignoreCase = true) }
    }
}

enum class InstallerAction(val scriptAction: String, val label: String) {
    INSTALL("Install", "Install"),
    VALIDATE("Validate", "Check"),
    DISABLE("Disable", "Turn off"),
}

data class GameRootValidation(
    val valid: Boolean,
    val normalizedRoot: Path?,
    val message: String,
)

data class CommandResult(
    val exitCode: Int,
    val summary: String,
) {
    val succeeded: Boolean get() = exitCode == 0
}

/** What the last game session says about DLSS, read from the logs it left behind. */
enum class SessionHealth { NO_SESSION, WORKING, FAILED }

data class InstallStatus(
    val profile: InstallProfile?,
    val feederBuild: String?,
    val health: SessionHealth,
    val healthDetail: String,
    val lastSession: Instant?,
)

class InstallerBackend(
    val packageRoot: Path,
    private val gameRunningCheck: () -> Boolean = ::ck3ProcessRunning,
) {
    val installerScript: Path = packageRoot.resolve("DLSS5-CK3.ps1")

    fun packageReady(): Boolean = isCompletePackage(packageRoot)

    /**
     * Copies the package (scripts, shortcuts, ReShade, layers, shaders, feeder, this app) into the
     * game folder, which is what "extract into the CK3 folder" used to require by hand. Existing
     * ReShade.ini / DLSS5-CK3.ini keep the user's settings. Nothing happens when the package already
     * is the game folder.
     */
    fun deployPackage(gameRoot: Path, onLine: (String) -> Unit): Int {
        val source = packageRoot.absolute().normalize()
        val target = gameRoot.absolute().normalize()
        if (source == target) return 0
        var copied = 0
        Files.walk(source).use { stream ->
            stream.filter { it.isRegularFile() }.forEach { file ->
                val rel = source.relativize(file)
                val relText = rel.toString().replace('\\', '/')
                val dest = target.resolve(rel.toString())
                if (relText in KeepUserCopy && dest.exists()) return@forEach
                Files.createDirectories(dest.parent)
                Files.copy(file, dest, java.nio.file.StandardCopyOption.REPLACE_EXISTING)
                copied++
            }
        }
        onLine("[CK3 DLSS] Copied $copied package files into the game folder")
        return copied
    }

    fun validateGameRoot(value: String): GameRootValidation {
        val candidate = try {
            Path.of(value.trim().trim('"')).absolute().normalize()
        } catch (_: InvalidPathException) {
            return GameRootValidation(false, null, "That folder path isn't valid.")
        }

        val normalized = if (candidate.name.equals("binaries", ignoreCase = true) &&
            candidate.resolve("ck3.exe").isRegularFile()
        ) {
            candidate.parent
        } else {
            candidate
        }

        return when {
            !normalized.exists() -> GameRootValidation(false, normalized, "That folder doesn't exist.")
            !normalized.resolve("binaries/ck3.exe").isRegularFile() ->
                GameRootValidation(false, normalized, "Crusader Kings III isn't in this folder (no binaries\\ck3.exe).")
            else -> GameRootValidation(true, normalized, "Crusader Kings III found")
        }
    }

    fun defaultGameRoot(): Path {
        val candidates = buildList {
            System.getenv("CK3_GAME_ROOT")?.takeIf(String::isNotBlank)?.let { add(Path.of(it)) }
            add(packageRoot)
            System.getenv("ProgramFiles(x86)")?.let {
                add(Path.of(it, "Steam", "steamapps", "common", "Crusader Kings III"))
            }
            add(Path.of("C:/Program Files (x86)/Steam/steamapps/common/Crusader Kings III"))
        }
        return candidates.firstOrNull { validateGameRoot(it.toString()).valid } ?: packageRoot
    }

    fun readActiveProfile(gameRoot: Path): InstallProfile? {
        val receipt = gameRoot.resolve("binaries/dlss-active/CK3-DLSS-RUNTIME.json")
        if (!receipt.isRegularFile()) return null
        val value = Regex("\"Profile\"\\s*:\\s*\"([^\"]+)\"")
            .find(runCatching { Files.readString(receipt) }.getOrDefault(""))
            ?.groupValues
            ?.get(1)
            ?: return null
        return InstallProfile.fromScriptValue(value)
    }

    /** Build name compiled into the active feeder, e.g. "ck3-frontier2-atlas.1". */
    fun readFeederBuild(gameRoot: Path): String? {
        val addon = gameRoot.resolve("binaries/dlss-active/dlss5-feed.addon64")
        if (!addon.isRegularFile()) return null
        val text = runCatching { String(Files.readAllBytes(addon), Charsets.ISO_8859_1) }.getOrNull() ?: return null
        return FeederBuildPattern.find(text)?.value
    }

    fun readStatus(gameRoot: Path): InstallStatus {
        val profile = readActiveProfile(gameRoot)
        val feeder = readFeederBuild(gameRoot)
        val binaries = gameRoot.resolve("binaries")
        val (health, detail, time) = sessionHealth(profile, binaries)
        return InstallStatus(profile, feeder, health, detail, time)
    }

    private fun sessionHealth(profile: InstallProfile?, binaries: Path): Triple<SessionHealth, String, Instant?> {
        if (profile == null) return Triple(SessionHealth.NO_SESSION, "Not installed", null)
        val reshadeLog = binaries.resolve("ReShade.log")
        val feederLog = binaries.resolve("dlss-active/dlss5-feed.log")
        if (profile.usesNeuralRendering) {
            if (!reshadeLog.isRegularFile()) return Triple(SessionHealth.NO_SESSION, "No game session yet", null)
            val text = readText(reshadeLog)
            val time = runCatching { reshadeLog.getLastModifiedTime().toInstant() }.getOrNull()
            val worked = text.contains("evaluation succeeded")
            val failed = NeuralFailurePattern.containsMatchIn(text)
            return when {
                failed && !worked -> Triple(SessionHealth.FAILED, "DLSS 5 didn't start last session", time)
                worked && !failed -> Triple(SessionHealth.WORKING, "DLSS 5 ran last session", time)
                worked -> Triple(SessionHealth.FAILED, "DLSS 5 ran, then stopped with errors", time)
                else -> Triple(SessionHealth.NO_SESSION, "DLSS 5 hasn't run yet", time)
            }
        }
        if (!feederLog.isRegularFile()) return Triple(SessionHealth.NO_SESSION, "No game session yet", null)
        val text = readText(feederLog)
        val time = runCatching { feederLog.getLastModifiedTime().toInstant() }.getOrNull()
        return when {
            text.contains("delivered") -> Triple(SessionHealth.WORKING, "DLSS ran last session", time)
            text.contains("failure:") || text.contains("stopped:") -> Triple(SessionHealth.FAILED, "DLSS didn't start last session", time)
            else -> Triple(SessionHealth.NO_SESSION, "DLSS hasn't run yet", time)
        }
    }

    private fun readText(path: Path): String = runCatching {
        String(Files.readAllBytes(path), Charsets.UTF_8)
    }.getOrDefault("")

    fun isGameRunning(): Boolean = gameRunningCheck()

    fun buildPowerShellCommand(
        action: InstallerAction,
        profile: InstallProfile,
        gameRoot: Path,
    ): List<String> = buildList {
        add(findPowerShell())
        // NonInteractive: a script that would prompt fails with a message instead of hanging the app.
        addAll(listOf("-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", installerScript.toString()))
        addAll(listOf("-Action", action.scriptAction))
        if (action == InstallerAction.INSTALL) {
            addAll(listOf("-Profile", profile.scriptValue))
        }
        addAll(listOf("-GameRoot", gameRoot.toString()))
        if (action == InstallerAction.INSTALL) {
            add("-AcceptDependencyLicenses")
            add("-AcceptRuntimeLicenses")
        }
    }

    fun runAction(
        action: InstallerAction,
        profile: InstallProfile,
        gameRoot: Path,
        onLine: (String) -> Unit,
    ): CommandResult {
        if (!packageReady()) return CommandResult(2, "The installer files are missing. Run this app from the CK3 DLSS package.")
        val validation = validateGameRoot(gameRoot.toString())
        if (!validation.valid || validation.normalizedRoot == null) return CommandResult(2, validation.message)
        if (action != InstallerAction.VALIDATE && isGameRunning()) {
            return CommandResult(3, "Close Crusader Kings III first.")
        }

        if (action == InstallerAction.INSTALL) {
            onLine("[CK3 DLSS] Copying the package into the game folder")
            deployPackage(validation.normalizedRoot, onLine)
        }
        val process = ProcessBuilder(buildPowerShellCommand(action, profile, validation.normalizedRoot))
            .directory(packageRoot.toFile())
            .redirectErrorStream(true)
            .apply { environment()["POWERSHELL_TELEMETRY_OPTOUT"] = "1" }
            .start()
        process.outputStream.close()

        var lastError: String? = null
        process.inputStream.bufferedReader(Charset.defaultCharset()).useLines { lines ->
            lines.forEach { line ->
                if (line.startsWith("ERROR:")) lastError = line.removePrefix("ERROR:").trim()
                onLine(line)
            }
        }
        val exit = process.waitFor()
        val summary = when {
            exit == 0 && action == InstallerAction.INSTALL -> "${profile.title} is installed"
            exit == 0 && action == InstallerAction.VALIDATE -> "Everything checks out"
            exit == 0 -> "DLSS is turned off. CK3 will start normally."
            lastError != null -> lastError!!
            else -> "${action.label} didn't finish (code $exit). See details."
        }
        return CommandResult(exit, summary)
    }

    fun launchGame(gameRoot: Path, onLine: (String) -> Unit): CommandResult {
        if (isGameRunning()) return CommandResult(3, "Crusader Kings III is already running.")
        val validationResult = runAction(InstallerAction.VALIDATE, InstallProfile.DLSS45, gameRoot, onLine)
        if (!validationResult.succeeded) return validationResult

        val root = validateGameRoot(gameRoot.toString()).normalizedRoot
            ?: return CommandResult(2, "The CK3 folder is invalid.")
        val binaries = root.resolve("binaries")
        val executable = binaries.resolve("ck3.exe")
        val pid = startWithCleanDllSearch(
            executable, listOf("-gdpr-compliant"), binaries,
            mapOf(
                "VK_LAYER_PATH" to binaries.resolve("dlss5-vulkan").toString(),
                "VK_INSTANCE_LAYERS" to "VK_LAYER_feed_vk;VK_LAYER_reshade",
                "DISABLE_VK_LAYER_reshade_1" to "1",
                "RESHADE_BASE_PATH_OVERRIDE" to binaries.toString(),
            ),
        ) ?: return CommandResult(1, "CK3 couldn't be started. See details.")
        onLine("Started CK3 (process $pid) with the package-local Vulkan layers.")
        return CommandResult(0, "Crusader Kings III is starting")
    }

    /**
     * The app's Java launcher sets its runtime\bin folder as the process DLL directory, and Windows
     * passes that to every child. CK3 then loads the runtime's old MSVCP140.dll instead of the
     * system one, and the DLSS feeder crashes at device creation. Start the program from a helper
     * that resets the DLL directory first. Returns the started process id, or null on failure.
     */
    fun startWithCleanDllSearch(executable: Path, args: List<String>, workDir: Path, env: Map<String, String>): Long? {
        fun quote(s: String) = "'" + s.replace("'", "''") + "'"
        val argLine = args.joinToString(" ") { if (it.any(Char::isWhitespace)) "\"$it\"" else it }
        val script = """
            Add-Type -Namespace Ck3Dlss -Name Native -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern bool SetDllDirectory(string p);'
            [void][Ck3Dlss.Native]::SetDllDirectory(${'$'}null)
            ${'$'}psi = New-Object System.Diagnostics.ProcessStartInfo ${quote(executable.toString())}
            ${'$'}psi.Arguments = ${quote(argLine)}
            ${'$'}psi.WorkingDirectory = ${quote(workDir.toString())}
            ${'$'}psi.UseShellExecute = ${'$'}false
            'PID=' + [System.Diagnostics.Process]::Start(${'$'}psi).Id
        """.trimIndent()
        val encoded = java.util.Base64.getEncoder().encodeToString(script.toByteArray(Charsets.UTF_16LE))
        val helper = ProcessBuilder(findPowerShell(), "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-EncodedCommand", encoded)
            .directory(workDir.toFile())
            .redirectErrorStream(true)
            .apply { environment().putAll(env) }
            .start()
        helper.outputStream.close()
        val output = helper.inputStream.bufferedReader().readText()
        helper.waitFor()
        return Regex("PID=(\\d+)").find(output)?.groupValues?.get(1)?.toLongOrNull()
    }

    /** The DLSS 5 on/off state that F6 toggles in game ([RenoDX.DLSS5] NeuralUplift). Null when ReShade isn't set up. */
    fun readNeuralUplift(gameRoot: Path): Boolean? {
        val ini = gameRoot.resolve("binaries/ReShade.ini")
        if (!ini.isRegularFile()) return null
        var inSection = false
        for (line in readText(ini).lines()) {
            val t = line.trim()
            if (t.startsWith("[")) inSection = t.equals("[RenoDX.DLSS5]", ignoreCase = true)
            else if (inSection && t.startsWith("NeuralUplift=", ignoreCase = true)) return t.substringAfter('=').trim() != "0"
        }
        return true   // unset: the feeder turns it on at the next start
    }

    fun setNeuralUplift(gameRoot: Path, on: Boolean): Boolean {
        val ini = gameRoot.resolve("binaries/ReShade.ini")
        if (!ini.isRegularFile() || isGameRunning()) return false
        val text = readText(ini)
        val eol = if (text.contains("\r\n")) "\r\n" else "\n"
        val lines = text.split(eol).toMutableList()
        val value = "NeuralUplift=${if (on) 1 else 0}"
        val header = lines.indexOfFirst { it.trim().equals("[RenoDX.DLSS5]", ignoreCase = true) }
        if (header < 0) {
            if (lines.isNotEmpty() && lines.last().isNotEmpty()) lines += ""
            lines.addAll(listOf("[RenoDX.DLSS5]", value, ""))
        } else {
            var end = header + 1
            while (end < lines.size && !lines[end].trim().startsWith("[")) end++
            val key = (header + 1 until end).firstOrNull { lines[it].trim().startsWith("NeuralUplift=", ignoreCase = true) }
            if (key != null) lines[key] = value else lines.add(header + 1, value)
        }
        Files.write(ini, lines.joinToString(eol).toByteArray(Charsets.UTF_8))
        return true
    }

    /**
     * Everything the package or its installer put into the game folder, and nothing else:
     * the game's own files, Steam files, other mods and screenshots are never listed.
     */
    fun uninstallTargets(gameRoot: Path): List<Path> {
        val binaries = gameRoot.resolve("binaries")
        val found = mutableListOf<Path>()
        fun add(path: Path) { if (path.exists()) found.add(path) }

        Files.list(gameRoot).use { entries ->
            entries.filter { it.isRegularFile() && PackageCmdPattern.matches(it.name) }.forEach(found::add)
        }
        RootFiles.forEach { add(gameRoot.resolve(it)) }
        gameRoot.resolve("README.md").takeIf { readText(it).lineSequence().firstOrNull()?.contains("CK3 DLSS") == true }?.let(::add)
        add(gameRoot.resolve("tools/RHI-Setup.exe"))
        add(gameRoot.resolve("tools/CK3-DLSS-Installer"))
        if (binaries.isDirectory()) {
            BinaryEntries.forEach { add(binaries.resolve(it)) }
            Files.list(binaries).use { entries ->
                entries.filter { it.name.startsWith(".ck3-dlss-graphics-stage-") }.forEach(found::add)
            }
        }
        return found
    }

    /** Restores CK3's renderer, then removes every package file. The running app's own folder is removed after it exits. */
    fun uninstall(gameRoot: Path, onLine: (String) -> Unit): CommandResult {
        val validation = validateGameRoot(gameRoot.toString())
        val root = validation.normalizedRoot?.takeIf { validation.valid } ?: return CommandResult(2, validation.message)
        if (isGameRunning()) return CommandResult(3, "Close Crusader Kings III first.")

        val settingsBackup = readInstalledSettingsPath(root)?.let { Path.of("$it.ck3-dlss-backup") }
        var rendererRestored = false
        if (root.resolve("DLSS5-CK3.ps1").isRegularFile() || installerScript.isRegularFile()) {
            onLine("[CK3 DLSS] Restoring CK3's original renderer")
            val script = root.resolve("DLSS5-CK3.ps1").takeIf { it.isRegularFile() } ?: installerScript
            val process = ProcessBuilder(
                findPowerShell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script.toString(),
                "-Action", "Disable", "-GameRoot", root.toString(),
            ).redirectErrorStream(true).start()
            process.outputStream.close()
            process.inputStream.bufferedReader(Charset.defaultCharset()).useLines { it.forEach(onLine) }
            rendererRestored = process.waitFor() == 0
            if (!rendererRestored) onLine("Renderer restore failed; CK3 stays on Vulkan, which it supports natively.")
        }

        val running = runningAppFolder()
        val deferred = mutableListOf<Path>()
        val failed = mutableListOf<Path>()
        for (target in uninstallTargets(root)) {
            if (running != null && (running.startsWith(target.absolute().normalize()))) { deferred.add(target); continue }
            onLine("[CK3 DLSS] Removing ${root.relativize(target)}")
            if (!target.toFile().deleteRecursively()) failed.add(target)
        }
        if (rendererRestored && settingsBackup != null && settingsBackup.isRegularFile()) {
            runCatching { Files.delete(settingsBackup) }.onSuccess { onLine("Removed $settingsBackup") }
        }
        val tools = root.resolve("tools")
        if (deferred.isEmpty()) deleteIfEmpty(tools)
        if (deferred.isNotEmpty()) scheduleRemovalAfterExit(deferred, tools, onLine)

        return when {
            failed.isNotEmpty() -> CommandResult(1, "Couldn't remove ${failed.size} item(s). Close anything using them and try again.")
                .also { failed.forEach { onLine("Could not remove $it") } }
            deferred.isNotEmpty() -> CommandResult(0, "Uninstalled. Close this window to finish removing the installer.")
            else -> CommandResult(0, "Uninstalled. CK3 is back to normal.")
        }
    }

    private fun readInstalledSettingsPath(root: Path): String? {
        val receipt = root.resolve("binaries/CK3-DLSS-INSTALL.json")
        return Regex("\"SettingsPath\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"").find(readText(receipt))
            ?.groupValues?.get(1)?.replace("\\\\", "\\")
    }

    private fun deleteIfEmpty(dir: Path) {
        if (dir.isDirectory() && Files.list(dir).use { !it.findAny().isPresent }) runCatching { Files.delete(dir) }
    }

    private fun runningAppFolder(): Path? = runCatching {
        val location = Path.of(InstallerBackend::class.java.protectionDomain.codeSource.location.toURI()).absolute().normalize()
        generateSequence(location) { it.parent }.firstOrNull { it.name.equals("CK3-DLSS-Installer", ignoreCase = true) } ?: location
    }.getOrNull()

    private fun scheduleRemovalAfterExit(paths: List<Path>, tools: Path, onLine: (String) -> Unit) {
        fun quote(p: Path) = "'" + p.toString().replace("'", "''") + "'"
        val script = buildString {
            append("Wait-Process -Id ${ProcessHandle.current().pid()} -ErrorAction SilentlyContinue; Start-Sleep -Seconds 1; ")
            paths.forEach { append("Remove-Item -LiteralPath ${quote(it)} -Recurse -Force -ErrorAction SilentlyContinue; ") }
            append("if ((Test-Path -LiteralPath ${quote(tools)}) -and -not (Get-ChildItem -LiteralPath ${quote(tools)} -Force)) { Remove-Item -LiteralPath ${quote(tools)} -Force }")
        }
        ProcessBuilder(findPowerShell(), "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", script).start()
        onLine("The installer's own folder will be removed when this window closes.")
    }

    fun openThirdPartyNotices(): Boolean {
        val notices = packageRoot.resolve("THIRD-PARTY-NOTICES.md")
        if (!notices.isRegularFile() || !Desktop.isDesktopSupported()) return false
        Desktop.getDesktop().open(notices.toFile())
        return true
    }

    fun openFolder(path: Path): Boolean {
        if (!path.isDirectory() || !Desktop.isDesktopSupported()) return false
        Desktop.getDesktop().open(path.toFile())
        return true
    }

    companion object {
        private val FeederBuildPattern = Regex("ck3-[a-z0-9]+(?:-[a-z0-9]+)*\\.[0-9]+")
        private val NeuralFailurePattern = Regex("evaluate failed|create failed")

        // Package shortcuts in the game root ("Install CK3 DLSS.cmd", "Launch CK3 with DLSS5.cmd", ...).
        private val PackageCmdPattern = Regex("(?i).*(CK3 DLSS|CK3 with DLSS|RHI Runtime Manager).*\\.cmd")
        private val RootFiles = listOf(
            "DLSS5-CK3.ps1", "DLSS-Runtime-Setup.ps1", "Graphics-Dependency-Setup.ps1",
            "README-INTERNAL.md", "THIRD-PARTY-NOTICES.md", "THIRD-PARTY-LICENSES",
            "BUILD-PROVENANCE.txt", "COMPLETE-TEST-BUNDLE.txt",
        )
        // Under binaries\: the package template plus what the install scripts create.
        private val BinaryEntries = listOf(
            "dlss-active", "dlss-payload", "dlss-cache", "dlss-backups", "dlss-profiles", "dlss5-vulkan",
            "reshade-shaders", "third-party", "dxgi.dll", "DLSS5-CK3.ini",
            "ReShade.ini", "ReShade2.ini", "ReShadePreset.ini", "ReShade.log", "ReShade.log1", "dlss5-dxgi.log",
            "CK3-DLSS-GRAPHICS.json", "CK3-DLSS-INSTALL.json",
        )

        /** Turns a raw script line into a short step description, or null for detail-only lines. */
        fun friendlyStep(line: String): String? {
            val match = Regex("^\\[CK3 DLSS(?: [a-z]+)?\\]\\s*(.+)$").find(line.trim()) ?: return null
            return match.groupValues[1].trim().takeIf(String::isNotEmpty)
        }

        fun locatePackageRoot(explicit: String? = null): Path {
            val seeds = buildList {
                explicit?.takeIf(String::isNotBlank)?.let { add(Path.of(it)) }
                System.getenv("CK3_DLSS_PACKAGE_ROOT")?.takeIf(String::isNotBlank)?.let { add(Path.of(it)) }
                add(Path.of(System.getProperty("user.dir")))
                System.getProperty("compose.application.resources.dir")?.let { add(Path.of(it)) }
                runCatching {
                    add(Path.of(InstallerBackend::class.java.protectionDomain.codeSource.location.toURI()))
                }
            }

            for (seed in seeds) {
                var current: Path? = if (Files.isDirectory(seed)) seed.absolute().normalize() else seed.parent
                repeat(8) {
                    val candidate = current ?: return@repeat
                    if (looksLikePackageRoot(candidate)) return candidate
                    // Developer checkout: the complete bundle in release\, else the template.
                    for (nested in listOf("release/CK3-DLSS-Complete-Test", "ck3-package")) {
                        val bundle = candidate.resolve(nested)
                        if (looksLikePackageRoot(bundle)) return bundle
                    }
                    current = candidate.parent
                }
            }
            return Path.of(System.getProperty("user.dir")).absolute().normalize()
        }

        // DLSS5-CK3.ps1 Test-BootstrapPackage: what an install needs from the package.
        private val BootstrapFiles = listOf(
            "DLSS5-CK3.ps1", "Graphics-Dependency-Setup.ps1", "DLSS-Runtime-Setup.ps1",
            "binaries/dxgi.dll", "binaries/dlss-payload/dlss5-feed.addon64", "binaries/ReShade.ini", "binaries/DLSS5-CK3.ini",
            "binaries/reshade-shaders/Shaders/DLSS5_Feed.fx", "binaries/dlss5-vulkan/ReShade64.json",
            "binaries/dlss5-vulkan/VkLayer_feed_vk.dll", "binaries/dlss5-vulkan/VkLayer_feed_vk.json",
        )
        private val KeepUserCopy = setOf("binaries/ReShade.ini", "binaries/DLSS5-CK3.ini")

        fun isCompletePackage(path: Path): Boolean = BootstrapFiles.all { path.resolve(it).isRegularFile() }

        private fun looksLikePackageRoot(path: Path): Boolean = isCompletePackage(path)

        private fun findPowerShell(): String {
            val systemRoot = System.getenv("SystemRoot") ?: "C:/Windows"
            val fullPath = Path.of(
                systemRoot,
                "System32",
                "WindowsPowerShell",
                "v1.0",
                "powershell.exe",
            )
            return if (fullPath.isRegularFile()) fullPath.toString() else "powershell.exe"
        }
    }
}

private fun ck3ProcessRunning(): Boolean = ProcessHandle.allProcesses().anyMatch { handle ->
    handle.info().command().map { Path.of(it).name.equals("ck3.exe", ignoreCase = true) }.orElse(false)
}
