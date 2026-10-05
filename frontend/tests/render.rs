//! Render checks on ratatui's TestBackend, plus an end-to-end run against the
//! real `wkr-core` (skipped when the backend binary has not been built).

use ratatui::backend::TestBackend;
use ratatui::Terminal;
use serde_json::json;
use wkr::app::{App, Screen};
use wkr::backend::Backend;
use wkr::ui;

fn render(app: &App) -> String {
    let mut terminal = Terminal::new(TestBackend::new(100, 30)).unwrap();
    terminal.draw(|f| ui::draw(f, app)).unwrap();
    let buffer = terminal.backend().buffer().clone();
    (0..buffer.area.height)
        .map(|y| {
            (0..buffer.area.width)
                .map(|x| buffer[(x, y)].symbol().to_string())
                .collect::<String>()
        })
        .collect::<Vec<_>>()
        .join("\n")
}

fn dashboard_app() -> App {
    let mut app = App::new();
    app.take_outbox();
    app.on_reply(Ok(json!({
        "has_data": true,
        "leeches": [{
            "characters": "太", "meanings": "fat", "percentage_correct": 40,
            "srs_stage": "Apprentice 2", "fails_meaning": true,
            "neighbors_heading": "Look-alikes you also failed today:",
            "neighbors": [{"characters": "犬", "meaning": "dog", "srs_label": "[new]"}],
            "advice": "Focus on the extra dot."
        }],
        "due": {"pairs": 3, "burned": 7},
        "drill": {"total": 4, "correct": 3, "current_streak": 2},
        "missed_pairs": [{"subject": "太", "distractor": "犬", "miss_count": 2}],
        "sessions": {"session_count": 5, "avg_failures_per_session": 1.5, "sessions_last_7_days": 2},
        "trend": []
    })));
    app
}

#[test]
fn dashboard_shows_leeches_due_counts_and_stats() {
    let text = render(&dashboard_app());
    for expected in [
        "Leeches",
        "太",
        "40% correct",
        "You fail the MEANING",
        "Look-alikes",
        "犬",
        "Focus on the extra dot",
        "drill pairs: 3",
        "burned items: 7",
        "3/4 correct (75%)",
        "Not enough sync history yet",
    ] {
        assert!(text.contains(expected), "missing {expected:?} in:\n{text}");
    }
}

#[test]
fn empty_dashboard_prompts_for_a_sync() {
    let mut app = App::new();
    app.take_outbox();
    app.on_reply(Ok(json!({"has_data": false})));
    let text = render(&app);
    assert!(text.contains("No data yet - press s to sync."), "{text}");
}

#[test]
fn session_screen_shows_prompt_options_and_feedback() {
    let mut app = dashboard_app();
    app.on_key(ratatui::crossterm::event::KeyEvent::from(
        ratatui::crossterm::event::KeyCode::Char('d'),
    ));
    app.take_outbox();
    app.on_reply(Ok(json!({"total": 2})));
    app.take_outbox();
    app.on_reply(Ok(json!({"done": false, "question": {
        "kind": "forced_choice", "prompt": "Which one means \"fat\"?", "options": ["太", "犬"], "index": 0}})));
    let text = render(&app);
    assert!(text.contains("Which one means"), "{text}");
    assert!(text.contains("1. 太") && text.contains("2. 犬"), "{text}");

    app.on_key(ratatui::crossterm::event::KeyEvent::from(
        ratatui::crossterm::event::KeyCode::Char('2'),
    ));
    app.take_outbox();
    app.on_reply(Ok(
        json!({"correct": false, "detail": "The correct answer was: 太",
                           "advice": "Mind the dot.", "reason": "Often confused."}),
    ));
    let text = render(&app);
    assert!(
        text.contains("Incorrect. The correct answer was: 太"),
        "{text}"
    );
    assert!(text.contains("Mind the dot."), "{text}");
    assert!(matches!(app.screen, Screen::Session(_)));
}

fn core_binary() -> Option<std::path::PathBuf> {
    let path = std::env::var_os("WKR_CORE")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| {
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../backend/build/wkr-core")
        });
    path.exists().then_some(path)
}

#[test]
fn talks_to_the_real_backend_over_stdio() {
    let Some(core) = core_binary() else {
        eprintln!("skipped: wkr-core not built");
        return;
    };
    let data = std::env::temp_dir().join(format!("wkr_fe_test_{}", std::process::id()));
    std::env::set_var("XDG_DATA_HOME", &data);
    std::env::set_var("XDG_CONFIG_HOME", &data);

    let mut backend = Backend::spawn(&core, &[]).unwrap();
    backend.send("kana.convert", json!({"text": "kanji"}));
    backend.send("dashboard.get", json!({}));
    backend.send("session.next", json!({}));
    let wait = std::time::Duration::from_secs(10);

    let kana = backend.replies.recv_timeout(wait).unwrap().result.unwrap();
    assert_eq!(kana["hiragana"], "かんじ");
    let dash = backend.replies.recv_timeout(wait).unwrap().result.unwrap();
    assert_eq!(dash["has_data"], false); // fresh, unsynced database
    let err = backend
        .replies
        .recv_timeout(wait)
        .unwrap()
        .result
        .unwrap_err();
    assert_eq!(err, "no active session");
    let _ = std::fs::remove_dir_all(data);
}
