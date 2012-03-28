/*

   AndServer, part of Android™ Virtual Input for Linux project

   Copyright 2012 Piotr Zintel
   zintelpiotr@gmail.com

   Android is a trademark of Google Inc.

*/

/*

   AndServer is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   any later version.

   AndServer is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "KeyboardClientHandler.h"

KeyboardClientHandler::KeyboardClientHandler(const int connectionSocketArg, class Logger *loggerArg, char* keyboardSemName, char* sslCertificateFileArg, char* sslPrivateKeyFileArg, char* keyboardFilePathArg) {
	sslCertificateFile = strcpy(new char[strlen(sslCertificateFileArg) + 1],sslCertificateFileArg);
	sslPrivateKeyFile = strcpy(new char[strlen(sslPrivateKeyFileArg) + 1],sslPrivateKeyFileArg);
	keyboardFilePath = strcpy(new char[strlen(keyboardFilePathArg) + 1],keyboardFilePathArg);
	connectionSocket = connectionSocketArg;
	receivedEndSignal = false;
	receivedEndMessage = false;
	logger = loggerArg;
	ssl_method = NULL;
	ssl_ctx = NULL;	
	ssl = NULL;
	keyboardHandler = NULL;
	if ((keyboardSem = sem_open(keyboardSemName,0,O_RDWR,0)) == SEM_FAILED) {
		logger->error("sem_open failed",errno);
	}
}

KeyboardClientHandler::~KeyboardClientHandler() {
	if (keyboardHandler) {
		keyboardHandler->closeKeyboard();
		delete keyboardHandler;
	}
	sem_close(keyboardSem);
	if (ssl) SSL_shutdown(ssl);
	if (ssl) SSL_free(ssl);
	if (ssl_ctx) SSL_CTX_free(ssl_ctx);
	if (sslCertificateFile) delete[] sslCertificateFile;
	if (sslPrivateKeyFile) delete[] sslPrivateKeyFile;
	if (keyboardFilePath) delete[] keyboardFilePath;
	close(connectionSocket);
}

bool KeyboardClientHandler::handleClient() {

	int *optval = new int(1);
	if (setsockopt(connectionSocket,SOL_SOCKET,SO_KEEPALIVE,optval, sizeof(int)) == -1) {
		char tmp[128];
		sprintf(tmp,"04.03.2012 03:28:41 setsockopt() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	if (setsockopt(connectionSocket,SOL_TCP,TCP_KEEPCNT,optval, sizeof(int)) == -1) {
		char tmp[128];
		sprintf(tmp,"04.03.2012 03:28:49 setsockopt() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}

	*optval = 60;
	if (setsockopt(connectionSocket,SOL_TCP,TCP_KEEPIDLE,optval, sizeof(int)) == -1) {
		char tmp[128];
		sprintf(tmp,"04.03.2012 03:28:56 setsockopt() error: (%d) %s",errno, strerror(errno));
		logger->error(tmp);
		delete optval;
		return false;
	}
	delete optval;

	if (!sslInit()) {
		logger->error("15.12.2011 13:16:40 sslInit() error");
		return false;
	}

	if ( !readyKeyboardHandler() ) {
		logger->error("11.12.2011 22:49:58 Could not ready KeyboardHandler");
		return false;
	}
	char buffer[sizeof(char)+sizeof(int)];
	unsigned char msgType;
	int msgLength;
	struct pollfd pollSock;

	pollSock.fd = connectionSocket;
	pollSock.events = (0 | POLLIN | POLLRDHUP); // there is input data or connection closed
	pollSock.revents = 0;

	if (!sendReady()) {
		logger->error("21.12.2011 14:29:30 sendReady() error");
		return true;
	}

	int errorCounter = 0;
	while (1) {
		if (receivedEndSignal) break;
		if (receivedEndMessage) break;

		int ret = poll(&pollSock,1,900000);
		if (ret == 0) {
			logger->error("16.12.2011 00:53:01 Client timeout");
			break;
		} else if (ret == -1) {
			if (errno != EINTR) {
				char tmp[128];
				sprintf(tmp,"16.12.2011 00:54:52 poll() error: (%d) %s",errno,strerror(errno));
				logger->error(tmp);
			}
			break;
		}
		if ( (pollSock.revents & POLLRDHUP) == POLLRDHUP ) {
			logger->printMessage("10.12.2011 23:59:58 Keyboard disconnected");
			break;
		}

		int readBytes = 0;
		int retVal = 0;
		while ( readBytes != sizeof(char)+sizeof(int)) {
			if (receivedEndSignal) {
				return true;
			}
			if ( (retVal = SSL_read(ssl, buffer+readBytes, 5)) == -1) {
				logger->error("21.12.2011 14:13:09 read() error");
				return true;
			}
			readBytes += retVal;
		}
		memcpy(&msgType, buffer, sizeof(char));
		memcpy(&msgLength, buffer+sizeof(char), sizeof(int));
		msgLength = ntohl(msgLength);

		char message[msgLength];
		readBytes = 0;
		retVal = 0;
		while (readBytes != msgLength) {
			if (receivedEndSignal) {
				return true;
			}
			if ( (retVal = SSL_read(ssl, message+readBytes, msgLength-readBytes )) == -1 ) {
				logger->error("21.12.2011 14:14:08 read() error");
				return true;
			}
			readBytes += retVal;
		}
		if (readBytes != msgLength) {
			char tmp[128];
			sprintf(tmp,"21.12.2011 14:14:16 could not read message (read %d bytes of expected %d). Client timeout.",readBytes,msgLength);
			logger->error(tmp);
			break;
		}

		switch (msgType) {
			case KBD_TEXT:
				sem_wait(keyboardSem);
				receiveKbdText(msgLength, message);
				sem_post(keyboardSem);
			break;

			case KBD_SPECIAL:
				sem_wait(keyboardSem);
				receiveKbdSpecial(msgLength, message);
				sem_post(keyboardSem);
			break;

			case MSG_POLL:
			break;

			default:
				char tmp[128];
				sprintf(tmp,"unknown message type: %u",(unsigned int)msgType);
				logger->error(tmp);
				if (++errorCounter >= 10) break;
			break;
		}
	}
	return true;
}

bool KeyboardClientHandler::sslInit() {
	int ret = 0;
	OpenSSL_add_all_algorithms();
	SSL_library_init() ;
	SSL_load_error_strings();

	if ( (ssl_method = TLSv1_server_method()) == NULL ) {
		errError("_server_method() error");
		return false;
	}

	if((ssl_ctx = SSL_CTX_new(ssl_method)) == NULL) {
		errError("16.12.2011 22:15:09 SSL_CTX_new() error");
		return false;
	}

	if(SSL_CTX_use_certificate_file(ssl_ctx,sslCertificateFile,SSL_FILETYPE_PEM) != 1) {
		errError("17.12.2011 00:12:29 SSL_CTX_use_certificate_file() error");
		return false;
	}

	if(SSL_CTX_use_PrivateKey_file(ssl_ctx,sslPrivateKeyFile,SSL_FILETYPE_PEM) != 1) {
		errError("17.12.2011 00:12:33 SSL_CTX_use_PrivateKey_file() error");
		return false;
	}

	if ((ssl = SSL_new(ssl_ctx)) == NULL ) {
		errError("16.12.2011 22:15:15 SSL_new() error");
		return false;
	}

	if(!SSL_CTX_check_private_key(ssl_ctx)) {
		errError("16.12.2011 22:15:22 SSL_CTX_check_private_key() error");
		return false;
	}

	if(SSL_set_fd(ssl, connectionSocket) != 1) {
		errError("17.12.2011 01:57:52 SSL_set_fd() error");
		return false;
	}

	if ((ret = SSL_accept(ssl)) <= 0) {
		int savedErrno = errno;
		errError("16.12.2011 22:15:33 SSL_accept() error");
		sslError(ret,savedErrno);
		return false;
	}
	return true;
}

bool KeyboardClientHandler::sendReady() {
	unsigned char *buffer = new unsigned char[1];
	unsigned char a = MSG_READY;
	memcpy(buffer,&a,sizeof(char));
	if ( SSL_write(ssl, buffer, sizeof(char)) != sizeof(char) ) {
		errError("16.12.2011 22:16:40 write() error");
		delete[] buffer;
		return false;
	}
	delete[] buffer;
	return true;
}

bool KeyboardClientHandler::receiveKbdText(const int msgLength, const char* message) {
	for (int i=0; i<msgLength; ++i) {
		keyboardHandler->sendKey(message[i]);
	}
	return true;
}

bool KeyboardClientHandler::receiveKbdSpecial(const int msgLength, const char* message) {
	char key;
	int value;
	for (int i=0; i<msgLength; ++i) {
		if (message[i] < 64) {
			value = 1;
			key = message[i];
		} else {
			value = 0;
			key = message[i] - 64;
		}

		switch (key) {
			case KEY_LCTRL:
				keyboardHandler->sendKeystroke(AVKBD_KEY_LEFTCTRL, value);
			break;

			case KEY_RCTRL:
				keyboardHandler->sendKeystroke(AVKBD_KEY_RIGHTCTRL, value);
			break;

			case KEY_LALT:
				keyboardHandler->sendKeystroke(AVKBD_KEY_LEFTALT, value);
			break;

			case KEY_RALT:
				keyboardHandler->sendKeystroke(AVKBD_KEY_RIGHTALT, value);
			break;

			case KEY_LSHIFT:
				keyboardHandler->sendKeystroke(AVKBD_KEY_LEFTSHIFT, value);
			break;

			case KEY_RSHIFT:
				keyboardHandler->sendKeystroke(AVKBD_KEY_RIGHTSHIFT, value);
			break;

			case KEY_F1:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F1, value);
			break;

			case KEY_F2:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F2, value);
			break;

			case KEY_F3:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F3, value);
			break;

			case KEY_F4:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F4, value);
			break;

			case KEY_F5:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F5, value);
			break;

			case KEY_F6:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F6, value);
			break;

			case KEY_F7:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F7, value);
			break;

			case KEY_F8:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F8, value);
			break;

			case KEY_F9:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F9, value);
			break;

			case KEY_F10:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F10, value);
			break;

			case KEY_F11:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F11, value);
			break;

			case KEY_F12:
				keyboardHandler->sendKeystroke(AVKBD_KEY_F12, value);
			break;

			case KEY_ESC:
				keyboardHandler->sendKeystroke(AVKBD_KEY_ESC, value);
			break;

			case KEY_BACKSPACE:
				keyboardHandler->sendKeystroke(AVKBD_KEY_BACKSPACE, value);
			break;

			case KEY_PAGEUP:
				keyboardHandler->sendKeystroke(AVKBD_KEY_PAGEUP, value);
			break;

			case KEY_PAGEDOWN:
				keyboardHandler->sendKeystroke(AVKBD_KEY_PAGEDOWN, value);
			break;

			case KEY_ENTER:
				keyboardHandler->sendKeystroke(AVKBD_KEY_ENTER, value);
			break;

			case KEY_TAB:
				keyboardHandler->sendKeystroke(AVKBD_KEY_TAB, value);
			break;

			case KEY_UP:
				keyboardHandler->sendKeystroke(AVKBD_KEY_UP, value);
			break;

			case KEY_DOWN:
				keyboardHandler->sendKeystroke(AVKBD_KEY_DOWN, value);
			break;

			case KEY_LEFT:
				keyboardHandler->sendKeystroke(AVKBD_KEY_LEFT, value);
			break;

			case KEY_RIGHT:
				keyboardHandler->sendKeystroke(AVKBD_KEY_RIGHT, value);
			break;

			case KEY_PRINTSCRN:
				keyboardHandler->sendKeystroke(AVKBD_KEY_PRINTSCRN, value);
			break;

			case KEY_KP1:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP1, value);
			break;

			case KEY_KP2:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP2, value);
			break;

			case KEY_KP3:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP3, value);
			break;

			case KEY_KP4:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP4, value);
			break;

			case KEY_KP5:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP5, value);
			break;

			case KEY_KP6:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP6, value);
			break;

			case KEY_KP7:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP7, value);
			break;

			case KEY_KP8:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP8, value);
			break;

			case KEY_KP9:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP9, value);
			break;

			case KEY_KP0:
				keyboardHandler->sendKeystroke(AVKBD_KEY_KP0, value);
			break;

			case KEY_VOLUMEUP:
				keyboardHandler->sendKeystroke(AVKBD_KEY_VOLUMEUP, value);
			break;

			case KEY_VOLUMEDOWN:
				keyboardHandler->sendKeystroke(AVKBD_KEY_VOLUMEDOWN, value);
			break;

			case KEY_MUTE:
				keyboardHandler->sendKeystroke(AVKBD_KEY_MUTE, value);
			break;

			case KEY_PLAYPAUSE:
				keyboardHandler->sendKeystroke(AVKBD_KEY_PLAYPAUSE, value);
			break;

			case KEY_PREVIOUSSONG:
				keyboardHandler->sendKeystroke(AVKBD_KEY_PREVIOUSSONG, value);
			break;

			case KEY_NEXTSONG:
				keyboardHandler->sendKeystroke(AVKBD_KEY_NEXTSONG, value);
			break;

			case KEY_HOME:
				keyboardHandler->sendKeystroke(AVKBD_KEY_HOME, value);
			break;

			case KEY_END:
				keyboardHandler->sendKeystroke(AVKBD_KEY_END, value);
			break;

			case KEY_META:
				keyboardHandler->sendKeystroke(AVKBD_KEY_LEFTMETA, value);
			break;

			case KEY_DELETE:
				keyboardHandler->sendKeystroke(AVKBD_KEY_DELETE, value);
			break;
		}
	}
	return true;
}

void KeyboardClientHandler::errError(const char* msg) {
	unsigned long e;
	char errBuffer[128];
	logger->error(msg);
	while ( (e = ERR_get_error()) != 0 ) {
		sprintf(errBuffer,"16.12.2011 00:33:38 %lu: %s",e,ERR_error_string(e,NULL));
		logger->error(errBuffer);
	}
}

void KeyboardClientHandler::sslError(int ret, int savedErrno) {
	unsigned long e;
	char errBuffer[256];
	e = SSL_get_error(ssl,ret);
	sprintf(errBuffer,"%d;%lu: %s",ret,e,ERR_error_string(e,NULL) );
	if ((e == SSL_ERROR_SYSCALL) && (errno != 0)) {
		sprintf(errBuffer,"%s, errno: (%d) %s",errBuffer,savedErrno, strerror(savedErrno) );
	}
	logger->error(errBuffer);
}

bool KeyboardClientHandler::readyKeyboardHandler(){

	if (!(keyboardHandler = new KeyboardHandler(keyboardFilePath,logger)) ) {
		logger->error("06.12.2011 01:34:40 error creating keyboardHandler");
		logger->error("readyHandlers() failed");
		return false;
	}

	if (!keyboardHandler->openKeyboard()) {
		logger->error("07.12.2011 15:29:53 error opening keyboard");
		logger->error("readyHandlers() failed");
		return false;
	}

	return true;
}

