#include "Engine.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QRawFont>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSizeF>

#include <algorithm>

namespace ptouch {

const QList<Tape> &tapes()
{
    // widthMm, page width in pt, printable dots at 180 dpi, at 360 dpi
    static const QList<Tape> list = {
        {3.5, 10, 24, 48},
        {6.0, 17, 32, 64},
        {9.0, 26, 52, 106},
        {12.0, 34, 76, 150},
        {18.0, 51, 120, 234},
        {21.0, 60, 124, 248},
        {24.0, 68, 128, 320},
        {36.0, 102, 192, 454},
    };
    return list;
}

const QList<PrinterModel> &printerModels()
{
    static const QList<PrinterModel> list = {
        {QStringLiteral("PT-1230PC"), 128, 180},
        {QStringLiteral("PT-1950"), 112, 180},
        {QStringLiteral("PT-2300"), 112, 180},
        {QStringLiteral("PT-2420PC"), 128, 180},
        {QStringLiteral("PT-2430PC"), 128, 180},
        {QStringLiteral("PT-2450PC"), 128, 180},
        {QStringLiteral("PT-2700"), 128, 180},
        {QStringLiteral("PT-2730"), 128, 180},
        {QStringLiteral("PT-3600"), 384, 360},
        {QStringLiteral("PT-9200DX"), 384, 360},
        {QStringLiteral("PT-D410"), 128, 180},
        {QStringLiteral("PT-D450"), 128, 180},
        {QStringLiteral("PT-D460BT"), 128, 180},
        {QStringLiteral("PT-D600"), 128, 180},
        {QStringLiteral("PT-D610BT"), 128, 180},
        {QStringLiteral("PT-E310BT"), 128, 180},
        {QStringLiteral("PT-E500"), 128, 180},
        {QStringLiteral("PT-E550W"), 128, 180},
        {QStringLiteral("PT-E560BT"), 128, 180},
        {QStringLiteral("PT-H500"), 128, 180},
        {QStringLiteral("PT-P700"), 128, 180},
        {QStringLiteral("PT-P710BT"), 128, 180},
        {QStringLiteral("PT-P750W"), 128, 180},
        {QStringLiteral("PT-P900Wc"), 560, 360},
    };
    return list;
}

const PrinterModel *modelFor(const QString &deviceName)
{
    const QString needle = QString(deviceName).remove(QLatin1Char(' ')).toUpper();
    const PrinterModel *best = nullptr;
    for (const PrinterModel &m : printerModels()) {
        if (needle.startsWith(m.name.toUpper())
            && (!best || m.name.size() > best->name.size()))
            best = &m;
    }
    return best;
}

const Tape *tapeFor(double widthMm)
{
    for (const Tape &t : tapes()) {
        if (qFuzzyCompare(t.widthMm, widthMm))
            return &t;
    }
    return nullptr;
}

int tapeIndex(double widthMm)
{
    const QList<Tape> &list = tapes();
    for (int i = 0; i < list.size(); ++i) {
        if (qFuzzyCompare(list.at(i).widthMm, widthMm))
            return i;
    }
    return -1;
}

QStringList Spec::lines() const
{
    QStringList l = text.split(QLatin1Char('\n'));
    if (l.isEmpty())
        l << QString();
    return l;
}

double Spec::tapePt() const
{
    const Tape *t = tapeFor(tapeMm);
    return t ? t->pageWidthPt : 34;
}

double Spec::printablePt() const
{
    const Tape *tape = tapeFor(tapeMm);
    if (!tape)
        return 76 / double(DefaultDpi) * 72.0;

    const PrinterModel *printer = modelFor(model);
    const int dpi = printer ? printer->dpi : DefaultDpi;
    const int headDots = printer ? printer->headDots : DefaultHeadDots;

    // Whichever is narrower: what fits on the tape, or what the head can reach.
    const int dots = std::min(dpi >= 360 ? tape->dots360 : tape->dots180, headDots);
    return dots / double(dpi) * 72.0;
}

double Spec::headPt() const
{
    const PrinterModel *printer = modelFor(model);
    const int dots = printer ? printer->headDots : DefaultHeadDots;
    const int dpi = printer ? printer->dpi : DefaultDpi;
    return dots / double(dpi) * 72.0;
}

double Spec::pagePt() const
{
    return std::min(tapePt(), headPt());
}

QFont buildFont(const Spec &spec, double sizePt)
{
    QFont f;
    // Fall back to symbol and emoji families for anything the chosen font lacks.
    // Outline fonts come first: their glyphs survive the trip through the PDF.
    f.setFamilies({spec.family,
                   QStringLiteral("Noto Sans Symbols2"),
                   QStringLiteral("Symbola"),
                   QStringLiteral("Noto Emoji"),
                   QStringLiteral("Noto Color Emoji")});
    f.setPixelSize(std::max(1, int(qRound(sizePt * UnitsPerPt))));
    f.setBold(spec.bold);
    f.setItalic(spec.italic);
    return f;
}

bool needsRasterGlyphs(const Spec &spec)
{
    // Guessing by code point does not work: ⚡ (U+26A1) sits well below the
    // pictograph blocks yet still comes from the colour font, while ★ (U+2605) is
    // a plain outline. So ask the fonts instead — if no outline family can draw a
    // character, only the colour bitmap font is left and the PDF would lose it.
    static QHash<QString, QRawFont> cache;
    const QStringList families = {spec.family,
                                  QStringLiteral("Noto Sans Symbols2"),
                                  QStringLiteral("Symbola"),
                                  QStringLiteral("Noto Emoji"),
                                  QStringLiteral("DejaVu Sans")};
    const QStringList installed = QFontDatabase::families();

    for (const uint ucs4 : spec.text.toUcs4()) {
        if (ucs4 < 0x80 || QChar::isSpace(ucs4))
            continue;
        bool drawable = false;
        for (const QString &family : families) {
            if (family.isEmpty() || !installed.contains(family))
                continue;
            QRawFont &raw = cache[family];
            if (!raw.isValid())
                raw = QRawFont::fromFont(QFont(family));
            if (raw.isValid() && raw.supportsCharacter(ucs4)) {
                drawable = true;
                break;
            }
        }
        if (!drawable)
            return true;
    }
    return false;
}

namespace {

struct Measurement {
    double lineHeight = 0;
    double inkHeight = 0;
    double inkTop = 0;      // relative to the first baseline
    double textWidth = 0;
    QList<double> widths;
};

Measurement measure(const Spec &spec, double sizePt)
{
    const QStringList lines = spec.lines();
    const QFontMetricsF fm(buildFont(spec, sizePt));
    const double u = UnitsPerPt;

    Measurement m;
    m.lineHeight = fm.height() / u * spec.lineSpacing;

    double top = 0, bottom = 0;
    bool first = true;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        const double advance = fm.horizontalAdvance(line) / u;
        m.widths << advance;
        m.textWidth = std::max(m.textWidth, advance);

        double lineTop, lineBottom;
        const QRectF tight = line.trimmed().isEmpty() ? QRectF() : fm.tightBoundingRect(line);
        if (tight.height() <= 0) {           // empty line: fall back to font metrics
            lineTop = -fm.ascent() / u * 0.72;
            lineBottom = 0.0;
        } else {
            lineTop = tight.top() / u;
            lineBottom = tight.bottom() / u;
        }
        lineTop += i * m.lineHeight;
        lineBottom += i * m.lineHeight;

        top = first ? lineTop : std::min(top, lineTop);
        bottom = first ? lineBottom : std::max(bottom, lineBottom);
        first = false;
    }
    m.inkTop = top;
    m.inkHeight = bottom - top;
    return m;
}

} // namespace

