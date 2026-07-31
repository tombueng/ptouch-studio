#include "Cli.h"
#include "MainWindow.h"
#include "Version.h"

#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QStringList>
#include <QTranslator>

int main(int argc, char *argv[])
{
    // Sub-commands need no interface, but they do need font metrics — hence
    // QApplication on the offscreen platform rather than QCoreApplication.
    const bool headless = argc > 1 && argv[1][0] != '-';
    if (headless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral(PTOUCH_APP_NAME));
    QApplication::setApplicationVersion(QStringLiteral(PTOUCH_VERSION));
    QApplication::setOrganizationName(QStringLiteral("ptouch-studio"));
    QApplication::setDesktopFileName(QStringLiteral(PTOUCH_APP_ID));

    // Follow the system language; English is the source and needs no catalogue.
    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    QTranslator translator;
    if (translator.load(QLocale(), QStringLiteral("ptouch-studio"), QStringLiteral("_"),
                        QStringLiteral(":/i18n")))
        QApplication::installTranslator(&translator);

    const QStringList arguments = QApplication::arguments();
    if (arguments.size() > 1)
        return ptouch::runCli(arguments);

    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(PTOUCH_APP_ID)));
    ptouch::MainWindow window;
    window.show();
    return QApplication::exec();
}
