// Launcher backend. It never uploads or bundles game files: the game code is generated and compiled on this PC from the
// folder the player picks. Beside a source checkout that folder is linked into the work folder (a directory junction);
// in the release zip the runtime's own setup copies or hard-links it into the install folder.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use std::fs;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdout, Command, Stdio};
use std::sync::{Arc, Mutex};

/// Kirby and the Forgotten Land, any region build of the base game (main.npdm's program id).
const PROGRAM_ID: u64 = 0x0100_4D30_0C5A_E000;
const MODULES: [&str; 3] = ["main", "sdk", "subsdk0"];
const SETTING_KEYS: [&str; 3] = ["fullscreen", "render_height", "volume"];
/// The release runtime, next to the launcher in the zip.
const PACKAGED_EXE: &str = "ForgottenLandRecomp.exe";
/// Packaged mode: the folder the player picked, remembered until Build reads it.
const PICKED_FILE: &str = "game-folder.txt";

#[derive(Clone, Copy, Debug, Default, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
enum Phase {
    #[default]
    Idle,
    Runtime,
    Image,
    Translate,
    Compile,
    Failed,
}

#[derive(Default)]
struct Job {
    phase: Phase,
    /// Packaged mode: the runtime's own fraction of the whole setup.
    progress: Option<f32>,
    /// Packaged mode: the file the compiler has been on for a while, as the runtime words it.
    detail: Option<String>,
    error: String,
    child: Option<Child>,
    cancelled: bool,
}

/// Beside a source checkout: the runtime and the game code are built with make through MSYS2.
struct Dev {
    repo: PathBuf,
    msys: PathBuf,
}

/// Decided once at startup from what sits next to the launcher.
enum Mode {
    Dev(Dev),
    /// The release zip: the runtime and the tools it builds the game code with are next to the launcher, and the
    /// install folder is the work folder (game files, saves, settings, logs).
    Packaged,
}

impl Mode {
    fn detect(exe_dir: &Path) -> Mode {
        if exe_dir.join("lasgen").join("lasgen.exe").is_file() && exe_dir.join("zig").join("zig.exe").is_file() {
            return Mode::Packaged;
        }
        Mode::Dev(Dev {
            repo: find_repo(),
            msys: std::env::var("KFL_MSYS2").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from(r"C:\msys64")),
        })
    }
}

/// One stdout line of the runtime's `--setup` (runtime/main.c setup_headless).
#[derive(Debug, PartialEq)]
enum SetupLine {
    Progress(Phase, f32, Option<String>),
    Done,
    Failed(String),
}

fn parse_setup_line(line: &str) -> Option<SetupLine> {
    let mut w = line.split_whitespace();
    match w.next()? {
        "phase" => {
            let phase = match w.next()? {
                "looking" | "copying" | "image" => Phase::Image,
                "reading" => Phase::Translate,
                "compiling" | "finishing" => Phase::Compile,
                _ => return None,
            };
            let fraction = w.next()?.parse().ok()?;
            let detail = w.collect::<Vec<_>>().join(" ");
            Some(SetupLine::Progress(phase, fraction, (!detail.is_empty()).then_some(detail)))
        }
        "done" => Some(SetupLine::Done),
        "failed" => Some(SetupLine::Failed(w.next().unwrap_or("").into())),
        _ => None,
    }
}

fn setup_failure(reason: &str) -> String {
    match reason {
        "not_found" => "No game files were found in that folder. Pick the folder that holds exefs and romfs.",
        "package" => "Those are packed game files (.xci, .nsp). Extract them first (exefs and romfs).",
        "version" => "That is not Kirby and the Forgotten Land.",
        "copy" => "The game files could not be copied. Check the free disk space and try again.",
        "tools" => "The game code builder is missing from this folder. Unzip the release again.",
        "bad_image" => "The game data image (romfs.bin) cannot be read. Pick a folder with the romfs folder itself.",
        _ => "The game code did not build. Open the setup log for the details.",
    }
    .into()
}

/// How the game ended, from its exit code and the end of game.log.
#[derive(Debug, PartialEq)]
enum Outcome {
    Quit,
    /// Non-zero exit with no crash report: the player or Windows ended it, or it failed at startup with its own message box.
    Stopped,
    /// The one-line reason.
    Crashed(String),
}

/// The last crash, until the player dismisses it or presses Play. `log` is a file name inside the work folder.
#[derive(Clone, Serialize)]
struct Crash {
    reason: String,
    log: String,
}

struct App {
    mode: Mode,
    work: PathBuf,
    job: Arc<Mutex<Job>>,
    game: Mutex<Option<Child>>,
    crash: Mutex<Option<Crash>>,
}

