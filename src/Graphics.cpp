#include "Graphics.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPainter>

#ifdef PTOUCH_HAVE_QRENCODE
#include <qrencode.h>
#endif

namespace ptouch {
namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("Graphics", text);
}

// Code 128 symbol patterns, one entry per value 0…106. Each is six pairs of
// bar/space widths, which is how the standard defines the symbology.
const char *const Code128Patterns[] = {
    "212222","222122","222221","121223","121322","131222","122213","122312","132212","221213",
    "221312","231212","112232","122132","122231","113222","123122","123221","223211","221132",
    "221231","213212","223112","312131","311222","321122","321221","312212","322112","322211",
    "212123","212321","232121","111323","131123","131321","112313","132113","132311","211313",
    "231113","231311","112133","112331","132131","113123","113321","133121","313121","211331",
    "231131","213113","213311","213131","311123","311321","331121","312113","312311","332111",
    "314111","221411","431111","111224","111422","121124","121421","141122","141221","112214",
    "112412","122114","122411","142112","142211","241211","221114","413111","241112","134111",
    "111242","121142","121241","114212","124112","124211","411212","421112","421211","212141",
    "214121","412121","111143","111341","131141","114113","114311","411113","411311","113141",
    "114131","311141","411131","211412","211214","211232","2331112",
};

// Code 128 set B covers printable ASCII, which is what labels use.
QImage renderCode128(const QString &data, QString *error)
{
    QList<int> values;
    values << 104;                       // start code B
    int checksum = 104;
    for (int i = 0; i < data.size(); ++i) {
        const ushort c = data.at(i).unicode();
        if (c < 32 || c > 126) {
            if (error)
                *error = tr("Code 128 takes plain ASCII only");
            return {};
        }
        const int value = c - 32;
        values << value;
        checksum += value * (i + 1);
    }
    values << checksum % 103;            // check digit
    values << 106;                       // stop pattern

    // Sum the module widths to size the image.
    int modules = 0;
    for (const int value : std::as_const(values)) {
        for (const char *p = Code128Patterns[value]; *p; ++p)
            modules += *p - '0';
    }
    if (modules <= 0)
        return {};

    // Two modules of quiet zone are the minimum the standard asks for; ten is
    // what scanners actually like.
    constexpr int Quiet = 10;
    QImage image(modules + 2 * Quiet, 1, QImage::Format_Grayscale8);
    image.fill(0xff);

    uchar *line = image.scanLine(0);
    int x = Quiet;
    for (const int value : std::as_const(values)) {
        bool bar = true;                 // patterns always start with a bar
        for (const char *p = Code128Patterns[value]; *p; ++p) {
            const int width = *p - '0';
            if (bar)
                for (int i = 0; i < width; ++i)
                    line[x + i] = 0x00;
            x += width;
            bar = !bar;
        }
    }
    return image;
}

QImage renderQr(const QString &data, QString *error)
{
#ifdef PTOUCH_HAVE_QRENCODE
    const QByteArray payload = data.toUtf8();
    QRcode *code = QRcode_encodeString(payload.constData(), 0, QR_ECLEVEL_M,
                                       QR_MODE_8, 1);
    if (!code) {
        if (error)
            *error = tr("cannot encode this text as a QR code");
        return {};
    }

    constexpr int Quiet = 4;             // the standard asks for four modules
    const int size = code->width + 2 * Quiet;
    QImage image(size, size, QImage::Format_Grayscale8);
    image.fill(0xff);
    for (int y = 0; y < code->width; ++y) {
        uchar *line = image.scanLine(y + Quiet);
        for (int x = 0; x < code->width; ++x) {
            if (code->data[y * code->width + x] & 1)
                line[x + Quiet] = 0x00;
        }
    }
    QRcode_free(code);
    return image;
#else
    Q_UNUSED(data)
    if (error)
        *error = tr("this build has no QR support (libqrencode was missing)");
    return {};
#endif
}

} // namespace

bool qrAvailable()
{
#ifdef PTOUCH_HAVE_QRENCODE
    return true;
#else
    return false;
#endif
}

QImage renderCode(CodeType type, const QString &data, QString *error)
{
    if (data.isEmpty() || type == CodeType::None)
        return {};
    switch (type) {
    case CodeType::Code128: return renderCode128(data, error);
    case CodeType::Qr:      return renderQr(data, error);
    default:                return {};
    }
}

QImage loadPicture(const QString &path, QString *error)
{
    if (path.isEmpty())
        return {};
    QImage image(path);
    if (image.isNull()) {
        if (error)
            *error = tr("cannot read %1").arg(QFileInfo(path).fileName());
        return {};
    }

    // Flatten transparency onto white first: a logo saved with an alpha channel
    // would otherwise turn into a solid black block on the tape.
    if (image.hasAlphaChannel()) {
        QImage flattened(image.size(), QImage::Format_RGB32);
        flattened.fill(Qt::white);
        QPainter p(&flattened);
        p.drawImage(0, 0, image);
        p.end();
        image = flattened;
    }
    return image.convertToFormat(QImage::Format_Grayscale8);
}

} // namespace ptouch
