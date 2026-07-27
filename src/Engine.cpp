#include "Engine.h"

#include <QFontMetricsF>
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
    static const QList<Tape> list = {
        {3.5, 10, 24},
        {6.0, 17, 32},
        {9.0, 26, 50},
        {12.0, 34, 70},
        {18.0, 51, 112},
        {24.0, 68, 128},
    };
    return list;
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
    const Tape *t = tapeFor(tapeMm);
    return (t ? t->printableDots : 70) / double(HeadDpi) * 72.0;
}

QFont buildFont(const Spec &spec, double sizePt)
{
    QFont f(spec.family);
    f.setPixelSize(std::max(1, int(qRound(sizePt * UnitsPerPt))));
    f.setBold(spec.bold);
    f.setItalic(spec.italic);
    return f;
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
    double size = spec.sizePt;

    if (spec.autoSize) {
        // Find the largest size whose glyphs still fit the printable tape height …
        double lo = 2.0, hi = 400.0;
        for (int i = 0; i < 40; ++i) {
            const double mid = (lo + hi) / 2;
            const Measurement m = measure(spec, mid);
            bool fits = m.inkHeight <= safeHeight;
            if (fits && !spec.autoLength) {
                const double usable = std::max(1.0, spec.lengthMm * PtPerMm - 2 * margin);
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
        layout.warnings << QStringLiteral("Text is %1 mm tall, only %2 mm are printable")
                               .arg(m.inkHeight / PtPerMm, 0, 'f', 1)
                               .arg(safeHeight / PtPerMm, 0, 'f', 1);
    }

    const double length = spec.autoLength
                              ? std::max(m.textWidth + 2 * margin, 8 * PtPerMm)
                              : spec.lengthMm * PtPerMm;

    if (m.textWidth > length - 2 * margin + 0.5) {
        layout.overflow = true;
        layout.warnings << QStringLiteral("Text needs %1 mm of length")
                               .arg((m.textWidth + 2 * margin) / PtPerMm, 0, 'f', 0);
    }

    // Centre the ink inside the printable strip, not on the tape as a whole.
    const double safeTop = (spec.tapePt() - safeHeight) / 2;

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
    const double safeTop = (spec.tapePt() - spec.printablePt()) / 2;

    painter.setPen(QPen(Qt::black));

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
        case Align::Left:  x = margin; break;
        case Align::Right: x = layout.lengthPt - margin - w; break;
        default:           x = (layout.lengthPt - w) / 2; break;
        }
        painter.drawText(QPointF(x * UnitsPerPt, y * UnitsPerPt), lines.at(i));
        y += layout.lineHeight;
    }
    painter.restore();
}

PageSize writePdf(const QString &path, const Spec &spec, const Layout &layout)
{
    const double wPt = spec.tapePt();
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
    render(p, spec, layout);
    p.end();

    return {int(qRound(wPt)), int(qRound(hPt))};
}

} // namespace ptouch
