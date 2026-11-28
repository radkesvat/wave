#pragma once

#include "browser_ui.h"
#include "preview_session.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace wave {

struct ConfigUsersState {
    enum class Source { offline, live, blocked, transition };
    enum class Editor { none, add, edit, remove, password, reset_traffic };
    enum class Section { account, access, limits, activity };
    enum class Detail { open, open_saved_password, lookup, password, toggle, review, reconcile, saved_password };
    enum class Mutation { create, edit, remove, password, toggle, reset_traffic };
    enum class Column : unsigned { id = 1, name, enabled, options, traffic_usage, upload_speed, download_speed, expiry, group };

    Source source = Source::blocked;
    int config_id = 0;
    std::string node;
    std::string run_id;
    std::string database_generation;
    std::string availability;
    std::string binding_error;
    nlohmann::json grants;
    Editor editor = Editor::none;
    Section section = Section::account;
    Detail detail = Detail::open;
    Mutation mutation = Mutation::edit;
    UsersClockSamples detail_clock;
    std::optional<std::uint64_t> subscription;
    std::optional<std::uint64_t> processed_roster_loss;
    std::vector<std::uint64_t> ordered_ids;
    Column sort_column = Column::id;
    nlohmann::json selected_user;
    nlohmann::json review_user;
    nlohmann::json database_status;
    std::optional<std::uint64_t> detail_request;
    std::optional<std::uint64_t> mutation_request;
    std::optional<std::uint64_t> status_request;
    std::string selection;
    std::string options_id;
    std::string user_id;
    std::string detail_id;
    std::string mutation_id;
    // A live create can confirm an occupied ID before the next roster update arrives.
    std::uint64_t recent_live_id = 0;
    Source mutation_source = Source::blocked;
    std::string mutation_run;
    std::string mutation_database;
    std::string create_target;
    std::string edit_token;
    std::string review_token;
    std::string name;
    std::string email;
    std::string notes;
    std::string network;
    std::string password;
    std::string saved_password;
    std::map<std::string, std::string, std::less<>> numbers;
    std::set<std::string, std::less<>> kept_numbers;
    std::string search_key;
    std::string search_input;
    std::string lookup_input;
    std::string validation;
    std::string list_error;
    std::string detail_error;
    std::string status_error;
    std::string mutation_message;
    std::vector<std::string> changed_fields;
    unsigned password_input_id = 0;
    bool connected = false;
    bool list_permitted = false;
    bool selection_from_roster = false;
    bool search_is_id = false;
    bool order_dirty = true;
    bool sort_descending = false;
    std::string_view viewed_table;
    bool draft_from_roster = false;
    bool draft_removed = false;
    bool draft_scope_lost = false;
    bool open_account = false;
    bool enabled = true;
    bool expected_enabled = true;
    bool editable = false;
    bool status_open = false;
    bool review_required = false;
    bool mutation_uncertain = false;
    bool missing_reconciled_create = false;
    bool create_target_lost = false;
    bool reconcile_needed = false;
    bool mutation_context_changed = false;
    std::size_t roster_generation = 0;
    std::size_t roster_commits = 0;
    double next_expiry_sort = 0;
    double sort_clock = 0;
    CanvasViewport scroll_bounds{};
};

struct LiveCreateReceipt {
    std::string id;
    std::string run;
    std::string database;
};

std::optional<LiveCreateReceipt> unconfirmed_create(ConfigUsersState& state, PreviewSession& session);
void restore_create_receipt(ConfigUsersState& state, const LiveCreateReceipt& receipt);
void sync_config_users(ConfigUsersState& state, PreviewSession& session, const nlohmann::json& access,
                       std::string_view core_state, bool active, bool legacy_live);
void render_config_users(ConfigUsersState& state, PreviewSession& session, int config_id,
                         std::string_view node, bool narrow);
void hide_config_users(ConfigUsersState& state, PreviewSession& session);
std::string describe_config_users(const ConfigUsersState& state, const PreviewSession& session);
void abandon_config_users(ConfigUsersState& state, PreviewSession& session);

} // namespace wave
