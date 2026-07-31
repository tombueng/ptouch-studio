// Everything that goes on a label besides text: imported pictures, barcodes and
// QR codes. All of them end up as a monochrome QImage that the renderer places
// beside the text, scaled to the printable tape height.
#pragma once

#include <QImage>
#include <QString>

namespace ptouch {

enum class CodeType {
    None,
    Code128,   // drawn here, no library needed
    Qr,        // needs libqrencode at build time
};

// Whether this build can produce QR codes.
bool qrAvailable();

// Renders `data` as a barcode image, black on white, one pixel per module.
// Scaling to the label happens later, so the result stays crisp at any size.
// Returns a null image if the data cannot be encoded.
QImage renderCode(CodeType type, const QString &data, QString *error = nullptr);

// Loads a picture and reduces it to black and white. Photographs rarely work on
// a two-colour tape; line art, logos and pictograms do.
QImage loadPicture(const QString &path, QString *error = nullptr);

} // namespace ptouch
