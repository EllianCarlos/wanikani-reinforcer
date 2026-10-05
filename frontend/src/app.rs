//! UI state machine. Pure: key presses and backend replies go in, requests
//! for the backend come out via `take_outbox`, so it is testable without a
//! terminal or a child process.

use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use serde::de::DeserializeOwned;
use serde_json::{json, Value};

use crate::protocol::*;

#[derive(Debug, Clone, PartialEq)]
pub enum Pending {
    Dashboard,
    Sync,
    Start,
    Next,
    Answer,
    Abort,
    /// The text the preview was requested for.
    Kana(String),
}

#[derive(Debug, Clone)]
pub struct Request {
    pub method: &'static str,
    pub params: Value,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum SessionKind {
    Drill,
    Burned,
}

impl SessionKind {
    pub fn rpc_name(self) -> &'static str {
        match self {
            SessionKind::Drill => "drill",
            SessionKind::Burned => "burned",
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Phase {
    Loading,
    Asking,
    Feedback,
    Summary,
    Aborting,
}

#[derive(Debug, Clone)]
pub struct Session {
    pub kind: SessionKind,
    pub phase: Phase,
    pub total: i64,
    pub question: Option<Question>,
    pub feedback: Option<Feedback>,
    pub score: Score,
    pub input: String,
    pub preview: String,
}

#[derive(Debug, Clone)]
pub enum SyncState {
    Running,
    Done(SyncSummary),
    Failed(String),
}

#[derive(Debug, Clone)]
pub enum Screen {
    Dashboard,
    Sync(SyncState),
    Session(Session),
}

pub struct App {
    pub screen: Screen,
    pub dashboard: Dashboard,
    pub status: String,
    pub scroll: u16,
    pub should_quit: bool,
    /// Ticks, for the spinner.
    pub tick: usize,
    busy: Option<Pending>,
    outbox: Vec<Request>,
}

impl Default for App {
    fn default() -> Self {
        Self::new()
    }
}

impl App {
    pub fn new() -> App {
        let mut app = App {
            screen: Screen::Dashboard,
            dashboard: Dashboard::default(),
            status: "Loading...".to_string(),
            scroll: 0,
            should_quit: false,
            tick: 0,
            busy: None,
            outbox: Vec::new(),
        };
        app.refresh_dashboard();
        app
    }

    pub fn is_busy(&self) -> bool {
        self.busy.is_some()
    }

    pub fn take_outbox(&mut self) -> Vec<Request> {
        std::mem::take(&mut self.outbox)
    }

    fn send(&mut self, pending: Pending, method: &'static str, params: Value) {
        self.busy = Some(pending);
        self.outbox.push(Request { method, params });
    }

    fn refresh_dashboard(&mut self) {
        self.send(Pending::Dashboard, "dashboard.get", json!({}));
    }

    fn start_session(&mut self, kind: SessionKind) {
        self.screen = Screen::Session(Session {
            kind,
            phase: Phase::Loading,
            total: 0,
            question: None,
            feedback: None,
            score: Score::default(),
            input: String::new(),
            preview: String::new(),
        });
        self.send(
            Pending::Start,
            "session.start",
            json!({"kind": kind.rpc_name()}),
        );
    }

    pub fn on_key(&mut self, key: KeyEvent) {
        if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
            self.should_quit = true;
            return;
        }
        if self.is_busy() {
            return; // one request in flight at a time; ignore input meanwhile
        }
        match self.screen {
            Screen::Dashboard => self.on_dashboard_key(key),
            Screen::Sync(_) => self.on_sync_key(key),
            Screen::Session(_) => self.on_session_key(key),
        }
    }

    fn on_dashboard_key(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Char('q') | KeyCode::Esc => self.should_quit = true,
            KeyCode::Char('d') => self.start_session(SessionKind::Drill),
            KeyCode::Char('b') => self.start_session(SessionKind::Burned),
            KeyCode::Char('s') => {
                self.screen = Screen::Sync(SyncState::Running);
                self.send(Pending::Sync, "sync.run", json!({}));
            }
            KeyCode::Char('r') => self.refresh_dashboard(),
            KeyCode::Down | KeyCode::Char('j') => self.scroll = self.scroll.saturating_add(1),
            KeyCode::Up | KeyCode::Char('k') => self.scroll = self.scroll.saturating_sub(1),
            _ => {}
        }
    }

