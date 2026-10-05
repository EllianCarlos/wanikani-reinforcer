#pragma once

#include <nlohmann/json.hpp>

#include "app.h"
#include "burned.h"
#include "drill.h"
#include "report.h"
#include "sync.h"

// nlohmann ADL serializers for everything the RPC layer sends to a client.

void to_json(nlohmann::json& j, const DrillQuestion& q);
void to_json(nlohmann::json& j, const DrillFeedback& f);
void to_json(nlohmann::json& j, const BurnedQuestion& q);
void to_json(nlohmann::json& j, const BurnedFeedback& f);
void to_json(nlohmann::json& j, const ReportNeighbor& n);
void to_json(nlohmann::json& j, const ReportEntry& e);
void to_json(nlohmann::json& j, const SyncSummary& s);
void to_json(nlohmann::json& j, const Dashboard& d);
