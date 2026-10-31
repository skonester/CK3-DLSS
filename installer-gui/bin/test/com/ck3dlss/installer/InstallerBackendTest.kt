package com.ck3dlss.installer

import java.nio.file.Files
import java.nio.file.Path
import kotlin.io.path.createDirectories
import kotlin.io.path.createFile
import kotlin.io.path.writeBytes
import kotlin.io.path.writeText
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNull
import kotlin.test.assertTrue

class InstallerBackendTest {
    private fun packageRoot(): Path = Files.createTempDirectory("ck3-package").also {
        it.resolve("DLSS5-CK3.ps1").createFile()
        it.resolve("binaries/dlss-payload").createDirectories()
    }

    private fun gameRoot(): Path = Files.createTempDirectory("ck3-game").also {
        it.resolve("binaries").createDirectories()
        it.resolve("binaries/ck3.exe").createFile()
    }

    private fun install(game: Path, profile: String): Path =
        game.resolve("binaries/dlss-active").createDirectories().also {
            it.resolve("CK3-DLSS-RUNTIME.json").writeText("""{ "SchemaVersion": 1, "Profile": "$profile" }""")
        }

    @Test
    fun acceptsGameRootAndBinariesFolder() {
        val game = gameRoot()
        val backend = InstallerBackend(packageRoot())

        assertTrue(backend.validateGameRoot(game.toString()).valid)
        val binariesResult = backend.validateGameRoot(game.resolve("binaries").toString())
        assertTrue(binariesResult.valid)
        assertEquals(game, binariesResult.normalizedRoot)
    }

    @Test
    fun rejectsFolderWithoutCk3Executable() {
        val result = InstallerBackend(packageRoot()).validateGameRoot(Files.createTempDirectory("not-ck3").toString())

        assertFalse(result.valid)
        assertTrue(result.message.contains("binaries\\ck3.exe"))
    }

    @Test
    fun buildsNonInteractiveInstallCommandForEveryProfile() {
        val backend = InstallerBackend(packageRoot())
        for (profile in InstallProfile.entries) {
            val command = backend.buildPowerShellCommand(InstallerAction.INSTALL, profile, gameRoot())
            assertTrue(command.contains("-NonInteractive"), "never waits for console input")
            assertTrue(command.windowedContains("-Action", "Install"))
            assertTrue(command.windowedContains("-Profile", profile.scriptValue))
            assertTrue(command.contains("-AcceptDependencyLicenses"))
            assertTrue(command.contains("-AcceptRuntimeLicenses"))
        }
        val validate = backend.buildPowerShellCommand(InstallerAction.VALIDATE, InstallProfile.DLSS45, gameRoot())
        assertTrue(validate.windowedContains("-Action", "Validate"))
        assertFalse(validate.contains("-Profile"))
    }

    @Test
    fun profilesMatchTheScriptValidateSet() {
        // DLSS5-CK3.ps1: [ValidateSet('Auto', 'DLSS45', 'DLSS5', 'DLSS5Extended', 'NativeStreamline')]
        assertEquals(
            listOf("DLSS45", "DLSS5", "DLSS5Extended", "NativeStreamline"),
            InstallProfile.entries.map { it.scriptValue },
        )
    }

    @Test
    fun readsEveryActiveProfileReceipt() {
        val backend = InstallerBackend(packageRoot())
        for (profile in InstallProfile.entries) {
            val game = gameRoot()
            install(game, profile.scriptValue)
            assertEquals(profile, backend.readActiveProfile(game))
        }
        assertNull(backend.readActiveProfile(gameRoot()))
    }

    @Test
    fun reportsFeederBuildAndNeuralSessionHealth() {
        val backend = InstallerBackend(packageRoot())
        val game = gameRoot()
        val active = install(game, "NativeStreamline")
        active.resolve("dlss5-feed.addon64").writeBytes(byteArrayOf(0, 1) + "DLSS 5 Feed ck3-frontier3-diag.1".toByteArray() + byteArrayOf(0))
        val log = game.resolve("binaries/ReShade.log")

        assertEquals(SessionHealth.NO_SESSION, backend.readStatus(game).health)

        log.writeText("ERROR | DLSS5 Generic: feature 18 create failed with 0xbad00001\n")
        val failed = backend.readStatus(game)
        assertEquals(InstallProfile.NATIVE_STREAMLINE, failed.profile)
        assertEquals("ck3-frontier3-diag.1", failed.feederBuild)
        assertEquals(SessionHealth.FAILED, failed.health)

        log.writeText("INFO | inline feature 18 evaluation succeeded\n")
        assertEquals(SessionHealth.WORKING, backend.readStatus(game).health)
    }

