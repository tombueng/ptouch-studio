#include "PreviewWidget.h"

#include <QPainter>
#include <QPalette>

#include <algorithm>

namespace ptouch {

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(170);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void PreviewWidget::setLabel(const Spec &spec, const Layout &layout)
{
    m_spec = spec;
    m_layout = layout;
    m_valid = true;
    update();
}

void PreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(rect(), palette().window());
    if (!m_valid)
        return;

    const bool dark = palette().window().color().lightness() < 128;
    const double pad = 20;
    const double availableWidth = width() - 2 * pad;
    const double availableHeight = height() - 2 * pad - 20;
    const double scale = std::min({availableWidth / std::max(m_layout.lengthPt, 1.0),
                                   availableHeight / std::max(m_spec.tapePt(), 1.0),
                                   8.0});
    const double w = m_layout.lengthPt * scale;
    const double h = m_spec.tapePt() * scale;
    const double ox = (width() - w) / 2;
    const double oy = (height() - 20 - h) / 2;

    p.save();
    p.translate(ox, oy);

    // A soft shadow lifts the tape off the background
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, dark ? 90 : 40));
    p.drawRoundedRect(QRectF(2, 3, w, h), 2, 2);

    p.setBrush(QColor(252, 252, 250));
    p.drawRect(QRectF(0, 0, w, h));

    // Zones the print head cannot reach
    const double edge = (m_spec.tapePt() - m_spec.printablePt()) / 2 * scale;
    p.setBrush(QColor(224, 231, 238));
    p.drawRect(QRectF(0, 0, w, edge));
    p.drawRect(QRectF(0, h - edge, w, edge));

    p.setPen(QPen(QColor(150, 158, 165)));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(0, 0, w, h));

    p.save();
    p.scale(scale, scale);
    if (m_spec.mirror) {
        p.translate(m_layout.lengthPt, 0);
        p.scale(-1, 1);
    }
    // 300 dpi is plenty for the preview and matches what the printer resolves.
    ptouch::paintLabel(p, m_spec, m_layout, 300);
    p.restore();
    p.restore();

    p.setPen(palette().text().color());
    QFont info = font();
    info.setPointSizeF(std::max(8.0, info.pointSizeF() - 1));
    p.setFont(info);
    p.drawText(QRectF(0, height() - 22, width(), 20), Qt::AlignCenter,
               QStringLiteral("%1 × %2 mm · font %3 pt · printable %4 mm")
                   .arg(m_layout.lengthPt / PtPerMm, 0, 'f', 0)
                   .arg(m_spec.tapeMm, 0, 'g', 3)
                   .arg(m_layout.sizePt, 0, 'f', 1)
                   .arg(m_spec.printablePt() / PtPerMm, 0, 'f', 1));
}

} // namespace ptouch
