// SPDX-License-Identifier: GPL-2.0-or-later
// Model tests for the launcher (scripts/macos/build_launcher.sh --test). One PASS line or exit 1.
import Foundation

var failures = 0
func check(_ condition: @autoclosure () -> Bool, _ what: String, line: Int = #line) {
    if !condition() {
        failures += 1
        print("FAIL (launcher/Tests/main.swift:\(line)): \(what)")
    }
}

// IniFile: comments, order and unknown keys survive; set, add, remove.
do {
    var ini = IniFile(text: "# bbport settings\nupscaler=fsr3\nweird line\nfps_cap=40\r\n\nrenderer=metal\n")
    check(ini["upscaler"] == "fsr3", "read a key")
    check(ini["fps_cap"] == "40", "CRLF value")
    check(ini["missing"] == nil, "missing key")
    ini["upscaler"] = "metalfx"
    ini["fullscreen"] = "1"
    ini["renderer"] = nil
    check(ini.text == "# bbport settings\nupscaler=metalfx\nweird line\nfps_cap=40\n\nfullscreen=1\n",
          "write keeps the rest: \(ini.text.debugDescription)")
    check(IniFile(text: "").text == "\n" && IniFile(text: "").lines.isEmpty, "empty file")
    check(IniFile(text: "a=1\na=2\n")["a"] == "2", "last occurrence wins")
    check(IniFile(text: "key = spaced value \n")["key"] == "spaced value", "trimmed")
    check(IniFile(text: "#commented=1\n")["#commented"] == nil, "comment is not a key")
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-test-\(getpid())/x/bbport.ini")
    try? ini.write(to: url)
    check(IniFile(contentsOf: url) == ini, "write and read back")
    check(IniFile(contentsOf: URL(fileURLWithPath: "/nonexistent/bbport.ini")).lines.isEmpty, "missing file")
    try? FileManager.default.removeItem(at: url.deletingLastPathComponent().deletingLastPathComponent())
}

// Resolution: render sizes match scripts/patches.py (even sizes) for every output and preset.
do {
    let expected: [PixelSize: [PixelSize]] = [
        PixelSize(1280, 720): [PixelSize(1280, 720), PixelSize(854, 480), PixelSize(752, 424), PixelSize(640, 360), PixelSize(426, 240)],
        PixelSize(1920, 1080): [PixelSize(1920, 1080), PixelSize(1280, 720), PixelSize(1130, 636), PixelSize(960, 540), PixelSize(640, 360)],
        PixelSize(2560, 1440): [PixelSize(2560, 1440), PixelSize(1706, 960), PixelSize(1506, 848), PixelSize(1280, 720), PixelSize(854, 480)],
        PixelSize(3840, 2160): [PixelSize(3840, 2160), PixelSize(2560, 1440), PixelSize(2258, 1270), PixelSize(1920, 1080), PixelSize(1280, 720)],
    ]
    check(Resolution.outputs == [PixelSize(1280, 720), PixelSize(1920, 1080), PixelSize(2560, 1440), PixelSize(3840, 2160)], "output list")
    for output in Resolution.outputs {
        for preset in 0..<Resolution.presetScales.count {
            let got = Resolution.renderSize(output: output, preset: preset)
            check(got == expected[output]![preset], "render size \(output) preset \(preset): \(got)")
        }
    }
    check(Resolution.renderSize(output: PixelSize(1920, 1080), preset: 99) == PixelSize(640, 360), "preset clamped")
    check(PixelSize(iniValue: "2560x1440") == PixelSize(2560, 1440), "parse WxH")
    check(PixelSize(iniValue: "3456X1944") == PixelSize(3456, 1944), "parse upper-case X")
    check(PixelSize(iniValue: "junk") == nil && PixelSize(iniValue: "0x0") == nil, "bad sizes")
    check(PixelSize(2560, 1440).iniValue == "2560x1440" && PixelSize(2560, 1440).description == "2560 × 1440", "format")
}