    fn on_sync_key(&mut self, key: KeyEvent) {
        if matches!(key.code, KeyCode::Enter | KeyCode::Esc | KeyCode::Char('q')) {
            self.screen = Screen::Dashboard;
            self.refresh_dashboard();
        }
    }

    fn on_session_key(&mut self, key: KeyEvent) {
        let Screen::Session(session) = &mut self.screen else {
            return;
        };
        match session.phase {
            Phase::Loading | Phase::Aborting => {}
            Phase::Summary => {
                if matches!(key.code, KeyCode::Enter | KeyCode::Esc | KeyCode::Char('q')) {
                    self.screen = Screen::Dashboard;
                    self.refresh_dashboard();
                }
            }
            Phase::Feedback => match key.code {
                KeyCode::Enter | KeyCode::Char(' ') => {
                    let retry = session.feedback.as_ref().is_some_and(|f| f.retry);
                    session.input.clear();
                    session.preview.clear();
                    if retry {
                        session.phase = Phase::Asking;
                    } else {
                        session.phase = Phase::Loading;
                        self.send(Pending::Next, "session.next", json!({}));
                    }
                }
                KeyCode::Esc => self.abort(),
                _ => {}
            },
            Phase::Asking => {
                let forced = session
                    .question
                    .as_ref()
                    .is_some_and(Question::is_forced_choice);
                let reading = session.question.as_ref().is_some_and(Question::is_reading);
                let option_count = session.question.as_ref().map_or(0, |q| q.options.len());
                match key.code {
                    KeyCode::Esc => self.abort(),
                    KeyCode::Enter => {
                        if !session.input.trim().is_empty() {
                            let answer = session.input.clone();
                            self.submit(answer);
                        }
                    }
                    KeyCode::Backspace => {
                        session.input.pop();
                        self.request_preview(reading);
                    }
                    KeyCode::Char(c) => {
                        if forced && session.input.is_empty() {
                            if let Some(n) = c.to_digit(10) {
                                if n >= 1 && (n as usize) <= option_count {
                                    self.submit(n.to_string());
                                    return;
                                }
                            }
                        }
                        session.input.push(c);
                        self.request_preview(reading);
                    }
                    _ => {}
                }
            }
        }
    }

    fn submit(&mut self, answer: String) {
        if let Screen::Session(s) = &mut self.screen {
            s.phase = Phase::Loading;
        }
        self.send(Pending::Answer, "session.answer", json!({"answer": answer}));
    }

    fn abort(&mut self) {
        if let Screen::Session(s) = &mut self.screen {
            s.phase = Phase::Aborting;
        }
        self.send(Pending::Abort, "session.abort", json!({}));
    }

    fn request_preview(&mut self, reading: bool) {
        let Screen::Session(s) = &mut self.screen else {
            return;
        };
        if !reading {
            return;
        }
        if s.input.is_empty() {
            s.preview.clear();
            return;
        }
        let text = s.input.clone();
        self.send(
            Pending::Kana(text.clone()),
            "kana.convert",
            json!({"text": text}),
        );
    }

    /// Feeds the reply to the request last sent (replies arrive in order).
    pub fn on_reply(&mut self, result: Result<Value, String>) {
        let Some(pending) = self.busy.take() else {
            return;
        };
        match result {
            Err(message) => self.on_error(pending, message),
            Ok(value) => self.on_ok(pending, value),
        }
    }

    fn on_error(&mut self, pending: Pending, message: String) {
        match pending {
            Pending::Sync => self.screen = Screen::Sync(SyncState::Failed(message)),
            Pending::Kana(_) => {} // preview is best-effort
            _ => {
                self.status = message;
                if !matches!(pending, Pending::Dashboard) {
                    self.screen = Screen::Dashboard;
                    self.refresh_dashboard();
                }
            }
        }
    }