Layout computeLayout(const Spec &spec)
{
    const double margin = spec.marginMm * PtPerMm;
    const double safeHeight = spec.printablePt();

    Layout layout;

    // Artwork first: it takes its share of the length, and the text has to make
    // do with the rest.
    if (!spec.picturePath.isEmpty())
        layout.artwork = loadPicture(spec.picturePath, &layout.artworkError);
    else if (spec.codeType != CodeType::None)
        layout.artwork = renderCode(spec.codeType, spec.codeData, &layout.artworkError);

    if (!layout.artwork.isNull()) {
        const double aspect = double(layout.artwork.width())
                              / std::max(1, layout.artwork.height());

        if (layout.artwork.height() == 1) {
            // A barcode is one pixel per module. Its width must follow from the
            // module width, not from the space available: squeezed below roughly
            // a quarter millimetre per bar, no scanner reads it. Two printer dots
            // give 0.28 mm at 180 dpi, which is the practical minimum.
            const double dotsPerModule = 2.0;
            layout.artworkWidth = layout.artwork.width() * dotsPerModule / HeadDpiDefault * 72.0;
            const double moduleMm = layout.artworkWidth / layout.artwork.width() / PtPerMm;
            if (moduleMm < 0.25) {
                layout.warnings << QCoreApplication::translate("Label",
                                    "barcode bars are %1 mm wide — most scanners need 0.25 mm")
                                       .arg(moduleMm, 0, 'f', 2);
            }
        } else {
            layout.artworkWidth = safeHeight * aspect;
            // Same concern for QR: below about 0.4 mm per module reading gets
            // unreliable, and on narrow tape that happens quickly.
            const double moduleMm = safeHeight / layout.artwork.height() / PtPerMm;
            if (spec.codeType == CodeType::Qr && moduleMm < 0.4) {
                layout.warnings << QCoreApplication::translate("Label",
                                    "QR modules are %1 mm — use wider tape or less content")
                                       .arg(moduleMm, 0, 'f', 2);
            }
        }
        layout.artworkWidth = std::max(layout.artworkWidth, 3.0);
    }
    if (!layout.artworkError.isEmpty())
        layout.warnings << layout.artworkError;

    const double artworkSpace = layout.artwork.isNull()
                                    ? 0.0
                                    : layout.artworkWidth + margin;

    double size = spec.sizePt;

    if (spec.autoSize) {
        // Find the largest size whose glyphs still fit the printable tape height …
        double lo = 2.0, hi = 400.0;
        for (int i = 0; i < 40; ++i) {
            const double mid = (lo + hi) / 2;
            const Measurement m = measure(spec, mid);
            bool fits = m.inkHeight <= safeHeight;
            if (fits && !spec.autoLength) {
                const double usable = std::max(1.0, spec.lengthMm * PtPerMm
                                                        - 2 * margin - artworkSpace);
                fits = m.textWidth <= usable;   // … and the width too, at a fixed length
            }
            if (fits)
                lo = mid;
            else
                hi = mid;
        }
        size = lo;
    }

    const Measurement m = measure(spec, size);
    if (m.inkHeight > safeHeight + 0.5) {
        layout.warnings << QCoreApplication::translate("Label", "Text is %1 mm tall, only %2 mm are printable")
                               .arg(m.inkHeight / PtPerMm, 0, 'f', 1)
                               .arg(safeHeight / PtPerMm, 0, 'f', 1);
    }

    const double length = spec.autoLength
                              ? std::max(m.textWidth + 2 * margin + artworkSpace, 8 * PtPerMm)
                              : spec.lengthMm * PtPerMm;

    if (m.textWidth > length - 2 * margin - artworkSpace + 0.5) {
        layout.overflow = true;
        layout.warnings << QCoreApplication::translate("Label", "Text needs %1 mm of length")
                               .arg((m.textWidth + 2 * margin + artworkSpace) / PtPerMm,
                                    0, 'f', 0);
    }

    // Centre the ink inside the printable strip, not on the tape as a whole.
    const double safeTop = (spec.pagePt() - safeHeight) / 2;

    layout.font = buildFont(spec, size);
    layout.sizePt = size;
    layout.lengthPt = length;
    layout.lineHeight = m.lineHeight;
    layout.inkHeight = m.inkHeight;
    layout.firstBaseline = safeTop + (safeHeight - m.inkHeight) / 2 - m.inkTop;
    layout.textWidth = m.textWidth;
    layout.lineWidths = m.widths;
    return layout;
}

