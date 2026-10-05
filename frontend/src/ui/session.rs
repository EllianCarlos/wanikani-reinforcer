use ratatui::layout::{Constraint, Layout};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::Line;
use ratatui::widgets::{Block, Borders, Gauge, Paragraph, Wrap};
use ratatui::Frame;

use crate::app::{App, Phase, Session, SessionKind};

pub fn draw(frame: &mut Frame, app: &App, s: &Session) {
    let [gauge_area, body, input_area, hint] = Layout::vertical([
        Constraint::Length(3),
        Constraint::Min(5),
        Constraint::Length(3),
        Constraint::Length(1),
    ])
    .areas(frame.area());

    let title = match s.kind {
        SessionKind::Drill => " Drill ",
        SessionKind::Burned => " Burned review ",
    };
    let answered = s.question.as_ref().map_or(0, |q| q.index);
    let ratio = if s.total > 0 {
        (answered as f64 / s.total as f64).clamp(0.0, 1.0)
    } else {
        0.0
    };
    frame.render_widget(
        Gauge::default()
            .block(Block::default().borders(Borders::ALL).title(title))
            .gauge_style(Style::default().fg(Color::Cyan))
            .ratio(ratio)
            .label(format!("{}/{}", answered, s.total)),
        gauge_area,
    );

    frame.render_widget(
        Paragraph::new(body_lines(app, s))
            .block(Block::default().borders(Borders::ALL))
            .wrap(Wrap { trim: false }),
        body,
    );

    if s.phase == Phase::Asking {
        let shown = if s.preview.is_empty() || s.preview == s.input {
            s.input.clone()
        } else {
            format!("{}  ->  {}", s.input, s.preview)
        };
        frame.render_widget(
            Paragraph::new(format!("> {shown}"))
                .block(Block::default().borders(Borders::ALL).title(" Answer ")),
            input_area,
        );
        let cursor_x = input_area.x + 3 + shown.chars().count() as u16;
        frame.set_cursor_position((
            cursor_x.min(input_area.right().saturating_sub(2)),
            input_area.y + 1,
        ));
    }

    let hint_text = match s.phase {
        Phase::Asking => "Enter submit   Esc abort   (number keys pick an option)",
        Phase::Feedback => "Enter continue   Esc abort",
        Phase::Summary => "Enter back to dashboard",
        _ => "",
    };
    frame.render_widget(
        Paragraph::new(hint_text).style(Style::default().fg(Color::DarkGray)),
        hint,
    );
}

fn body_lines(app: &App, s: &Session) -> Vec<Line<'static>> {
    match s.phase {
        Phase::Loading | Phase::Aborting => {
            vec![Line::from(format!(
                "{} working...",
                super::spinner(app.tick)
            ))]
        }
        Phase::Summary => vec![
            Line::styled(
                "Session complete",
                Style::default().add_modifier(Modifier::BOLD),
            ),
            Line::from(format!("{}/{} correct", s.score.correct, s.score.asked)),
        ],
        Phase::Asking | Phase::Feedback => {
            let mut lines = Vec::new();
            if let Some(q) = &s.question {
                lines.push(Line::styled(
                    q.prompt.clone(),
                    Style::default().add_modifier(Modifier::BOLD),
                ));
                if q.is_forced_choice() {
                    lines.push(Line::default());
                    for (i, o) in q.options.iter().enumerate() {
                        lines.push(Line::from(format!("  {}. {}", i + 1, o)));
                    }
                }
            }
            if let Some(fb) = &s.feedback {
                lines.push(Line::default());
                if fb.retry {
                    lines.push(Line::styled(
                        fb.detail.clone(),
                        Style::default().fg(Color::Yellow),
                    ));
                } else if fb.correct {
                    lines.push(Line::styled("Correct!", Style::default().fg(Color::Green)));
                } else {
                    lines.push(Line::styled(
                        format!("Incorrect. {}", fb.detail),
                        Style::default().fg(Color::Red),
                    ));
                    if !fb.advice.is_empty() {
                        lines.push(Line::from(fb.advice.clone()));
                    }
                }
                if !fb.reason.is_empty() {
                    lines.push(Line::styled(
                        fb.reason.clone(),
                        Style::default().fg(Color::DarkGray),
                    ));
                }
            }
            lines
        }
    }
}
