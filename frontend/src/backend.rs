//! Client for `wkr-core serve`: a worker thread owns the child's pipes so the
//! UI thread never blocks on a slow call such as `sync.run`.

use std::io::{BufRead, BufReader, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::thread;

use anyhow::{anyhow, Context, Result};
use serde_json::{json, Value};

/// Where to find the backend binary: `$WKR_CORE`, then next to this
/// executable, then `PATH`.
pub fn locate_core() -> PathBuf {
    if let Some(p) = std::env::var_os("WKR_CORE") {
        return PathBuf::from(p);
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let sibling = dir.join("wkr-core");
            if sibling.exists() {
                return sibling;
            }
        }
    }
    PathBuf::from("wkr-core")
}

struct Job {
    tag: u64,
    method: String,
    params: Value,
}

pub struct Reply {
    pub result: Result<Value, String>,
}

pub struct Backend {
    jobs: Sender<Job>,
    pub replies: Receiver<Reply>,
    child: Child,
    next_tag: u64,
}

impl Backend {
    pub fn spawn(core: &std::path::Path, serve_args: &[&str]) -> Result<Backend> {
        let mut child = Command::new(core)
            .arg("serve")
            .args(serve_args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .with_context(|| format!("could not start {} (set WKR_CORE)", core.display()))?;
        let stdin = child.stdin.take().ok_or_else(|| anyhow!("no stdin"))?;
        let stdout = child.stdout.take().ok_or_else(|| anyhow!("no stdout"))?;

        let (jobs, job_rx) = channel::<Job>();
        let (reply_tx, replies) = channel::<Reply>();
        thread::spawn(move || worker(stdin, BufReader::new(stdout), job_rx, reply_tx));
        Ok(Backend {
            jobs,
            replies,
            child,
            next_tag: 1,
        })
    }

    /// Queues a request; its [`Reply`] arrives on `replies` (in send order).
    pub fn send(&mut self, method: &str, params: Value) {
        let tag = self.next_tag;
        self.next_tag += 1;
        let _ = self.jobs.send(Job {
            tag,
            method: method.to_string(),
            params,
        });
    }
}

impl Drop for Backend {
    fn drop(&mut self) {
        // Closing stdin (the worker owns it and exits once `jobs` drops) ends
        // the server; kill as a backstop so quitting never hangs.
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

fn worker(
    mut stdin: ChildStdin,
    mut stdout: BufReader<ChildStdout>,
    jobs: Receiver<Job>,
    replies: Sender<Reply>,
) {
    for job in jobs {
        let request =
            json!({"jsonrpc": "2.0", "id": job.tag, "method": job.method, "params": job.params});
        let result = exchange(&mut stdin, &mut stdout, &request);
        if replies.send(Reply { result }).is_err() {
            return;
        }
    }
}

fn exchange(
    stdin: &mut ChildStdin,
    stdout: &mut BufReader<ChildStdout>,
    request: &Value,
) -> Result<Value, String> {
    writeln!(stdin, "{request}").map_err(|e| format!("backend write failed: {e}"))?;
    stdin
        .flush()
        .map_err(|e| format!("backend write failed: {e}"))?;
    let mut line = String::new();
    let n = stdout
        .read_line(&mut line)
        .map_err(|e| format!("backend read failed: {e}"))?;
    if n == 0 {
        return Err("backend exited".to_string());
    }
    let mut response: Value =
        serde_json::from_str(&line).map_err(|e| format!("bad backend response: {e}"))?;
    if let Some(err) = response.get("error") {
        let message = err
            .get("message")
            .and_then(Value::as_str)
            .unwrap_or("unknown backend error");
        return Err(message.to_string());
    }
    Ok(response["result"].take())
}