void render(QPainter &painter, const Spec &spec, const Layout &layout)
{
    const double margin = spec.marginMm * PtPerMm;
    const double safeTop = (spec.pagePt() - spec.printablePt()) / 2;

    painter.setPen(QPen(Qt::black));

    // Artwork sits on its side of the label, vertically centred in the strip the
    // head can reach; the text keeps the remaining width.
    double textLeft = margin;
    double textRight = layout.lengthPt - margin;
    if (!layout.artwork.isNull()) {
        const double aspect = double(layout.artwork.width())
                              / std::max(1, layout.artwork.height());
        const double height = layout.artwork.height() == 1
                                  ? spec.printablePt() * 0.75
                                  : std::min(spec.printablePt(), layout.artworkWidth / aspect);
        const double width = layout.artworkWidth;
        const double y = safeTop + (spec.printablePt() - height) / 2;
        const double x = spec.artworkSide == Spec::Side::Left
                             ? margin
                             : layout.lengthPt - margin - width;

        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, layout.artwork.height() > 1);
        painter.drawImage(QRectF(x, y, width, height), layout.artwork);
        painter.restore();

        if (spec.artworkSide == Spec::Side::Left)
            textLeft += width + margin;
        else
            textRight -= width + margin;
    }

    if (spec.frame) {
        const double inset = 1.0;
        painter.save();
        painter.setPen(QPen(Qt::black, 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(inset, safeTop + inset,
                                layout.lengthPt - 2 * inset,
                                spec.printablePt() - 2 * inset));
        painter.restore();
    }

    // The font is defined in units, so the coordinate system is scaled down for
    // the text output to keep one unit at 1/10 pt.
    painter.save();
    painter.scale(1.0 / UnitsPerPt, 1.0 / UnitsPerPt);
    painter.setFont(layout.font);

    double y = layout.firstBaseline;
    const QStringList lines = spec.lines();
    for (int i = 0; i < lines.size(); ++i) {
        const double w = i < layout.lineWidths.size() ? layout.lineWidths.at(i) : 0.0;
        double x;
        switch (spec.align) {
        case Align::Left:  x = textLeft; break;
        case Align::Right: x = textRight - w; break;
        default:           x = textLeft + (textRight - textLeft - w) / 2; break;
        }
        painter.drawText(QPointF(x * UnitsPerPt, y * UnitsPerPt), lines.at(i));
        y += layout.lineHeight;
    }
    painter.restore();
}