#[derive(Serialize)]
struct Status {
    work: String,
    game_dir: Option<String>,
    game_error: Option<String>,
    tools_missing: Vec<String>,
    built: bool,
    running: bool,
    phase: Phase,
    progress: Option<f32>,
    detail: Option<String>,
    error: String,
    settings: Vec<(String, String)>,
    crash: Option<Crash>,
    packaged: bool,
}

fn find_repo() -> PathBuf {
    if let Ok(p) = std::env::var("KFL_REPO") {
        return PathBuf::from(p);
    }
    let exe = std::env::current_exe().unwrap_or_default();
    exe.ancestors()
        .find(|d| d.join("Makefile").is_file() && d.join("runtime").is_dir())
        .map(Path::to_path_buf)
        .unwrap_or_else(|| exe.parent().map(Path::to_path_buf).unwrap_or_default())
}

fn program_id(npdm: &Path) -> Option<u64> {
    let b = fs::read(npdm).ok()?;
    if b.get(..4)? != b"META" {
        return None;
    }
    let aci = u32::from_le_bytes(b.get(0x70..0x74)?.try_into().ok()?) as usize;
    if b.get(aci..aci + 4)? != b"ACI0" {
        return None;
    }
    Some(u64::from_le_bytes(b.get(aci + 0x10..aci + 0x18)?.try_into().ok()?))
}

fn check_game(dir: &Path) -> Result<(), String> {
    let exefs = dir.join("exefs");
    for m in MODULES {
        if !exefs.join(m).is_file() {
            return Err(format!("This folder has no exefs\\{m}. Pick the folder that holds your extracted exefs and romfs."));
        }
    }
    if !dir.join("romfs").is_dir() {
        return Err("This folder has no romfs folder next to exefs.".into());
    }
    match program_id(&exefs.join("main.npdm")) {
        Some(PROGRAM_ID) => Ok(()),
        Some(id) => Err(format!("These files are another game (program id {id:016X}).")),
        None => Err("exefs\\main.npdm is missing or unreadable.".into()),
    }
}

fn count(dir: &Path, ext: &str) -> usize {
    let Ok(rd) = fs::read_dir(dir) else { return 0 };
    rd.flatten()
        .map(|e| {
            let p = e.path();
            if p.is_dir() {
                count(&p, ext)
            } else {
                (p.extension().and_then(|x| x.to_str()) == Some(ext)) as usize
            }
        })
        .sum()
}

// The runtime prints one of two markers on a crash (runtime/rt.h FATAL, runtime/dispatch.c rt_fault_report) and then abort()s,
// which exits with code 3. A normal close (window closed, or the game asking to exit) exits with code 0.
fn classify(code: i32, tail: &str) -> Outcome {
    let marker = tail.lines().map(str::trim).find(|l| l.starts_with("FATAL:") || l.starts_with("*** exception"));
    match (marker, code) {
        (Some(line), _) => Outcome::Crashed(line.chars().take(300).collect()),
        (None, 0) => Outcome::Quit,
        (None, 3) => Outcome::Crashed("The game aborted without a report (exit code 3).".into()),
        // an NTSTATUS exit code is a native fault the runtime's handler never saw
        (None, c) if c as u32 >= 0xC000_0000 => Outcome::Crashed(format!("Windows ended the game with error {:08X}.", c as u32)),
        _ => Outcome::Stopped,
    }
}

const KEPT_CRASH_LOGS: usize = 5;

fn log_tail(path: &Path) -> String {
    let read = || -> std::io::Result<Vec<u8>> {
        let mut f = fs::File::open(path)?;
        f.seek(SeekFrom::End(-(1 << 16)))
            .or_else(|_| f.seek(SeekFrom::Start(0)))?;
        let mut buf = Vec::new();
        f.read_to_end(&mut buf)?;
        Ok(buf)
    };
    String::from_utf8_lossy(&read().unwrap_or_default()).into_owned()
}

fn is_crash_log(name: &str) -> bool {
    let digits = |s: &str, n: usize| s.len() == n && s.bytes().all(|b| b.is_ascii_digit());
    name.strip_prefix("crash-")
        .and_then(|r| r.strip_suffix(".log"))
        .and_then(|r| r.split_once('-'))
        .is_some_and(|(d, t)| digits(d, 8) && digits(t, 6))
}