    fn on_ok(&mut self, pending: Pending, value: Value) {
        match pending {
            Pending::Dashboard => {
                if let Some(d) = decode::<Dashboard>(value, &mut self.status) {
                    self.status = if d.has_data {
                        String::new()
                    } else {
                        "No data yet - press s to sync.".to_string()
                    };
                    self.dashboard = d;
                    self.scroll = 0;
                }
            }
            Pending::Sync => {
                if let Some(s) = decode::<SyncSummary>(value, &mut self.status) {
                    self.screen = Screen::Sync(SyncState::Done(s));
                }
            }
            Pending::Start => {
                let Some(r) = decode::<StartResult>(value, &mut self.status) else {
                    self.screen = Screen::Dashboard;
                    return;
                };
                if let Some(msg) = r.nothing_due {
                    self.status = msg;
                    self.screen = Screen::Dashboard;
                    return;
                }
                if let Screen::Session(s) = &mut self.screen {
                    s.total = r.total.unwrap_or(0);
                }
                self.send(Pending::Next, "session.next", json!({}));
            }
            Pending::Next => {
                let Some(r) = decode::<NextResult>(value, &mut self.status) else {
                    return;
                };
                if let Screen::Session(s) = &mut self.screen {
                    s.input.clear();
                    s.preview.clear();
                    s.feedback = None;
                    if r.done {
                        s.score = Score {
                            asked: r.asked.unwrap_or(0),
                            correct: r.correct.unwrap_or(0),
                        };
                        s.phase = Phase::Summary;
                    } else {
                        s.question = r.question;
                        s.phase = Phase::Asking;
                    }
                }
            }
            Pending::Answer => {
                if let Some(fb) = decode::<Feedback>(value, &mut self.status) {
                    if let Screen::Session(s) = &mut self.screen {
                        s.feedback = Some(fb);
                        s.phase = Phase::Feedback;
                    }
                }
            }
            Pending::Abort => {
                self.screen = Screen::Dashboard;
                self.refresh_dashboard();
            }
            Pending::Kana(text) => {
                if let (Some(k), Screen::Session(s)) =
                    (decode::<Kana>(value, &mut self.status), &mut self.screen)
                {
                    if s.input == text {
                        s.preview = k.hiragana;
                    }
                }
            }
        }
    }
}

fn decode<T: DeserializeOwned>(value: Value, status: &mut String) -> Option<T> {
    match serde_json::from_value(value) {
        Ok(v) => Some(v),
        Err(e) => {
            *status = format!("unexpected backend response: {e}");
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ratatui::crossterm::event::{KeyEvent, KeyEventKind, KeyEventState};
    use serde_json::json;

    fn key(code: KeyCode) -> KeyEvent {
        KeyEvent {
            code,
            modifiers: KeyModifiers::NONE,
            kind: KeyEventKind::Press,
            state: KeyEventState::NONE,
        }
    }

    fn started_drill() -> App {
        let mut app = App::new();
        app.take_outbox();
        app.on_reply(Ok(json!({"has_data": true})));
        app.on_key(key(KeyCode::Char('d')));
        assert_eq!(app.take_outbox()[0].method, "session.start");
        app.on_reply(Ok(json!({"total": 2})));
        assert_eq!(app.take_outbox()[0].method, "session.next");
        app
    }

    fn question(kind: &str, options: &[&str]) -> Value {
        json!({"done": false, "question": {"kind": kind, "prompt": "Q?", "options": options, "index": 0}})
    }

    #[test]
    fn starts_by_requesting_the_dashboard() {
        let mut app = App::new();
        let out = app.take_outbox();
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].method, "dashboard.get");
        assert!(app.is_busy());
    }

    #[test]
    fn input_is_ignored_while_a_request_is_in_flight() {
        let mut app = App::new();
        app.on_key(key(KeyCode::Char('d')));
        assert!(matches!(app.screen, Screen::Dashboard));
    }

    #[test]
    fn nothing_due_returns_to_the_dashboard_with_a_message() {
        let mut app = App::new();
        app.take_outbox();
        app.on_reply(Ok(json!({"has_data": true})));
        app.on_key(key(KeyCode::Char('b')));
        app.take_outbox();
        app.on_reply(Ok(json!({"nothing_due": "Nothing due."})));
        assert!(matches!(app.screen, Screen::Dashboard));
        assert_eq!(app.status, "Nothing due.");
    }

    #[test]
    fn forced_choice_number_key_answers_immediately() {
        let mut app = started_drill();
        app.on_reply(Ok(question("forced_choice", &["a", "b", "c"])));
        app.on_key(key(KeyCode::Char('2')));
        let out = app.take_outbox();
        assert_eq!(out[0].method, "session.answer");
        assert_eq!(out[0].params, json!({"answer": "2"}));
    }

    #[test]
    fn reading_input_requests_a_kana_preview_and_applies_it() {
        let mut app = started_drill();
        app.on_reply(Ok(question("reading", &[])));
        app.on_key(key(KeyCode::Char('t')));
        let out = app.take_outbox();
        assert_eq!(out[0].method, "kana.convert");
        app.on_reply(Ok(json!({"hiragana": "t"})));
        app.on_key(key(KeyCode::Char('a')));
        app.take_outbox();
        app.on_reply(Ok(json!({"hiragana": "た"})));
        let Screen::Session(s) = &app.screen else {
            panic!()
        };
        assert_eq!(s.input, "ta");
        assert_eq!(s.preview, "た");
    }

    #[test]
    fn stale_kana_preview_is_dropped() {
        let mut app = started_drill();
        app.on_reply(Ok(question("reading", &[])));
        app.on_key(key(KeyCode::Char('t')));
        app.take_outbox();
        // Reply arrives for "t" but input already moved on (simulated by
        // replying with a different pending text).
        if let Screen::Session(s) = &mut app.screen {
            s.input.push('a');
        }
        app.on_reply(Ok(json!({"hiragana": "t"})));
        let Screen::Session(s) = &app.screen else {
            panic!()
        };
        assert!(s.preview.is_empty());
    }

    #[test]
    fn retry_feedback_reasks_the_same_question_without_calling_next() {
        let mut app = started_drill();
        app.on_reply(Ok(question("reading", &[])));
        for c in "ab".chars() {
            app.on_key(key(KeyCode::Char(c)));
            app.take_outbox();
            app.on_reply(Ok(json!({"hiragana": ""})));
        }
        app.on_key(key(KeyCode::Enter));
        assert_eq!(app.take_outbox()[0].method, "session.answer");
        app.on_reply(Ok(
            json!({"correct": false, "retry": true, "detail": "Try again."}),
        ));
        app.on_key(key(KeyCode::Enter));
        assert!(app.take_outbox().is_empty());
        let Screen::Session(s) = &app.screen else {
            panic!()
        };
        assert_eq!(s.phase, Phase::Asking);
        assert!(s.input.is_empty());
    }

    #[test]
    fn finishing_shows_the_summary_then_returns_to_the_dashboard() {
        let mut app = started_drill();
        app.on_reply(Ok(json!({"done": true, "asked": 2, "correct": 1})));
        let Screen::Session(s) = &app.screen else {
            panic!()
        };
        assert_eq!(s.phase, Phase::Summary);
        assert_eq!(s.score.correct, 1);
        app.on_key(key(KeyCode::Enter));
        assert!(matches!(app.screen, Screen::Dashboard));
        assert_eq!(app.take_outbox()[0].method, "dashboard.get");
    }

    #[test]
    fn escape_aborts_the_session_then_refreshes() {
        let mut app = started_drill();
        app.on_reply(Ok(question("production", &[])));
        app.on_key(key(KeyCode::Esc));
        assert_eq!(app.take_outbox()[0].method, "session.abort");
        app.on_reply(Ok(json!({"asked": 0, "correct": 0})));
        assert!(matches!(app.screen, Screen::Dashboard));
        assert_eq!(app.take_outbox()[0].method, "dashboard.get");
    }

    #[test]
    fn a_failed_sync_is_shown_on_the_sync_screen() {
        let mut app = App::new();
        app.take_outbox();
        app.on_reply(Ok(json!({"has_data": false})));
        app.on_key(key(KeyCode::Char('s')));
        app.take_outbox();
        app.on_reply(Err("no API token".to_string()));
        assert!(matches!(&app.screen, Screen::Sync(SyncState::Failed(m)) if m == "no API token"));
    }
}
