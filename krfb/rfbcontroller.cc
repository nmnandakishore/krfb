/***************************************************************************
                              rfbcontroller.cpp
                             -------------------
    begin                : Sun Dec 9 2001
    copyright            : (C) 2001-2002 by Tim Jansen
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

#include "rfbcontroller.h"

#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>

#include <kapp.h>
#include <kdebug.h>
#include <kmessagebox.h>
#include <klocale.h>
#include <kextsock.h>
#include <qcursor.h>
#include <qwindowdefs.h>
#include <qtimer.h>
#include <qcheckbox.h>
#include <qpushbutton.h>
#include <qglobal.h>
#include <qlabel.h>
#include <qmutex.h>

#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>

#ifndef ASSERT
#define ASSERT(x) Q_ASSERT(x)
#endif

#define IDLE_PAUSE (1000/50)

static XTestDisabler disabler;

// only one controller exists, so we can do this workaround for functions:
static RFBController *self;

class AppLocker
{
public:
	AppLocker() {
		KApplication::kApplication()->lock();
	}
	
	~AppLocker() {
		KApplication::kApplication()->unlock();
	}
};

static enum rfbNewClientAction newClientHook(struct _rfbClientRec *cl) 
{
	return self->handleNewClient(cl);
}

static Bool passwordCheck(rfbClientPtr cl, 
			  char* encryptedPassword,
			  int len)
{
	AppLocker a;
	self->handleCheckPassword(encryptedPassword, len);
}

static void keyboardHook(Bool down, KeySym keySym, rfbClientPtr)
{
	// todo!
	AppLocker a;
	self->handleKeyEvent(down?true:false, keySym);
}

static void pointerHook(int bm, int x, int y, rfbClientPtr)
{
	// todo!
	AppLocker a;
	self->handlePointerEvent(bm, x, y);
}

static void clientGoneHook(rfbClientPtr cl) 
{
	// todo!
	AppLocker a;
	self->handleClientGone();
}


void ConnectionDialog::closeEvent(QCloseEvent *) 
{
	emit closed();
}



RFBController::RFBController(Configuration *c) :
	allowRemoteControl(false),
	connectionNum(0),
	configuration(c)
{
	self = this;
	connect(dialog.acceptConnectionButton, SIGNAL(clicked()),
		SLOT(dialogAccepted()));
	connect(dialog.refuseConnectionButton, SIGNAL(clicked()),
		SLOT(dialogRefused()));
	connect(&dialog, SIGNAL(closed()), SLOT(dialogRefused()));
	connect(&idleTimer, SIGNAL(timeout()), SLOT(idleSlot()));

	startServer();
}

RFBController::~RFBController() 
{
	stopServer();
}



void RFBController::startServer(bool xtestGrab) 
{
	framebufferImage = XGetImage(qt_xdisplay(),
				     QApplication::desktop()->winId(),
				     0,
				     0,
				     QApplication::desktop()->width(),
				     QApplication::desktop()->height(),
				     AllPlanes,
				     ZPixmap);

	int w = framebufferImage->width;
	int h = framebufferImage->height;
	char *fb = framebufferImage->data;

	server = rfbGetScreen(0, 0, w, h,
			      framebufferImage->bits_per_pixel,
			      8,
			      framebufferImage->bits_per_pixel/8);
   
	server->paddedWidthInBytes = framebufferImage->bytes_per_line;
	
	server->rfbServerFormat.bitsPerPixel = framebufferImage->bits_per_pixel;
	server->rfbServerFormat.depth = framebufferImage->depth;
	//rfbEndianTest = framebufferImage->bitmap_bit_order != MSBFirst;
	server->rfbServerFormat.trueColour = TRUE;
	
	if ( server->rfbServerFormat.bitsPerPixel == 8 ) {
		server->rfbServerFormat.redShift = 0;
		server->rfbServerFormat.greenShift = 2;
		server->rfbServerFormat.blueShift = 5;
		server->rfbServerFormat.redMax   = 3;
		server->rfbServerFormat.greenMax = 7;
		server->rfbServerFormat.blueMax  = 3;
	} else {
		server->rfbServerFormat.redShift = 0;
		if ( framebufferImage->red_mask )
			while ( ! ( framebufferImage->red_mask & (1 << server->rfbServerFormat.redShift) ) )
				server->rfbServerFormat.redShift++;
		server->rfbServerFormat.greenShift = 0;
		if ( framebufferImage->green_mask )
			while ( ! ( framebufferImage->green_mask & (1 << server->rfbServerFormat.greenShift) ) )
				server->rfbServerFormat.greenShift++;
		server->rfbServerFormat.blueShift = 0;
		if ( framebufferImage->blue_mask )
			while ( ! ( framebufferImage->blue_mask & (1 << server->rfbServerFormat.blueShift) ) )
				server->rfbServerFormat.blueShift++;
		server->rfbServerFormat.redMax   = framebufferImage->red_mask   >> server->rfbServerFormat.redShift;
		server->rfbServerFormat.greenMax = framebufferImage->green_mask >> server->rfbServerFormat.greenShift;
		server->rfbServerFormat.blueMax  = framebufferImage->blue_mask  >> server->rfbServerFormat.blueShift;
	}

	server->frameBuffer = fb;
	server->rfbPort = configuration->port();
	//server->udpPort = configuration->port();
	
	server->kbdAddEvent = keyboardHook;
	server->ptrAddEvent = pointerHook;
	server->newClientHook = newClientHook;
	server->passwordCheck = passwordCheck;

	scanner = new XUpdateScanner(qt_xdisplay(), 
				     QApplication::desktop()->winId(), 
				     (unsigned char*)fb, w, h, 
				     server->rfbServerFormat.bitsPerPixel,
				     server->paddedWidthInBytes);

	rfbInitServer(server);
	state = RFB_WAITING;

	if (xtestGrab) {
		disabler.disable = false;
		XTestGrabControl(qt_xdisplay(), true); 
	}

	rfbRunEventLoop(server, -1, TRUE);
}

void RFBController::stopServer(bool xtestUngrab) 
{
	rfbScreenCleanup(server);
	state = RFB_STOPPED;
	delete scanner;

	XDestroyImage(framebufferImage);

	if (xtestUngrab) {
		disabler.disable = true;
		QTimer::singleShot(0, &disabler, SLOT(exec()));
	}
}

void RFBController::rebind() 
{
	stopServer(false);
	startServer(false);
}

void RFBController::connectionAccepted(bool aRC)
{
	if (state != RFB_CONNECTING)
		return;

	allowRemoteControl = aRC;
	connectionNum++;
	idleTimer.start(IDLE_PAUSE);

	client->clientGoneHook = clientGoneHook;
	state = RFB_CONNECTED;
	emit sessionEstablished();
}

void RFBController::acceptConnection(bool aRC) 
{
	if (state != RFB_CONNECTING)
		return;

	connectionAccepted(aRC);
	rfbStartOnHoldClient(client);
}

void RFBController::refuseConnection() 
{
	if (state != RFB_CONNECTING)
		return;
	rfbRefuseOnHoldClient(client);
	state = RFB_WAITING;
}

void RFBController::connectionClosed() 
{
	idleTimer.stop();
	connectionNum--;
	state = RFB_WAITING;
	client = 0;
	emit sessionFinished();
}

void RFBController::closeConnection() 
{
	if (state == RFB_CONNECTED) {
		rfbCloseClient(client);
		connectionClosed();
	}
	else if (state == RFB_CONNECTING)
		refuseConnection();
}

void RFBController::idleSlot() 
{
	if (state != RFB_CONNECTED)
		return;

	rfbUndrawCursor(server);

	QList<Hint> v;
	v.setAutoDelete(true);
	scanner->searchUpdates(v);

	Hint *h;

	for (h = v.first(); h != 0; h = v.next()) {
		rfbMarkRectAsModified(server, h->left(),
				      h->top(),
				      h->right(),
				      h->bottom());
	}
	QPoint p = QCursor::pos();
	defaultPtrAddEvent(0, p.x(),p.y(), client);
}

void RFBController::dialogAccepted() 
{
	dialog.hide();
	acceptConnection(dialog.allowRemoteControlCB->isChecked());
}

void RFBController::dialogRefused() 
{
	refuseConnection();
	dialog.hide();
	emit sessionRefused();
}

bool RFBController::handleCheckPassword(const char *, int) 
{
	return TRUE;
	// TODO
}

enum rfbNewClientAction RFBController::handleNewClient(rfbClientPtr cl) 
{
	int socket = cl->sock;

	if ((connectionNum > 0) ||
	    (state != RFB_WAITING)) 
		return RFB_CLIENT_REFUSE;

	client = cl;
	state = RFB_CONNECTING;

	if (!configuration->askOnConnect()) {
		connectionAccepted(configuration->allowDesktopControl());
		return RFB_CLIENT_ACCEPT;
	}

	QString host, port;
	KExtendedSocket::resolve(KExtendedSocket::peerAddress(socket),
				 host, port);
	dialog.ipLabel->setText(host);
	dialog.allowRemoteControlCB->setChecked(configuration->allowDesktopControl());
	dialog.setFixedSize(dialog.sizeHint());
	dialog.show();
	return RFB_CLIENT_ON_HOLD;
}

void RFBController::handleClientGone()
{
	connectionClosed();
}



#define LEFTSHIFT 1
#define RIGHTSHIFT 2
#define ALTGR 4
char ModifierState = 0;

/* this function adjusts the modifiers according to mod (as from modifiers) and ModifierState */

