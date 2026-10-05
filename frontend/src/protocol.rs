//! Serde mirrors of the wkr-core JSON-RPC payloads (see docs/protocol.md).

use serde::Deserialize;

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Neighbor {
    pub characters: String,
    pub meaning: String,
    pub srs_label: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Leech {
    pub characters: String,
    pub meanings: String,
    pub percentage_correct: i64,
    pub srs_stage: String,
    pub fails_meaning: bool,
    #[serde(default)]
    pub neighbors_heading: String,
    #[serde(default)]
    pub neighbors: Vec<Neighbor>,
    #[serde(default)]
    pub advice: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Due {
    pub pairs: i64,
    pub burned: i64,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct DrillStats {
    pub total: i64,
    pub correct: i64,
    pub current_streak: i64,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct MissedPair {
    pub subject: String,
    pub distractor: String,
    pub miss_count: i64,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct SessionStats {
    pub session_count: i64,
    pub avg_failures_per_session: f64,
    pub sessions_last_7_days: i64,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Trend {
    pub characters: String,
    pub snapshot_count: i64,
    pub first_percentage: i64,
    pub last_percentage: i64,
    pub delta: i64,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Dashboard {
    pub has_data: bool,
    #[serde(default)]
    pub leeches: Vec<Leech>,
    #[serde(default)]
    pub due: Due,
    #[serde(default)]
    pub drill: DrillStats,
    #[serde(default)]
    pub missed_pairs: Vec<MissedPair>,
    #[serde(default)]
    pub sessions: SessionStats,
    #[serde(default)]
    pub trend: Vec<Trend>,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct SyncSummary {
    pub subjects: i64,
    pub stats: i64,
    pub assignments: i64,
    pub study_materials: i64,
    pub failures: i64,
    pub similarity_edge_count: i64,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct StartResult {
    pub total: Option<i64>,
    pub nothing_due: Option<String>,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Question {
    /// `forced_choice`, `production`, `meaning` or `reading`.
    pub kind: String,
    pub prompt: String,
    #[serde(default)]
    pub options: Vec<String>,
    #[serde(default)]
    pub index: i64,
}

impl Question {
    pub fn is_forced_choice(&self) -> bool {
        self.kind == "forced_choice"
    }

    /// Reading questions accept romaji, shown as live kana.
    pub fn is_reading(&self) -> bool {
        self.kind == "reading"
    }
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct NextResult {
    pub done: bool,
    pub question: Option<Question>,
    pub asked: Option<i64>,
    pub correct: Option<i64>,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Feedback {
    pub correct: bool,
    #[serde(default)]
    pub retry: bool,
    #[serde(default)]
    pub detail: String,
    #[serde(default)]
    pub advice: String,
    #[serde(default)]
    pub reason: String,
}

#[derive(Debug, Clone, Default, Deserialize)]
pub struct Score {
    pub asked: i64,
    pub correct: i64,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Kana {
    pub hiragana: String,
}
