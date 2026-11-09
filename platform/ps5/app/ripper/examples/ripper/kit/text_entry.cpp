// RipperPS: typing a line of text on the pad. See text_entry.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_entry.hpp"

#include "core/tween.hpp"
#include "ripper_app.hpp"

#include <algorithm>
#include <cmath>

namespace ripper
{

namespace ui = hui::ui;
using hui::Action;
using hui::gfx::Rect;

namespace
{
const Rect kPanel{300, 150, 1320, 800};
const Rect kField{360, 330, 1200, 84};
const Rect kKeys{360, 480, 1200, 420};
} // namespace

TextEntry::TextEntry()
{
    keys_.set_layouts({ui::KeyboardLayout::letters(), ui::KeyboardLayout::symbols()});
    keys_.style.bindings = ui::KeyboardBindings::standard();
    keys_.style.max_length = 240;
    keys_.on_text = [this](std::string_view t) { text_ += t; };
    keys_.on_backspace = [this] {
        // One whole character, however many bytes it takes.
        std::size_t at = text_.size();
        while (at > 0 && (static_cast<unsigned char>(text_[at - 1]) & 0xC0) == 0x80)
            --at;
        text_.erase(at > 0 ? at - 1 : 0);
    };
    keys_.set_bounds(kKeys);
}

void TextEntry::open(const std::string &title, const std::string &text, bool secret, const std::string &hint)
{
    title_ = title;
    text_ = text;
    hint_ = hint;
    secret_ = secret;
    active_ = true;
    age_ = 0.0f;
    keys_.set_focus(1, 0);
    keys_.enter();
}

int TextEntry::update(const hui::InputFrame &input, float dt, ui::Feedback &feedback, const Look &look)
{
    if (!active_)
        return 0;
    age_ += dt;
    keys_.style.theme = *look.theme;
    if (input.is_pressed(Action::back))
    {
        active_ = false;
        feedback.play(hui::audio::Cue::back);
        return -1;
    }
    keys_.set_length(static_cast<int>(text_.size()));
    const bool done = keys_.handle(input, feedback) == ui::Event::activated;
    keys_.update(dt);
    if (done)
    {
        while (!text_.empty() && text_.back() == ' ')
            text_.pop_back();
        active_ = false;
        return 1;
    }
    return 0;
}

void TextEntry::draw(hui::gfx::DrawList &list, const ui::Fonts &fonts, const Look &look, float clock) const
{
    if (!active_)
        return;
    const float in = hui::tween::clamp01(age_ * 6.0f);
    list.push_opacity(in);
    list.rounded_rect({0, 0, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight}, 0, look.page.with_alpha(0.75f));
    // Opaque under the panel: a frosted look's panel would let the page through.
    list.rounded_rect(kPanel, 36, look.page);
    draw_panel(list, look, kPanel, 36);
    ui::text(list, fonts.display, fonts.display.font->fit(title_, 40, kField.w), kField.x, kPanel.y + 110, 40,
             look.panel_text);
    list.bordered_rect(kField, 18, look.panel_text.with_alpha(0.06f), 2.0f, look.focus);
    std::string shown = secret_ ? std::string(text_.size(), '*') : plain_text(text_);
    // The end of a long line, where the typing is.
    const float room = kField.w - 64;
    while (!shown.empty() && fonts.regular.font->measure(shown, 32) > room)
        shown.erase(0, char_boundary(shown, 1));
    const float w = ui::text(list, fonts.regular, shown, kField.x + 28, kField.y + 54, 32, look.panel_text);
    if (std::fmod(clock, 1.0f) < 0.6f)
        list.rounded_rect({kField.x + 30 + w, kField.y + 20, 3, 44}, 1.5f, look.accent);
    if (!hint_.empty())
        ui::text(list, fonts.regular, fonts.regular.font->fit(hint_, 20, kField.w), kField.x, kField.y + 120, 20,
                 look.panel_muted);
    ui::Canvas canvas{list, fonts, 0, clock};
    keys_.draw(canvas);
    list.pop_opacity();
}

std::vector<ui::Hint> TextEntry::hints() const
{
    return {{ui::Button::square, "Delete"}, {ui::Button::triangle, "Space"}, {ui::Button::r2, "Symbols"},
            {ui::Button::circle, "Back"}};
}

} // namespace ripper
