// Layout and rendering of a single label.
//
// Everything is measured in PostScript points (1 pt = 1/72 inch) because that is
// how CUPS takes page sizes. Preview, PDF export and printing share the same
// functions, so what you see on screen is what ends up on the tape.
#pragma once

#include <QFont>
#include <QImage>

#include "Graphics.h"
#include <QList>
#include <QString>
#include <QStringList>

class QPainter;

namespace ptouch {

constexpr double PtPerMm = 72.0 / 25.4;

// Fonts are sized in pixels rather than points: Qt converts point sizes against
// the device DPI, which makes the on-screen preview and the 600 dpi PDF diverge
// by a factor of DPI/72. One unit = 1/UnitsPerPt point.
constexpr int UnitsPerPt = 10;

// Resolution assumed when no model is known — nearly every P-touch prints at 180 dpi.
constexpr int HeadDpiDefault = 180;


struct Tape {
    double widthMm;
    int pageWidthPt;   // page width for CUPS
    int dots180;       // printable dots on a 180 dpi head
    int dots360;       // printable dots on a 360 dpi head
};

struct PrinterModel {
    QString name;      // as Brother writes it, e.g. "PT-P710BT"
    int headDots;      // width of the print head
    int dpi;
};

// The head never reaches the full tape width — a margin stays blank along both
// edges. These figures, and the head widths below, are taken from ptouch-print
// by Dominic Radermacher (GPL-3.0, https://dominic.familie-radermacher.ch/
// projekte/ptouch-print/), which maintains them against real hardware. Only the
// numbers are used here; none of its code is.
const QList<Tape> &tapes();
const Tape *tapeFor(double widthMm);
int tapeIndex(double widthMm);   // position in tapes(), -1 if unknown

const QList<PrinterModel> &printerModels();
// Longest model whose name the given device name starts with — Bluetooth names
// carry a serial number ("PT-P710BT3015"), so an exact match will not do.
const PrinterModel *modelFor(const QString &deviceName);
constexpr int DefaultHeadDots = 128;   // what nearly every P-touch has
constexpr int DefaultDpi = 180;

enum class Align { Left, Center, Right };

struct Spec {
    QString text = QStringLiteral("Example");
    double tapeMm = 12.0;
    QString family = QStringLiteral("DejaVu Sans");
    bool bold = true;
    bool italic = false;
    bool autoSize = true;
    double sizePt = 20.0;
    Align align = Align::Center;
    bool autoLength = true;
    double lengthMm = 50.0;
    double marginMm = 3.0;
    double lineSpacing = 1.1;
    bool frame = false;
    bool mirror = false;
    bool autocut = true;
    int copies = 1;
    QString model;     // empty means "assume a 128 dot, 180 dpi head"

    // Optional artwork beside the text: an imported picture or a code. Both are
    // scaled to the printable height and placed on the chosen side.
    enum class Side { Left, Right };
    QString picturePath;
    CodeType codeType = CodeType::None;
    QString codeData;
    Side artworkSide = Side::Left;
    bool rotated = false;   // text and artwork turned 90°, for wide tape

    QStringList lines() const;
    double tapePt() const;        // physical tape width
    double printablePt() const;   // height the print head can blacken

    // Width of the page handed to CUPS. Never wider than the print head: the
    // driver drops whatever exceeds it, and from one side only, which shifts the
    // whole label. On tape narrower than the head this is the tape width, so
    // nothing changes there — the clamp only bites on 24 mm and wider.
    double pagePt() const;
    double headPt() const;   // width of the print head
};

struct Layout {
    QFont font;
    QImage artwork;        // picture or code, already monochrome
    double artworkWidth = 0;   // space it occupies, in points
    QString artworkError;
    double sizePt = 0;
    double lengthPt = 0;
    double lineHeight = 0;
    double inkHeight = 0;      // height of the actual glyphs, not of the font metrics
    double firstBaseline = 0;  // tape edge -> baseline of the first line
    double textWidth = 0;
    QList<double> lineWidths;
    bool overflow = false;
    QStringList warnings;
};

QFont buildFont(const Spec &spec, double sizePt);
Layout computeLayout(const Spec &spec);

// True when the text needs glyphs that only a colour bitmap font provides.
// Those cannot be embedded into a PDF — such labels are rendered as an image.
bool needsRasterGlyphs(const Spec &spec);

// Draws the label lying down: x runs along the tape, y across it, origin top left.
void render(QPainter &painter, const Spec &spec, const Layout &layout);

// Same, but falls back to an image where glyphs cannot be drawn as outlines.
// `dpi` controls the resolution of that fallback.
void paintLabel(QPainter &painter, const Spec &spec, const Layout &layout, int dpi);

// Renders the label into a greyscale image — what a monochrome printer will get.
QImage renderToImage(const Spec &spec, const Layout &layout, int dpi);

// Writes a PDF whose page size matches the label exactly; returns it in points.
struct PageSize { int widthPt; int heightPt; };
PageSize writePdf(const QString &path, const Spec &spec, const Layout &layout);

} // namespace ptouch
