// RipperPS: QR codes. See qr.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "qr.hpp"

#include "qrcodegen.h"

#include <cmath>
#include <cstdint>

namespace ripper
{

using hui::gfx::Color;

QrCode QrCode::encode(const std::string &text)
{
    QrCode out;
    std::vector<std::uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), tmp(qrcodegen_BUFFER_LEN_MAX);
    if (!qrcodegen_encodeText(text.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return out;
    out.size = qrcodegen_getSize(qr.data());
    out.modules.resize(static_cast<std::size_t>(out.size * out.size));
    for (int y = 0; y < out.size; ++y)
        for (int x = 0; x < out.size; ++x)
            out.modules[static_cast<std::size_t>(y * out.size + x)] = qrcodegen_getModule(qr.data(), x, y);
    return out;
}

void draw_qr(hui::gfx::DrawList &list, const QrCode &qr, const hui::gfx::Rect &rect)
{
    list.rounded_rect(rect, 10, Color::rgb(0xffffff));
    if (qr.size == 0)
        return;
    const float cell = std::floor(rect.w / static_cast<float>(qr.size + 4)); // two modules of quiet zone a side
    const float x0 = rect.x + std::floor((rect.w - cell * static_cast<float>(qr.size)) * 0.5f);
    const float y0 = rect.y + std::floor((rect.h - cell * static_cast<float>(qr.size)) * 0.5f);
    const Color ink = Color::rgb(0x000000);
    for (int y = 0; y < qr.size; ++y)
    {
        // Runs of dark modules as one rectangle each.
        int x = 0;
        while (x < qr.size)
        {
            if (!qr.dark(x, y))
            {
                ++x;
                continue;
            }
            int end = x;
            while (end < qr.size && qr.dark(end, y))
                ++end;
            list.rounded_rect({x0 + static_cast<float>(x) * cell, y0 + static_cast<float>(y) * cell,
                               static_cast<float>(end - x) * cell, cell},
                              0, ink);
            x = end;
        }
    }
}

} // namespace ripper
