// Checks the measurements that were verified against the real device.
//
// The crux is font scaling: Qt converts point sizes against the device DPI. Scaling
// the coordinate system on top of that (600 dpi PDF) makes the text come out DPI/72
// too large while every other measurement looks right. So this does not merely do
// arithmetic — it rasterises the produced PDF and measures the ink.
#include "Engine.h"
#include "Sdp.h"
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
    check(tapes().size() == 8, "eight tape widths known");
    check(tapeFor(12.0) != nullptr, "12 mm is a known tape width");
    check(tapeFor(13.0) == nullptr, "13 mm is not a known tape width");
    check(tapeIndex(24.0) == 6, "24 mm sits between 21 and 36 mm");

    Spec s;
    s.tapeMm = 12.0;
    checkNear(s.tapePt(), 34.0, 0.01, "12 mm tape is 34 pt wide");
    checkNear(s.printablePt() / PtPerMm, 10.73, 0.05, "12 mm tape: printable height");
    s.tapeMm = 24.0;
    checkNear(s.printablePt() / PtPerMm, 18.06, 0.05, "24 mm tape: printable height");

    // The head is the limit whenever the tape could take more: 36 mm tape would
    // allow 192 dots, but a 128 dot head reaches only 18.06 mm of it.
    s.tapeMm = 36.0;
    s.model = QStringLiteral("PT-P710BT");
    checkNear(s.printablePt() / PtPerMm, 18.06, 0.05, "128 dot head limits wide tape");
    s.model = QStringLiteral("PT-P900Wc");
    checkNear(s.printablePt() / PtPerMm, 32.0, 0.6, "360 dpi head prints wider");
    s.tapeMm = 12.0;
    s.model = QStringLiteral("PT-2300");           // 112 dot head
    checkNear(s.printablePt() / PtPerMm, 10.73, 0.05, "narrow head, narrow tape unaffected");
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
    checkNear(ink.acrossMm, 10.6, 0.8, "ink uses the printable tape height");
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

