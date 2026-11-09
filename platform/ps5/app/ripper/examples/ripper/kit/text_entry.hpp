// RipperPS: typing a line of text on the pad, over the screen: a rip's name,
// a share's address, a password. The kit's on-screen keyboard under a field.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "ripper_look.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"

#include <string>
#include <vector>

namespace ripper
{

class TextEntry
{
  public:
    TextEntry();

    // secret: shown as dots. hint: a line under the field.
    void open(const std::string &title, const std::string &text, bool secret = false, const std::string &hint = "");
    bool active() const { return active_; }
    // 1 when it's done (text()), -1 when it was left with Circle, 0 meanwhile.
    int update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback, const Look &look);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, float clock) const;
    const std::string &text() const { return text_; }
    std::vector<hui::ui::Hint> hints() const;

  private:
    hui::ui::Keyboard keys_;
    std::string title_, text_, hint_;
    bool secret_ = false;
    bool active_ = false;
    float age_ = 0.0f;
};

} // namespace ripper
