use ratatui::layout::{Constraint, Layout};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph, Wrap};
use ratatui::Frame;

use crate::app::App;
use crate::protocol::Dashboard;

pub fn draw(frame: &mut Frame, app: &App) {
    let [main, footer] =
        Layout::vertical([Constraint::Min(3), Constraint::Length(2)]).areas(frame.area());
    let [left, right] =
        Layout::horizontal([Constraint::Percentage(62), Constraint::Percentage(38)]).areas(main);

    let leeches = Paragraph::new(leech_lines(&app.dashboard))
        .block(Block::default().borders(Borders::ALL).title(" Leeches "))
        .wrap(Wrap { trim: false })
        .scroll((app.scroll, 0));
    frame.render_widget(leeches, left);

    let stats = Paragraph::new(stats_lines(&app.dashboard))
        .block(Block::default().borders(Borders::ALL).title(" Progress "))
        .wrap(Wrap { trim: false });
    frame.render_widget(stats, right);

    let mut footer_lines =
        vec![
            Line::from("d drill   b burned review   s sync   r refresh   j/k scroll   q quit")
                .style(Style::default().fg(Color::DarkGray)),
        ];
    let status = if app.is_busy() {
        format!("{} working...", super::spinner(app.tick))
    } else {
        app.status.clone()
    };
    footer_lines.push(Line::from(status).style(Style::default().fg(Color::Yellow)));
    frame.render_widget(Paragraph::new(footer_lines), footer);
}

fn leech_lines(d: &Dashboard) -> Vec<Line<'static>> {
    if d.leeches.is_empty() {
        let text = if d.has_data {
            "No leeches found yet. Keep reviewing and sync again."
        } else {
            "No data yet."
        };
        return vec![Line::from(text)];
    }
    let mut lines = Vec::new();
    for l in &d.leeches {
        lines.push(Line::from(vec![
            Span::styled(
                format!("{}  ", l.characters),
                Style::default().fg(Color::Red).add_modifier(Modifier::BOLD),
            ),
            Span::raw(format!(
                "{}   {}% correct   {}",
                l.meanings, l.percentage_correct, l.srs_stage
            )),
        ]));
        let (fail, other) = if l.fails_meaning {
            ("MEANING", "reading")
        } else {
            ("READING", "meaning")
        };
        lines.push(Line::from(format!(
            "  You fail the {fail}, not the {other}."
        )));
        if !l.neighbors_heading.is_empty() {
            lines.push(Line::from(format!("  {}", l.neighbors_heading)));
            for n in &l.neighbors {
                lines.push(Line::from(format!(
                    "    {} {} {}",
                    n.characters, n.meaning, n.srs_label
                )));
            }
        }
        if !l.advice.is_empty() {
            lines.push(Line::styled("  Focus:", Style::default().fg(Color::Cyan)));
            lines.extend(l.advice.lines().map(|a| Line::from(format!("    {a}"))));
        }
        lines.push(Line::default());
    }
    lines
}

fn stats_lines(d: &Dashboard) -> Vec<Line<'static>> {
    let heading = |t: &str| Line::styled(t.to_string(), Style::default().fg(Color::Cyan));
    let mut lines = vec![
        heading("Due now"),
        Line::from(format!("  drill pairs: {}", d.due.pairs)),
        Line::from(format!("  burned items: {}", d.due.burned)),
        Line::default(),
        heading("Drill history"),
    ];
    if d.drill.total > 0 {
        let pct = 100 * d.drill.correct / d.drill.total;
        lines.push(Line::from(format!(
            "  {}/{} correct ({}%), streak {}",
            d.drill.correct, d.drill.total, pct, d.drill.current_streak
        )));
        for m in &d.missed_pairs {
            lines.push(Line::from(format!(
                "  {} vs {}  missed {}x",
                m.subject, m.distractor, m.miss_count
            )));
        }
    } else {
        lines.push(Line::from("  No drills recorded yet"));
    }
    lines.push(Line::default());
    lines.push(heading("Sessions"));
    if d.sessions.session_count > 0 {
        lines.push(Line::from(format!(
            "  {} sessions, {:.1} failures avg, {} in last 7 days",
            d.sessions.session_count,
            d.sessions.avg_failures_per_session,
            d.sessions.sessions_last_7_days
        )));
    } else {
        lines.push(Line::from("  No sessions recorded yet"));
    }
    lines.push(Line::default());
    lines.push(heading("Leech trend"));
    let trends: Vec<_> = d.trend.iter().filter(|t| t.snapshot_count >= 2).collect();
    if trends.is_empty() {
        lines.push(Line::from("  Not enough sync history yet"));
    }
    for t in trends {
        lines.push(Line::from(format!(
            "  {}  {}% -> {}% ({:+})",
            t.characters, t.first_percentage, t.last_percentage, t.delta
        )));
    }
    lines
}
