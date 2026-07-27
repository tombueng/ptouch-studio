// Checks the measurements that were verified against the real device.
//
// The crux is font scaling: Qt converts point sizes against the device DPI. Scaling
// the coordinate system on top of that (600 dpi PDF) makes the text come out DPI/72
// too large while every other measurement looks right. So this does not merely do
// arithmetic — it rasterises the produced PDF and measures the ink.
#include "Engine.h"
#include "Status.h"

#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>

using namespace ptouch;

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    std::printf("%s  %s\n", condition ? "ok  " : "FAIL", qPrintable(what));
    if (!condition)
        ++failures;
}

void checkNear(double actual, double expected, double tolerance, const QString &what)
{
    const bool ok = std::fabs(actual - expected) <= tolerance;
    std::printf("%s  %s (%.2f, expected %.2f ± %.2f)\n",
                ok ? "ok  " : "FAIL", qPrintable(what), actual, expected, tolerance);
    if (!ok)
        ++failures;
}

// Rasterises the PDF at 10 pixels per millimetre and measures the actual ink.
struct Ink {
    bool valid = false;
    double pageWidthMm = 0, pageHeightMm = 0;
    double acrossMm = 0, alongMm = 0;
    double marginTopMm = 0, marginBottomMm = 0;
    double marginLeadMm = 0, marginTrailMm = 0;
};

