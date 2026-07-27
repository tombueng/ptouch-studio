// True-to-scale preview, including the tape edges the print head cannot reach.
#pragma once

#include <QWidget>

#include "Engine.h"

namespace ptouch {

class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget *parent = nullptr);

    void setLabel(const Spec &spec, const Layout &layout);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Spec m_spec;
    Layout m_layout;
    bool m_valid = false;
};

} // namespace ptouch