void RFBController::tweakModifiers(char mod, bool down)
{
	Display *dpy = qt_xdisplay();

	bool isShift = ModifierState & (LEFTSHIFT|RIGHTSHIFT);
	if(mod < 0) 
		return;
	
	if(isShift && mod != 1) {
		if(ModifierState & LEFTSHIFT)
			XTestFakeKeyEvent(dpy, leftShiftCode,
					  !down, CurrentTime);
		if(ModifierState & RIGHTSHIFT)
			XTestFakeKeyEvent(dpy, rightShiftCode,
					  !down, CurrentTime);
	}
	
	if(!isShift && mod==1)
		XTestFakeKeyEvent(dpy, leftShiftCode,
				  down, CurrentTime);

	if(ModifierState&ALTGR && mod != 2)
		XTestFakeKeyEvent(dpy, altGrCode,
				  !down, CurrentTime);
	if(!(ModifierState&ALTGR) && mod==2)
		XTestFakeKeyEvent(dpy, altGrCode,
				  down, CurrentTime);
}

void RFBController::initKeycodes()
{
	Display *dpy = qt_xdisplay();
	KeySym key,*keymap;
	int i,j,minkey,maxkey,syms_per_keycode;
	
	memset(modifiers,-1,sizeof(modifiers));
	
	XDisplayKeycodes(dpy,&minkey,&maxkey);
	keymap=XGetKeyboardMapping(dpy,minkey,(maxkey - minkey + 1),&syms_per_keycode);

	for (i = minkey; i <= maxkey; i++)
		for(j=0;j<syms_per_keycode;j++) {
			key=keymap[(i-minkey)*syms_per_keycode+j];
			if(key>=' ' && key<0x100 && i==XKeysymToKeycode(dpy,key)) {
				keycodes[key]=i;
				modifiers[key]=j;
			}
		}
	
	leftShiftCode = XKeysymToKeycode(dpy,XK_Shift_L);
	rightShiftCode = XKeysymToKeycode(dpy,XK_Shift_R);
	altGrCode = XKeysymToKeycode(dpy,XK_Mode_switch);

	XFree ((char *)keymap);
}

