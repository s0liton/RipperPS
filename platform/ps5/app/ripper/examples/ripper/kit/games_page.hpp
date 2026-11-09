// RipperPS: the Games tab. The PS4 and PS5 games installed on this console,
// each dumped (decrypted by the console) on Cross: a sheet with the dump's
// options, where it goes and what it's called.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/tween.hpp"
#include "ripper_app.hpp"
#include "rows.hpp"
#include "text_entry.hpp"

#include <map>
#include <string>
#include <vector>

namespace ripper
{

class GamesPage
{
  public:
    void enter();
    // Returns true when a dump started (the screen shows the queue).
    bool update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, Color accent, float clock,
              float age) const;
    void draw_overlay(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, float clock) const;
    std::vector<hui::ui::Hint> hints() const;
    // The dump sheet or the keyboard is open.
    bool nested() const { return sheet_ || entry_.active(); }

  private:
    enum Kind
    {
        kNote,
        kDestination,
        kName,
        kFolder,
        kGo,
        kOption = 1000,
    };
    std::vector<Row> sheet_rows(std::vector<int> *kinds) const;
    const std::vector<Option> *options() const;
    std::string option_value(const Option &o) const;
    std::string options_json() const;
    void open_sheet(const Game &g);

    int row_ = 0;
    hui::tween::Spring position_, scroll_;
    bool sheet_ = false;
    bool busy_ = false;
    Game game_;
    std::string destination_, name_, folder_;
    std::map<std::string, std::string> options_;
    RowList list_;
    TextEntry entry_;
    int entry_for_ = kName;
};

} // namespace ripper
