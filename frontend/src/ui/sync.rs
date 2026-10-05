use ratatui::layout::{Constraint, Layout};
use ratatui::style::{Color, Style};
use ratatui::text::Line;
use ratatui::widgets::{Block, Borders, Paragraph, Wrap};
use ratatui::Frame;

use crate::app::{App, SyncState};

pub fn draw(frame: &mut Frame, app: &App, state: &SyncState) {
    let [area] = Layout::vertical([Constraint::Min(3)]).areas(frame.area());
    let lines = match state {
        SyncState::Running => vec![Line::from(format!(
            "{} Syncing with WaniKani...",
            super::spinner(app.tick)
        ))],
        SyncState::Done(s) => {
            let mut v = vec![
                Line::styled("Sync complete", Style::default().fg(Color::Green)),
                Line::from(format!("  {} subjects", s.subjects)),
                Line::from(format!("  {} stats ({} failures)", s.stats, s.failures)),
                Line::from(format!("  {} assignments", s.assignments)),
                Line::from(format!("  {} study materials", s.study_materials)),
            ];
            if s.similarity_edge_count >= 0 {
                v.push(Line::from(format!(
                    "  rebuilt similarity graph ({} edges)",
                    s.similarity_edge_count
                )));
            }
            v.push(Line::default());
            v.push(Line::styled(
                "Enter to continue",
                Style::default().fg(Color::DarkGray),
            ));
            v
        }
        SyncState::Failed(msg) => vec![
            Line::styled("Sync failed", Style::default().fg(Color::Red)),
            Line::from(msg.clone()),
            Line::default(),
            Line::styled("Enter to continue", Style::default().fg(Color::DarkGray)),
        ],
    };
    frame.render_widget(
        Paragraph::new(lines)
            .block(Block::default().borders(Borders::ALL).title(" Sync "))
            .wrap(Wrap { trim: false }),
        area,
    );
}
