// icon.cpp -- the app icon: a chequered flag on a pole, drawn at 16x16 and
// scaled by whole pixels so every size shows the same picture. app.ico holds the
// same drawing for Explorer; make_icon.py writes it.
#include "icon.h"

#include <wx/image.h>

namespace {

wxImage FlagTile() {
    wxImage tile(16, 16);
    tile.InitAlpha();
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) tile.SetAlpha(x, y, 0);
    auto put = [&](int x, int y, unsigned char v) {
        tile.SetRGB(x, y, v, v, v);
        tile.SetAlpha(x, y, 255);
    };
    for (int y = 0; y < 16; ++y) {  // the pole
        put(2, y, 0x60);
        put(3, y, 0x90);
    }
    // The flag: a dark border round 5 x 4 squares of 2 x 2 pixels.
    for (int y = 1; y <= 10; ++y)
        for (int x = 4; x <= 15; ++x) {
            const bool border = y == 1 || y == 10 || x == 4 || x == 15;
            const bool dark = border || ((x - 5) / 2 + (y - 2) / 2) % 2 == 0;
            put(x, y, dark ? 0x10 : 0xFF);
        }
    return tile;
}

}  // namespace

wxBitmap AppBitmap(int size) { return wxBitmap(FlagTile().Scale(size, size, wxIMAGE_QUALITY_NEAREST)); }

wxIconBundle AppIcons() {
    wxIconBundle icons;
    for (int size : {16, 20, 24, 32, 48, 64, 256}) {
        wxIcon icon;
        icon.CopyFromBitmap(AppBitmap(size));
        icons.AddIcon(icon);
    }
    return icons;
}