// Copies game.log aside, because the next Play overwrites it. Returns the copy's name (game.log itself if the copy fails).
fn keep_crash_log(work: &Path) -> String {
    let name = format!("crash-{}.log", chrono::Local::now().format("%Y%m%d-%H%M%S"));
    if fs::copy(work.join("game.log"), work.join(&name)).is_err() {
        return "game.log".into();
    }
    let mut all: Vec<String> = fs::read_dir(work).into_iter().flatten().flatten().filter_map(|e| e.file_name().into_string().ok()).filter(|n| is_crash_log(n)).collect();
    all.sort();
    let old = all.len().saturating_sub(KEPT_CRASH_LOGS);
    for f in &all[..old] {
        let _ = fs::remove_file(work.join(f));
    }
    name
}

impl App {
    // true while the game runs; when it has just ended, records a crash for the status
    fn poll_game(&self) -> bool {
        let mut g = self.game.lock().unwrap();
        let Some(child) = g.as_mut() else { return false };
        let ended = match child.try_wait() {
            Ok(None) => return true,
            Ok(Some(st)) => Some(st.code().unwrap_or(1)),
            Err(_) => None,
        };
        *g = None;
        if let Some(code) = ended {
            if let Outcome::Crashed(reason) = classify(code, &log_tail(&self.work.join("game.log"))) {
                *self.crash.lock().unwrap() = Some(Crash { reason, log: keep_crash_log(&self.work) });
            }
        }
        false
    }
    fn game(&self) -> PathBuf {
        self.work.join("extracted")
    }
    fn exe(&self) -> PathBuf {
        match self.mode {
            Mode::Dev(_) => self.work.join("build").join("lasrecomp.exe"),
            Mode::Packaged => self.work.join(PACKAGED_EXE),
        }
    }
    fn lib(&self) -> PathBuf {
        match self.mode {
            Mode::Dev(_) => self.work.join("build").join("lasgame.dll"),
            Mode::Packaged => self.game().join("lasgame.dll"),
        }
    }
    fn image(&self) -> PathBuf {
        self.game().join("romfs.bin")
    }
    fn ini(&self) -> PathBuf {
        self.work.join("larecomp.ini")
    }
    fn built(&self) -> bool {
        self.exe().is_file() && self.lib().is_file() && self.image().is_file()
    }
    /// The game files the player picked: the junction in developer mode, the remembered folder in packaged mode (or,
    /// once that is forgotten, the install folder's own copy).
    fn game_source(&self) -> Option<PathBuf> {
        let set_up = self.game().join("exefs").is_dir().then(|| self.game());
        match self.mode {
            Mode::Dev(_) => set_up,
            Mode::Packaged => fs::read_to_string(self.work.join(PICKED_FILE)).ok().map(|s| PathBuf::from(s.trim())).or(set_up),
        }
    }
    fn tools_missing(&self) -> Vec<String> {
        let needed = match &self.mode {
            Mode::Dev(dev) => vec![
                ("MSYS2 make", dev.msys.join("usr\\bin\\make.exe")),
                ("MSYS2 clang", dev.msys.join("ucrt64\\bin\\clang.exe")),
                ("Python environment (.venv)", dev.repo.join(".venv\\Scripts\\python.exe")),
            ],
            Mode::Packaged => vec![
                ("the game runtime (ForgottenLandRecomp.exe)", self.exe()),
                ("the game code generator (lasgen)", self.work.join("lasgen\\lasgen.exe")),
                ("the compiler (zig)", self.work.join("zig\\zig.exe")),
                ("the game code headers (gamelib-src)", self.work.join("gamelib-src\\cpu.h")),
                ("the runtime's import library (lasrecomp.dll.a)", self.work.join("lasrecomp.dll.a")),
            ],
        };
        needed.into_iter().filter(|(_, path)| !path.is_file()).map(|(name, _)| name.to_string()).collect()
    }
    /// The runtime, started in the work folder. Packaged mode pins its data root there: without it, a folder named
    /// `extracted` next to the install folder would be taken for a source checkout's.
    fn runtime(&self) -> Command {
        let mut c = Command::new(self.exe());
        c.current_dir(&self.work);
        match &self.mode {
            Mode::Dev(dev) => c.env("PATH", dev.tool_path()),
            Mode::Packaged => c.env("LARECOMP_DATA_ROOT", &self.work),
        };
        c
    }
    // returns at once when a valid image exists
    fn build_image(&self) -> Command {
        let mut c = self.runtime();
        c.args(["extracted", "--build-romfs"]);
        hide_window(&mut c);
        c
    }
    fn settings(&self) -> Vec<(String, String)> {
        let text = fs::read_to_string(self.ini()).unwrap_or_default();
        SETTING_KEYS
            .iter()
            .map(|k| {
                let v = text.lines().find_map(|l| {
                    let (key, val) = l.split_once('=')?;
                    (key.trim() == *k).then(|| val.trim().to_string())
                });
                (k.to_string(), v.unwrap_or_default())
            })
            .collect()
    }
}

