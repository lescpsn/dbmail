/*
 * serverchild.c
 *
 * function implementations of server children code (connection handling)
 */

#include "debug.h"
#include "serverchild.h"
#include "db.h"
#include "auth.h"
#include "clientinfo.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>

int ChildStopRequested = 0;
void *conn = NULL;
clientinfo_t client;

int PerformChildTask(ChildInfo_t *info);


void ChildSigHandler(int sig, siginfo_t *info, void *data)
{
  static int triedDisconnect = 0;

  trace(TRACE_ERROR, "ChildSighandler(): got signal [%d]", sig);

  /* perform reinit at SIGHUP otherwise exit */
  switch (sig)
    {
    case SIGALRM:
      trace(TRACE_DEBUG, "ChildSighandler(): timeout received");
      
      if (client.tx)
	{
	  trace(TRACE_DEBUG, "ChildSighandler(): write stream open, closing");
	  fprintf(client.tx, "%s", client.timeoutMsg);
	  fflush(client.tx);
	  fclose(client.tx); /* closes clientSocket as well */
	  client.tx = NULL;
	}

      if (client.rx)
	{
	  trace(TRACE_DEBUG, "ChildSighandler(): read stream open, closing");
	  shutdown(fileno(client.rx), SHUT_RDWR);
	  fclose(client.rx);
	  client.rx = NULL;
	}

      break;

    case SIGHUP: 
    case SIGTERM:
    case SIGQUIT:
    case SIGSTOP:
      if (ChildStopRequested)
	{
	  trace(TRACE_DEBUG, "ChildSighandler(): already caught a stop request. Closing right now");

	  /* already caught this signal, exit the hard way now */
	  if (client.tx)
	    {
	      trace(TRACE_DEBUG, "ChildSighandler(): write stream still open, closing");
	      fflush(client.tx);
	      fclose(client.tx); /* closes clientSocket as well */
	      client.tx = NULL;
	    }

	  if (client.rx)
	    {
	      trace(TRACE_DEBUG, "ChildSighandler(): read stream still open, closing");
	      shutdown(fileno(client.rx), SHUT_RDWR);
	      fclose(client.rx);
	      client.rx = NULL;
	    }

	  if (conn)
	    {
	      trace(TRACE_DEBUG, "ChildSighandler(): database connection still open, closing");
	      db_disconnect();
	      conn = NULL;
	    }

	  trace(TRACE_DEBUG, "ChildSighandler(): exit");
	  exit(1);
	}
      trace(TRACE_DEBUG, "ChildSighandler(): setting stop request");
      ChildStopRequested = 1; 
      break;

    default:
      /* bad shit, exit */
      trace(TRACE_DEBUG, "ChildSighandler(): cannot ignore this. Terminating");

      if (!triedDisconnect)
	{
	  triedDisconnect = 1;

	  if (client.tx)
	    {
	      trace(TRACE_DEBUG, "ChildSighandler(): write stream still open, closing");
	      fflush(client.tx);
	      fclose(client.tx); /* closes clientSocket as well */
	      client.tx = NULL;
	    }

	  if (client.rx)
	    {
	      trace(TRACE_DEBUG, "ChildSighandler(): read stream still open, closing");
	      shutdown(fileno(client.rx), SHUT_RDWR);
	      fclose(client.rx);
	      client.rx = NULL;
	    }

	  if (conn)
	    {
	      trace(TRACE_DEBUG, "ChildSighandler(): database connection still open, closing");
	      db_disconnect();
	    }

	  conn = NULL;
	}
      
      trace(TRACE_DEBUG, "ChildSighandler(): exit");
      exit(1);
    }
}



/*
 * SetChildSigHandler()
 * 
 * sets the signal handler for a child proces
 */

int SetChildSigHandler()
{
  struct sigaction act;

  /* init & install signal handlers */
  memset(&act, 0, sizeof(act));

  act.sa_sigaction = ChildSigHandler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = SA_SIGINFO;

  sigaction(SIGINT, &act, 0);
  sigaction(SIGQUIT, &act, 0);
  sigaction(SIGILL, &act, 0);
  sigaction(SIGBUS, &act, 0);
  sigaction(SIGPIPE, &act, 0);
  sigaction(SIGFPE, &act, 0);
  sigaction(SIGSEGV, &act, 0);
  sigaction(SIGTERM, &act, 0);
  sigaction(SIGHUP, &act, 0);
  sigaction(SIGALRM, &act, 0);

  return 0;
}


