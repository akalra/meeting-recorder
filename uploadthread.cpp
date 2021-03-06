/*
  Copyright (c) 2015-2016 University of Helsinki

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <QDebug>
#include <QDir>

#include "uploadthread.h"

extern "C" {
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
}

// ---------------------------------------------------------------------

UploadThread::UploadThread(QString _dir) : directory(_dir)
{
  buffersize = 1024*100;

  server_ip = "";
  server_path = "";
  username = "";
  password = "";
}

// ---------------------------------------------------------------------

// Q_DECL_OVERRIDE produces an error on OS X 10.11 / Qt 5.6: 
void UploadThread::run() Q_DECL_OVERRIDE {

  emit uploadMessage("uploadthread starting");

  if (username == "" || username.startsWith("MISSING")) {
    emit uploadMessage("username not set, exiting");
    return;
  }
  if (server_ip == "" || server_ip.startsWith("MISSING")) {
    emit uploadMessage("server_ip not set, exiting");
    return;
  }
  if (server_path == "" || server_path.startsWith("MISSING")) {
    emit uploadMessage("server_path not set, exiting");
    return;
  }
  server_path_user = server_path + "/" + username;
  server_path_meeting = server_path_user + "/" + directory.section('/', -1);
  emit uploadMessage("target directory: "+server_path_meeting);

  int rc = libssh2_init(0);
  if (rc != 0) {
    emit uploadMessage(QString("libssh2_init() failed (%1), exiting")
		       .arg(rc));
    return;
  }
  emit uploadMessage("libssh2 initialization successful");

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  unsigned long hostaddr = inet_addr(server_ip.toStdString().c_str());

  struct sockaddr_in sin;
  sin.sin_family = AF_INET;
  sin.sin_port = htons(22);
  sin.sin_addr.s_addr = hostaddr;
  if (::connect(sock, (struct sockaddr*)(&sin),
		sizeof(struct sockaddr_in)) != 0) {
    emit uploadMessage("failed to connect(), exiting");
    return;
  }
  emit uploadMessage("connection established");

  LIBSSH2_SESSION *session = libssh2_session_init();
  if (!session) {
    emit uploadMessage("libssh2_session_init() failed, exiting");
    return;
  }
  emit uploadMessage("libssh2 session initialization successful");

  libssh2_session_set_blocking(session, 1);
  rc = libssh2_session_handshake(session, sock);
  if (rc) {
    emit uploadMessage(QString("failure establishing SSH session (%1)"
			       ", exiting").arg(rc));
    return;
  }
  emit uploadMessage("session established");

  const char *fingerprint = libssh2_hostkey_hash(session,
						 LIBSSH2_HOSTKEY_HASH_SHA1);
  QString fp = "fingerprint: ";
  for(int i = 0; i < 20; i++) {
    QChar c = (QChar)fingerprint[i];
      fp += QString::number(c.unicode());
      fp += " ";
  }
  emit uploadMessage(fp);

  LIBSSH2_SFTP *sftp_session;

  QDir dir(directory);
  dir.setFilter(QDir::NoDotAndDotDot|QDir::Files);
  QStringList files = dir.entryList();
  long long totalsize = 0;

  LIBSSH2_AGENT* agent = trySshAgent(session);

  if (!agent) {

  // authentication by public key:
  emit uploadMessage("ssh-agent failed or not found, trying authentication without it");
  QString pubkey = QDir::homePath()+"/.ssh/id_rsa.pub";
  QString prikey = QDir::homePath()+"/.ssh/id_rsa";
  emit uploadMessage("using public key: "+ pubkey);
  emit uploadMessage("using private key: "+ prikey);

  const char *dummypassword="password";
  rc = libssh2_userauth_publickey_fromfile(session, 
					   username.toStdString().c_str(),
					   pubkey.toStdString().c_str(),
					   prikey.toStdString().c_str(),
					   dummypassword);

  if (rc) {
    emit uploadMessage(QString("authentication by public key failed: %1").arg(rc));

    //authentication via password
    emit uploadMessage("trying to authenticate with password");
    mutex.lock();
    emit passwordRequested();
    passwordNeeded.wait(&mutex);
    mutex.unlock();
    if (password == "") {
      emit uploadMessage("authentication by password cancelled");
      goto shutdown;
    }
    if (libssh2_userauth_password(session,
				  username.toStdString().c_str(),
				  password.toStdString().c_str())) {
      emit uploadMessage("authentication by password failed");
      goto shutdown;
    } else
      emit uploadMessage("authentication by password successful");

  } else
    emit uploadMessage("authentication by public key successful");
  }

  emit uploadMessage("libssh2_sftp_init()!");
  sftp_session = libssh2_sftp_init(session);
  if (!sftp_session) {
    emit uploadMessage("unable to init SFTP session");
    goto shutdown;
  }

  if (!checkDirectory(sftp_session, server_path_user))
    goto shutdown;
  if (!checkDirectory(sftp_session, server_path_meeting))
    goto shutdown;

  for (int i = 0; i < files.size(); ++i) {
    QString lf = directory + "/" + files.at(i);
    QFileInfo lf_info(lf);
    totalsize += lf_info.size();
  }
  emit nBlocks(totalsize/buffersize+1);

  for (int i = 0; i < files.size(); ++i)
    if (!processFile(sftp_session, files.at(i)))
      goto shutdown;

  libssh2_sftp_shutdown(sftp_session);

shutdown:
  if (agent)
    shutdownAgent(agent);

  libssh2_session_disconnect(session,
			     "Normal Shutdown, Thank you for playing");
  libssh2_session_free(session);

  close(sock);
  emit uploadMessage("all done");

  libssh2_exit();

  emit uploadMessage("uploadthread ending");
  emit uploadFinished();
}

// ---------------------------------------------------------------------

bool UploadThread::checkDirectory(LIBSSH2_SFTP *sftp_session, 
				  const QString &dir) {

  QByteArray ba_sp = dir.toLatin1();
  const char *sftppath = ba_sp.data();
  LIBSSH2_SFTP_ATTRIBUTES fileinfo;

  int rc = libssh2_sftp_stat(sftp_session, sftppath, &fileinfo);

  if (rc) {
    emit uploadMessage(QString("server path %1 does not exist, creating it").
		       arg(dir));
    rc = libssh2_sftp_mkdir(sftp_session, sftppath,
			    LIBSSH2_SFTP_S_IRWXU|
			    LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IXGRP|
			    LIBSSH2_SFTP_S_IROTH|LIBSSH2_SFTP_S_IXOTH);

    if (rc) {
      emit uploadMessage(QString("libssh2_sftp_mkdir failed: (%1)").arg(rc));
      return false;
    }
  } else
    emit uploadMessage(QString("server path %1 exists").arg(dir));

  return true;
}

// ---------------------------------------------------------------------

// Connect to the ssh-agent 
LIBSSH2_AGENT* UploadThread::trySshAgent(LIBSSH2_SESSION* session) {

  emit uploadMessage("trying to authenticate with ssh-agent");
  int rc = 0;
  LIBSSH2_AGENT *agent = libssh2_agent_init(session);

  if (!agent) {
    emit uploadMessage("failure initializing ssh-agent support");
    rc = 1;
    return shutdownAgent(agent);
  }

  if (libssh2_agent_connect(agent)) {
    emit uploadMessage("failure connecting to ssh-agent");
    rc = 1;
    return shutdownAgent(agent);
  }

  if (libssh2_agent_list_identities(agent)) {
    emit uploadMessage("failure requesting identities to ssh-agent");
    rc = 1;
    return shutdownAgent(agent);
  }

  struct libssh2_agent_publickey *identity, *prev_identity = NULL;
  while (1) {
    rc = libssh2_agent_get_identity(agent, &identity, prev_identity);

    if (rc == 1) {
      emit uploadMessage("failure due reaching end of public keys");
      return shutdownAgent(agent);
    }

    if (rc < 0) {
      emit uploadMessage("failure in obtaining identity from ssh-agent");
      return shutdownAgent(agent);
    }

    if (libssh2_agent_userauth(agent, username.toStdString().c_str(),
    			       identity)) {
      emit uploadMessage(QString("authentication with username %1 and "
				 "public key %2 failed")
			 .arg(username).arg(identity->comment));
      return shutdownAgent(agent);

    } else {
      emit uploadMessage(QString("authentication with username %1 and "
				 "public key %2 succeeded")
			 .arg(username).arg(identity->comment));
      break;
    }
    prev_identity = identity;
  }

  return agent;
}

//------------------------------------------------------------------------------

LIBSSH2_AGENT* UploadThread::shutdownAgent(LIBSSH2_AGENT* agent) {
    libssh2_agent_disconnect(agent);
    libssh2_agent_free(agent);

    return NULL;
}

// ---------------------------------------------------------------------

bool UploadThread::processFile(LIBSSH2_SFTP *sftp_session,
			       const QString &filename) {

  QString lf = directory + "/" + filename;

  QByteArray ba_lf = lf.toLatin1();
  const char *loclfile = ba_lf.data();
  FILE *local = fopen(loclfile, "rb");
  if (!local) {
    emit uploadMessage(QString("can't open local file %1")
		       .arg(loclfile));
    return false;
  }
  emit uploadMessage(QString("opened local file %1").arg(loclfile));

  QString sp = server_path_meeting + "/" + filename;
  QByteArray ba_sp = sp.toLatin1();
  const char *sftppath = ba_sp.data();

  emit uploadMessage(QString("libssh2_sftp_open() for %1").arg(sftppath));
  /* Request a file via SFTP */
  LIBSSH2_SFTP_HANDLE *sftp_handle =
    libssh2_sftp_open(sftp_session, sftppath,
                      LIBSSH2_FXF_WRITE|LIBSSH2_FXF_CREAT|LIBSSH2_FXF_TRUNC,
                      LIBSSH2_SFTP_S_IRUSR|LIBSSH2_SFTP_S_IWUSR|
                      LIBSSH2_SFTP_S_IRGRP|LIBSSH2_SFTP_S_IROTH);
  
  if (!sftp_handle) {
    emit uploadMessage("unable to open file with SFTP");
    if (local)
      fclose(local);
    return false;
  }

  emit uploadMessage("libssh2_sftp_open() is done, now sending data");

  char mem[buffersize], *ptr;
  int rc;

  do {
    size_t nread = fread(mem, 1, sizeof(mem), local);
    if (nread <= 0) {
      /* end of file */
      break;
    }
    ptr = mem;
    
    do {
      /* write data in a loop until we block */
      rc = libssh2_sftp_write(sftp_handle, ptr, nread);
      if(rc < 0)
	break;
      ptr += rc;
      nread -= rc;
    } while (nread);

    emit blockSent();
  } while (rc > 0);
  
  libssh2_sftp_close(sftp_handle);

  if (local)
    fclose(local);  

  emit uploadMessage("data sent successfully");

  return true;
}

// ---------------------------------------------------------------------

void UploadThread::setPreferences(const QString &_un, const QString &_ip,
				  const QString &_sp) {
  username  = _un;
  server_ip = _ip;
  server_path = _sp;
}

// ---------------------------------------------------------------------

void UploadThread::setPassword(const QString &_password) {
  password = _password;
  mutex.lock();
  passwordNeeded.wakeAll();
  mutex.unlock();
}

// ---------------------------------------------------------------------