impl Dev {
    fn tool_path(&self) -> String {
        let old = std::env::var("PATH").unwrap_or_default();
        format!("{};{};{old}", self.msys.join("ucrt64\\bin").display(), self.msys.join("usr\\bin").display())
    }
    // WORK relative to the repo, on one drive: the link line carries about 540 object paths, which only fit what Windows
    // allows (Argument list too long) while WORK is a short relative path
    fn work_from_repo(&self, work: &Path) -> PathBuf {
        let repo = fs::canonicalize(&self.repo).unwrap_or(self.repo.clone());
        let strip = |p: &Path| PathBuf::from(p.to_string_lossy().trim_start_matches(r"\\?\").to_lowercase());
        let (r, w) = (strip(&repo), strip(work));
        let (rc, wc): (Vec<_>, Vec<_>) = (r.components().collect(), w.components().collect());
        if rc.first() != wc.first() {
            return work.to_path_buf();
        }
        let same = rc.iter().zip(&wc).take_while(|(a, b)| a == b).count();
        let mut rel = PathBuf::new();
        (same..rc.len()).for_each(|_| rel.push(".."));
        work.components().skip(same).for_each(|c| rel.push(c));
        rel
    }
    fn make(&self, work: &Path, args: &[&str]) -> Command {
        let mut c = Command::new(self.msys.join("usr\\bin\\make.exe"));
        // MSYS make wants forward slashes
        let work = self.work_from_repo(work).to_string_lossy().replace('\\', "/");
        c.current_dir(&self.repo).env("PATH", self.tool_path()).env("MSYSTEM", "UCRT64").arg(format!("WORK={work}")).args(args);
        hide_window(&mut c);
        c
    }
}

#[cfg(windows)]
fn hide_window(c: &mut Command) {
    use std::os::windows::process::CommandExt;
    c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
}
#[cfg(not(windows))]
fn hide_window(_: &mut Command) {}

type St<'a> = tauri::State<'a, Arc<App>>;

#[tauri::command]
fn status(app: St) -> Status {
    let built = app.built();
    let (game_dir, game_error) = match app.game_source() {
        Some(game) => {
            let shown = fs::canonicalize(&game).unwrap_or(game.clone());
            // packaged mode plays from its own copy: the picked folder may be gone once the game code is built
            let check = !(built && matches!(app.mode, Mode::Packaged));
            (Some(shown.to_string_lossy().trim_start_matches(r"\\?\").to_string()), check.then(|| check_game(&game).err()).flatten())
        }
        None => (None, None),
    };
    let running = app.poll_game();
    let job = app.job.lock().unwrap();
    let progress = match (&app.mode, job.phase) {
        (Mode::Packaged, _) => job.progress,
        (Mode::Dev(_), Phase::Compile) => {
            let total = count(&app.work.join("generated"), "c").max(1);
            Some(count(&app.work.join("build").join("obj"), "o") as f32 / total as f32)
        }
        _ => None,
    };
    Status {
        work: app.work.to_string_lossy().into(),
        game_dir,
        game_error,
        tools_missing: app.tools_missing(),
        built,
        running,
        phase: job.phase,
        progress,
        detail: job.detail.clone(),
        error: job.error.clone(),
        settings: app.settings(),
        crash: app.crash.lock().unwrap().clone(),
        packaged: matches!(app.mode, Mode::Packaged),
    }
}

#[tauri::command]
fn set_game_dir(app: St, path: String) -> Result<(), String> {
    let src = PathBuf::from(path);
    check_game(&src)?;
    if let Mode::Packaged = app.mode {
        return fs::write(app.work.join(PICKED_FILE), src.to_string_lossy().as_bytes()).map_err(|e| e.to_string());
    }
    let link = app.game();
    if fs::canonicalize(&link).ok() == fs::canonicalize(&src).ok() {
        return Ok(());
    }
    fs::create_dir_all(&app.work).map_err(|e| e.to_string())?;
    link_game_dir(&link, &src)
}

// A junction made through the Windows API, not `cmd /C mklink`: cmd re-parses `&`, `^` and `%` in the path
fn link_game_dir(link: &Path, src: &Path) -> Result<(), String> {
    // symlink_metadata sees a dead junction (the player moved their dump), which exists() follows and misses
    if fs::symlink_metadata(link).is_ok() {
        // remove_dir only removes a junction or an empty folder: real game files are never deleted here
        fs::remove_dir(link).map_err(|_| format!("{} already holds game files. Move that folder away first.", link.display()))?;
    }
    junction::create(src, link).map_err(|e| e.to_string())
}

// Starts the step as the job's child and hands back its stdout when piped
fn spawn_step(app: &App, phase: Phase, cmd: &mut Command) -> Result<Option<ChildStdout>, String> {
    // the flag check and the registration share one lock hold, so a cancel never lands between spawn and registration
    let mut job = app.job.lock().unwrap();
    if job.cancelled {
        return Err("Cancelled.".into());
    }
    let mut child = cmd.stdin(Stdio::null()).spawn().map_err(|e| format!("{cmd:?}: {e}"))?;
    job.phase = phase;
    let out = child.stdout.take();
    job.child = Some(child);
    Ok(out)
}

fn run_step(app: &App, phase: Phase, mut cmd: Command, log: &mut fs::File) -> Result<(), String> {
    let out = log.try_clone().map_err(|e| e.to_string())?;
    let err = log.try_clone().map_err(|e| e.to_string())?;
    let _ = writeln!(log, "\n== {cmd:?}");
    spawn_step(app, phase, cmd.stdout(out).stderr(err))?;
    wait_step(app)
}

// Packaged mode's Build: the runtime's headless setup, its progress lines mapped onto the phases
fn run_setup(app: &App, mut cmd: Command, log: &mut fs::File) -> Result<(), String> {
    let err = log.try_clone().map_err(|e| e.to_string())?;
    let _ = writeln!(log, "== {cmd:?}");
    let out = spawn_step(app, Phase::Image, cmd.stdout(Stdio::piped()).stderr(err))?.ok_or("no setup output")?;
    let mut end = None;
    for line in BufReader::new(out).lines().map_while(Result::ok) {
        match parse_setup_line(&line) {
            Some(SetupLine::Progress(phase, fraction, detail)) => {
                let mut job = app.job.lock().unwrap();
                (job.phase, job.progress, job.detail) = (phase, Some(fraction), detail);
            }
            other => end = other.or(end),
        }
    }
    match (wait_step(app), end) {
        (Err(_), Some(SetupLine::Failed(reason))) => Err(setup_failure(&reason)),
        (result, _) => result,
    }
}

fn wait_step(app: &App) -> Result<(), String> {
    loop {
        std::thread::sleep(std::time::Duration::from_millis(200));
        let mut job = app.job.lock().unwrap();
        let Some(child) = job.child.as_mut() else { return Err("Cancelled.".into()) };
        if let Ok(Some(code)) = child.try_wait() {
            job.child = None;
            return if code.success() { Ok(()) } else { Err("A build step failed. Open the setup log for the details.".into()) };
        }
    }
}

#[tauri::command]
fn start_setup(app: St) -> Result<(), String> {
    let game = app.game_source().ok_or("Select your game folder first.")?;
    check_game(&game)?;
    let missing = app.tools_missing();
    if !missing.is_empty() {
        return Err(format!("Missing build tools: {}.", missing.join(", ")));
    }
    {
        let mut job = app.job.lock().unwrap();
        if !matches!(job.phase, Phase::Idle | Phase::Failed) {
            return Ok(());
        }
        let phase = if let Mode::Packaged = app.mode { Phase::Image } else { Phase::Runtime };
        *job = Job { phase, ..Job::default() };
    }
    let app = Arc::clone(&app);
    std::thread::spawn(move || {
        let jobs = format!("-j{}", std::thread::available_parallelism().map_or(4, |n| n.get()));
        let result = fs::File::create(app.work.join("setup.log")).map_err(|e| e.to_string()).and_then(|mut log| match &app.mode {
            Mode::Dev(dev) => {
                let exe = format!("{}/build/lasrecomp.exe", dev.work_from_repo(&app.work).to_string_lossy().replace('\\', "/"));
                run_step(&app, Phase::Runtime, dev.make(&app.work, &[&jobs, &exe]), &mut log)?;
                run_step(&app, Phase::Image, app.build_image(), &mut log)?;
                run_step(&app, Phase::Translate, dev.make(&app.work, &["gen"]), &mut log)?;
                run_step(&app, Phase::Compile, dev.make(&app.work, &[&jobs]), &mut log)
            }
            Mode::Packaged => {
                let mut c = app.runtime();
                c.arg("--setup").arg(&game);
                hide_window(&mut c);
                run_setup(&app, c, &mut log)
            }
        });
        let mut job = app.job.lock().unwrap();
        match result {
            Ok(()) => job.phase = Phase::Idle,
            Err(e) => {
                job.phase = if job.cancelled { Phase::Idle } else { Phase::Failed };
                job.error = if job.cancelled { String::new() } else { e };
            }
        }
    });
    Ok(())
}

#[tauri::command]
fn cancel_setup(app: St) {
    cancel(&app);
}

fn cancel(app: &App) {
    let child = {
        let mut job = app.job.lock().unwrap();
        job.cancelled = true;
        job.child.take()
    };
    // killed outside the lock so the status poll does not stall
    if let Some(mut child) = child {
        // make's compiler children too
        let mut k = Command::new("taskkill");
        k.args(["/F", "/T", "/PID", &child.id().to_string()]);
        hide_window(&mut k);
        let _ = k.status();
        let _ = child.wait();
    }
}

#[tauri::command]
fn play(app: St) -> Result<(), String> {
    let mut game = app.game.lock().unwrap();
    if game.as_mut().is_some_and(|c| matches!(c.try_wait(), Ok(None))) {
        return Ok(());
    }
    *app.crash.lock().unwrap() = None;
    let log = fs::File::create(app.work.join("game.log")).map_err(|e| e.to_string())?;
    let child = app
        .runtime()
        .args(["extracted", "--play"])
        .env("LARECOMP_GAME_LIB", app.lib())
        .stdin(Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log)
        .spawn()
        .map_err(|e| format!("Could not start the game: {e}"))?;
    *game = Some(child);
    Ok(())
}

#[tauri::command]
fn set_setting(app: St, key: String, value: String) -> Result<(), String> {
    if !SETTING_KEYS.contains(&key.as_str()) || value.contains(['\n', '\r']) {
        return Err("Unknown setting.".into());
    }
    let text = fs::read_to_string(app.ini()).unwrap_or_default();
    let mut lines: Vec<String> = text.lines().filter(|l| l.split_once('=').map(|(k, _)| k.trim()) != Some(key.as_str())).map(String::from).collect();
    lines.push(format!("{key} = {value}"));
    fs::create_dir_all(&app.work).map_err(|e| e.to_string())?;
    fs::write(app.ini(), lines.join("\n") + "\n").map_err(|e| e.to_string())
}

#[tauri::command]
fn dismiss_crash(app: St) {
    *app.crash.lock().unwrap() = None;
}

#[tauri::command]
fn open_path(app: St, which: String) -> Result<(), String> {
    let p = match which.as_str() {
        "work" => app.work.clone(),
        "setup_log" => app.work.join("setup.log"),
        "game_log" => app.work.join("game.log"),
        // the file name is the one the backend recorded, never a path from the page
        "crash_log" => match &*app.crash.lock().unwrap() {
            Some(c) => app.work.join(&c.log),
            None => return Err("There is no crash log.".into()),
        },
        _ => return Err("Unknown path.".into()),
    };
    if !p.exists() {
        return Err(format!("{} does not exist yet.", p.display()));
    }
    Command::new("explorer").arg(p).spawn().map(|_| ()).map_err(|e| e.to_string())
}

fn main() {
    let exe_dir = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)).unwrap_or_default();
    let mode = Mode::detect(&exe_dir);
    let work = match &mode {
        Mode::Dev(dev) => std::env::var("KFL_WORK").map(PathBuf::from).unwrap_or_else(|_| dev.repo.join("..").join("work").join("kfl")),
        Mode::Packaged => exe_dir,
    };
    let app = Arc::new(App {
        work: fs::canonicalize(&work).map(|p| PathBuf::from(p.to_string_lossy().trim_start_matches(r"\\?\"))).unwrap_or(work),
        mode,
        job: Arc::new(Mutex::new(Job::default())),
        game: Mutex::new(None),
        crash: Mutex::new(None),
    });
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(app)
        .invoke_handler(tauri::generate_handler![status, set_game_dir, start_setup, cancel_setup, play, set_setting, open_path, dismiss_crash])
        .run(tauri::generate_context!())
        .expect("launcher failed to start");
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dev(repo: &str) -> Dev {
        Dev { repo: repo.into(), msys: PathBuf::new() }
    }

    fn app(repo: &str, work: &str) -> App {
        App { mode: Mode::Dev(dev(repo)), work: work.into(), job: Arc::new(Mutex::new(Job::default())), game: Mutex::new(None), crash: Mutex::new(None) }
    }

    #[test]
    fn work_from_repo_is_relative_on_one_drive() {
        assert_eq!(dev(r"E:\a\repo").work_from_repo(Path::new(r"E:\a\kfl-e2e")), PathBuf::from(r"..\kfl-e2e"));
        assert_eq!(dev(r"E:\a\repo").work_from_repo(Path::new(r"E:\b\w")), PathBuf::from(r"..\..\b\w"));
        assert_eq!(dev(r"E:\a\repo").work_from_repo(Path::new(r"D:\w")), PathBuf::from(r"D:\w"));
    }

    #[test]
    fn the_bundled_tools_select_packaged_mode() {
        let root = scratch("mode");
        assert!(matches!(Mode::detect(&root), Mode::Dev(_)));
        for tool in ["lasgen\\lasgen.exe", "zig\\zig.exe"] {
            fs::create_dir_all(root.join(tool).parent().unwrap()).unwrap();
            fs::write(root.join(tool), "").unwrap();
        }
        assert!(matches!(Mode::detect(&root), Mode::Packaged));
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn setup_lines_map_onto_the_phases() {
        assert_eq!(parse_setup_line("phase copying 0.010"), Some(SetupLine::Progress(Phase::Image, 0.01, None)));
        assert_eq!(parse_setup_line("phase reading 0.200"), Some(SetupLine::Progress(Phase::Translate, 0.2, None)));
        assert_eq!(parse_setup_line("phase finishing 0.990"), Some(SetupLine::Progress(Phase::Compile, 0.99, None)));
        assert_eq!(
            parse_setup_line("phase compiling 0.925 (main_180.c, 7 min)"),
            Some(SetupLine::Progress(Phase::Compile, 0.925, Some("(main_180.c, 7 min)".into())))
        );
        assert_eq!(parse_setup_line("done"), Some(SetupLine::Done));
        assert_eq!(parse_setup_line("failed version"), Some(SetupLine::Failed("version".into())));
        assert_eq!(parse_setup_line("[paths] Forgotten Land Recomp 0.1.0"), None);
        assert_eq!(parse_setup_line("phase someday 0.5"), None);
        assert!(setup_failure("version").contains("not Kirby"));
    }

    #[test]
    fn a_failed_setup_reports_its_reason() {
        let root = scratch("setupfail");
        let app = app("", "");
        let mut c = Command::new("cmd");
        c.args(["/C", "echo phase compiling 0.500& echo failed copy& exit 1"]);
        let r = run_setup(&app, c, &mut fs::File::create(root.join("log")).unwrap());
        assert_eq!(r, Err(setup_failure("copy")));
        assert_eq!(app.job.lock().unwrap().progress, Some(0.5));
        fs::remove_dir_all(&root).unwrap();
    }

    fn scratch(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("kfl-test-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn junction_survives_shell_metacharacters() {
        let root = scratch("meta");
        let src = root.join("D&Kirby^Co %PATH% (a b) 100%");
        fs::create_dir(&src).unwrap();
        fs::write(src.join("probe.txt"), "ok").unwrap();
        let link = root.join("extracted");
        link_game_dir(&link, &src).unwrap();
        assert_eq!(fs::read_to_string(link.join("probe.txt")).unwrap(), "ok");
        assert_eq!(fs::canonicalize(&link).unwrap(), fs::canonicalize(&src).unwrap());
        assert!(!root.join("Co").exists() && !root.join("PATH").exists());
        fs::remove_dir(&link).unwrap();
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn dead_junction_is_replaced_and_real_folder_is_kept() {
        let root = scratch("dead");
        let (old, new, link) = (root.join("old"), root.join("new"), root.join("extracted"));
        fs::create_dir(&old).unwrap();
        fs::create_dir(&new).unwrap();
        link_game_dir(&link, &old).unwrap();
        fs::remove_dir(&old).unwrap();
        assert!(!link.exists() && fs::symlink_metadata(&link).is_ok());
        link_game_dir(&link, &new).unwrap();
        assert_eq!(fs::canonicalize(&link).unwrap(), fs::canonicalize(&new).unwrap());
        fs::remove_dir(&link).unwrap();
        fs::create_dir(&link).unwrap();
        fs::write(link.join("keep.bin"), "x").unwrap();
        assert!(link_game_dir(&link, &new).is_err());
        assert!(link.join("keep.bin").is_file());
        fs::remove_dir_all(&root).unwrap();
    }

    const START: &str = "[paths] Forgotten Land Recomp 0.1.0, data: E:\\work\\kfl\n[gamelib] E:\\work\\kfl\\build\\lasgame.dll: main, 24961 functions\n[vk] t=2.1 frame 120 60.1 fps\n";

    #[test]
    fn a_fatal_report_is_a_crash() {
        let log = format!("{START}FATAL: LARECOMP_GAME_LIB: no file E:\\work\\kfl\\build\\lasgame.dll\nhost backtrace (guest functions are f_<addr>):\n  #0  00007ff6288c8208 lasrecomp+68208\n");
        assert_eq!(classify(3, &log), Outcome::Crashed("FATAL: LARECOMP_GAME_LIB: no file E:\\work\\kfl\\build\\lasgame.dll".into()));
    }

    #[test]
    fn an_exception_report_is_a_crash() {
        let log = format!("{START}\n*** exception c0000005 accessing 0xffffffffffffffff\n    host pc 7ff6288c8208 (?+0)\n    host lr 7ff6288617b8 (?)\nhost backtrace (guest functions are f_<addr>):\n  #0  00007ff6288c8208 lasrecomp+68208\nguest: sp=723fffe910 lr=700051f500\n");
        assert_eq!(classify(3, &log), Outcome::Crashed("*** exception c0000005 accessing 0xffffffffffffffff".into()));
    }

    #[test]
    fn the_marker_decides_even_when_the_exit_code_looks_normal() {
        assert!(matches!(classify(0, "FATAL: out of memory\n"), Outcome::Crashed(_)));
    }

    #[test]
    fn closing_the_window_is_a_normal_quit() {
        assert_eq!(classify(0, &format!("{START}window closed\n")), Outcome::Quit);
    }

    #[test]
    fn a_kill_without_a_report_is_not_a_crash() {
        assert_eq!(classify(1, START), Outcome::Stopped);
        assert_eq!(classify(-1073741510, START), Outcome::Crashed("Windows ended the game with error C000013A.".into()));
    }

    #[test]
    fn an_abort_without_a_report_is_a_crash() {
        assert!(matches!(classify(3, START), Outcome::Crashed(_)));
    }

    #[test]
    fn only_the_newest_crash_logs_are_kept() {
        let root = scratch("crashlogs");
        for i in 1..=7 {
            fs::write(root.join(format!("crash-20200101-00000{i}.log")), "old").unwrap();
        }
        for other in ["crash-notes.log", "crash-20200101-00000.log", "setup.log", "game.log"] {
            fs::write(root.join(other), "x").unwrap();
        }
        let name = keep_crash_log(&root);
        assert!(is_crash_log(&name) && fs::read_to_string(root.join(&name)).unwrap() == "x");
        let mut left: Vec<String> = fs::read_dir(&root).unwrap().flatten().map(|e| e.file_name().into_string().unwrap()).filter(|n| is_crash_log(n)).collect();
        left.sort();
        assert_eq!(left, ["crash-20200101-000004.log", "crash-20200101-000005.log", "crash-20200101-000006.log", "crash-20200101-000007.log", name.as_str()]);
        assert!(root.join("crash-notes.log").is_file() && root.join("setup.log").is_file());
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn the_tail_of_a_huge_log_is_read() {
        let root = scratch("tail");
        fs::write(root.join("game.log"), "x".repeat(300_000) + "\nFATAL: late\n").unwrap();
        assert!(log_tail(&root.join("game.log")).ends_with("FATAL: late\n"));
        assert!(log_tail(&root.join("game.log")).len() <= 1 << 16);
        assert_eq!(log_tail(&root.join("nope")), "");
        fs::remove_dir_all(&root).unwrap();
    }

    fn sleeper() -> Command {
        let mut c = Command::new("ping");
        c.args(["-n", "30", "127.0.0.1"]);
        c
    }

    #[test]
    fn cancel_before_spawn_starts_nothing() {
        let root = scratch("precancel");
        let app = app("", "");
        cancel(&app);
        let mut log = fs::File::create(root.join("log")).unwrap();
        let mut c = Command::new("cmd");
        c.args(["/C", "set /a 40+2"]);
        assert_eq!(run_step(&app, Phase::Runtime, c, &mut log), Err("Cancelled.".into()));
        std::thread::sleep(std::time::Duration::from_millis(1000));
        // a spawned step would have printed 42 into the log
        assert!(!fs::read_to_string(root.join("log")).unwrap().contains("42"));
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn cancel_kills_the_running_step() {
        let root = scratch("cancel");
        let app = Arc::new(app("", ""));
        let (a, log) = (Arc::clone(&app), root.join("log"));
        let t = std::thread::spawn(move || run_step(&a, Phase::Runtime, sleeper(), &mut fs::File::create(log).unwrap()));
        std::thread::sleep(std::time::Duration::from_millis(600));
        assert!(app.job.lock().unwrap().child.is_some());
        let started = std::time::Instant::now();
        cancel(&app);
        assert_eq!(t.join().unwrap(), Err("Cancelled.".into()));
        assert!(started.elapsed().as_secs() < 5 && app.job.lock().unwrap().child.is_none());
        fs::remove_dir_all(&root).unwrap();
    }
}