/*
 * CreateChild()
 *
 * creates a new child, returning only to the parent process
 */
pid_t CreateChild(ChildInfo_t *info)
{
  pid_t pid = fork();
  
  if (pid != 0)
    return pid;

  ChildStopRequested = 0;
  SetChildSigHandler();
  trace(TRACE_INFO, "CreateChild(): signal handler placed, going to perform task now");

  PerformChildTask(info);

  usleep(10000);
  exit(0);
}


/*
 * checks if a child is still alive
 */
int CheckChildAlive(pid_t pid)
{
  return (kill(pid, 0) == -1) ? 0 : 1;
}


int PerformChildTask(ChildInfo_t *info)
{
  int i,len,clientSocket;
  struct sockaddr_in saClient;

  if (!info)
    {
      trace(TRACE_ERROR, "PerformChildTask(): NULL info supplied");
      return -1;
    }

  if ( db_connect() != 0)
    {
      trace(TRACE_ERROR, "PerformChildTask(): could not connect to database");
      return -1;
    }

  conn = db_get_connection();
  auth_set_connection(conn); /* use the same connection for authentication */

  for (i=0; i<info->maxConnect && !ChildStopRequested; i++)
    {
      trace(TRACE_INFO, "PerformChildTask(): waiting for connection");
      
      /* wait for connect */
      len = sizeof(saClient);
      clientSocket = accept(info->listenSocket, (struct sockaddr*)&saClient, &len);

      if (clientSocket == -1)
	{
	  i--;                /* don't count this as a connect */
	  trace(TRACE_INFO, "PerformChildTask(): accept failed");
	  continue;    /* accept failed, refuse connection & continue */
	}

      trace(TRACE_ERROR, "PerformChildTask(): incoming connection from [%s]", inet_ntoa(saClient.sin_addr));
      
      memset(&client, 0, sizeof(client));               /* zero-init */
      client.conn = conn;                               /* db connection */
      client.timeoutMsg = info->timeoutMsg;
      strncpy(client.ip, inet_ntoa(saClient.sin_addr), IPNUM_LEN);
      client.clientname[0] = '\0'; /* do not perform ip-lookup's yet */

      /* make streams */
      if (! (client.rx = fdopen(dup(clientSocket), "r")) )
      {
	/* read-FILE opening failure */
	trace(TRACE_ERROR, "PerformChildTask(): error opening read file stream");
	close(clientSocket); 
	continue;
      }

      if (! (client.tx = fdopen(clientSocket, "w")) )
	{
	  /* write-FILE opening failure */
	  trace(TRACE_ERROR, "PerformChildTask(): error opening write file stream");
	  fclose(client.rx);
	  close(clientSocket); 
	  memset(&client, 0, sizeof(client));
	  continue;
	}
      
      setlinebuf(client.tx);
      setlinebuf(client.rx);

      trace(TRACE_DEBUG, "PerformChildTask(): client info init complete, calling client handler");
      
      /* streams are ready, set timeout value & perform handling */
      alarm(info->timeout);
      info->ClientHandler(&client);
      alarm(0);

      trace(TRACE_DEBUG, "PerformChildTask(): client handling complete, closing streams");

      if (client.tx)
	{
	  fflush(client.tx);
	  fclose(client.tx); /* closes clientSocket as well */
	  client.tx = NULL;
	}

      if (client.rx)
	{
	  shutdown(fileno(client.rx), SHUT_RDWR);
	  fclose(client.rx);
	  client.rx = NULL;
	}
	    
      trace(TRACE_INFO, "PerformChildTask(): connection closed");
    }

  db_disconnect();
  conn = NULL;

  if (!ChildStopRequested)
    trace(TRACE_ERROR, "PerformChildTask(): maximum number of connections reached, stopping now");
  else
    trace(TRACE_ERROR, "PerformChildTask(): stop requested");
    
  return 0;
}

