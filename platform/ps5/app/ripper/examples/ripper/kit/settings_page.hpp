// RipperPS: the Settings tab. The look, how ripping goes, the destinations
// (added and edited here: shares and their logins, RomM signed in with a code),
// what each kind of disc rips to (its profile), and what's been ripped so far.
// The same settings as the web UI's, through the same API.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "qr.hpp"
#include "ripper_app.hpp"
#include "rows.hpp"
#include "text_entry.hpp"

#include <string>
#include <vector>

namespace ripper
{

class SettingsPage
{
  public:
    void enter();
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, float clock, float age) const;
    // Drawn over everything (the keyboard).
    void draw_overlay(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, float clock) const;
    std::vector<hui::ui::Hint> hints() const;
    // A page under the main list is open (Circle comes back here first).
    bool nested() const { return mode_ != kMain || entry_.active(); }

  private:
    enum Mode
    {
        kMain,
        kDestination, // editing one (or a new one)
        kProfile,     // a system's profile
        kPairing,     // RomM sign-in: the code to approve
    };
    // What a row of the list does.
    struct Item
    {
        enum Kind
        {
            none,
            look,
            sounds,
            notify,
            eject,
            auto_start,
            keep_front,
            retries,
            awake,
            tmdb,
            drive_test, // key: the drive
            test_disc,  // index: the disc's number in the test
            test_eject, // key: the drive
            test_finish,
            test_discard,
            destination, // index: settings().destinations
            add_destination,
            profile, // key: the system
            field,   // the destination's field (key)
            dest_name,
            dest_type,
            pair,
            test,
            save,
            remove,
            prof_format,
            prof_destination,
            prof_folder,
            prof_option, // key: the option
        } kind = none;
        int index = -1;
        std::string key;
    };

    std::vector<Row> rows(std::vector<Item> *items) const;
    void main_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    void destination_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    void profile_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    void activate(const Item &item, int step, hui::ui::Feedback &feedback);
    void open_destination(int index, const std::string &type);
    void open_profile(const std::string &system);
    void back(hui::ui::Feedback &feedback);
    void draw_pairing(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look) const;
    void draw_side(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look) const;

    Mode mode_ = kMain;
    RowList list_;
    int main_focus_ = 0; // where the main list was, to come back to
    TextEntry entry_;
    Item editing_; // the row the keyboard is for
    // The destination being edited: a copy, saved as a whole.
    Destination dest_;
    int dest_index_ = -1; // in settings().destinations; -1: a new one
    std::string tested_;  // what Test said
    bool test_ok_ = false;
    int armed_remove_ = 0; // Remove takes two presses
    // The profile being edited.
    std::string prof_system_;
    Profile prof_;
    mutable QrCode qr_;
    mutable std::string qr_for_;
    int awake_ = 0; // the keep-awake choice
    bool discard_armed_ = false; // Throw away asks once more
};

} // namespace ripper