    @Test
    fun reportsDlss45SessionFromFeederLog() {
        val backend = InstallerBackend(packageRoot())
        val game = gameRoot()
        val active = install(game, "DLSS45")
        active.resolve("dlss5-feed.log").writeText("[feed] frame 1 delivered (1920x1080, reset=1)\n")

        assertEquals(SessionHealth.WORKING, backend.readStatus(game).health)
    }

    @Test
    fun turnsScriptStatusLinesIntoSteps() {
        assertEquals("Base package validation passed.", InstallerBackend.friendlyStep("[CK3 DLSS] Base package validation passed."))
        assertEquals("Activated DLSS 5 Extended.", InstallerBackend.friendlyStep("[CK3 DLSS runtime] Activated DLSS 5 Extended."))
        assertNull(InstallerBackend.friendlyStep("  Detected: NVIDIA GeForce RTX 3060"))
    }

    @Test
    fun uninstallRemovesOnlyPackageFiles() {
        val game = gameRoot()
        val keep = listOf(
            "binaries/ck3.exe", "binaries/steam_api64.dll", "binaries/steam_api64_o.dll", "binaries/SmokeAPI.config.json",
            "binaries/ck3 2026-10-02 06-48-16_1.png", "binaries/d3dcompiler_47.dll", "game/common/x.txt", "jomini/y.txt",
            "clausewitz_rev.txt", "launcher/launcher-settings.json",
        )
        val remove = listOf(
            "Install CK3 DLSS.cmd", "Launch CK3 with DLSS5.cmd", "Open RHI Runtime Manager.cmd", "Install CK3 DLSS 5 Extended Test.cmd",
            "DLSS5-CK3.ps1", "DLSS-Runtime-Setup.ps1", "Graphics-Dependency-Setup.ps1", "README-INTERNAL.md",
            "THIRD-PARTY-NOTICES.md", "THIRD-PARTY-LICENSES/ReShade-LICENSE.txt", "BUILD-PROVENANCE.txt",
            "tools/RHI-Setup.exe", "tools/CK3-DLSS-Installer/app/x.jar",
            "binaries/dxgi.dll", "binaries/ReShade.ini", "binaries/ReShade2.ini", "binaries/ReShade.log", "binaries/dlss5-dxgi.log",
            "binaries/CK3-DLSS-GRAPHICS.json", "binaries/DLSS5-CK3.ini", "binaries/dlss-active/dlss5-feed.addon64",
            "binaries/dlss-payload/dlss5-feed.addon64", "binaries/dlss-cache/downloads/a.zip", "binaries/dlss-backups/b/c.cfg",
            "binaries/dlss5-vulkan/ReShade64.dll", "binaries/reshade-shaders/Shaders/ReShade.fxh",
            "binaries/third-party/vort_Shaders/v.fx", "binaries/.ck3-dlss-graphics-stage-123/tmp",
        )
        (keep + remove).forEach { rel ->
            val file = game.resolve(rel)
            file.parent.createDirectories()
            if (!Files.exists(file)) file.writeText("x")
        }
        game.resolve("README.md").writeText("# CK3 DLSS Vulkan - All Profiles\n")
        game.resolve("DLSS5-CK3.ps1").writeText("exit 0\n")

        val result = InstallerBackend(packageRoot()) { false }.uninstall(game) { }

        assertTrue(result.succeeded, result.summary)
        keep.forEach { assertTrue(Files.exists(game.resolve(it)), "kept $it") }
        remove.forEach { assertFalse(Files.exists(game.resolve(it)), "removed $it") }
        assertFalse(Files.exists(game.resolve("README.md")), "package README removed")
        assertFalse(Files.exists(game.resolve("tools")), "empty tools folder removed")
        assertTrue(Files.exists(game.resolve("binaries")), "binaries folder kept")
    }

    @Test
    fun uninstallRefusesWhileTheGameRuns() {
        val game = gameRoot()
        game.resolve("Install CK3 DLSS.cmd").writeText("x")
        val result = InstallerBackend(packageRoot()) { true }.uninstall(game) { }
        assertFalse(result.succeeded)
        assertTrue(Files.exists(game.resolve("Install CK3 DLSS.cmd")))
    }

