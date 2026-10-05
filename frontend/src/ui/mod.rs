mod dashboard;
mod session;
mod sync;

use ratatui::Frame;

use crate::app::{App, Screen};

const SPINNER: [&str; 4] = ["|", "/", "-", "\\"];

pub fn spinner(tick: usize) -> &'static str {
    SPINNER[tick % SPINNER.len()]
}

pub fn draw(frame: &mut Frame, app: &App) {
    match &app.screen {
        Screen::Dashboard => dashboard::draw(frame, app),
        Screen::Sync(state) => sync::draw(frame, app, state),
        Screen::Session(session) => session::draw(frame, app, session),
    }
}
