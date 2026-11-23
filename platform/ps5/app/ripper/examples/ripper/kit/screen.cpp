// RipperPS - the screen: Drives, Queue and Settings for the ripper payload.
// Copyright (C) 2026 BlackBearReloaded (the Aurora Shelf design it grew from)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// RommPS's shell (examples/rommps/kit/screen.cpp in RomM Sync), grown from
// PS5_VKHomebrewUI's "Aurora Shelf". What the screen shows comes from the
// payload through ripper_app.hpp; the screen only draws it and sends what the
// pad asks for.
//
//   - L1 / R1 move between Drives, Queue and Settings.
//   - Drives: a card per drive. A disc going in slides onto it and spins
//     while it's read; then its label says what it is, and its cover (an
//     album's, a game's) is printed on it. Cross opens the confirm sheet
//     (what to rip it to, where, under which name), Triangle ejects, Square
//     reads the disc again. While it rips: the stage, how far, the time left.
//   - Queue: the rips, each in two stages (reading the disc, then the copy or
//     upload to its destination); holding Cross cancels one, Cross sends a
//     failed one again, Square clears the finished ones.
//   - Settings (settings_page.hpp): the look, ripping, destinations, what each
//     kind of disc rips to, and what's been ripped.
//
// Until the payload answers the screen says so, and starts the copy RipperPS
// carries. Every colour comes from the look picked in Settings
// (ripper_look.hpp): the disc's colours, or one of the kit's themes.

#include "concepts/concepts.hpp"