// GameSettings: defaults, reading, values the launcher does not offer, writing back.
do {
    let defaults = GameSettings(ini: IniFile())
    check(defaults == GameSettings(), "empty ini = defaults")
    check(defaults.output == PixelSize(1920, 1080) && defaults.upscaler == .off && defaults.preset == 0, "default display")
    check(defaults.frameCap == .fps60 && !defaults.fullscreen && defaults.renderer == .vulkan, "default cap, window, renderer")
    check(defaults.effects["effect_dof"] == true && defaults.effects["effect_ssr"] == false && defaults.effects["skip_intro"] == false, "default effects")
    check(defaults.sharpen && defaults.sharpness == 0.3 && !defaults.showFPS && defaults.shaderCompile == .balanced, "default extras")

    let read = GameSettings(ini: IniFile(text: "upscaler=metalfx\npreset=2\noutput_res=2560x1440\nfps_cap=40\nfullscreen=1\neffect_dof=0\nshow_fps=1\nrenderer=metal\nshader_compile=skip\nsharpness=0.75\n"))
    check(read.upscaler == .metalfx && read.preset == 2 && read.output == PixelSize(2560, 1440), "read display")
    check(read.frameCap == .fps40 && read.fullscreen && read.renderer == .metal && read.shaderCompile == .skip, "read launcher keys")
    check(read.effects["effect_dof"] == false && read.showFPS && read.sharpness == 0.75, "read effects")
    check(read.renderSize == PixelSize(1506, 848), "render size follows output and preset")
    check(GameSettings(ini: IniFile(text: "upscaler=taa\npreset=3\n")).renderSize == PixelSize(1920, 1080), "TAA renders native")

    // Review focus 2: values the launcher does not offer survive unless changed.
    var other = IniFile(text: "output_res=3456x1944\nupscaler=fsr4\nfps_cap=45\npreset=9\nsharpness=7\nfsr4_auto_exposure=1\n")
    let odd = GameSettings(ini: other)
    check(odd.output == PixelSize(3456, 1944) && odd.upscaler == .fsr4, "unlisted output and FSR 4 kept")
    check(odd.frameCap == .fps60 && odd.preset == 0 && odd.sharpness == 0.3, "invalid values -> defaults")
    odd.apply(to: &other, includeRenderer: false)
    check(other["output_res"] == "3456x1944" && other["upscaler"] == "fsr4", "written back unchanged")
    check(other["fsr4_auto_exposure"] == "1" && other["renderer"] == nil, "other keys kept; no renderer key")
    var changed = read
    changed.output = PixelSize(3840, 2160)
    changed.effects["effect_ssr"] = true
    var file = IniFile(text: "# header\nupscaler=metalfx\njitter=1\n")
    changed.apply(to: &file, includeRenderer: true)
    check(file["output_res"] == "3840x2160" && file["effect_ssr"] == "1" && file["jitter"] == "1", "apply writes and keeps")
    check(file["fps_cap"] == "40" && file["fullscreen"] == "1" && file["renderer"] == "metal" && file["sharpness"] == "0.75", "apply launcher keys")
    check(file.text.hasPrefix("# header\n"), "header kept")
    // Review focus 3: reloading after the game's menu changed the file picks up its values.
    file["preset"] = "3"
    check(GameSettings(ini: file).preset == 3, "reload sees the menu's change")
}