    @Test
    fun installCopiesACompletePackageIntoACleanGameFolder() {
        val pkg = Files.createTempDirectory("ck3-bundle")
        val files = listOf(
            "DLSS5-CK3.ps1", "Graphics-Dependency-Setup.ps1", "DLSS-Runtime-Setup.ps1", "Install CK3 DLSS.cmd",
            "binaries/dxgi.dll", "binaries/dlss-payload/dlss5-feed.addon64", "binaries/ReShade.ini", "binaries/DLSS5-CK3.ini",
            "binaries/reshade-shaders/Shaders/DLSS5_Feed.fx", "binaries/dlss5-vulkan/ReShade64.json",
            "binaries/dlss5-vulkan/VkLayer_feed_vk.dll", "binaries/dlss5-vulkan/VkLayer_feed_vk.json",
            "tools/CK3-DLSS-Installer/CK3 DLSS Installer.exe",
        )
        files.forEach { pkg.resolve(it).also { f -> f.parent.createDirectories(); f.writeText("package $it") } }
        // Simulate a zip extracted with times hours in the future.
        val future = java.nio.file.attribute.FileTime.from(java.time.Instant.now().plusSeconds(5 * 3600))
        files.forEach { Files.setLastModifiedTime(pkg.resolve(it), future) }
        val backend = InstallerBackend(pkg) { false }
        assertTrue(backend.packageReady(), "complete package recognised")
        assertFalse(InstallerBackend(packageRoot()).packageReady(), "a package without the bootstrap files is not ready")

        val game = gameRoot()
        game.resolve("binaries/ReShade.ini").writeText("user settings")
        backend.deployPackage(game) { }

        files.filter { it != "binaries/ReShade.ini" }.forEach { assertEquals("package $it", Files.readString(game.resolve(it)), it) }
        assertEquals("user settings", Files.readString(game.resolve("binaries/ReShade.ini")), "existing ReShade settings kept")
        assertTrue(InstallerBackend.isCompletePackage(game), "the game folder now holds a complete package")
        val latest = java.time.Instant.now().plusSeconds(60)
        files.filter { it != "binaries/ReShade.ini" }.forEach {
            assertTrue(Files.getLastModifiedTime(game.resolve(it)).toInstant().isBefore(latest), "$it is not future-dated")
        }
    }

    @Test
    fun dlss5SwitchEditsTheSettingF6Uses() {
        val game = gameRoot()
        val ini = game.resolve("binaries/ReShade.ini")
        val backend = InstallerBackend(packageRoot()) { false }
        assertNull(backend.readNeuralUplift(game), "no ReShade yet")

        ini.writeText("[GENERAL]\r\nEffectSearchPaths=x\r\n[RenoDX.DLSS5]\r\nEnableHooks=2\r\nNeuralUplift=1\r\n[OVERLAY]\r\nA=1\r\n")
        assertEquals(true, backend.readNeuralUplift(game))
        assertTrue(backend.setNeuralUplift(game, false))
        assertEquals(false, backend.readNeuralUplift(game))
        assertEquals(
            "[GENERAL]\r\nEffectSearchPaths=x\r\n[RenoDX.DLSS5]\r\nEnableHooks=2\r\nNeuralUplift=0\r\n[OVERLAY]\r\nA=1\r\n",
            Files.readString(ini), "only the one key changes; CRLF kept",
        )

        ini.writeText("[GENERAL]\nA=1\n")
        assertEquals(true, backend.readNeuralUplift(game), "unset means the feeder will turn it on")
        assertTrue(backend.setNeuralUplift(game, false))
        assertEquals(false, backend.readNeuralUplift(game), "section added when missing")

        assertFalse(InstallerBackend(packageRoot()) { true }.setNeuralUplift(game, true), "not while the game runs")
    }

    @Test
    fun uninstallKeepsAForeignReadme() {
        val game = gameRoot()
        game.resolve("README.md").writeText("# Some other mod\n")
        InstallerBackend(packageRoot()) { false }.uninstall(game) { }
        assertTrue(Files.exists(game.resolve("README.md")))
    }

    private fun List<String>.windowedContains(first: String, second: String): Boolean =
        windowed(2).any { it[0] == first && it[1] == second }
}