void RFBController::handleKeyEvent(bool down, KeySym keySym) {
	if (!allowRemoteControl)
		return;

	Display *dpy = qt_xdisplay();

#define ADJUSTMOD(sym,state) \
  if(keySym==sym) { if(down) ModifierState|=state; else ModifierState&=~state; }
	
	ADJUSTMOD(XK_Shift_L,LEFTSHIFT);
	ADJUSTMOD(XK_Shift_R,RIGHTSHIFT);
	ADJUSTMOD(XK_Mode_switch,ALTGR);

	if(keySym>=' ' && keySym<0x100) {
		KeyCode k;
		if (down)
			tweakModifiers(modifiers[keySym],True);
		//tweakModifiers(modifiers[keySym],down);
		//k = XKeysymToKeycode( dpy,keySym );
		k = keycodes[keySym];
		if(k!=NoSymbol) 
			XTestFakeKeyEvent(dpy,k,down,CurrentTime);

		/*XTestFakeKeyEvent(dpy,keycodes[keySym],down,CurrentTime);*/
		if (down)
			tweakModifiers(modifiers[keySym],False);
	} else {
		KeyCode k = XKeysymToKeycode( dpy,keySym );
		if(k!=NoSymbol)
			XTestFakeKeyEvent(dpy,k,down,CurrentTime);
	}
}

void RFBController::handlePointerEvent(int button_mask, int x, int y) {
	if (!allowRemoteControl)
		return;

	Display *dpy = qt_xdisplay();
  	XTestFakeMotionEvent(dpy, 0, x, y, CurrentTime);
	for(int i = 0; i < 5; i++) 
		if ((buttonMask&(1<<i))!=(button_mask&(1<<i)))
			XTestFakeButtonEvent(dpy,
					     i+1,
					     (button_mask&(1<<i))?True:False,
					     CurrentTime);

	buttonMask = button_mask;
}

bool RFBController::checkX11Capabilities() {
	int bp1, bp2, majorv, minorv;
	Bool r = XTestQueryExtension(qt_xdisplay(), &bp1, &bp2, 
				     &majorv, &minorv);
	if ((!r) || (((majorv*1000)+minorv) < 2002)) {
		KMessageBox::error(0, 
		   i18n("Your X11 Server does not support the required XTest extension version 2.2. Sharing your desktop is not possible."),
				   i18n("Desktop Sharing Error"));
		return false;
	}

	r = XShmQueryExtension(qt_xdisplay());
	if (!r) {
		KMessageBox::error(0, 
		   i18n("Your X11 Server does not support the required XShm extension. You can only share a local desktop."),
				   i18n("Desktop Sharing Error"));
		return false;
	}
	return true;
}


XTestDisabler::XTestDisabler() :
	disable(false) {
}

void XTestDisabler::exec() {
	if (disable)
		XTestDiscard(qt_xdisplay());
}

#include "rfbcontroller.moc"