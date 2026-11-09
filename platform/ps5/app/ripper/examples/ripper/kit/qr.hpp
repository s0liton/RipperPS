// RipperPS: QR codes for addresses a phone should open (RomM's sign-in page,
// the web UI). RommPS's (kit/credits.cpp), with Project Nayuki's encoder.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/draw_list.hpp"

#include <string>
#include <vector>

namespace ripper
{

struct QrCode
{
    int size = 0;
    std::vector<bool> modules;
    static QrCode encode(const std::string &text);
    bool dark(int x, int y) const { return modules[static_cast<std::size_t>(y * size + x)]; }
};

// Draws a QR code on a white plate (its quiet zone included) filling rect.
void draw_qr(hui::gfx::DrawList &list, const QrCode &qr, const hui::gfx::Rect &rect);

} // namespace ripper
