// Launcher backend. It never copies, uploads or bundles game files: the folder the player picks is linked into the
// work folder (a directory junction), and the game code is generated and compiled on this PC from that folder.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};

/// Kirby and the Forgotten Land, any region build of the base game (main.npdm's program id).
const PROGRAM_ID: u64 = 0x0100_4D30_0C5A_E000;
const MODULES: [&str; 3] = ["main", "sdk", "subsdk0"];
const SETTING_KEYS: [&str; 3] = ["fullscreen", "render_height", "volume"];

#[derive(Clone, Copy, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
enum Phase {
    Idle,
    Runtime,
    Image,
    Translate,
    Compile,
    Failed,
}

struct Job {
    phase: Phase,
    error: String,
    child: Option<Child>,
    cancelled: bool,
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
    repo: PathBuf,
    work: PathBuf,
    msys: PathBuf,
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
    error: String,
    settings: Vec<(String, String)>,
    crash: Option<Crash>,
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
        self.work.join("build").join("lasrecomp.exe")
    }
    fn lib(&self) -> PathBuf {
        self.work.join("build").join("lasgame.dll")
    }
    fn image(&self) -> PathBuf {
        self.game().join("romfs.bin")
    }
    fn ini(&self) -> PathBuf {
        self.work.join("larecomp.ini")
    }
    fn tool_path(&self) -> String {
        let old = std::env::var("PATH").unwrap_or_default();
        format!("{};{};{old}", self.msys.join("ucrt64\\bin").display(), self.msys.join("usr\\bin").display())
    }
    fn tools_missing(&self) -> Vec<String> {
        let mut missing = Vec::new();
        for (name, path) in [
            ("MSYS2 make", self.msys.join("usr\\bin\\make.exe")),
            ("MSYS2 clang", self.msys.join("ucrt64\\bin\\clang.exe")),
            ("Python environment (.venv)", self.repo.join(".venv\\Scripts\\python.exe")),
        ] {
            if !path.is_file() {
                missing.push(name.to_string());
            }
        }
        missing
    }
    // WORK relative to the repo, on one drive: the link line carries about 540 object paths, which only fit what Windows
    // allows (Argument list too long) while WORK is a short relative path
    fn work_from_repo(&self) -> PathBuf {
        let repo = fs::canonicalize(&self.repo).unwrap_or(self.repo.clone());
        let strip = |p: &Path| PathBuf::from(p.to_string_lossy().trim_start_matches(r"\\?\").to_lowercase());
        let (r, w) = (strip(&repo), strip(&self.work));
        let (rc, wc): (Vec<_>, Vec<_>) = (r.components().collect(), w.components().collect());
        if rc.first() != wc.first() {
            return self.work.clone();
        }
        let same = rc.iter().zip(&wc).take_while(|(a, b)| a == b).count();
        let mut rel = PathBuf::new();
        (same..rc.len()).for_each(|_| rel.push(".."));
        self.work.components().skip(same).for_each(|c| rel.push(c));
        rel
    }
    fn make(&self, args: &[&str]) -> Command {
        let mut c = Command::new(self.msys.join("usr\\bin\\make.exe"));
        // MSYS make wants forward slashes
        let work = self.work_from_repo().to_string_lossy().replace('\\', "/");
        c.current_dir(&self.repo).env("PATH", self.tool_path()).env("MSYSTEM", "UCRT64").arg(format!("WORK={work}")).args(args);
        hide_window(&mut c);
        c
    }
    // returns at once when a valid image exists
    fn build_image(&self) -> Command {
        let mut c = Command::new(self.exe());
        c.args(["extracted", "--build-romfs"]).current_dir(&self.work).env("PATH", self.tool_path());
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
    let game = app.game();
    let (game_dir, game_error) = if game.join("exefs").is_dir() {
        let shown = fs::canonicalize(&game).unwrap_or(game.clone());
        (Some(shown.to_string_lossy().trim_start_matches(r"\\?\").to_string()), check_game(&game).err())
    } else {
        (None, None)
    };
    let running = app.poll_game();
    let job = app.job.lock().unwrap();
    let progress = match job.phase {
        Phase::Compile => {
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
        built: app.exe().is_file() && app.lib().is_file() && app.image().is_file(),
        running,
        phase: job.phase,
        progress,
        error: job.error.clone(),
        settings: app.settings(),
        crash: app.crash.lock().unwrap().clone(),
    }
}

#[tauri::command]
fn set_game_dir(app: St, path: String) -> Result<(), String> {
    let src = PathBuf::from(path);
    check_game(&src)?;
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

fn run_step(app: &App, phase: Phase, mut cmd: Command, log: &mut fs::File) -> Result<(), String> {
    let out = log.try_clone().map_err(|e| e.to_string())?;
    let err = log.try_clone().map_err(|e| e.to_string())?;
    let _ = writeln!(log, "\n== {cmd:?}");
    {
        // the flag check and the registration share one lock hold, so a cancel never lands between spawn and registration
        let mut job = app.job.lock().unwrap();
        if job.cancelled {
            return Err("Cancelled.".into());
        }
        let child = cmd.stdin(Stdio::null()).stdout(out).stderr(err).spawn().map_err(|e| format!("{cmd:?}: {e}"))?;
        job.phase = phase;
        job.child = Some(child);
    }
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
    check_game(&app.game())?;
    let missing = app.tools_missing();
    if !missing.is_empty() {
        return Err(format!("Missing build tools: {}.", missing.join(", ")));
    }
    {
        let mut job = app.job.lock().unwrap();
        if !matches!(job.phase, Phase::Idle | Phase::Failed) {
            return Ok(());
        }
        *job = Job { phase: Phase::Runtime, error: String::new(), child: None, cancelled: false };
    }
    let app = Arc::clone(&app);
    std::thread::spawn(move || {
        let jobs = format!("-j{}", std::thread::available_parallelism().map_or(4, |n| n.get()));
        let result = fs::File::create(app.work.join("setup.log")).map_err(|e| e.to_string()).and_then(|mut log| {
            let exe = format!("{}/build/lasrecomp.exe", app.work_from_repo().to_string_lossy().replace('\\', "/"));
            run_step(&app, Phase::Runtime, app.make(&[&jobs, &exe]), &mut log)?;
            run_step(&app, Phase::Image, app.build_image(), &mut log)?;
            run_step(&app, Phase::Translate, app.make(&["gen"]), &mut log)?;
            run_step(&app, Phase::Compile, app.make(&[&jobs]), &mut log)
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
    let child = Command::new(app.exe())
        .args(["extracted", "--play"])
        .current_dir(&app.work)
        .env("PATH", app.tool_path())
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
    let repo = find_repo();
    let work = std::env::var("KFL_WORK").map(PathBuf::from).unwrap_or_else(|_| repo.join("..").join("work").join("kfl"));
    let app = Arc::new(App {
        work: fs::canonicalize(&work).map(|p| PathBuf::from(p.to_string_lossy().trim_start_matches(r"\\?\"))).unwrap_or(work),
        repo,
        msys: std::env::var("KFL_MSYS2").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from(r"C:\msys64")),
        job: Arc::new(Mutex::new(Job { phase: Phase::Idle, error: String::new(), child: None, cancelled: false })),
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

    fn app(repo: &str, work: &str) -> App {
        App { repo: repo.into(), work: work.into(), msys: PathBuf::new(), job: Arc::new(Mutex::new(Job { phase: Phase::Idle, error: String::new(), child: None, cancelled: false })), game: Mutex::new(None), crash: Mutex::new(None) }
    }

    #[test]
    fn work_from_repo_is_relative_on_one_drive() {
        assert_eq!(app(r"E:\a\repo", r"E:\a\kfl-e2e").work_from_repo(), PathBuf::from(r"..\kfl-e2e"));
        assert_eq!(app(r"E:\a\repo", r"E:\b\w").work_from_repo(), PathBuf::from(r"..\..\b\w"));
        assert_eq!(app(r"E:\a\repo", r"D:\w").work_from_repo(), PathBuf::from(r"D:\w"));
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
