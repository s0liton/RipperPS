// RipperPS: the Settings tab. The look, how ripping goes, the destinations
// (added and edited here: folders picked from a list, shares and their logins,
// RomM signed in with a code), each kind of disc's default rip settings (its
// profile), and what's been ripped so far. The same settings as the web UI's,
// through the same API.
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
        kProfiles,    // Default Rip Settings: each kind of disc
        kProfile,     // a system's profile
        kPairing,     // RomM sign-in: the code to approve
        kFolder,      // picking a destination's folder, or a key file
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
            retries,
            tmdb,
            profiles, // opens Default Rip Settings
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
            folder_open, // index: folders_
            folder_use,
            folder_up,
            folder_new,
            key_file,    // index: settings().key_files
            file_pick,   // index: files_
            key_default,
        } kind = none;
        int index = -1;
        std::string key;
    };

    std::vector<Row> rows(std::vector<Item> *items) const;
    void main_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    void destination_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    void profiles_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    void profile_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    void folder_rows(std::vector<Row> &rows, std::vector<Item> &items) const;
    // The folder picker, for the destination's field (key), from its value.
    void open_folders(const std::string &field);
    // The same picker, for a key file (index in settings().key_files).
    void open_key_file(int index);
    void list_folder(const std::string &path);
    void activate(const Item &item, int step, hui::ui::Feedback &feedback);
    void open_destination(int index, const std::string &type);
    void open_profile(const std::string &system);
    void back(hui::ui::Feedback &feedback);
    void draw_pairing(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look) const;
    void draw_side(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look) const;
    void draw_credits(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look,
                      const hui::gfx::Rect &r) const;

    Mode mode_ = kMain;
    RowList list_;
    int main_focus_ = 0;     // where the main list was, to come back to
    int profiles_focus_ = 1; // and the Default Rip Settings list
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
    // The folder picker: the folder listed and what's in it. The service
    // only lists the console's storage, or only the USB drives ("scope").
    struct Folder
    {
        std::string name, path;
    };
    std::vector<Folder> folders_;
    std::vector<Folder> files_; // listed when picking a key file
    int key_index_ = -1;        // the key file being picked, -1: a folder
    std::string folder_path_, folder_parent_, folder_field_, folder_error_;
    bool folder_loading_ = false;
    int folder_seq_ = 0; // the listing asked for last: older answers are dropped
    bool discard_armed_ = false; // Throw away asks once more
};

} // namespace ripper