#include "cJSON.h"
#include "core/tween.hpp"
#include "ripper_app.hpp"
#include "ripper_look.hpp"
#include "rows.hpp"
#include "games_page.hpp"
#include "settings_page.hpp"
#include "text_entry.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace hui::screen
{

namespace
{

using gfx::Color;
using gfx::Rect;
using ripper::App;
using ripper::sentence;

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);

constexpr float kMargin = 96.0f;
constexpr float kPi = 3.14159265f;

enum Tab
{
    kDrives,
    kGames,
    kQueue,
    kSettings,
    kTabs
};
constexpr const char *kTabNames[kTabs] = {"Drives", "Games", "Queue", "Settings"};

constexpr const char *kTechniques[] = {
    "The background service's state, read through its local API on a worker thread",
    "A card per drive: a disc that spins while it's read, then wears its cover",
    "Frosted confirm sheet over the blurred screen, with the kit's keyboard",
    "The kit's thirty themes, shared with RommPS",
};

// "12 min left", "" when it isn't known.
std::string left(double eta)
{
    return eta > 0 ? ripper::duration(eta) + " left" : "";
}

// A job's current stage in words: "Ripping", "Moving to Local Location", "Uploading to RomM".
std::string stage_label(const ripper::Job &j)
{
    if (j.stage == "transfer")
        return (j.transfer_kind == "upload" ? "Uploading to " : "Moving to ") +
               (j.destination_name.empty() ? std::string("the destination") : j.destination_name);
    if (!j.phase.empty() && j.phase != "reading" && j.phase != "converting")
        return sentence(j.phase);
    return j.phase == "converting" ? "Converting" : "Ripping";
}

class RipperPS final : public app::Concept
{
  public:
    explicit RipperPS(app::Context &context) : context_(context), app_(ripper::app())
    {
        // Before the first answer is read (that's in update): names keep what these fonts can draw
        ripper::set_text_fonts(context.fonts);
        apply_palette(ripper::Palette{}, true);
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "ripper",
            "RipperPS",
            "Rip games, music and movies from the drives",
            "examples/ripper/kit/screen.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x76d6ff),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        sheet_open_ = false;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        update_frame(input, dt, feedback);
        // Interface sounds turned off in Settings: what this frame asked for
        // isn't played. Rumble still is.
        if (!ripper::ui_sounds())
            feedback.cues.clear();
    }

    void update_frame(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        age_ += dt;
        clock_ += dt;
        app_.tick(dt, static_cast<gfx::Renderer *>(ripper_renderer()));
        cancel_seen_ = false;
        const bool ready = app_.status().reachable;
        track_discs();

        const bool busy = sheet_open_ || entry_.active() || (tab_ == kSettings && settings_.nested()) ||
                          (tab_ == kGames && games_.nested());
        if (ready && !busy)
        {
            if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
            {
                tab_ = (tab_ + (input.is_pressed(Action::page_next) ? 1 : kTabs - 1)) % kTabs;
                tab_age_ = 0.0f;
                feedback.play(audio::Cue::tab);
                if (tab_ == kSettings)
                    settings_.enter();
                if (tab_ == kGames)
                    games_.enter();
            }
        }
        if (!ready)
        {
            sheet_open_ = false;
            // ripper not running and RipperPS couldn't start it: Cross tries again.
            if (app_.status().known && !app_.payload_starting() && input.is_pressed(Action::confirm))
            {
                app_.start_payload();
                feedback.play(audio::Cue::focus);
            }
        }
        else if (entry_.active())
        {
            if (entry_.update(input, dt, feedback, look()) == 1)
            {
                if (entry_for_ == kName && !entry_.text().empty())
                {
                    sheet_name_ = entry_.text();
                    // A movie renamed to its real title: its poster and year, by that name.
                    if (const ripper::Drive *d = app_.drive(sheet_drive_); d && d->group == "movies")
                        app_.lookup(d->id, sheet_name_);
                }
                else if (entry_for_ == kFolder)
                    sheet_folder_ = entry_.text();
            }
        }
        else if (sheet_open_)
            update_sheet(input, feedback);
        else if (tab_ == kDrives)
            update_drives(input, feedback);
        else if (tab_ == kQueue)
            update_queue(input, dt, feedback);
        else if (tab_ == kGames)
        {
            if (games_.update(input, dt, feedback))
            {
                tab_ = kQueue;
                tab_age_ = 0.0f;
                job_row_ = 0;
            }
        }
        else
            settings_.update(input, dt, feedback);

        // ---- animation state ----
        tab_age_ += dt;
        tab_position_.target = static_cast<float>(tab_);
        tab_position_.update(dt, 16.0f);
        // The backdrop eases toward the disc in focus: its cover's colours, else its system's.
        ripper::Palette target;
        if (const ripper::Drive *d = focused_drive(); d && d->has_offer)
        {
            target = ripper::system_palette(d->group, d->system);
            if (const ripper::Cover *c = app_.cover(*d))
                target = c->palette;
        }
        apply_palette(target, false);
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 4.0f);
        nudge_.update(dt, 9.0f);
        sheet_.target = sheet_open_ ? 1.0f : 0.0f;
        sheet_.update(dt, context_.settings.reduced_motion ? 40.0f : 13.0f);
        drive_position_.target = static_cast<float>(drive_);
        drive_position_.update(dt, 20.0f);
        sheet_list_.update(dt, static_cast<int>(sheet_rows(nullptr).size()), kSheetRowsHeight);
        const int jcount = static_cast<int>(app_.jobs().size());
        job_row_ = std::clamp(job_row_, 0, std::max(0, jcount - 1));
        if (!cancel_seen_) // focus left the job (another row or tab): the hold is gone
        {
            cancel_hold_ = 0;
            cancel_job_ = 0;
        }
        job_position_.target = static_cast<float>(job_row_);
        job_position_.update(dt, 20.0f);
        // The six-row window, eased so the list scrolls with the focus.
        job_scroll_.target = static_cast<float>(std::clamp(job_row_ - 5, 0, std::max(0, jcount - 6)));
        job_scroll_.update(dt, 14.0f);
    }

    void draw(app::Frame &frame) const override
    {
        const ripper::Look &L = look();
        if (L.covers)
        {
            frame.backdrop.mode = gfx::BackdropMode::aurora;
            for (int i = 0; i < 4; ++i)
                frame.backdrop.colors[i] = palette_[i].value();
        }
        else
            frame.backdrop = L.backdrop;
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        const float back = sheet_.value;
        list.push_transform(1.0f - 0.035f * back, 960, 540, 0, 0);
        draw_top_bar(list);
        if (!app_.status().reachable)
            draw_not_ready(list);
        else if (tab_ == kDrives)
            draw_drives(list);
        else if (tab_ == kQueue)
            draw_queue(list);
        else if (tab_ == kGames)
            games_.draw(list, context_.fonts, L, accent(), clock_, tab_age_);
        else
            settings_.draw(list, context_.fonts, L, clock_, tab_age_);
        list.pop_transform();
        if (back > 0.01f)
        {
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, L.page.with_alpha(0.45f * back));
            frame.glass = true;
            draw_sheet(frame.overlay, frame.glass_texture);
        }
        // The hints first: the toast keeps clear of them.
        hints_left_ = 1920.0f;
        const bool over = back > 0.5f || entry_.active() || (tab_ == kSettings && settings_.nested()) ||
                          (tab_ == kGames && games_.nested());
        if (tab_ == kSettings)
            settings_.draw_overlay(frame.overlay, context_.fonts, L, clock_);
        if (tab_ == kGames)
            games_.draw_overlay(frame.overlay, context_.fonts, L, clock_);
        entry_.draw(frame.overlay, context_.fonts, L, clock_);
        draw_hints(over ? frame.overlay : frame.scene);
        draw_toast(over ? frame.overlay : frame.scene);
    }

  private:
    // ---------------------------------------------------------------- look

    static const ripper::Look &look()
    {
        return ripper::look(ripper::current_look());
    }

    // Highlights: the disc's colour, or the theme's accent.
    Color accent() const
    {
        return look().covers ? palette_[3].value() : look().accent;
    }

    // The backdrop's four colours, from the disc (or the default).
    void apply_palette(const ripper::Palette &p, bool snap)
    {
        const Color targets[4] = {
            gfx::mix(p.dark, Color::rgb(0x05060c), 0.35f),
            gfx::mix(p.dark, p.mid, 0.35f),
            p.mid,
            gfx::mix(p.mid, p.accent, 0.55f),
        };
        for (int i = 0; i < 4; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
    }

    void draw_panel(gfx::DrawList &list, const Rect &r, float radius = -1.0f) const
    {
        ripper::draw_panel(list, look(), r, radius);
    }

    // ---------------------------------------------------------------- data

    // The drive the Drives tab's focus is on (nullptr when there is none).
    const ripper::Drive *focused_drive() const
    {
        if (!app_.status().reachable)
            return nullptr;
        if (sheet_open_)
            return app_.drive(sheet_drive_);
        if (tab_ != kDrives)
            return nullptr;
        const auto &drives = app_.drives();
        return drive_ >= 0 && drive_ < static_cast<int>(drives.size()) ? &drives[static_cast<std::size_t>(drive_)]
                                                                        : nullptr;
    }

    // When each drive's disc came in and when it was identified, for the
    // disc's animations: it slides in and spins while it's read, and its
    // label changes once it's known.
    struct DiscMotion
    {
        int seq = -1;
        std::string state;
        float since = 0.0f;     // the clock when the state changed
        float known_at = -1.0f; // when it became ready, -1 before
        float spin = 0.0f;      // the disc's angle
        float speed = 0.0f;     // radians a second, eased
    };

    void track_discs()
    {
        for (const ripper::Drive &d : app_.drives())
        {
            DiscMotion &m = motion_[d.id];
            if (m.state != d.state || m.seq != d.disc_seq)
            {
                if (m.state == "empty" || m.state.empty() || d.state == "loading")
                    m.since = clock_;
                if ((d.state == "ready" || d.state == "unsupported" || d.state == "installing") &&
                    m.seq != d.disc_seq)
                    m.known_at = clock_;
                if (d.state == "empty")
                    m.known_at = -1.0f;
                m.state = d.state;
                m.seq = d.disc_seq;
            }
        }
    }

    // ---------------------------------------------------------------- input

    void refuse(app::Feedback &feedback, float direction)
    {
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        nudge_direction_ = direction;
        nudge_.trigger();
    }

    void update_drives(const InputFrame &input, app::Feedback &feedback)
    {
        const int count = static_cast<int>(app_.drives().size());
        // Drives come and go (a USB drive pulled out).
        drive_ = std::clamp(drive_, 0, std::max(0, count - 1));
        if ((input.nav == Direction::left || input.nav == Direction::right) && count > 0)
        {
            const int next = drive_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < count)
            {
                drive_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
                refuse(feedback, input.nav == Direction::right ? 1.0f : -1.0f);
        }
        const ripper::Drive *d = focused_drive();
        if (!d)
            return;
        if (input.is_pressed(Action::confirm))
        {
            if (d->job && app_.job(d->job))
            {
                // Ripping: its row in the queue.
                const auto &jobs = app_.jobs();
                for (std::size_t i = 0; i < jobs.size(); ++i)
                    if (jobs[i].id == d->job)
                        job_row_ = static_cast<int>(i);
                tab_ = kQueue;
                tab_age_ = 0.0f;
                feedback.play(audio::Cue::tab);
            }
            else if (d->rippable())
            {
                open_sheet(*d);
                feedback.play(audio::Cue::open);
            }
            else
                refuse(feedback, 0.0f);
        }
        else if (input.is_pressed(Action::north))
        {
            // Not while the console installs from it: that would stop the install.
            if (d->state == "ripping" || d->state == "empty" || d->state == "installing")
                refuse(feedback, 0.0f);
            else
            {
                app_.eject(d->id);
                feedback.play(audio::Cue::back);
            }
        }
        else if (input.is_pressed(Action::west))
        {
            if (!d->has_disc || d->state == "ripping" || d->reading())
                refuse(feedback, 0.0f);
            else
            {
                app_.identify(d->id);
                feedback.play(audio::Cue::select);
            }
        }
    }

    // ---------------------------------------------------------------- the confirm sheet

    static constexpr float kSheetRowsHeight = 7 * ripper::RowList::kRowHeight;

    enum SheetItem
    {
        kFormat,
        kDestination,
        kName,
        kFolder,
        kGo,
        kOption = 1000, // kOption + the option's index in the format's list
    };

    void open_sheet(const ripper::Drive &d)
    {
        sheet_drive_ = d.id;
        sheet_seq_ = d.disc_seq;
        sheet_open_ = true;
        sheet_busy_ = false;
        // The system's profile picks the format and destination; a format
        // that can't be used here gives way to the first one that can.
        sheet_format_ = d.suggestion.format;
        const ripper::Format *f = sheet_format_info(d);
        if (!f || !f->available)
            for (const ripper::Format &other : d.formats)
                if (other.available)
                {
                    sheet_format_ = other.id;
                    break;
                }
        sheet_destination_ = d.suggestion.destination;
        const auto &dests = app_.settings().destinations;
        if (!app_.settings().destination(sheet_destination_) && !dests.empty())
            sheet_destination_ = dests.front().id;
        sheet_name_ = d.suggestion.name.empty() ? d.disc_name() : d.suggestion.name;
        sheet_folder_ = d.suggestion.folder;
        load_options(d);
        sheet_list_.set_focus(static_cast<int>(sheet_rows(nullptr).size()) - 1);
    }

    // The format's options: the profile's when it's the profile's format, else the defaults.
    void load_options(const ripper::Drive &d)
    {
        sheet_options_.clear();
        if (sheet_format_ == d.suggestion.format)
        {
            std::unique_ptr<cJSON, void (*)(cJSON *)> j(cJSON_Parse(d.suggestion.options.c_str()), cJSON_Delete);
            const cJSON *v;
            cJSON_ArrayForEach(v, j.get())
            {
                if (cJSON_IsString(v))
                    sheet_options_[v->string] = v->valuestring;
                else if (cJSON_IsBool(v))
                    sheet_options_[v->string] = cJSON_IsTrue(v) ? "1" : "0";
                else if (cJSON_IsNumber(v))
                    sheet_options_[v->string] = std::to_string(v->valueint);
            }
        }
    }

    const ripper::Format *sheet_format_info(const ripper::Drive &d) const
    {
        for (const ripper::Format &f : d.formats)
            if (f.id == sheet_format_)
                return &f;
        return nullptr;
    }

    // The catalog's options of the sheet's format.
    const std::vector<ripper::Option> *sheet_option_list(const ripper::Drive &d) const
    {
        const ripper::System *sys = app_.catalog().system(d.system);
        const ripper::FormatInfo *f = sys ? sys->format(sheet_format_) : nullptr;
        return f ? &f->options : nullptr;
    }

    std::string option_value(const ripper::Option &o) const
    {
        const auto v = sheet_options_.find(o.id);
        return v != sheet_options_.end() ? v->second : o.def;
    }

    // The options as the API takes them: toggles as booleans.
    std::string options_json(const ripper::Drive &d) const
    {
        std::string out = "{";
        if (const auto *opts = sheet_option_list(d))
            for (const ripper::Option &o : *opts)
            {
                const std::string v = option_value(o);
                out += (out.size() > 1 ? "," : "") + ripper::json_string(o.id) + ":" +
                       (o.type == "toggle" ? (v == "1" || v == "true" ? "true" : "false") : ripper::json_string(v));
            }
        return out + "}";
    }

    // The folder the rip goes in, as the payload will name it.
    std::string sheet_folder_shown(const ripper::Drive &d) const
    {
        std::string folder = sheet_folder_;
        auto put = [&](const std::string &key, const std::string &value) {
            for (std::size_t at; (at = folder.find(key)) != std::string::npos;)
                folder.replace(at, key.size(), value);
        };
        put("{system}", d.system);
        put("{name}", sheet_name_);
        put("{title}", sheet_name_);
        put("{artist}", d.disc.artist.empty() ? "Unknown Artist" : d.disc.artist);
        return folder;
    }

    std::vector<ripper::Row> sheet_rows(std::vector<int> *kinds) const
    {
        std::vector<ripper::Row> rows;
        std::vector<int> local;
        std::vector<int> &k = kinds ? *kinds : local;
        k.clear();
        const ripper::Drive *d = app_.drive(sheet_drive_);
        if (!d)
            return rows;
        const ripper::Format *f = sheet_format_info(*d);
        ripper::Row format{ripper::RowKind::choice, "Rip to", f ? f->label : sheet_format_};
        if (f && !f->available)
            format.detail = f->reason.empty() ? "Not available for this disc" : sentence(f->reason);
        format.enabled = d->formats.size() > 1;
        rows.push_back(format);
        k.push_back(kFormat);
        const ripper::Destination *dest = app_.settings().destination(sheet_destination_);
        ripper::Row where{ripper::RowKind::choice, "Save to", dest ? dest->name : "No destination"};
        where.detail = dest ? dest->where : "Add one in Settings";
        where.enabled = app_.settings().destinations.size() > 1;
        rows.push_back(where);
        k.push_back(kDestination);
        rows.push_back({ripper::RowKind::action, "Name", sheet_name_, "Cross to rename"});
        k.push_back(kName);
        const std::string folder = sheet_folder_shown(*d);
        rows.push_back({ripper::RowKind::action, "Folder", folder.empty() ? "-" : folder});
        k.push_back(kFolder);
        if (const auto *opts = sheet_option_list(*d))
            for (std::size_t i = 0; i < opts->size(); ++i)
            {
                const ripper::Option &o = (*opts)[i];
                const std::string v = option_value(o);
                if (o.type == "toggle")
                    rows.push_back({ripper::RowKind::toggle, o.label, "", "", v == "1" || v == "true"});
                else
                {
                    std::string label = v;
                    for (const ripper::Choice &c : o.choices)
                        if (c.id == v)
                            label = c.label;
                    rows.push_back({ripper::RowKind::choice, o.label, label});
                }
                k.push_back(kOption + static_cast<int>(i));
            }
        ripper::Row go{ripper::RowKind::action, sheet_busy_ ? "Starting" : "Rip"};
        go.enabled = !sheet_busy_ && f && f->available && dest && d->disc_seq == sheet_seq_ && d->rippable();
        if (f && !f->available)
            go.detail = "Pick another format";
        else if (d->disc_seq != sheet_seq_)
            go.detail = "The disc changed";
        rows.push_back(go);
        k.push_back(kGo);
        return rows;
    }

    void update_sheet(const InputFrame &input, app::Feedback &feedback)
    {
        const ripper::Drive *d = app_.drive(sheet_drive_);
        if (input.is_pressed(Action::back) || !d)
        {
            sheet_open_ = false;
            feedback.play(audio::Cue::back);
            return;
        }
        std::vector<int> kinds;
        const std::vector<ripper::Row> rows = sheet_rows(&kinds);
        const ripper::RowEvent e = sheet_list_.handle(rows, input, feedback);
        if (e.row < 0 || e.row >= static_cast<int>(kinds.size()))
            return;
        const int kind = kinds[static_cast<std::size_t>(e.row)];
        const int step = e.step ? e.step : 1;
        if (kind == kFormat && e.step && !d->formats.empty())
        {
            int at = 0;
            const int n = static_cast<int>(d->formats.size());
            for (int i = 0; i < n; ++i)
                if (d->formats[static_cast<std::size_t>(i)].id == sheet_format_)
                    at = i;
            sheet_format_ = d->formats[static_cast<std::size_t>(((at + e.step) % n + n) % n)].id;
            load_options(*d);
        }
        else if (kind == kDestination && e.step)
        {
            const auto &dests = app_.settings().destinations;
            const int n = static_cast<int>(dests.size());
            if (n > 0)
            {
                int at = 0;
                for (int i = 0; i < n; ++i)
                    if (dests[static_cast<std::size_t>(i)].id == sheet_destination_)
                        at = i;
                sheet_destination_ = dests[static_cast<std::size_t>(((at + e.step) % n + n) % n)].id;
            }
        }
        else if (kind == kName && e.activated)
        {
            entry_for_ = kName;
            entry_.open("Name the rip", sheet_name_, false, "The files and the folder are named after it");
        }
        else if (kind == kFolder && e.activated)
        {
            entry_for_ = kFolder;
            entry_.open("Folder", sheet_folder_, false, "{system}, {name} and {artist} are filled in");
        }
        else if (kind >= kOption)
        {
            const std::size_t index = static_cast<std::size_t>(kind - kOption);
            const auto *opts = sheet_option_list(*d);
            if (opts && index < opts->size())
            {
                const ripper::Option &o = (*opts)[index];
                const std::string v = option_value(o);
                if (o.type == "toggle")
                    sheet_options_[o.id] = v == "1" || v == "true" ? "0" : "1";
                else
                {
                    // Only the choices for this kind of disc.
                    std::vector<const ripper::Choice *> usable;
                    for (const ripper::Choice &c : o.choices)
                        if (c.only.empty() || c.only == d->system)
                            usable.push_back(&c);
                    const int n = static_cast<int>(usable.size());
                    int at = 0;
                    for (int i = 0; i < n; ++i)
                        if (usable[static_cast<std::size_t>(i)]->id == v)
                            at = i;
                    if (n)
                        sheet_options_[o.id] = usable[static_cast<std::size_t>(((at + step) % n + n) % n)]->id;
                }
            }
        }
        else if (kind == kGo && e.activated)
        {
            sheet_busy_ = true;
            app_.rip(*d, sheet_format_, sheet_destination_, sheet_name_, sheet_folder_, options_json(*d),
                     [this](bool ok) {
                         sheet_busy_ = false;
                         if (ok)
                         {
                             sheet_open_ = false;
                             tab_ = kQueue;
                             tab_age_ = 0.0f;
                             job_row_ = 0;
                         }
                     });
            feedback.play(audio::Cue::launch);
            feedback.rumble(0.5f, 0.12f);
        }
    }

    // ---------------------------------------------------------------- queue

    // Cancelling a rip throws away what came so far, so it takes holding
    // Cross for a second, not a press: the row fills up meanwhile, and letting
    // go before the end keeps the rip going.
    static constexpr float kCancelHold = 1.0f;

    bool hold_to_cancel(const InputFrame &input, float dt, const ripper::Job *j, app::Feedback &feedback)
    {
        cancel_seen_ = true;
        if (!j || !j->active() || !input.is_held(Action::confirm))
        {
            cancel_hold_ = 0;
            cancel_job_ = 0;
            return false;
        }
        if (input.is_pressed(Action::confirm))
        {
            cancel_job_ = j->id; // every hold starts with a press, from nothing
            cancel_hold_ = 0;
            feedback.play(audio::Cue::tick, 1.0f, 0.0f, 0.5f);
        }
        else if (cancel_job_ != j->id)
            return false; // held from before, or from another row
        if (cancel_hold_ < 0)
            return false; // done: waits for the button to come up
        cancel_hold_ += dt;
        if (cancel_hold_ < kCancelHold)
            return false;
        app_.cancel_job(j->id);
        feedback.play(audio::Cue::back);
        feedback.rumble(0.8f, 0.2f);
        cancel_hold_ = -1;
        return true;
    }

    // 0..1 while Cross is held on this job.
    float cancel_progress(int job_id) const
    {
        return job_id && job_id == cancel_job_ && cancel_hold_ > 0 ? std::min(1.0f, cancel_hold_ / kCancelHold) : 0.0f;
    }

    void update_queue(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        const auto &jobs = app_.jobs();
        const int count = static_cast<int>(jobs.size());
        // The list can get shorter at any tick (finished ones cleared, the payload restarted).
        job_row_ = std::clamp(job_row_, 0, std::max(0, count - 1));
        if ((input.nav == Direction::up || input.nav == Direction::down) && count > 0)
        {
            const int next = std::clamp(job_row_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
            if (next != job_row_)
            {
                job_row_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
        }
        if (input.is_pressed(Action::west) && count > 0)
        {
            if (std::any_of(jobs.begin(), jobs.end(), [](const ripper::Job &j) { return j.ended(); }))
            {
                app_.clear_jobs();
                job_row_ = 0;
                feedback.play(audio::Cue::back);
            }
            else
                refuse(feedback, 0.0f);
            return;
        }
        if (count > 0 && job_row_ < count)
        {
            const ripper::Job &j = jobs[static_cast<std::size_t>(job_row_)];
            if (j.active())
                hold_to_cancel(input, dt, &j, feedback);
            else if (j.retryable && input.is_pressed(Action::confirm))
            {
                app_.retry_job(j.id);
                feedback.play(audio::Cue::select);
            }
            else if (input.is_pressed(Action::confirm))
                refuse(feedback, 0.0f);
        }
    }

    // ---------------------------------------------------------------- drawing

    void draw_top_bar(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ripper::Look &L = look();
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in);
        float x = kMargin;
        const ripper::Status &s = app_.status();
        ui::text(list, fonts.display, "RipperPS", x, 96 - 10 * (1.0f - in), 34, L.text);
        x += 220;
        if (s.reachable)
        {
            for (int i = 0; i < kTabs; ++i)
            {
                const bool active = i == tab_;
                const float w = ui::text(list, active ? fonts.semibold : fonts.regular, kTabNames[i], x,
                                         92 - 10 * (1.0f - in), 26, active ? L.text : L.muted);
                if (active)
                    list.rounded_rect({x, 104, w, 4}, 2, accent());
                // Rips running: a count beside Queue.
                if (i == kQueue && s.jobs_active > 0)
                {
                    const std::string n = std::to_string(s.jobs_active);
                    const float bw = std::max(30.0f, fonts.semibold.font->measure(n, 18) + 16);
                    list.rounded_rect({x + w + 8, 70, bw, 28}, 14, accent());
                    ui::text(list, fonts.semibold, n, x + w + 8 + bw * 0.5f, 90, 18, kInk, gfx::Align::center);
                    x += bw + 8;
                }
                x += w + 44;
            }
        }
        // Right: the service, or what's wrong.
        float left_x = 1824;
        std::string where;
        if (s.reachable)
            where = "RipperPS " + s.version + (s.ip.empty() ? "" : "  \xC2\xB7  " + s.ip + ":" + std::to_string(s.port));
        else
            where = !s.known ? "Connecting" : app_.payload_starting() ? "Starting the service" : "The service isn't running";
        left_x -= ui::text(list, fonts.regular, fonts.regular.font->fit(where, 22, 640), left_x, 90, 22, L.muted,
                           gfx::Align::right);
        list.circle(left_x - 24, 83, 7, s.reachable ? L.success : app_.payload_starting() ? L.warning : L.danger);
        list.pop_opacity();
    }

    // The payload isn't answering.
    void draw_not_ready(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ripper::Look &L = look();
        const ripper::Status &s = app_.status();
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const Rect panel{360, 300, 1200, 440};
        draw_panel(list, panel, 36);
        const float x = panel.x + 72;
        const bool starting = app_.payload_starting() || (s.known && app_.payload_error().empty());
        ui::text(list, fonts.display,
                 !s.known ? "Connecting" : starting ? "Starting the background service" : "The background service isn't running", x,
                 panel.y + 120, 56, L.panel_text);
        ui::paragraph(list, fonts.regular,
                      starting ? "This takes a few seconds."
                               : "RipperPS couldn't start its background service. Press Cross to try again, or send "
                                 "ripper.elf to your payload loader.",
                      x, panel.y + 190, 28, 1060, 42, L.panel_muted, 4);
        if (!starting && !app_.payload_error().empty())
            ui::text(list, fonts.regular, fonts.regular.font->fit(ripper::plain_text(app_.payload_error()), 22, 1060),
                     x, panel.y + 340, 22, L.panel_muted.with_alpha(0.8f));
        if (s.known && !s.error.empty())
            ui::text(list, fonts.regular, fonts.regular.font->fit(s.error, 22, 1060), x, panel.y + 380, 22,
                     L.panel_muted.with_alpha(0.6f));
        list.pop_opacity();
    }

    // A drive's card: which drive, the disc on it, what it is and what's next.
    static constexpr float kCardW = 560.0f, kCardH = 640.0f, kCardGap = 36.0f;

    void draw_drives(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ripper::Look &L = look();
        const auto &drives = app_.drives();
        const float in = tween::stagger(tab_age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.display, "Drives", kMargin, 240, 64, L.text);
        if (drives.empty())
        {
            ui::text(list, fonts.regular, "Looking for drives.", kMargin, 310, 28, L.muted);
            ui::text(list, fonts.regular, "The console's own drive, and USB drives plugged in, show here.", kMargin,
                     348, 22, L.muted.with_alpha(0.7f));
            list.pop_opacity();
            return;
        }
        // Three cards fit; past that the row slides with the focus.
        const float slide = std::max(0.0f, drive_position_.value - 2.0f) * (kCardW + kCardGap);
        const float shake = ui::shake(nudge_.value, clock_, 10.0f, 9.0f) * nudge_direction_;
        for (int i = 0; i < static_cast<int>(drives.size()); ++i)
        {
            const float appear = tween::stagger(tab_age_, 2 + i, 0.07f, 0.5f);
            const bool focused = i == drive_;
            const Rect card{kMargin + static_cast<float>(i) * (kCardW + kCardGap) - slide + (focused ? shake : 0.0f),
                            290 + 24 * (1.0f - appear), kCardW, kCardH};
            list.push_opacity(appear);
            draw_drive(list, fonts, L, drives[static_cast<std::size_t>(i)], card, focused);
            list.pop_opacity();
        }
        list.pop_opacity();
    }

    // The disc on a card: it slides in when it goes in and spins while it's
    // read, then wears its cover (an album's, a game's) once there is one.
    void draw_disc(gfx::DrawList &list, const ripper::Look &L, const ripper::Drive &d, float cx, float cy, float r,
                   Color acc) const
    {
        const auto found = motion_.find(d.id);
        const DiscMotion *m = found != motion_.end() ? &found->second : nullptr;
        const bool reading = d.reading();
        const bool ripping = d.state == "ripping";
        if (!d.has_disc && !reading)
        {
            // An empty tray: the disc's outline, faint.
            list.ring(cx, cy, r, 2.0f, L.panel_muted.with_alpha(0.25f));
            list.ring(cx, cy, r * 0.16f, 2.0f, L.panel_muted.with_alpha(0.25f));
            return;
        }
        // Sliding in, eased, over the first half second after it went in.
        const float t = m ? tween::clamp01((static_cast<float>(clock_) - m->since) * 2.2f) : 1.0f;
        const float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        cx += (1.0f - ease) * 220.0f;
        list.push_opacity(ease);
        // The spin: fast while it's read or ripped, slowing to a stop after.
        const float speed = reading ? 9.0f : ripping ? 5.0f : 0.0f;
        const float angle = static_cast<float>(clock_) * speed;
        const ripper::Cover *cover = app_.cover(d);
        list.shadow({cx - r, cy - r + 14, r * 2, r * 2}, r, 30, Color::rgb(0x000000, L.dark ? 0.45f : 0.18f));
        if (cover && cover->texture)
        {
            // The cover printed on the disc: a square from its middle.
            Rect uv{0, 0, 1, 1};
            if (cover->aspect > 1.0f)
                uv = {(1.0f - 1.0f / cover->aspect) * 0.5f, 0, 1.0f / cover->aspect, 1};
            else if (cover->aspect < 1.0f)
                uv = {0, (1.0f - cover->aspect) * 0.5f, 1, cover->aspect};
            list.image(cover->texture, {cx - r, cy - r, r * 2, r * 2}, uv, kWhite, r);
        }
        else
        {
            // A plain disc: silver with the system's tint, and its grooves.
            list.circle(cx, cy, r, gfx::mix(Color::rgb(0xc9ced8), acc, 0.25f));
            for (float g = 0.36f; g < 0.98f; g += 0.08f)
                list.ring(cx, cy, r * g, 1.0f, Color::rgb(0xffffff, 0.08f));
        }
        // The light catching it, which is what shows the spin.
        list.arc(cx, cy, r * 0.96f, r * 0.55f, angle, 0.7f, Color::rgb(0xffffff, 0.16f), false);
        list.arc(cx, cy, r * 0.96f, r * 0.55f, angle + kPi, 0.7f, Color::rgb(0xffffff, 0.10f), false);
        list.ring(cx, cy, r, 2.0f, Color::rgb(0xffffff, 0.35f));
        // The hub.
        list.circle(cx, cy, r * 0.17f, Color::rgb(0xe8ebf0, 0.9f));
        list.circle(cx, cy, r * 0.07f, L.panel);
        list.ring(cx, cy, r * 0.17f, 1.5f, Color::rgb(0x000000, 0.25f));
        if (reading)
        {
            // A ring of light going round the edge while it's read.
            list.arc(cx, cy, r + 10, 4.0f, -angle * 0.5f, 1.4f, acc, true);
        }
        list.pop_opacity();
    }

    void draw_drive(gfx::DrawList &list, const ui::Fonts &fonts, const ripper::Look &L, const ripper::Drive &d,
                    const Rect &card, bool focused) const
    {
        if (focused)
            list.glow(card, L.radius, 22, accent().with_alpha(0.3f));
        draw_panel(list, card);
        if (focused)
            list.bordered_rect(card, L.radius, Color::rgb(0x000000, 0.0f), 3.0f, L.focus);
        const float x = card.x + 40, w = card.w - 80;
        const char *kind = d.internal() ? "CONSOLE DRIVE" : d.source == "virtual" ? "IMAGE FILE" : "USB DRIVE";
        ui::text(list, fonts.semibold, kind, x, card.y + 52, 16, L.panel_muted, gfx::Align::left, 3.0f);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(d.model(), 24, w), x, card.y + 88, 24, L.panel_text);

        Color acc = L.accent;
        if (d.has_offer)
        {
            const ripper::Cover *c = app_.cover(d);
            acc = L.covers ? (c ? c->palette.accent : ripper::system_palette(d.group, d.system).accent) : L.accent;
        }
        draw_disc(list, L, d, card.x + card.w * 0.5f, card.y + 245, 120, acc);

        // Under the disc: what it is, fading in once it's known.
        const auto found = motion_.find(d.id);
        const float known = found != motion_.end() && found->second.known_at >= 0
                                ? tween::clamp01((static_cast<float>(clock_) - found->second.known_at) * 3.0f)
                                : 1.0f;
        float y = card.y + 424;
        if (!d.has_disc && !d.reading())
        {
            ui::text(list, fonts.display, "No disc", card.x + card.w * 0.5f, y + 20, 36, L.panel_muted,
                     gfx::Align::center);
            return;
        }
        if (d.reading())
        {
            const float pulse = 0.55f + 0.45f * std::sin(clock_ * 3.0f);
            ui::text(list, fonts.semibold, "READING THE DISC", card.x + card.w * 0.5f, y, 18,
                     acc.with_alpha(pulse), gfx::Align::center, 3.0f);
            return;
        }
        list.push_opacity(known);
        const std::string system = d.has_offer ? ui::upper(d.system_name) : ui::upper(d.disc.kind);
        ui::text(list, fonts.semibold, system, x, y, 16, acc, gfx::Align::left, 3.0f);
        y = ui::paragraph(list, fonts.display, d.disc_name(), x - 2, y + 44, 34, w, 40, L.panel_text, 2);
        // The kind, unless the name already says it (an unknown CD's name is its kind).
        std::string facts = !d.disc.subtitle.empty() ? d.disc.subtitle : d.disc_name() != d.disc.kind ? d.disc.kind : "";
        if (d.disc.size > 0 && d.disc.subtitle.empty())
            facts += "  \xC2\xB7  " + ripper::human_size(d.disc.size);
        if (d.disc.audio_tracks > 0)
            facts += (facts.empty() ? "" : "  \xC2\xB7  ") + std::to_string(d.disc.audio_tracks) +
                     (d.disc.audio_tracks == 1 ? " track" : " tracks");
        ui::text(list, fonts.regular, fonts.regular.font->fit(facts, 20, w), x, y - 4, 20, L.panel_muted);
        list.pop_opacity();

        // The foot: what happens next, or why nothing can.
        const float foot = card.y + card.h - 40;
        if (d.state == "ripping")
        {
            const ripper::Job *j = app_.job(d.job);
            const float done = j ? j->progress() : 0.0f;
            std::string line = j ? stage_label(*j) : "Ripping";
            char pct[16];
            std::snprintf(pct, sizeof pct, "  \xC2\xB7  %d%%", static_cast<int>(done * 100.0f));
            line += pct;
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(line, 20, w * 0.66f), x, foot - 22, 20,
                     L.panel_text);
            if (j)
                ui::text(list, fonts.regular, left(j->eta), x + w, foot - 22, 20, L.panel_muted, gfx::Align::right);
            const Rect bar{x, foot - 4, w, 8};
            list.rounded_rect(bar, 4, L.panel_muted.with_alpha(0.25f));
            if (done > 0)
                list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * done), bar.h}, 4, acc);
            return;
        }
        if (d.state == "installing")
        {
            // The console copies the game off the disc first; the dump waits for it.
            const float done = d.install.progress();
            std::string line = d.install.playable ? "Installing, playable already" : "Installing";
            char pct[16];
            std::snprintf(pct, sizeof pct, "  \xC2\xB7  %d%%", static_cast<int>(done * 100.0f));
            line += pct;
            if (d.install.rate > 0)
                line += "  \xC2\xB7  " + ripper::human_size(d.install.rate) + "/s";
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(line, 20, w * 0.72f), x, foot - 22, 20,
                     L.panel_text);
            ui::text(list, fonts.regular, left(d.install.eta), x + w, foot - 22, 20, L.panel_muted, gfx::Align::right);
            const Rect bar{x, foot - 4, w, 8};
            list.rounded_rect(bar, 4, L.panel_muted.with_alpha(0.25f));
            if (done > 0)
                list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * done), bar.h}, 4, acc);
            ui::text(list, fonts.regular, "Leave the disc in until it's done.", x, foot + 26, 16, L.panel_muted);
            return;
        }
        if (d.state == "unsupported" || !d.has_offer)
        {
            const std::string why = d.message.empty() ? "Can't rip this disc" : d.message;
            ui::paragraph(list, fonts.regular, sentence(why), x, foot - 30, 20, w, 28, L.warning, 2);
            return;
        }
        const bool any = std::any_of(d.formats.begin(), d.formats.end(), [](const ripper::Format &f) {
            return f.available;
        });
        if (!any)
        {
            const std::string why = !d.formats.empty() && !d.formats.front().reason.empty() ? d.formats.front().reason
                                                                                             : "Nothing to rip it to here";
            ui::paragraph(list, fonts.regular, sentence(why), x, foot - 30, 20, w, 28, L.warning, 2);
            return;
        }
        std::string to;
        for (const ripper::Format &f : d.formats)
            if (f.id == d.suggestion.format)
                to = f.label;
        if (to.empty())
            to = d.formats.front().label;
        if (d.state == "done")
            ui::text(list, fonts.semibold, "Ripped", x + w, foot, 20, L.success, gfx::Align::right);
        ui::text(list, fonts.regular, fonts.regular.font->fit("Rips to " + to, 20, w * 0.7f), x, foot, 20,
                 L.panel_text.with_alpha(0.85f));
    }

    void draw_queue(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ripper::Look &L = look();
        const auto &jobs = app_.jobs();
        const float in = tween::stagger(tab_age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.display, "Queue", kMargin, 240, 64, L.text);
        // Top right: how fast the running ones go together.
        double total_rate = 0;
        int running = 0;
        for (const ripper::Job &j : jobs)
            if (j.active())
            {
                total_rate += j.rate;
                ++running;
            }
        if (running)
        {
            ui::text(list, fonts.semibold, ripper::human_size(total_rate) + "/s", 1824, 228, 40, accent(),
                     gfx::Align::right);
            ui::text(list, fonts.regular,
                     running == 1 ? std::string("1 rip running") : std::to_string(running) + " rips running", 1824,
                     262, 22, L.muted, gfx::Align::right);
        }
        if (jobs.empty())
        {
            ui::text(list, fonts.regular, "Nothing ripping.", kMargin, 310, 28, L.muted);
            ui::text(list, fonts.regular, "Rips started here or in the web UI show up here.", kMargin, 348, 22,
                     L.muted.with_alpha(0.7f));
            list.pop_opacity();
            return;
        }
        constexpr float kRowH = 110.0f;
        const float top = 300.0f;
        const float scroll = job_scroll_.value;
        // One row past the band on each side, hidden by the clip below.
        const int first = std::max(0, static_cast<int>(std::floor(scroll)) - 1);
        list.rounded_rect({kMargin - 24, top + (job_position_.value - scroll) * kRowH, 1776, kRowH - 14}, 26,
                          L.text.with_alpha(0.1f));
        list.push_clip({0, top, 1920, 6 * kRowH});
        for (int i = first; i < static_cast<int>(jobs.size()) && i < first + 9; ++i)
        {
            const ripper::Job &j = jobs[static_cast<std::size_t>(i)];
            const float y = top + (static_cast<float>(i) - scroll) * kRowH;
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(j.name, 28, 1000), kMargin, y + 42, 28, L.text);
            // "PlayStation  ·  CHD to Local Location  ·  Ripping"
            std::string meta = j.system_name.empty() ? "" : j.system_name + "  \xC2\xB7  ";
            meta += j.format_label + (j.destination_name.empty() ? "" : " to " + j.destination_name) + "  \xC2\xB7  ";
            Color meta_colour = L.muted;
            if (j.active())
            {
                meta += stage_label(j);
                if (!j.note.empty())
                    meta += "  \xC2\xB7  " + j.note;
            }
            else if (j.state == "failed")
            {
                meta += sentence(j.error.empty() ? "failed" : j.error);
                meta_colour = L.danger;
            }
            else if (j.state == "cancelled")
                meta += "Cancelled";
            else
                meta += !j.note.empty() ? "Done  \xC2\xB7  " + j.note : "Done";
            const float hold = cancel_progress(j.id);
            if (hold > 0)
            {
                meta = "Keep holding to cancel";
                meta_colour = L.danger;
            }
            ui::text(list, fonts.regular, fonts.regular.font->fit(meta, 22, 1100), kMargin, y + 80, 22, meta_colour);
            draw_stages(list, fonts, L, j, Rect{1240, y + 26, 540, 60}, hold);
        }
        list.pop_clip();
        list.pop_opacity();
    }

    // A job's two stages side by side: reading the disc, then the copy or
    // upload. Each its own bar, with how far and how long.
    void draw_stages(gfx::DrawList &list, const ui::Fonts &fonts, const ripper::Look &L, const ripper::Job &j,
                     const Rect &r, float hold) const
    {
        const bool two = j.has_transfer;
        const float gap = 20.0f;
        const float rip_w = two ? (r.w - gap) * 0.62f : r.w;
        const float xfer_w = r.w - gap - rip_w;
        // The stage a failure happened in turns red: the rip, or the transfer after it.
        const bool failed = j.state == "failed";
        auto bar = [&](float x, float w, const char *label, float done, bool active, bool finished, bool broke,
                       const std::string &right) {
            ui::text(list, fonts.semibold, label, x, r.y + 16, 16, active ? L.text : L.muted, gfx::Align::left, 2.0f);
            if (!right.empty())
                ui::text(list, fonts.regular, fonts.regular.font->fit(right, 18, w - 80), x + w, r.y + 16, 18,
                         L.muted, gfx::Align::right);
            const Rect b{x, r.y + 30, w, 10};
            list.rounded_rect(b, 5, L.muted.with_alpha(0.22f));
            if (done > 0)
                list.rounded_rect({b.x, b.y, std::max(10.0f, b.w * done), b.h}, 5,
                                  broke ? L.danger : finished ? L.success : accent());
            else if (broke)
                list.rounded_rect({b.x, b.y, 10.0f, b.h}, 5, L.danger);
        };
        const bool ripping = j.state == "ripping";
        const bool moving = j.stage == "transfer";
        char pct[48];
        std::string rip_right;
        if (ripping)
        {
            std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(j.progress() * 100.0f));
            rip_right = ripper::human_size(j.rate) + "/s  " + pct;
        }
        else if (j.rip_seconds > 0)
            rip_right = ripper::duration(j.rip_seconds);
        const bool rip_broke = failed && !j.ripped;
        bar(r.x, rip_w, "RIP", j.rip_progress(), ripping, !ripping && !rip_broke && j.rip_progress() >= 1.0f, rip_broke,
            rip_right);
        if (two)
        {
            std::string right;
            if (moving)
            {
                std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(j.progress() * 100.0f));
                right = pct;
            }
            bar(r.x + rip_w + gap, xfer_w, j.transfer_kind == "upload" ? "UPLOAD" : "MOVE",
                rip_broke ? 0.0f : j.transfer_progress(), moving, j.state == "done", failed && j.ripped, right);
        }
        if (j.active() && j.eta > 0)
            ui::text(list, fonts.regular, left(j.eta), r.x + r.w, r.y + 64, 18, L.muted, gfx::Align::right);
        if (hold > 0)
            list.rounded_rect({r.x, r.y + 30, std::max(10.0f, r.w * hold), 10}, 5, L.danger);
    }

    static constexpr float kSheetHeight = 720.0f;

    void draw_sheet(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ripper::Drive *d = app_.drive(sheet_drive_);
        if (!d)
            return;
        const ripper::Look &L = look();
        const ripper::Palette palette = ripper::system_palette(d->group, d->system);
        const Color acc = L.covers ? palette.accent : L.accent;
        const float t = sheet_.value;
        const Rect sheet{120, gfx::kVirtualHeight - kSheetHeight * t - 40.0f * t + 60.0f * (1.0f - t), 1680,
                         kSheetHeight};
        list.push_opacity(tween::clamp01(t * 1.4f));
        list.shadow({sheet.x, sheet.y + 20, sheet.w, sheet.h}, 44, 60, Color::rgb(0x000000, 0.5f));
        list.glass(glass, sheet, 44, kWhite);
        list.rounded_rect(sheet, 44, L.covers ? gfx::mix(palette.dark, kInk, 0.5f).with_alpha(0.62f) : L.panel);
        list.bordered_rect(sheet, 44, Color::rgb(0x000000, 0.0f), 1.5f, L.outline);

        // Left: the disc, with its cover.
        const float x = sheet.x + 72;
        draw_disc(list, L, *d, x + 130, sheet.y + 200, 120, acc);
        float y = sheet.y + 380;
        ui::text(list, fonts.semibold, ui::upper(d->system_name), x, y, 18, acc, gfx::Align::left, 4.0f);
        y = ui::paragraph(list, fonts.display, d->disc_name(), x - 2, y + 50, 40, 560, 48, L.panel_text, 2) - 6;
        std::string facts = !d->disc.subtitle.empty() ? d->disc.subtitle : d->disc.kind;
        if (d->disc.size > 0)
            facts += "  \xC2\xB7  " + ripper::human_size(d->disc.size);
        ui::text(list, fonts.regular, fonts.regular.font->fit(facts, 20, 560), x, y, 20, L.panel_muted);
        ui::text(list, fonts.regular, fonts.regular.font->fit("In " + d->model(), 20, 560), x, y + 34, 20,
                 L.panel_muted);

        // Right: the choices, and Rip.
        sheet_list_.draw(list, fonts, L, sheet_rows(nullptr), {sheet.x + 720, sheet.y + 60, 900, kSheetRowsHeight},
                         clock_);
        list.pop_opacity();
    }

    void draw_toast(gfx::DrawList &list) const
    {
        const std::string &message = app_.toast();
        if (message.empty())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float a = tween::clamp01(app_.toast_age() * 5.0f) * tween::clamp01((4.0f - app_.toast_age()) * 2.0f);
        list.push_opacity(a);
        // Beside the button hints, on their line, when there's room for it;
        // above them when they leave too little.
        const float room = std::min(1180.0f, hints_left_ - 24.0f - kMargin);
        const Rect r = room >= 480.0f ? Rect{kMargin, 978, room, 64} : Rect{kMargin, 892, 1180, 64};
        const std::string fitted = fonts.regular.font->fit(message, 24, r.w - 64);
        const ripper::Look &L = look();
        list.rounded_rect(r, 32, L.covers ? Color::rgb(0x14182e, 0.92f) : L.page.with_alpha(0.95f));
        list.bordered_rect(r, 32, Color::rgb(0x000000, 0.0f), 1.5f, L.outline);
        ui::text(list, fonts.regular, fitted, r.x + 32, r.y + 41, 24, L.text);
        list.pop_opacity();
    }

    // The hint row floats over the page's bottom corner: it gets the panel
    // treatment (draw_panel: shadow, fill, outline), so it reads over what it
    // crosses instead of hovering on it.
    void hint_row(gfx::DrawList &list, const ui::Fonts &fonts, const ui::GlyphStyle &style, const ui::Hint *hints,
                  int count) const
    {
        if (count <= 0)
            return;
        const ui::HintLayout layout; // the kit's bottom row: glyphs 40 tall, cy 1010
        const float width = ui::measure_hints(fonts, hints, count, layout);
        const Rect plate{1824.0f - width - 30.0f, layout.cy - 38.0f, width + 60.0f, 76.0f};
        hints_left_ = std::min(hints_left_, plate.x);
        draw_panel(list, plate);
        ui::draw_hints(list, fonts, style, hints, count, 1824, true);
    }

    void draw_hints(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = look().glyphs();
        const ui::Hint close{ui::Button::options, "Hold to close"}; // ripper.cpp's HoldToLeave
        if (!app_.status().reachable)
        {
            const bool retry = app_.status().known && !app_.payload_starting();
            const ui::Hint hints[] = {close, {ui::Button::cross, "Try again"}};
            hint_row(list, fonts, style, hints, retry ? 2 : 1);
            return;
        }
        if (entry_.active())
        {
            const std::vector<ui::Hint> hints = entry_.hints();
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
            return;
        }
        if (sheet_open_)
        {
            std::vector<int> kinds;
            const std::vector<ripper::Row> rows = sheet_rows(&kinds);
            const int focus = sheet_list_.focus();
            std::vector<ui::Hint> hints;
            if (focus >= 0 && focus < static_cast<int>(rows.size()))
            {
                const ripper::Row &r = rows[static_cast<std::size_t>(focus)];
                if (r.kind == ripper::RowKind::choice)
                    hints.push_back({ui::Button::dpad, "Change"});
                else if (r.kind == ripper::RowKind::toggle)
                    hints.push_back({ui::Button::cross, r.on ? "Turn off" : "Turn on"});
                else if (kinds[static_cast<std::size_t>(focus)] == kGo)
                    hints.push_back({ui::Button::cross, "Rip"});
                else
                    hints.push_back({ui::Button::cross, "Edit"});
            }
            hints.push_back({ui::Button::circle, "Back"});
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
            return;
        }
        if (tab_ == kSettings || tab_ == kGames)
        {
            std::vector<ui::Hint> hints = tab_ == kSettings ? settings_.hints() : games_.hints();
            if (!(tab_ == kSettings ? settings_.nested() : games_.nested()))
                hints.insert(hints.begin(), close);
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
            return;
        }
        list.push_opacity(tween::stagger(age_, 8, 0.08f, 0.5f) * (1.0f - sheet_.value));
        const ui::Hint pages{ui::Button::l1, "Pages", ui::Button::r1};
        std::vector<ui::Hint> hints{close, pages};
        if (tab_ == kDrives)
        {
            if (const ripper::Drive *d = focused_drive())
            {
                if (d->has_disc && d->state != "ripping" && d->state != "installing")
                    hints.push_back({ui::Button::triangle, "Eject"});
                if (d->has_disc && d->state != "ripping" && !d->reading())
                    hints.push_back({ui::Button::square, "Read again"});
                if (d->job && app_.job(d->job))
                    hints.push_back({ui::Button::cross, "Show in queue"});
                else if (d->rippable())
                    hints.push_back({ui::Button::cross, "Rip"});
            }
        }
        else
        {
            // Cross on a running rip stops it, on a failed one sends it again;
            // Square takes the finished ones off the list.
            const auto &jobs = app_.jobs();
            const bool on_row = job_row_ < static_cast<int>(jobs.size());
            const ripper::Job *j = on_row ? &jobs[static_cast<std::size_t>(job_row_)] : nullptr;
            if (std::any_of(jobs.begin(), jobs.end(), [](const ripper::Job &x) { return x.ended(); }))
                hints.push_back({ui::Button::square, "Clear finished"});
            if (j && j->active())
                hints.push_back({ui::Button::cross, "Hold to cancel"});
            else if (j && j->retryable)
                hints.push_back({ui::Button::cross, "Send again"});
        }
        hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
        list.pop_opacity();
    }

    app::Context &context_;
    App &app_;
    int tab_ = kDrives;
    float tab_age_ = 0.0f;
    tween::Spring tab_position_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    ui::SpringColor palette_[4];
    ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    mutable float hints_left_ = 1920.0f; // where this frame's button hints start, for the toast
    int drive_ = 0;                      // the Drives tab's focus
    tween::Spring drive_position_;
    std::map<std::string, DiscMotion> motion_; // by drive id
    // The confirm sheet: the drive and disc it's for, and what's picked.
    bool sheet_open_ = false;
    bool sheet_busy_ = false; // the rip asked for, no answer yet
    tween::Spring sheet_;
    std::string sheet_drive_, sheet_format_, sheet_destination_, sheet_name_, sheet_folder_;
    std::map<std::string, std::string> sheet_options_;
    int sheet_seq_ = 0;
    ripper::RowList sheet_list_;
    ripper::TextEntry entry_;
    int entry_for_ = kName;
    int job_row_ = 0;
    float cancel_hold_ = 0; // seconds Cross has been held to cancel cancel_job_; -1 once done
    int cancel_job_ = 0;
    bool cancel_seen_ = false; // hold_to_cancel ran this frame
    tween::Spring job_position_;
    tween::Spring job_scroll_; // the window's first row, eased
    ripper::SettingsPage settings_;
    ripper::GamesPage games_;
};

} // namespace

std::unique_ptr<app::Concept> make_ripper(app::Context &context)
{
    return std::make_unique<RipperPS>(context);
}

} // namespace hui::screen

// The title's screen, for its program (ps5/ui/kit.hpp declares it)
namespace ps5ui
{
std::unique_ptr<hui::app::Concept> make_screen(hui::app::Context &context)
{
    return hui::screen::make_ripper(context);
}
} // namespace ps5ui
