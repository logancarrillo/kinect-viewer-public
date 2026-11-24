#include "qr_draw.hpp"

#include "qrcodegen.hpp"

#include <algorithm>

namespace kvwin {

int drawQrCode(HDC dc, const std::string& text, int centre_x, int top, int box) {
    const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int n = qr.getSize();
    const int scale = std::max(1, box / (n + 8));
    const int size = scale * (n + 8);
    const int x0 = centre_x - size / 2;
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    for (int qy = 0; qy < n; ++qy) {
        for (int qx = 0; qx < n; ++qx) {
            if (!qr.getModule(qx, qy)) continue;
            RECT m{x0 + (qx + 4) * scale, top + (qy + 4) * scale, x0 + (qx + 5) * scale, top + (qy + 5) * scale};
            FillRect(dc, &m, black);
        }
    }
    return size;
}

} // namespace kvwin
