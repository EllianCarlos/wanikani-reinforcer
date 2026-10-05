use std::process::Command;
use std::time::Duration;

use anyhow::Result;
use ratatui::crossterm::event::{self, Event, KeyEventKind};

use wkr::app::{App, Request};
use wkr::backend::{locate_core, Backend};
use wkr::ui;

fn main() -> Result<()> {
    let core = locate_core();

    // `wkr <args>` keeps working as the scriptable CLI (cron `wkr sync`, ...):
    // hand any arguments straight to the backend.
    let args: Vec<String> = std::env::args().skip(1).collect();
    if !args.is_empty() {
        let status = Command::new(&core).args(&args).status()?;
        std::process::exit(status.code().unwrap_or(1));
    }

    let backend = Backend::spawn(&core, &[])?;
    let mut terminal = ratatui::init(); // also installs a panic hook that restores the terminal
    let result = run(&mut terminal, backend);
    ratatui::restore();
    result
}

fn run(terminal: &mut ratatui::DefaultTerminal, mut backend: Backend) -> Result<()> {
    let mut app = App::new();
    flush(&mut app, &mut backend);

    while !app.should_quit {
        terminal.draw(|f| ui::draw(f, &app))?;

        if event::poll(Duration::from_millis(80))? {
            if let Event::Key(key) = event::read()? {
                if key.kind == KeyEventKind::Press {
                    app.on_key(key);
                }
            }
        }
        app.tick = app.tick.wrapping_add(1);

        while let Ok(reply) = backend.replies.try_recv() {
            app.on_reply(reply.result);
        }
        flush(&mut app, &mut backend);
    }
    Ok(())
}

fn flush(app: &mut App, backend: &mut Backend) {
    for Request { method, params, .. } in app.take_outbox() {
        backend.send(method, params);
    }
}