void testArtwork()
{
    // A barcode has to be sized by module width, not by available space: below
    // roughly a quarter millimetre per bar no scanner reads it.
    Spec bc;
    bc.tapeMm = 12.0;
    bc.text = QStringLiteral("Stock");
    bc.codeType = CodeType::Code128;
    bc.codeData = QStringLiteral("ABC-12345");
    const Layout lbc = computeLayout(bc);
    check(!lbc.artwork.isNull(), "barcode is rendered");
    const double moduleMm = lbc.artworkWidth / std::max(1, lbc.artwork.width()) / PtPerMm;
    checkNear(moduleMm, 0.28, 0.05, "barcode modules are wide enough to scan");
    check(lbc.lengthPt > bc.marginMm * 2 * PtPerMm + lbc.artworkWidth,
          "label grew to hold barcode and text");

    // Code 128 set B is ASCII only; anything else has to be refused rather than
    // silently mangled.
    QString error;
    check(renderCode(CodeType::Code128, QStringLiteral("Grüße"), &error).isNull()
              && !error.isEmpty(), "non-ASCII is rejected for Code 128");

    if (qrAvailable()) {
        Spec qr;
        qr.tapeMm = 24.0;
        qr.codeType = CodeType::Qr;
        qr.codeData = QStringLiteral("https://example.org");
        const Layout lqr = computeLayout(qr);
        check(!lqr.artwork.isNull(), "QR code is rendered");
        check(lqr.artwork.width() == lqr.artwork.height(), "QR code is square");
        checkNear(lqr.artworkWidth, qr.printablePt(), 1.0, "QR code fills the tape height");
    } else {
        std::printf("skipped: built without libqrencode\n");
    }
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

// SDP data elements, built by hand the way a printer sends them.
QByteArray sdpSeq(const QByteArray &content)
{
    return QByteArray(1, char(0x35)) + char(content.size()) + content;
}
QByteArray sdpUInt8(quint8 v) { return QByteArray::fromHex("08") + char(v); }
QByteArray sdpUInt16(quint16 v) { return QByteArray::fromHex("09") + char(v >> 8) + char(v & 0xff); }
QByteArray sdpUuid16(quint16 v) { return QByteArray::fromHex("19") + char(v >> 8) + char(v & 0xff); }
QByteArray sdpUuid128(const char *hex) { return QByteArray::fromHex("1c") + QByteArray::fromHex(hex); }
QByteArray sdpText(const QByteArray &s) { return QByteArray::fromHex("25") + char(s.size()) + s; }

QByteArray sdpRecord(const QByteArray &serviceClass, int channel, const QByteArray &name)
{
    QByteArray attributes = sdpUInt16(0x0001) + sdpSeq(serviceClass);
    if (channel > 0)
        attributes += sdpUInt16(0x0004)
                      + sdpSeq(sdpSeq(sdpUuid16(0x0100))
                               + sdpSeq(sdpUuid16(0x0003) + sdpUInt8(quint8(channel))));
    attributes += sdpUInt16(0x0100) + sdpText(name);
    return sdpSeq(attributes);
}

// The outer list with a two-byte length, as longer replies use.
QByteArray sdpLists(const QByteArray &records)
{
    return QByteArray::fromHex("36") + char(records.size() >> 8) + char(records.size() & 0xff)
           + records;
}

void testSdpChannel()
{
    const QByteArray iap2 = sdpUuid128("00000000decafadedecadeafdecacaff");
    const QByteArray serialPort = sdpUuid16(0x1101);

    // Issue #3: PT-P710BT with iAP2 on channel 1 and the serial port on 2.
    QList<SdpService> services = parseSdpAttributeLists(
        sdpLists(sdpRecord(iap2, 1, "iAP2") + sdpRecord(serialPort, 2, "SPP SERVER")));
    check(services.size() == 2, "SDP: both records are decoded");
    check(services.value(0).isIap2() && services.value(0).channel == 1,
          "SDP: the iAP2 record on channel 1 is recognised");
    check(services.value(1).isSerialPort() && services.value(1).name == QStringLiteral("SPP SERVER"),
          "SDP: the serial port record and its name are read");
    check(pickSerialChannel(services) == 2, "SDP: channel 2 is chosen past iAP2 on channel 1");

    // Older firmware: the serial port alone, on channel 1.
    services = parseSdpAttributeLists(sdpLists(sdpRecord(serialPort, 1, "SPP")));
    check(pickSerialChannel(services) == 1, "SDP: a lone serial port on channel 1 is chosen");

    // The Serial Port class spelled as a full 128-bit UUID, name NUL-terminated.
    services = parseSdpAttributeLists(sdpLists(
        sdpRecord(sdpUuid128("0000110100001000800000805f9b34fb"), 3,
                  QByteArray("Serial\0", 7))));
    check(services.size() == 1 && services.first().isSerialPort(),
          "SDP: a 128-bit Serial Port UUID counts as serial port");
    check(services.value(0).name == QStringLiteral("Serial"), "SDP: trailing NUL is dropped");
    check(pickSerialChannel(services) == 3, "SDP: its channel is chosen");

    // No Serial Port class at all: one RFCOMM service is taken, two are not guessed at.
    const QByteArray other = sdpUuid16(0x1234);
    services = parseSdpAttributeLists(sdpLists(sdpRecord(other, 4, "Printer")
                                               + sdpRecord(other, 0, "no RFCOMM")));
    check(pickSerialChannel(services) == 4, "SDP: the only RFCOMM service is taken");
    services = parseSdpAttributeLists(sdpLists(sdpRecord(other, 4, "A") + sdpRecord(other, 5, "B")));
    check(pickSerialChannel(services) == 0, "SDP: no guess between two unknown services");
    services = parseSdpAttributeLists(sdpLists(sdpRecord(iap2, 1, "iAP2")));
    check(pickSerialChannel(services) == 0, "SDP: iAP2 alone is never chosen");

    // Truncated reply: nothing, and no crash.
    const QByteArray full = sdpLists(sdpRecord(serialPort, 2, "SPP SERVER"));
    check(parseSdpAttributeLists(full.left(full.size() - 3)).isEmpty(),
          "SDP: a truncated reply is rejected");
    check(parseSdpAttributeLists(QByteArray()).isEmpty(), "SDP: an empty reply is rejected");

    // What the reporter read from channel 1, and what a status frame looks like.
    check(looksLikeIap2(QByteArray::fromHex("ff550200ee10ff550200ee10ff550200ee10ff550200ee10")),
          "iAP2 packets on the port are recognised");
    check(looksLikeIap2(QByteArray::fromHex("0200ee10ff550200ee10")),
          "iAP2 packets are recognised mid-packet too");
    QByteArray frame(StatusLength, '\0');
    frame[0] = char(0x80);
    frame[1] = 0x20;
    frame[2] = 'B';
    frame[10] = char(0xff);
    frame[11] = 0x55;
    check(!looksLikeIap2(frame), "a status frame is never taken for iAP2");
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
    testArtwork();
    testStatusDecoding();
    testSdpChannel();

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : qPrintable(QStringLiteral("%1 check(s) failed").arg(failures)));
    return failures == 0 ? 0 : 1;
}