Ink measurePdf(const QString &pdfPath, const QString &workDir)
{
    Ink ink;
    const QString stem = workDir + QStringLiteral("/raster");
    QProcess pdftoppm;
    pdftoppm.start(QStringLiteral("pdftoppm"),
                   {QStringLiteral("-r"), QStringLiteral("254"), QStringLiteral("-gray"),
                    QStringLiteral("-png"), pdfPath, stem});
    if (!pdftoppm.waitForFinished(30000) || pdftoppm.exitCode() != 0)
        return ink;   // without poppler-utils this part is skipped

    QImage image(stem + QStringLiteral("-1.png"));
    if (image.isNull())
        image = QImage(stem + QStringLiteral("-01.png"));
    if (image.isNull())
        return ink;

    image = image.convertToFormat(QImage::Format_Grayscale8);
    const int w = image.width(), h = image.height();
    int minX = w, maxX = -1, minY = h, maxY = -1;
    for (int y = 0; y < h; ++y) {
        const uchar *line = image.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (line[x] < 128) {
                minX = std::min(minX, x); maxX = std::max(maxX, x);
                minY = std::min(minY, y); maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0)
        return ink;

    ink.valid = true;
    ink.pageWidthMm = w / 10.0;
    ink.pageHeightMm = h / 10.0;
    ink.acrossMm = (maxX - minX + 1) / 10.0;
    ink.alongMm = (maxY - minY + 1) / 10.0;
    ink.marginTopMm = minX / 10.0;
    ink.marginBottomMm = (w - 1 - maxX) / 10.0;
    ink.marginLeadMm = minY / 10.0;
    ink.marginTrailMm = (h - 1 - maxY) / 10.0;
    return ink;
}

void testTapeTable()
{
    check(tapes().size() == 6, "six tape widths known");
    check(tapeFor(12.0) != nullptr, "12 mm is a known tape width");
    check(tapeFor(13.0) == nullptr, "13 mm is not a known tape width");
    check(tapeIndex(24.0) == 5, "24 mm is the last entry");

    Spec s;
    s.tapeMm = 12.0;
    checkNear(s.tapePt(), 34.0, 0.01, "12 mm tape is 34 pt wide");
    checkNear(s.printablePt() / PtPerMm, 9.88, 0.05, "12 mm tape: printable height");
    s.tapeMm = 24.0;
    checkNear(s.printablePt() / PtPerMm, 18.06, 0.05, "24 mm tape: printable height");
}

void testAutoSizeFillsTape()
{
    // Automatic sizing has to use the printable height rather than the font
    // metrics, otherwise the text stays noticeably smaller than it could be.
    for (const double width : {12.0, 24.0}) {
        Spec s;
        s.tapeMm = width;
        s.text = QStringLiteral("Workshop");
        const Layout l = computeLayout(s);
        checkNear(l.inkHeight, s.printablePt(), 0.6,
                  QStringLiteral("%1 mm: text fills the printable height").arg(width));
        check(l.warnings.isEmpty(), QStringLiteral("%1 mm: no warning").arg(width));
    }
}

void testMultiLineShrinks()
{
    Spec one, two;
    one.text = QStringLiteral("Fuse box");
    two.text = QStringLiteral("Fuse box\nF3 16A");
    const Layout l1 = computeLayout(one);
    const Layout l2 = computeLayout(two);
    check(l2.sizePt < l1.sizePt, "two lines yield a smaller font");
    checkNear(l2.inkHeight, two.printablePt(), 0.6, "two lines fill the tape height");
}

void testFixedLength()
{
    Spec s;
    s.text = QStringLiteral("Left");
    s.autoLength = false;
    s.lengthMm = 60;
    const Layout l = computeLayout(s);
    checkNear(l.lengthPt / PtPerMm, 60.0, 0.01, "fixed length is honoured");
    check(!l.overflow, "short text fits the fixed length");
}

void testOverflowDetected()
{
    Spec s;
    s.text = QStringLiteral("A very long text that will not fit");
    s.autoLength = false;
    s.autoSize = false;
    s.sizePt = 30;
    s.lengthMm = 20;
    const Layout l = computeLayout(s);
    check(l.overflow, "overlong text is detected");
    check(!l.warnings.isEmpty(), "overlong text produces a warning");
}

void testPdfGeometry(const QString &workDir)
{
    Spec s;
    s.tapeMm = 12.0;
    s.text = QStringLiteral("Workshop");
    const Layout l = computeLayout(s);

    const QString pdf = workDir + QStringLiteral("/label.pdf");
    const PageSize page = writePdf(pdf, s, l);
    check(page.widthPt == 34, "PDF page is 34 pt wide");
    checkNear(page.heightPt / PtPerMm, l.lengthPt / PtPerMm, 0.5,
              "PDF page length matches the layout");

    const Ink ink = measurePdf(pdf, workDir);
    if (!ink.valid) {
        std::printf("skipped: pdftoppm not available\n");
        return;
    }
    checkNear(ink.pageWidthMm, 12.0, 0.15, "rasterised page is 12 mm wide");
    checkNear(ink.acrossMm, 9.8, 0.7, "ink uses the printable tape height");
    checkNear(ink.marginTopMm, ink.marginBottomMm, 0.5, "text is centred on the tape");
    // The measured margin is the configured one plus the glyph's side bearing,
    // which depends on the first and last letter — hence the generous tolerance.
    checkNear(ink.marginLeadMm, s.marginMm, 1.0, "leading side margin as configured");
    checkNear(ink.marginTrailMm, s.marginMm, 1.0, "trailing side margin as configured");
}

void testGlyphDetection()
{
    // Plain text and outline symbols draw as vectors; anything only the colour
    // bitmap font provides has to take the image path or the PDF loses it.
    Spec plain;
    plain.text = QStringLiteral("Workshop 12");
    check(!needsRasterGlyphs(plain), "plain text draws as outlines");

    Spec star;
    star.text = QStringLiteral("★ Shelf");
    check(!needsRasterGlyphs(star), "outline symbols draw as outlines");

    Spec emoji;
    emoji.text = QStringLiteral("🔧 Tool");
    check(needsRasterGlyphs(emoji), "pictographs need the image path");

    // The image has to carry actual ink, otherwise the emoji silently vanishes.
    const Layout l = computeLayout(emoji);
    const QImage image = renderToImage(emoji, l, 300);
    int dark = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 200)
                ++dark;
    check(dark > 500, "rendered image contains the pictograph");
    check(image.format() == QImage::Format_Grayscale8, "image is greyscale for the tape");
}

void testStatusDecoding()
{
    QByteArray frame(StatusLength, '\0');
    frame[10] = 12;      // tape width
    frame[11] = 0x01;    // laminated
    Status s = decodeStatus(frame);
    check(s.ok, "clean status counts as ready");
    check(s.tapeMm == 12, "tape width is read from byte 10");
    check(s.mediaType == QStringLiteral("laminated"), "media type is recognised");

    frame[8] = 0x01;     // no tape
    s = decodeStatus(frame);
    check(!s.ok, "an error bit invalidates the status");
    check(s.errors.contains(QStringLiteral("no tape inserted")), "error text recognised");

    s = decodeStatus(QByteArray(8, '\0'));
    check(!s.error.isEmpty(), "a short reply is rejected");
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    QTemporaryDir dir;
    testTapeTable();
    testAutoSizeFillsTape();
    testMultiLineShrinks();
    testFixedLength();
    testOverflowDetected();
    testPdfGeometry(dir.path());
    testGlyphDetection();
    testStatusDecoding();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : qPrintable(QStringLiteral("%1 check(s) failed").arg(failures)));
    return failures == 0 ? 0 : 1;
}
