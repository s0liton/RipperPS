// RipperPS: the Games tab. See games_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "games_page.hpp"

#include "ui/glyphs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ripper
{

namespace ui = hui::ui;
using hui::Action;
using hui::Direction;
using hui::audio::Cue;
using hui::gfx::Rect;

namespace
{
constexpr float kMargin = 96.0f;
constexpr float kRowH = 110.0f;
const Rect kSheet{120, 230, 1680, 760};
constexpr float kRowsHeight = 7 * RowList::kRowHeight;
} // namespace

void GamesPage::enter()
{
    sheet_ = false;
    app().refresh_games();
}

void GamesPage::open_sheet(const Game &g)
{
    const Settings &s = app().settings();
    const auto prof = s.profiles.find(g.system);
    game_ = g;
    name_ = g.name.empty() ? g.title : g.name;
    destination_ = prof != s.profiles.end() ? prof->second.destination : "";
    folder_ = prof != s.profiles.end() ? prof->second.folder : "{system}";
    options_ = prof != s.profiles.end() && prof->second.format == "dump" ? prof->second.options
                                                                         : std::map<std::string, std::string>{};
    if (!s.destination(destination_) && !s.destinations.empty())
        destination_ = s.destinations.front().id;
    sheet_ = true;
    busy_ = false;
    list_.set_focus(static_cast<int>(sheet_rows(nullptr).size()) - 1);
}

const std::vector<Option> *GamesPage::options() const
{
    const System *sys = app().catalog().system(game_.system);
    const FormatInfo *f = sys ? sys->format("dump") : nullptr;
    return f ? &f->options : nullptr;
}

std::string GamesPage::option_value(const Option &o) const
{
    const auto v = options_.find(o.id);
    return v != options_.end() ? v->second : o.def;
}

std::string GamesPage::options_json() const
{
    std::string out = "{";
    if (const auto *opts = options())
        for (const Option &o : *opts)
        {
            const std::string v = option_value(o);
            out += (out.size() > 1 ? "," : "") + json_string(o.id) + ":" +
                   (o.type == "toggle" ? (v == "1" || v == "true" ? "true" : "false") : json_string(v));
        }
    return out + "}";
}

std::vector<Row> GamesPage::sheet_rows(std::vector<int> *kinds) const
{
    std::vector<Row> rows;
    std::vector<int> local;
    std::vector<int> &k = kinds ? *kinds : local;
    k.clear();
    if (!game_.mounted)
    {
        rows.push_back({RowKind::info, game_.game_running ? "Closes the game that's running" : "Starts the game for a moment",
                        "", "The system mounts its files that way"});
        k.push_back(kNote);
    }
    if (const auto *opts = options())
        for (std::size_t i = 0; i < opts->size(); ++i)
        {
            const Option &o = (*opts)[i];
            const std::string v = option_value(o);
            if (o.type == "toggle")
                rows.push_back({RowKind::toggle, o.label, "", "", v == "1" || v == "true"});
            else
            {
                std::string label = v;
                for (const Choice &c : o.choices)
                    if (c.id == v)
                        label = c.label;
                rows.push_back({RowKind::choice, o.label, label});
            }
            k.push_back(kOption + static_cast<int>(i));
        }
    const Destination *dest = app().settings().destination(destination_);
    Row where{RowKind::choice, "Save to", dest ? dest->name : "No destination"};
    where.detail = dest ? dest->where : "Add one in Settings";
    where.enabled = app().settings().destinations.size() > 1;
    rows.push_back(where);
    k.push_back(kDestination);
    rows.push_back({RowKind::action, "Name", name_});
    k.push_back(kName);
    rows.push_back({RowKind::action, "Folder", folder_});
    k.push_back(kFolder);
    Row go{RowKind::action, busy_ ? "Starting" : "Dump"};
    go.enabled = !busy_ && dest && game_.complete && !game_.partial;
    if (!game_.complete)
        go.detail = "Still installing";
    else if (game_.partial)
        go.detail = "Not all of it is installed";
    rows.push_back(go);
    k.push_back(kGo);
    return rows;
}

bool GamesPage::update(const hui::InputFrame &input, float dt, ui::Feedback &feedback)
{
    App &a = app();
    const Look &l = look(current_look());
    bool started = false;
    if (entry_.active())
    {
        if (entry_.update(input, dt, feedback, l) == 1)
        {
            if (entry_for_ == kName && !entry_.text().empty())
                name_ = entry_.text();
            else if (entry_for_ == kFolder)
                folder_ = entry_.text();
        }
        return false;
    }
    if (sheet_)
    {
        if (input.is_pressed(Action::back))
        {
            sheet_ = false;
            feedback.play(Cue::back);
            return false;
        }
        std::vector<int> kinds;
        const std::vector<Row> rows = sheet_rows(&kinds);
        const RowEvent e = list_.handle(rows, input, feedback);
        if (e.row >= 0 && e.row < static_cast<int>(kinds.size()))
        {
            const int kind = kinds[static_cast<std::size_t>(e.row)];
            const int step = e.step ? e.step : 1;
            if (kind == kDestination && e.step)
            {
                const auto &dests = a.settings().destinations;
                const int n = static_cast<int>(dests.size());
                int at = 0;
                for (int i = 0; i < n; ++i)
                    if (dests[static_cast<std::size_t>(i)].id == destination_)
                        at = i;
                if (n)
                    destination_ = dests[static_cast<std::size_t>(((at + e.step) % n + n) % n)].id;
            }
            else if (kind == kName && e.activated)
            {
                entry_for_ = kName;
                entry_.open("Name the dump", name_);
            }
            else if (kind == kFolder && e.activated)
            {
                entry_for_ = kFolder;
                entry_.open("Folder", folder_, false, "{system} and {name} are filled in");
            }
            else if (kind >= kOption)
            {
                const auto *opts = options();
                const std::size_t index = static_cast<std::size_t>(kind - kOption);
                if (opts && index < opts->size())
                {
                    const Option &o = (*opts)[index];
                    const std::string v = option_value(o);
                    if (o.type == "toggle")
                        options_[o.id] = v == "1" || v == "true" ? "0" : "1";
                    else
                    {
                        // Only the choices for this console (a PS4 backport isn't a PS5's).
                        std::vector<const Choice *> usable;
                        for (const Choice &c : o.choices)
                            if (c.only.empty() || c.only == game_.system)
                                usable.push_back(&c);
                        const int n = static_cast<int>(usable.size());
                        int at = 0;
                        for (int i = 0; i < n; ++i)
                            if (usable[static_cast<std::size_t>(i)]->id == v)
                                at = i;
                        if (n)
                            options_[o.id] = usable[static_cast<std::size_t>(((at + step) % n + n) % n)]->id;
                    }
                }
            }
            else if (kind == kGo && e.activated)
            {
                busy_ = true;
                a.dump(game_, destination_, name_, folder_, options_json(), [this](bool ok) {
                    busy_ = false;
                    if (ok)
                        sheet_ = false;
                });
                feedback.play(Cue::launch);
                feedback.rumble(0.5f, 0.12f);
                started = true;
            }
        }
        list_.update(dt, static_cast<int>(sheet_rows(nullptr).size()), kRowsHeight);
        return started;
    }
    const auto &games = a.games();
    const int count = static_cast<int>(games.size());
    row_ = std::clamp(row_, 0, std::max(0, count - 1));
    if ((input.nav == Direction::up || input.nav == Direction::down) && count > 0)
    {
        const int next = std::clamp(row_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
        if (next != row_)
        {
            row_ = next;
            feedback.play(Cue::focus);
        }
    }
    if (input.is_pressed(Action::west))
    {
        a.refresh_games();
        feedback.play(Cue::select);
    }
    if (input.is_pressed(Action::confirm) && count > 0)
    {
        open_sheet(games[static_cast<std::size_t>(row_)]);
        feedback.play(Cue::open);
    }
    position_.target = static_cast<float>(row_);
    position_.update(dt, 20.0f);
    scroll_.target = static_cast<float>(std::clamp(row_ - 5, 0, std::max(0, count - 6)));
    scroll_.update(dt, 14.0f);
    return false;
}

void GamesPage::draw(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look, Color accent, float clock,
                     float age) const
{
    const App &a = app();
    const float in = hui::tween::stagger(age, 1, 0.08f, 0.6f);
    list.push_opacity(in);
    ui::text(list, fonts.display, "Games", kMargin, 240, 64, look.text);
    const auto &games = a.games();
    if (games.empty())
    {
        ui::text(list, fonts.regular, a.games_loading() ? "Looking at what's installed." : "No PS4 or PS5 games installed.",
                 kMargin, 310, 28, look.muted);
        ui::text(list, fonts.regular, "Install a game from its disc to dump it.", kMargin, 348, 22,
                 look.muted.with_alpha(0.7f));
        list.pop_opacity();
        return;
    }
    char count[48];
    std::snprintf(count, sizeof count, games.size() == 1 ? "1 game" : "%d games", static_cast<int>(games.size()));
    ui::text(list, fonts.regular, count, 1824, 240, 24, look.muted, hui::gfx::Align::right);
    const float top = 300.0f;
    const float scroll = scroll_.value;
    const int first = std::max(0, static_cast<int>(std::floor(scroll)) - 1);
    list.rounded_rect({kMargin - 24, top + (position_.value - scroll) * kRowH, 1776, kRowH - 14}, 26,
                      look.text.with_alpha(0.1f));
    list.push_clip({0, top, 1920, 6 * kRowH});
    for (int i = first; i < static_cast<int>(games.size()) && i < first + 9; ++i)
    {
        const Game &g = games[static_cast<std::size_t>(i)];
        const float y = top + (static_cast<float>(i) - scroll) * kRowH;
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(g.title, 28, 1300), kMargin, y + 42, 28, look.text);
        std::string meta = std::string(g.system == "ps5" ? "PlayStation 5" : "PlayStation 4") + "  \xC2\xB7  " +
                           g.title_id + "  \xC2\xB7  " + human_size(g.size);
        ui::text(list, fonts.regular, fonts.regular.font->fit(meta, 22, 1300), kMargin, y + 80, 22, look.muted);
        if (!g.complete || g.partial)
            ui::text(list, fonts.semibold, g.complete ? "Can't dump yet" : "Still installing", 1780, y + 60, 22,
                     look.warning, hui::gfx::Align::right);
    }
    list.pop_clip();
    (void)accent;
    (void)clock;
    list.pop_opacity();
}

void GamesPage::draw_overlay(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look, float clock) const
{
    if (sheet_)
    {
        list.rounded_rect({0, 0, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight}, 0, look.page.with_alpha(0.7f));
        list.rounded_rect(kSheet, 44, look.page);
        draw_panel(list, look, kSheet, 44);
        const float x = kSheet.x + 72;
        ui::text(list, fonts.semibold, game_.system == "ps5" ? "PLAYSTATION 5" : "PLAYSTATION 4", x, kSheet.y + 100, 18,
                 look.accent, hui::gfx::Align::left, 4.0f);
        const float y = ui::paragraph(list, fonts.display, game_.title, x - 2, kSheet.y + 160, 44, 560, 52,
                                      look.panel_text, 3);
        ui::text(list, fonts.regular, game_.title_id + "  \xC2\xB7  " + human_size(game_.size), x, y, 22,
                 look.panel_muted);
        list_.draw(list, fonts, look, sheet_rows(nullptr), {kSheet.x + 720, kSheet.y + 60, 900, kRowsHeight}, clock);
    }
    entry_.draw(list, fonts, look, clock);
}

std::vector<ui::Hint> GamesPage::hints() const
{
    if (entry_.active())
        return entry_.hints();
    if (sheet_)
    {
        std::vector<int> kinds;
        const std::vector<Row> rows = sheet_rows(&kinds);
        const int focus = list_.focus();
        std::vector<ui::Hint> hints;
        if (focus >= 0 && focus < static_cast<int>(rows.size()))
        {
            const Row &r = rows[static_cast<std::size_t>(focus)];
            if (r.kind == RowKind::choice)
                hints.push_back({ui::Button::dpad, "Change"});
            else if (r.kind == RowKind::toggle)
                hints.push_back({ui::Button::cross, r.on ? "Turn off" : "Turn on"});
            else if (kinds[static_cast<std::size_t>(focus)] == kGo)
                hints.push_back({ui::Button::cross, "Dump"});
            else if (r.kind == RowKind::action)
                hints.push_back({ui::Button::cross, "Edit"});
        }
        hints.push_back({ui::Button::circle, "Back"});
        return hints;
    }
    std::vector<ui::Hint> hints{{ui::Button::l1, "Pages", ui::Button::r1}, {ui::Button::square, "Look again"}};
    if (!app().games().empty())
        hints.push_back({ui::Button::cross, "Dump"});
    return hints;
}

} // namespace ripper