// Shell quoting and launch.conf (review focus 1: spaces, quotes; bash must source it).
do {
    let tricky = ["/Users/a/Library/Application Support/Bloodborne", "/Volumes/My Drive/Hunter's saves", "/tmp/$HOME `x` \\ \"q\""]
    for path in tricky {
        check(Shell.unquote(Shell.quote(path)) == path, "quote round trip \(path)")
    }
    check(Shell.unquote("/Users/a/My\\ Games/CUSA03173") == "/Users/a/My Games/CUSA03173", "printf %q style")
    check(Shell.unquote("\"/a b\"") == "/a b", "double quotes")

    var conf = IniFile(text: "BB_GAME_DIR=/Users/a/My\\ Games/CUSA03173\nBB_DATA_DIR=/tmp/data\nBB_FPS=uncap\n")
    var config = LaunchConfig(ini: conf)
    check(config.gameDir?.path == "/Users/a/My Games/CUSA03173" && config.saveDir == nil && config.showLauncher, "read launch.conf")
    check(config.dataDir?.path == "/tmp/data", "data folder from launch.conf")
    config.saveDir = URL(fileURLWithPath: tricky[1])
    config.gameDir = URL(fileURLWithPath: tricky[2])
    config.showLauncher = false
    config.apply(to: &conf)
    check(LaunchConfig(ini: conf) == config, "launch.conf round trip")
    check(conf["BB_FPS"] == "uncap" && conf["BB_DATA_DIR"] == "/tmp/data", "other assignments kept")

    let url = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-conf-\(getpid()).conf")
    try? conf.write(to: url)
    let bash = Process()
    bash.executableURL = URL(fileURLWithPath: "/bin/bash")
    bash.arguments = ["-c", "source \"$1\" && printf '%s\\n%s\\n%s' \"$BB_GAME_DIR\" \"$BB_SAVE_DIR\" \"$BB_SHOW_LAUNCHER\"", "_", url.path]
    let pipe = Pipe()
    bash.standardOutput = pipe
    try? bash.run()
    bash.waitUntilExit()
    let sourced = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
    check(bash.terminationStatus == 0 && sourced == "\(tricky[2])\n\(tricky[1])\n0", "bash sources launch.conf: \(sourced.debugDescription)")
    try? FileManager.default.removeItem(at: url)
    config.saveDir = nil
    config.apply(to: &conf)
    check(conf["BB_SAVE_DIR"] == nil, "default save folder: no key")
}

// SaveFolder (review focus 4).
do {
    let fm = FileManager.default
    let root = fm.temporaryDirectory.appendingPathComponent("launcher-saves-\(getpid())")
    let old = root.appendingPathComponent("old"), new = root.appendingPathComponent("new folder")
    try? fm.createDirectory(at: old.appendingPathComponent("1/CUSA00207/SPRJ0005"), withIntermediateDirectories: true)
    try? Data("save".utf8).write(to: old.appendingPathComponent("1/CUSA00207/SPRJ0005/userdata0000"))
    check(SaveFolder.defaultURL(dataDir: URL(fileURLWithPath: "/d")).path == "/d/user/savedata", "default save folder")
    check(SaveFolder.hasSaves(old) && !SaveFolder.hasSaves(new), "has saves")
    check(SaveFolder.shouldOfferCopy(from: old, to: new), "offer copy to an empty folder")
    check(!SaveFolder.shouldOfferCopy(from: old, to: old) && !SaveFolder.shouldOfferCopy(from: old, to: URL(fileURLWithPath: old.path + "/")), "same folder: no offer")
    check(!SaveFolder.shouldOfferCopy(from: new, to: old), "nothing to copy: no offer")
    try? fm.createDirectory(at: new.appendingPathComponent("1"), withIntermediateDirectories: true)
    try? Data("mine".utf8).write(to: new.appendingPathComponent("1/keep"))
    check(!SaveFolder.shouldOfferCopy(from: old, to: new), "target has saves: no offer")
    do { try SaveFolder.copySaves(from: old, to: new) } catch { check(false, "copy: \(error)") }
    check(fm.fileExists(atPath: old.appendingPathComponent("1/CUSA00207/SPRJ0005/userdata0000").path), "original kept")
    check((try? String(contentsOf: new.appendingPathComponent("1/keep"), encoding: .utf8)) == "mine", "existing file not overwritten")
    check(SaveFolder.problem(root.appendingPathComponent("made"), create: true) == nil, "creates a writable folder")
    check(SaveFolder.problem(URL(fileURLWithPath: "/Volumes/no-such-drive-\(getpid())/saves"), create: false) != nil, "missing folder reported")
    try? fm.removeItem(at: root)
}

if failures > 0 {
    print("FAIL: launcher model, \(failures) failed")
    exit(1)
}
print("PASS: launcher model (ini, resolutions, settings, launch.conf, saves)")