QImage renderToImage(const Spec &spec, const Layout &layout, int dpi)
{
    const double scale = dpi / 72.0;
    const int w = std::max(1, int(qRound(layout.lengthPt * scale)));
    const int h = std::max(1, int(qRound(spec.pagePt() * scale)));

    QImage image(w, h, QImage::Format_RGB32);
    image.fill(Qt::white);

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.scale(scale, scale);
    render(p, spec, layout);
    p.end();

    // The printer is monochrome, so grey is closer to the result than colour.
    QImage grey = image.convertToFormat(QImage::Format_Grayscale8);

    // Emoji artwork lives mostly in the middle greys, which a monochrome tape
    // dithers away to almost nothing. Stretching the range keeps the shapes
    // legible without turning them into black blobs.
    uchar curve[256];
    for (int i = 0; i < 256; ++i) {
        const double v = std::clamp((i / 255.0 - 0.12) / 0.76, 0.0, 1.0);
        curve[i] = uchar(qRound(v * 255));
    }
    for (int y = 0; y < grey.height(); ++y) {
        uchar *line = grey.scanLine(y);
        for (int x = 0; x < grey.width(); ++x)
            line[x] = curve[line[x]];
    }
    return grey;
}

void paintLabel(QPainter &painter, const Spec &spec, const Layout &layout, int dpi)
{
    if (!needsRasterGlyphs(spec)) {
        render(painter, spec, layout);
        return;
    }
    const QImage image = renderToImage(spec, layout, dpi);
    painter.drawImage(QRectF(0, 0, layout.lengthPt, spec.pagePt()), image);
}

PageSize writePdf(const QString &path, const Spec &spec, const Layout &layout)
{
    const double wPt = spec.pagePt();
    const double hPt = layout.lengthPt;

    QPdfWriter writer(path);
    writer.setResolution(600);
    writer.setPageSize(QPageSize(QSizeF(wPt, hPt), QPageSize::Point,
                                 QStringLiteral("label"), QPageSize::ExactMatch));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Point);

    QPainter p(&writer);
    const double s = writer.resolution() / 72.0;
    p.scale(s, s);
    p.translate(wPt, 0);          // the tape runs across the page direction
    p.rotate(90);
    if (spec.mirror) {
        p.translate(layout.lengthPt, 0);
        p.scale(-1, 1);
    }
    paintLabel(p, spec, layout, writer.resolution());
    p.end();

    return {int(qRound(wPt)), int(qRound(hPt))};
}

} // namespace ptouch
