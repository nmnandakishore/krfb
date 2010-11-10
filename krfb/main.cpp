/***************************************************************************
                                   main.cpp
                             -------------------
    begin                : Sat Dec  8 03:23:02 CET 2001
    copyright            : (C) 2001-2003 by Tim Jansen
    email                : tim@tjansen.de
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "manageinvitationsdialog.h"
#include "trayicon.h"
#include "invitationsrfbserver.h"

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KAction>
#include <KApplication>
#include <KCmdLineArgs>
#include <KDebug>
#include <KLocale>
#include <KMessageBox>
#include <KNotification>

#include <QtGui/QPixmap>
#include <QtGui/qwindowdefs.h>

#ifdef KRFB_WITH_TELEPATHY_TUBES
# include "tubesclienthandler.h"
# include <TelepathyQt4/ClientRegistrar>
#endif

#include <signal.h>
#include <X11/extensions/XTest.h>

#define VERSION "1.0"

static const char description[] = I18N_NOOP("VNC-compatible server to share "
                                  "KDE desktops");

static bool checkX11Capabilities()
{
    int bp1, bp2, majorv, minorv;
    Bool r = XTestQueryExtension(QX11Info::display(), &bp1, &bp2,
                                 &majorv, &minorv);

    if ((!r) || (((majorv * 1000) + minorv) < 2002)) {
        KMessageBox::error(0,
                           i18n("Your X11 Server does not support the required XTest extension "
                                "version 2.2. Sharing your desktop is not possible."),
                           i18n("Desktop Sharing Error"));
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    KAboutData aboutData("krfb", 0, ki18n("Desktop Sharing"),
                         VERSION, ki18n(description), KAboutData::License_GPL,
                         ki18n("(c) 2007, Alessandro Praduroux\n"
                               "(c) 2001-2003, Tim Jansen\n"
                               "(c) 2001, Johannes E. Schindelin\n"
                               "(c) 2000, heXoNet Support GmbH, D-66424 Homburg\n"
                               "(c) 2000-2001, Const Kaplinsky\n"
                               "(c) 2000, Tridia Corporation\n"
                               "(c) 1999, AT&T Laboratories Boston\n"));
    aboutData.addAuthor(ki18n("Alessandro Praduroux"), ki18n("KDE4 porting"), "pradu@pradu.it");
    aboutData.addAuthor(ki18n("Tim Jansen"), KLocalizedString(), "tim@tjansen.de");
    aboutData.addAuthor(ki18n("Ian Reinhart Geiser"), ki18n("DCOP interface"), "geiseri@kde.org");
    aboutData.addCredit(ki18n("Johannes E. Schindelin"),
                        ki18n("libvncserver"));
    aboutData.addCredit(ki18n("Const Kaplinsky"),
                        ki18n("TightVNC encoder"));
    aboutData.addCredit(ki18n("Tridia Corporation"),
                        ki18n("ZLib encoder"));
    aboutData.addCredit(ki18n("AT&T Laboratories Boston"),
                        ki18n("original VNC encoders and "
                              "protocol design"));
    aboutData.addCredit(ki18n("Jens Wagner (heXoNet Support GmbH)"),
                        ki18n("X11 update scanner, "
                              "original code base"));
    aboutData.addCredit(ki18n("Jason Spisak"),
                        ki18n("Connection side image"),
                        "kovalid@yahoo.com");
    aboutData.addCredit(ki18n("Karl Vogel"),
                        ki18n("KDesktop background deactivation"));
    KCmdLineArgs::init(argc, argv, &aboutData);

    KCmdLineOptions options;
    options.add("nodialog", ki18n("Do not show the invitations management dialog at startup"));
    KCmdLineArgs::addCmdLineOptions(options);

    KApplication app;
    app.setQuitOnLastWindowClosed(false);

    if (!checkX11Capabilities()) {
        return 1;
    }

    //init the core
    InvitationsRfbServer::init();

#ifdef KRFB_WITH_TELEPATHY_TUBES
    Tp::ClientRegistrarPtr clientRegistrar = Tp::ClientRegistrar::create();
    clientRegistrar->registerClient(Tp::AbstractClientPtr(new TubesClientHandler),
                                    "krfb_rfb_handler");
#endif

    //init the GUI
    ManageInvitationsDialog invitationsDialog;
    TrayIcon trayicon(&invitationsDialog);

    if (KCmdLineArgs::parsedArgs()->isSet("dialog")) {
        invitationsDialog.show();
    }

    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGPIPE);
    sigprocmask(SIG_BLOCK, &sigs, 0);

    int ret = app.exec();

    //cleanup
    InvitationsRfbServer::cleanup();
}

