/* $Id$
 * (c) 2000-2002 IC&S, The Netherlands (http://www.ic-s.nl)
 *
 * postgresql driver file
 * Functions for connecting and talking to the PostgreSQL database */

#include "../db.h"
#include "/usr/local/pgsql/include/libpq-fe.h"
#include "../config.h"
#include "../pop3.h"
#include "../list.h"
#include "../mime.h"
#include "../pipe.h"
#include "../memblock.h"
#include <time.h>
#include <ctype.h>
#include <sys/types.h>
#include "../rfcmsg.h"
#include "../auth.h"


PGconn *conn;  
PGresult *res;
PGresult *checkres;
char *query = 0;
char *value = NULL; /* used for PQgetvalue */
unsigned long PQcounter = 0; /* used for PQgetvalue loops */

const char *db_flag_desc[] = 
{
  "seen_flag", "answered_flag", "deleted_flag", "flagged_flag", "draft_flag", "recent_flag"
};


int db_connect ()
{
  char connectionstring[255];
  /* create storage space for queries */
  query = (char*)my_malloc(DEF_QUERYSIZE);
  if (!query)
    {
      trace(TRACE_WARNING,"db_connect(): not enough memory for query\n");
      return -1;
    }

  /* connecting */

  sprintf (connectionstring, "host=%s user=%s password=%s dbname=%s",
	   HOST, USER, PASS, MAILDATABASE);

  conn = PQconnectdb(connectionstring);

  if (PQstatus(conn) == CONNECTION_BAD) 
    {
      trace(TRACE_ERROR,"dbconnect(): PQconnectdb failed: %s",PQerrorMessage(conn));
      return -1;
    }

  /* database connection OK */  

  return 0;
}

u64_t db_insert_result (const char *sequence_identifier)
{
  u64_t insert_result;

  /* postgres uses the currval call on a sequence to
     determine the result value of an insert query */

  snprintf (query, DEF_QUERYSIZE,"SELECT currval('%s_seq');",sequence_identifier);

  db_query (query);

  if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
      PQclear (res);
      return 0;
    }

  if (PQntuples(res)==0)
    {
      PQclear (res);
      return 0;
    }

  insert_result = strtoull(PQgetvalue(res, 0, 0), NULL, 10); /* should only be one result value */

  PQclear(res);

  return insert_result;
}


int db_query (const char *thequery)
{
  unsigned int querysize = 0;
  int PQresultStatusVar;

  if (thequery != NULL)
    {
      querysize = strlen(thequery);

      if (querysize > 0 )
        {
	  res = PQexec (conn, thequery);

	  if (!res)
            {
	      return -1;
            }

	  PQresultStatusVar = PQresultStatus (res);

	  if (PQresultStatusVar != PGRES_COMMAND_OK && PQresultStatusVar != PGRES_TUPLES_OK)
            {
	      trace(TRACE_ERROR,"db_query(): Error executing query [%s] : [%s]\n", 
		    thequery, 
		    PQresultErrorMessage(res));

	      PQclear(res);
	      return -1;
            }
        }
      else
        {
	  trace (TRACE_ERROR,"db_query(): query size is too small");
	  return -1;
        }
    }
  else
    {
      trace (TRACE_ERROR,"db_query(): query buffer is NULL, this is not supposed to happen\n");
      return -1;
    }
  return 0;
}


/*
 * clears the configuration table
 */
int db_clear_config()
{
  return db_query("DELETE FROM config");
}


int db_insert_config_item (char *item, char *val)
{
  /* insert_config_item will insert a configuration item in the database */

  snprintf (query, DEF_QUERYSIZE,"INSERT INTO config (item,value) VALUES ('%s', '%s')",item, val);
  trace (TRACE_DEBUG,"insert_config_item(): executing query: [%s]",query);

  if (db_query(query)==-1)
    {
      trace (TRACE_DEBUG,"insert_config_item(): item [%s] value [%s] failed",item,value);
      return -1;
    }
  else 
    {
      return 0;
    }
}


char *db_get_config_item (char *item, int type)
{
  /* retrieves an config item from database */
  char *result = NULL;

  snprintf (query, DEF_QUERYSIZE, "SELECT value FROM config WHERE item='%s'",item);
  trace (TRACE_DEBUG,"db_get_config_item(): retrieving config_item %s by query [%s]\n",
	 item, query);

  if (db_query(query)==-1)
    {
      if (type == CONFIG_MANDATORY)
        {
	  db_disconnect();
	  trace (TRACE_FATAL,"db_get_config_item(): query failed could not get value for %s. "
		 "This is needed to continue\n",item);
        }
      else
	if (type == CONFIG_EMPTY)
	  trace (TRACE_ERROR,"db_get_config_item(): query failed. Could not get value for %s\n",
		 item);

      return NULL;
    }

  if (PQntuples (res)==0)
    {
      if (type == CONFIG_MANDATORY)
	trace (TRACE_FATAL,"db_get_config_item(): configvalue not found for %s"
	       "This is needed to continue\n",item);
      else
	if (type == CONFIG_EMPTY)
	  trace (TRACE_ERROR,"db_get_config_item(): configvalue not found. "
		 "Could not get value for %s\n",item);

      PQclear(res);
      return NULL;
    }

  value = PQgetvalue (res, 0, 0);
  result=(char *)my_malloc(strlen(value+1));
  if (result!=NULL)
    {
      strcpy (result, value);
      trace (TRACE_DEBUG,"Ok result [%s]\n",result);
    }

  PQclear(res);
  return result;
}


/*
 * returns the number of bytes used by user userid or (u64_t)(-1) on dbase failure
 */
u64_t db_get_quotum_used(u64_t userid)
{
  u64_t q=0;

  snprintf(query, DEF_QUERYSIZE, "SELECT SUM(messagesize) FROM messages WHERE "
	   "mailbox_idnr IN (SELECT mailbox_idnr FROM mailboxes WHERE owner_idnr = %llu)",
	   userid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_quotum_used(): could not execute query");
      return -1;
    }

  if (PQntuples(res) < 1)
    {
      trace(TRACE_ERROR, "db_get_quotum_used(): SUM did not give result (?)");
      PQclear(res);
      return -1;
    }

  q = strtoull(PQgetvalue(res, 0, 0), NULL, 10);
  PQclear(res);
  
  trace(TRACE_DEBUG, "db_get_quotum_used(): found quotum usage of [%llu] bytes", q);
  return q;
}
      

/* 
 * adds an alias for a specific user 
 */
int db_addalias (u64_t useridnr, char *alias, int clientid)
{
  /* check if this alias already exists */
  snprintf (query, DEF_QUERYSIZE,
            "SELECT alias_idnr FROM aliases WHERE alias = '%s' AND deliver_to = '%llu' "
	    "AND client_idnr = %d", alias, useridnr, clientid);

  if (db_query(query) == -1)
    {
      /* query failed */
      trace (TRACE_ERROR, "db_addalias(): query for searching alias failed");
      return -1;
    }

  if (PQntuples(res) > 0)
    {
      trace(TRACE_INFO, "db_addalias(): alias [%s] for user [%llu] already exists", 
	    alias, useridnr);

      PQclear(res);
      return 0;
    }
  
  PQclear(res);

  snprintf (query, DEF_QUERYSIZE,
            "INSERT INTO aliases (alias,deliver_to,client_idnr) VALUES ('%s','%llu',%d)",
            alias, useridnr, clientid);


  if (db_query(query) == -1)
    {
      /* query failed */
      trace (TRACE_ERROR, "db_addalias(): query for adding alias failed");
      return -1;
    }

  return 0;
}


int db_addalias_ext(char *alias, char *deliver_to, int clientid)
{
  /* check if this alias already exists */
  snprintf (query, DEF_QUERYSIZE,
            "SELECT alias_idnr FROM aliases WHERE alias = '%s' AND deliver_to = '%s' "
	    "AND client_idnr = %d", alias, deliver_to, clientid);

  if (db_query(query) == -1)
    {
      /* query failed */
      trace (TRACE_ERROR, "db_addalias_ext(): query for searching alias failed");
      return -1;
    }

  if (PQntuples(res) > 0)
    {
      trace(TRACE_INFO, "db_addalias_ext(): alias [%s] --> [%s] already exists", 
	    alias, deliver_to);

      PQclear(res);
      return 0;
    }
  
  PQclear(res);

  snprintf (query, DEF_QUERYSIZE,
            "INSERT INTO aliases (alias,deliver_to,client_idnr) VALUES ('%s','%s',%d)",
            alias, deliver_to, clientid);


  if (db_query(query) == -1)
    {
      /* query failed */
      trace (TRACE_ERROR, "db_addalias_ext(): query for adding alias failed");
      return -1;
    }

  return 0;
}


int db_removealias (u64_t useridnr,const char *alias)
{
  snprintf (query, DEF_QUERYSIZE,
            "DELETE FROM aliases WHERE deliver_to='%llu' AND alias = '%s'", useridnr, alias);

  if (db_query(query) == -1)
    {
      /* query failed */
      trace (TRACE_ERROR, "db_removealias(): query for removing alias failed : [%s]", query);
      return -1;
    }

  return 0;
}



/* 
 * 
 * returns the mailbox id (of mailbox inbox) for a user or a 0 if no mailboxes were found 
 *
 */
u64_t db_get_inboxid (u64_t *useridnr)
{
  u64_t inboxid;

  snprintf (query, DEF_QUERYSIZE,"SELECT mailbox_idnr FROM mailboxes WHERE "
            "name ~* 'INBOX' AND owner_idnr=%llu",
            *useridnr);

  trace(TRACE_DEBUG,"db_get_inboxid(): executing query : [%s]",query);
  if (db_query(query)==-1)
    return 0;

  if (PQntuples(res)<1) 
    {
      trace (TRACE_DEBUG,"db_get_mailboxid(): user has no INBOX");
      PQclear(res);

      return 0; 
    } 

  value = PQgetvalue(res, 0, 0);
  inboxid = (value) ? strtoull(value, NULL, 10) : 0; 

  PQclear (res);

  return inboxid;
}


u64_t db_get_message_mailboxid (u64_t *messageidnr)
{
  /* returns the mailbox id of a message */
  u64_t mailboxid;

  snprintf (query, DEF_QUERYSIZE,"SELECT mailbox_idnr FROM messages WHERE message_idnr = %llu",
            *messageidnr);

  trace(TRACE_DEBUG,"db_get_message_mailboxid(): executing query : [%s]",query);
  if (db_query(query)==-1)
    {
      return 0;
    }

  if (PQntuples(res)<1) 
    {
      trace (TRACE_DEBUG,"db_get_message_mailboxid(): this message had no mailboxid? "
	     "Message without a mailbox!");
      PQclear(res);

      return 0; 
    } 

  value = PQgetvalue (res, 0, 0);
  mailboxid = (value) ? strtoull(value, NULL, 10) : 0;

  PQclear (res);


  return mailboxid;
}


u64_t db_get_useridnr (u64_t messageidnr)
{
  /* returns the userid from a messageidnr */
  u64_t mailboxidnr;
  u64_t userid;

  snprintf (query, DEF_QUERYSIZE,"SELECT mailbox_idnr FROM messages WHERE message_idnr = %llu",
            messageidnr);

  trace(TRACE_DEBUG,"db_get_useridnr(): executing query : [%s]",query);
  if (db_query(query)==-1)
    {
      return 0;
    }

  if (PQntuples(res)<1) 
    {
      trace (TRACE_DEBUG,"db_get_useridnr(): this is not right!");
      PQclear(res);

      return 0; 
    } 

  value = PQgetvalue (res, 0, 0);
  mailboxidnr = (value) ? strtoull(value, NULL, 10) : -1;
  PQclear(res);

  if (mailboxidnr == -1)
    return 0;

  snprintf (query, DEF_QUERYSIZE, "SELECT owner_idnr FROM mailboxes WHERE mailbox_idnr = %llu",
            mailboxidnr);

  if (db_query(query)==-1)
    return 0;

  if (PQntuples(res)<1) 
    {
      trace (TRACE_DEBUG,"db_get_useridnr(): this is not right!");
      PQclear(res);
      return 0; 
    } 

  value = PQgetvalue (res, 0, 0);	
  userid = (value) ? strtoull(value, NULL, 10) : 0;

  PQclear (res);

  return userid;
}


/* 
 * inserts into inbox ! 
 */
u64_t db_insert_message (u64_t *useridnr)
{
  char timestr[30];
  time_t td;
  struct tm tm;

  time(&td);              /* get time */
  tm = *localtime(&td);   /* get components */
  strftime(timestr, sizeof(timestr), "%G-%m-%d %H:%M:%S", &tm);

  snprintf (query, DEF_QUERYSIZE,"INSERT INTO messages(mailbox_idnr,messagesize,unique_id,internal_date)"
            " VALUES (%llu,0,' ','%s')",
            db_get_inboxid(useridnr), timestr);

  trace (TRACE_DEBUG,"db_insert_message(): inserting message query [%s]",query);
  if (db_query (query)==-1)
    {
      trace(TRACE_STOP,"db_insert_message(): dbquery failed");
    }	

  return db_insert_result("message_idnr");
}


u64_t db_update_message (u64_t *messageidnr, char *unique_id,
			 u64_t messagesize)
{
  snprintf (query, DEF_QUERYSIZE,
            "UPDATE messages SET messagesize=%llu, unique_id=\'%s\' where message_idnr=%llu",
            messagesize, unique_id, *messageidnr);

  trace (TRACE_DEBUG,"db_update_message(): updating message query [%s]",query);
  if (db_query (query)==-1)
    trace(TRACE_STOP,"db_update_message(): dbquery failed");

  return 0;
}


/*
 * insert a msg block
 * returns msgblkid on succes, -1 on failure
 */
u64_t db_insert_message_block (char *block, u64_t messageidnr)
{
  char *escblk=NULL, *tmpquery=NULL;
  int len,esclen=0;

  if (block != NULL)
    {
      len = strlen(block);

      trace (TRACE_DEBUG,"db_insert_message_block(): inserting a %d bytes block\n",
	     len);

      /* allocate memory twice as much, for eacht character might be escaped 
	 added aditional 250 bytes for possible function err */

      memtst((escblk=(char *)my_malloc(((len*2)+250)))==NULL); 

      /* escape the string */
      if ((esclen = PQescapeString (escblk, block, len)) > 0)
        {
	  /* add an extra 500 characters for the query */
	  memtst((tmpquery=(char *)my_malloc(esclen + 500))==NULL);

	  snprintf (tmpquery, esclen+500,
                    "INSERT INTO messageblks(messageblk,blocksize,message_idnr) "
                    "VALUES ('%s',%d,%llu)",
                    escblk,len,messageidnr);

	  if (db_query (tmpquery)==-1)
            {
	      my_free(escblk);
	      my_free(tmpquery);
	      trace(TRACE_ERROR,"db_insert_message_block(): dbquery failed\n");
	      return -1;
            }

	  PQclear(res);
	  /* freeing buffers */
	  my_free(tmpquery);
	  my_free(escblk);

	  return db_insert_result("messageblk_idnr");
        }
      else
        {
	  trace (TRACE_ERROR,"db_insert_message_block(): PQescapeString() "
		 "returned empty value\n");

	  my_free(escblk);
	  return -1;
        }
    }
  else
    {
      trace (TRACE_ERROR,"db_insert_message_block(): value of block cannot be NULL, "
	     "insertion not possible\n");
      return -1;
    }

  return -1;
}



/* get a list of aliases associated with user userid */
/* return -1 on db error, -2 on mem error, 0 on succes */
int db_get_user_aliases(u64_t userid, struct list *aliases)
{
  if (!aliases)
    {
      trace(TRACE_ERROR,"db_get_user_aliases(): got a NULL pointer as argument\n");
      return -2;
    }

  list_init(aliases);

  /* do a inverted (DESC) query because adding the names to the final list inverts again */
  snprintf(query, DEF_QUERYSIZE, "SELECT alias FROM aliases WHERE deliver_to = '%llu' ORDER BY alias "
	   "DESC", userid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR,"db_get_user_aliases(): could not retrieve  list\n");
      return -1;
    }

  if (PQntuples(res)>0)
    {
      for (PQcounter=0; PQcounter < PQntuples(res); PQcounter++)
        {
	  value = PQgetvalue (res, PQcounter, 0);
	  if (!list_nodeadd(aliases, value, strlen(value)+1))
            {
	      list_freelist(&aliases->start);
	      return -2;
            }
        }
    }

  PQclear(res);
  return 0;
}




/* 
   this function writes "lines" to fstream.
   if lines == -2 then the whole message is dumped to fstream 
   newlines are rewritten to crlf 
   This is excluding the header 
 */
int db_send_message_lines (void *fstream, u64_t messageidnr, long lines, int no_end_dot)
{
  char *buffer = NULL;
  char *nextpos, *tmppos = NULL;
  int block_count;
  u64_t rowlength;

  trace (TRACE_DEBUG,"db_send_message_lines(): request for [%d] lines",lines);


  memtst ((buffer=(char *)my_malloc(READ_BLOCK_SIZE*2))==NULL);

  snprintf (query, DEF_QUERYSIZE, 
            "SELECT * FROM messageblks WHERE message_idnr=%llu ORDER BY messageblk_idnr ASC",
            messageidnr);
  trace (TRACE_DEBUG,"db_send_message_lines(): executing query [%s]",query);

  if (db_query(query)==-1)
    {
      my_free(buffer);
      return 0;
    }

  if (PQntuples(res)>0)
    {
      trace (TRACE_DEBUG,"db_send_message_lines(): sending [%d] lines from message [%llu]",
	     lines,messageidnr);

      block_count=0;

      PQcounter = 0;

      while ((PQcounter<PQntuples(res)) && ((lines>0) || (lines==-2) || (block_count==0)))
        {
	  value = PQgetvalue (res, PQcounter, 2);
	  nextpos = value;
	  rowlength = PQgetlength(res, PQcounter, 2);

	  /* reset our buffer */
	  memset (buffer, '\0', (READ_BLOCK_SIZE)*2);

	  while ((*nextpos!='\0') && (rowlength>0) && ((lines>0) || (lines==-2) || (block_count==0)))
            {
	      if (*nextpos=='\n')
                {
		  /* first block is always the full header 
		     so this should not be counted when parsing
		     if lines == -2 none of the lines should be counted 
		     since the whole message is requested */
		  if ((lines!=-2) && (block_count!=0))
		    lines--;

		  if (tmppos!=NULL)
                    {
		      if (*tmppos=='\r')
			sprintf (buffer,"%s%c",buffer,*nextpos);
		      else 
			sprintf (buffer,"%s\r%c",buffer,*nextpos);
                    }
		  else 
		    sprintf (buffer,"%s\r%c",buffer,*nextpos);
                }
	      else
                {
		  if (*nextpos=='.')
                    {
		      if (tmppos!=NULL)
                        {
			  if (*tmppos=='\n')
			    sprintf (buffer,"%s.%c",buffer,*nextpos);
			  else
			    sprintf (buffer,"%s%c",buffer,*nextpos);
                        }
		      else 
			sprintf (buffer,"%s%c",buffer,*nextpos);
                    }
		  else	
		    sprintf (buffer,"%s%c",buffer,*nextpos);
                }

	      tmppos=nextpos;

	      /* get the next character */
	      nextpos++;
	      rowlength--;

	      if (rowlength%3000==0)  /* purge buffer at every 3000 bytes  */
                {
		  /* fprintf ((FILE *)fstream,"%s",buffer); */
		  /* fflush ((FILE *)fstream); */

		  fwrite (buffer, sizeof(char), strlen(buffer), (FILE *)fstream);

		  /*  cleanup the buffer  */
		  memset (buffer, '\0', (READ_BLOCK_SIZE*2));
                }
            }

	  /* next block in while loop */
	  block_count++;
	  trace (TRACE_DEBUG,"db_send_message_lines(): getting nextblock [%d]\n",block_count);

	  /* flush our buffer */
	  /* fprintf ((FILE *)fstream,"%s",buffer); */
	  fwrite (buffer, sizeof(char), strlen(buffer), (FILE *)fstream);
	  /* fflush ((FILE *)fstream); */

	  PQcounter++;

        }

      /* delimiter */
      if (no_end_dot == 0)
	fprintf ((FILE *)fstream,"\r\n.\r\n");
    }

  PQclear(res);

  my_free(buffer);
  return 1;
}


void db_session_cleanup (struct session *sessionptr)
{
  /* cleanups a session 
     removes a list and all references */

  sessionptr->totalsize=0;
  sessionptr->virtual_totalsize=0;
  sessionptr->totalmessages=0;
  sessionptr->virtual_totalmessages=0;
  list_freelist(&(sessionptr->messagelst.start));
}


/* returns 1 with a successfull session, -1 when something goes wrong 
 * sessionptr is changed with the right session info
 * useridnr is the userid index for the user whose mailbox we're viewing 
 */
int db_createsession (u64_t useridnr, struct session *sessionptr)
{
  /* first we do a query on the messages of this user */
  struct message tmpmessage;
  u64_t messagecounter=0,mboxid;

  mboxid = (db_get_inboxid(&useridnr));

  /* query is <2 because we don't want deleted messages 
     * the unique_id should not be empty, this could mean that the message is still being delivered */
  snprintf (query, DEF_QUERYSIZE, "SELECT * FROM messages WHERE mailbox_idnr=%llu AND status<002 AND "
            "unique_id!=\'\' order by status ASC",
            mboxid);

  if (db_query(query)==-1)
    return -1;

  sessionptr->totalmessages=0;
  sessionptr->totalsize=0;


  if ((messagecounter=PQntuples(res))<1)
    {
      /* there are no messages for this user */
      PQclear(res);
      return 1;
    }

  /* messagecounter is total message, +1 tot end at message 1 */
  messagecounter+=1;

  /* filling the list */

  trace (TRACE_DEBUG,"db_createsession(): adding items to list");
  for (PQcounter = 0; PQcounter < PQntuples(res); PQcounter++)
    {
      value = PQgetvalue(res, PQcounter, 2);
      tmpmessage.msize = value ? strtoull(value, NULL, 10) : 0;

      value = PQgetvalue(res, PQcounter, 0);
      tmpmessage.realmessageid = value ? strtoull(value, NULL, 10) : 0;

      value = PQgetvalue(res, PQcounter, 11);
      tmpmessage.messagestatus = value ? strtoull(value, NULL, 10) : 0;

      value = PQgetvalue (res, PQcounter, 9);
      strncpy(tmpmessage.uidl, value, strlen(value)+1);

      tmpmessage.virtual_messagestatus = tmpmessage.messagestatus;

      sessionptr->totalmessages++;
      sessionptr->totalsize+=tmpmessage.msize;

      /* descending to create inverted list */
      messagecounter--;
      tmpmessage.messageid=messagecounter;
      list_nodeadd (&sessionptr->messagelst, &tmpmessage, sizeof (tmpmessage));
    }

  trace (TRACE_DEBUG,"db_createsession(): adding succesfull");

    /* setting all virtual values */
  sessionptr->virtual_totalmessages=sessionptr->totalmessages;
  sessionptr->virtual_totalsize=sessionptr->totalsize;

  PQclear(res);


  return 1;
}

int db_update_pop (struct session *sessionptr)
{

  struct element *tmpelement;

  /* get first element in list */
  tmpelement=list_getstart(&sessionptr->messagelst);


  while (tmpelement!=NULL)
    {
      /* check if they need an update in the database */
      if (((struct message *)tmpelement->data)->virtual_messagestatus!=
	  ((struct message *)tmpelement->data)->messagestatus) 
        {
	  /* yes they need an update, do the query */
	  snprintf (query,DEF_QUERYSIZE,
                    "UPDATE messages set status=%llu WHERE message_idnr=%llu AND status<002",
                    ((struct message *)tmpelement->data)->virtual_messagestatus,
                    ((struct message *)tmpelement->data)->realmessageid);

	  /* FIXME: a message could be deleted already if it has been accessed
	   * by another interface and be deleted by sysop
	   * we need a check if the query failes because it doesn't exists anymore
	   * now it will just bailout */

	  if (db_query(query)==-1)
            {
	      trace(TRACE_ERROR,"db_update_pop(): could not execute query: [%s]", query);
	      return -1;
            }
        }
      tmpelement=tmpelement->nextnode;
    }

  return 0;
}


/* 
 * checks the size of a mailbox 
 * 
 * returns (u64_t)(-1) on error
 * preserves current PGresult (== global var res, see top of file)
 */
u64_t db_check_mailboxsize (u64_t mailboxid)
{
  PGresult *saveres = res;
  char *localrow;

  u64_t size;

  /* checking current size */
  snprintf (query, DEF_QUERYSIZE,
            "SELECT SUM(messagesize) FROM messages WHERE mailbox_idnr = %llu AND status<002",
            mailboxid);

  trace (TRACE_DEBUG,"db_check_mailboxsize(): executing query [%s]\n",
	 query);

  if (db_query(query) != 0)
    {
      trace (TRACE_ERROR,"db_check_mailboxsize(): could not execute query [%s]\n",
	     query);

      res = saveres;
      return -1;
    }

  if (PQntuples(res)<1)
    {
      trace (TRACE_ERROR,"db_check_mailboxsize(): weird, cannot execute SUM query\n");
      PQclear(res);
      res = saveres;
      return 0;
    }

  localrow = PQgetvalue (res, 0, 0);

  size = (localrow) ? strtoull(localrow, NULL, 10) : 0;
  PQclear(res);

  res = saveres;
  return size;
}


u64_t db_check_sizelimit (u64_t addblocksize, u64_t messageidnr, 
			  u64_t *useridnr)
{
  /* returns -1 when a block cannot be inserted due to dbase failure
   *          1 when a block cannot be inserted due to quotum exceed
   *         -2 when a block cannot be inserted due to quotum exceed and a dbase failure occured
   * also does a complete rollback when this occurs 
   * returns 0 when situation is ok 
   */

  u64_t currmail_size = 0, maxmail_size = 0, j, n;

  *useridnr = db_get_useridnr (messageidnr);

    /* checking current size */
  snprintf (query, DEF_QUERYSIZE,"SELECT mailbox_idnr FROM mailboxes WHERE owner_idnr = %llu",
            *useridnr);


  if (db_query(query) != 0)
    {
      trace (TRACE_ERROR,"db_check_sizelimit(): could not execute query [%s]\n",
	     query);
      return -1;
    }

  if (PQntuples(res)<1)
    {
      trace (TRACE_ERROR,"db_check_sizelimit(): user has NO mailboxes\n");
      PQclear(res);
      return 0;
    }

  for (PQcounter = 0; PQcounter < PQntuples (res); PQcounter++)
    {
      trace (TRACE_DEBUG,"db_check_sizelimit(): checking mailbox [%s]\n",value);
      value = PQgetvalue (res, PQcounter, 0);

      n = value ? strtoull(value, NULL, 10) : 0;
      j = db_check_mailboxsize(n);

      if (j == (u64_t)-1)
        {
	  trace(TRACE_ERROR,"db_check_sizelimit(): could not verify mailboxsize\n");

	  PQclear(res);
	  return -1;
        }

      currmail_size += j;
    }

  PQclear(res);

    /* current mailsize from INBOX is now known, now check the maxsize for this user */
  maxmail_size = db_getmaxmailsize(*useridnr);


  trace (TRACE_DEBUG, "db_check_sizelimit(): comparing currsize + blocksize  [%llu], maxsize [%llu]\n",
	 currmail_size, maxmail_size);


  /* currmail already represents the current size of messages from this user */

  if (((currmail_size) > maxmail_size) && (maxmail_size != 0))
    {
      trace (TRACE_INFO,"db_check_sizelimit(): mailboxsize of useridnr %llu exceed with %llu bytes\n", 
	     *useridnr, (currmail_size)-maxmail_size);

      /* user is exceeding, we're going to execute a rollback now */
      /* FIXME: this should be a transaction based roll back in PostgreSQL */

      snprintf (query,DEF_QUERYSIZE,"DELETE FROM messageblks WHERE message_idnr = %llu", 
                messageidnr);

      if (db_query(query) != 0)
        {
	  trace (TRACE_ERROR,"db_check_sizelimit(): rollback of mailbox add failed\n");
	  return -2;
        }

      snprintf (query,DEF_QUERYSIZE,"DELETE FROM messages WHERE message_idnr = %llu",
                messageidnr);

      if (db_query(query) != 0)
        {
	  trace (TRACE_ERROR,"db_check_sizelimit(): rollblock of mailbox add failed."
		 " DB might be inconsistent."
		 " run dbmail-maintenance\n");
	  return -2;
        }

      return 1;
    }

  return 0;
}


/* purges all the messages with a deleted status */
u64_t db_deleted_purge()
{
  u64_t affected_rows=0;

  /* first we're deleting all the messageblks */
  snprintf (query,DEF_QUERYSIZE,"SELECT message_idnr FROM messages WHERE status=003");
  trace (TRACE_DEBUG,"db_deleted_purge(): executing query [%s]",query);

  if (db_query(query)==-1)
    {
      trace(TRACE_ERROR,"db_deleted_purge(): Cound not execute query [%s]",query);
      return -1;
    }

  if (PQntuples(res)<1)
    {

      PQclear(res);
      return 0;
    }

  for (PQcounter = 0; PQcounter < PQntuples (res); PQcounter ++)
    {
      snprintf (query,DEF_QUERYSIZE,"DELETE FROM messageblks WHERE message_idnr=%s",
                PQgetvalue (res, PQcounter, 0));
      trace (TRACE_DEBUG,"db_deleted_purge(): executing query [%s]",query);
      if (db_query(query)==-1)
        {
	  PQclear(res);
	  return -1;
        }
    }

  /* messageblks are deleted. Now delete the messages */
  snprintf (query,DEF_QUERYSIZE,"DELETE FROM messages WHERE status=003");
  trace (TRACE_DEBUG,"db_deleted_purge(): executing query [%s]",query);
  if (db_query(query)==-1)
    {
      PQclear(res);
      return -1;
    }

  value = PQcmdTuples(res);
  affected_rows = value ? strtoull(value, NULL, 10) : 0;

  PQclear(res);

  return affected_rows;
}


/* sets al messages with status 002 to status 003 for final
 * deletion 
 */
u64_t db_set_deleted ()
{
  u64_t affected_rows;

  /* first we're deleting all the messageblks */
  snprintf (query,DEF_QUERYSIZE,"UPDATE messages SET status=003 WHERE status=002");
  trace (TRACE_DEBUG,"db_set_deleted(): executing query [%s]",query);

  if (db_query(query)==-1)
    {
      trace(TRACE_ERROR,"db_set_deleted(): Could not execute query [%s]",query);
      return -1;
    }

  value = PQcmdTuples(res);
  affected_rows = value ? strtoull(value, NULL, 10) : 0;

  PQclear (res);

  return affected_rows;
}



/*
 * will add the ip number to a table
 * needed for POP/IMAP_BEFORE_SMTP
 */
int db_log_ip(const char *ip)
{
  char timestr[30];
  time_t td;
  struct tm tm;
  u64_t id=0;
  char *row;

  time(&td);              /* get time */
  tm = *localtime(&td);   /* get components */
  strftime(timestr, sizeof(timestr), "%G-%m-%d %H:%M:%S", &tm);

  snprintf(query, DEF_QUERYSIZE, "SELECT idnr FROM pbsp WHERE ipnumber = '%s'", ip);
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR,"db_log_ip(): could not access ip-log table (pop/imap-before-smtp): %s",
	    query);
      return -1;
    }

  if (PQntuples>0)
    {
      row = PQgetvalue (res, 0, 0);
      id = row ? strtoull(row, NULL, 10) : 0;
    }
  else
    id = 0;

  PQclear(res);

  if (id)
    {
      /* this IP is already in the table, update the 'since' field */
      snprintf(query, DEF_QUERYSIZE, "UPDATE pbsp SET since = '%s' WHERE idnr=%llu",timestr,id);

      if (db_query(query) == -1)
        {
	  trace(TRACE_ERROR,"db_log_ip(): could not update ip-log (pop/imap-before-smtp): %s",
		query);
	  return -1;
        }
    }
  else
    {
      /* IP not in table, insert row */
      snprintf(query, DEF_QUERYSIZE, "INSERT INTO pbsp (since, ipnumber) VALUES ('%s','%s')", 
	       timestr, ip);

      if (db_query(query) == -1)
        {
	  trace(TRACE_ERROR,"db_log_ip(): could not log IP number to dbase (pop/imap-before-smtp): %s",
		query);
	  return -1;
        }
    }

  trace(TRACE_DEBUG,"db_log_ip(): ip [%s] logged\n",ip);
  return 0;
}


/*
 * removes all entries from the IP log with a date/time before lasttokeep
 */
int db_cleanup_iplog(const char *lasttokeep)
{
  snprintf(query, DEF_QUERYSIZE, "DELETE FROM pbsp WHERE since < '%s'",lasttokeep);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR,"db_cleanup_log(): error executing query [%s]",
	    query);
      return -1;
    }

  return 0;
}



/* 
 * will check for messageblks that are not
 * connected to messages 
 *
 * returns -1 on dbase error, -2 on memory error, 0 on succes
 * on succes lostlist will be a list containing nlost items being the messageblknr's of the
 * lost messageblks
 *
 * the caller should free this memory!
 */
int db_icheck_messageblks(int *nlost, u64_t **lostlist)
{
  int i;
  char *row;
  *nlost = 0;
  *lostlist = NULL;

  snprintf(query, DEF_QUERYSIZE, "SELECT messageblk_idnr FROM messageblks WHERE message_idnr NOT IN "
	   "(SELECT message_idnr FROM messages)");

  trace (TRACE_DEBUG,"db_icheck_messageblks(): executing query [%s]",query);

  if (db_query(query)==-1)
    {
      trace (TRACE_ERROR,"db_icheck_messageblks(): Could not execute query [%s]",query);
      return -1;
    }

  *nlost = PQntuples(res);
  trace(TRACE_DEBUG,"db_icheck_messageblks(): found %d lost message blocks\n", *nlost);

  if (*nlost == 0)
    return 0;


  *lostlist = (u64_t*)my_malloc(sizeof(u64_t) * (*nlost));
  if (!*lostlist)
    {
      *nlost = 0;
      trace(TRACE_ERROR,"db_icheck_messageblks(): out of memory when allocatin %d items\n",*nlost);
      return -2;
    }

  i = 0;
  PQcounter = 0;


  while ((PQcounter<PQntuples(res)) && i<*nlost)
    {
      row = PQgetvalue (res, PQcounter, 0);    
      (*lostlist)[i++] = strtoull(row, NULL, 10);
      PQcounter++;
    }
  return 0;
}


/* 
 * will check for messages that are not
 * connected to mailboxes 
 *
 * returns -1 on dbase error, -2 on memory error, 0 on succes
 * on succes lostlist will be a list containing nlost items being the messageid's of the
 * lost messages
 *
 * the caller should free this memory!
 */
int db_icheck_messages(int *nlost, u64_t **lostlist)
{
  int i;
  char *row;
  *nlost = 0;
  *lostlist = NULL;


  snprintf(query, DEF_QUERYSIZE, "SELECT message_idnr FROM messages WHERE mailbox_idnr NOT IN "
	   "(SELECT mailbox_idnr FROM mailboxes)");

  trace (TRACE_DEBUG,"db_icheck_messages(): executing query [%s]",query);

  if (db_query(query)==-1)
    {
      trace (TRACE_ERROR,"db_icheck_messages(): Could not execute query [%s]",query);
      return -1;
    }

  *nlost = PQntuples(res);
  trace(TRACE_DEBUG,"db_icheck_messages(): found %d lost messages\n", *nlost);

  if (*nlost == 0)
    return 0;


  *lostlist = (u64_t*)my_malloc(sizeof(u64_t) * (*nlost));
  if (!*lostlist)
    {
      *nlost = 0;
      trace(TRACE_ERROR,"db_icheck_messages(): out of memory when allocating %d items\n",*nlost);
      return -2;
    }

  i = 0;
  PQcounter = 0;

  while ((PQcounter<PQntuples(res)) && i<*nlost)
    {
      row = PQgetvalue (res, PQcounter, 0);
      (*lostlist)[i++] = strtoull(row, NULL, 10);
      PQcounter++;
    }

  return 0;
}


/* 
 * will check for mailboxes that are not
 * connected to users 
 *
 * returns -1 on dbase error, -2 on memory error, 0 on succes
 * on succes lostlist will be a list containing nlost items being the mailboxid's of the
 * lost mailboxes
 *
 * the caller should free this memory!
 */
int db_icheck_mailboxes(int *nlost, u64_t **lostlist)
{
  int i;
  *nlost = 0;
  *lostlist = NULL;

  snprintf(query,DEF_QUERYSIZE,"SELECT mailbox_idnr FROM mailboxes WHERE owner_idnr NOT IN "
	   "(SELECT user_idnr FROM users)");


  trace (TRACE_DEBUG,"db_icheck_mailboxes(): executing query [%s]",query);

  if (db_query(query)==-1)
    {
      trace (TRACE_ERROR,"db_icheck_mailboxes(): Could not execute query [%s]",query);
      return -1;
    }

  *nlost = PQntuples(res);
  trace(TRACE_DEBUG,"db_icheck_mailboxes(): found %d lost mailboxes\n", *nlost);

  if (*nlost == 0)
    return 0;


  *lostlist = (u64_t*)my_malloc(sizeof(u64_t) * (*nlost));
  if (!*lostlist)
    {
      *nlost = 0;
      trace(TRACE_ERROR,"db_icheck_mailboxes(): out of memory when allocating %d items\n",*nlost);
      return -2;
    }

  i = 0;

  PQcounter = 0;  

  while ((PQcounter < PQntuples(res)) && i<*nlost)
    {
      (*lostlist)[i++] = strtoull(PQgetvalue (res, PQcounter, 0), NULL, 10);
      PQcounter++;
    }
  return 0;
}


/*
 * deletes the specified block. used by maintenance
 */
int db_delete_messageblk(u64_t uid)
{
  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messageblks WHERE messageblk_idnr = %llu",uid);
  return db_query(query);
}


/*
 * deletes the specified message. used by maintenance
 */
int db_delete_message(u64_t uid)
{
  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messageblks WHERE message_idnr = %llu",uid);
  if (db_query(query) == -1)
    return -1;

  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messages WHERE message_idnr = %llu",uid);
  return db_query(query);
}

/*
 * deletes the specified mailbox. used by maintenance
 */
int db_delete_mailbox(u64_t uid)
{
  u64_t msgid;
  snprintf(query, DEF_QUERYSIZE, "SELECT message_idnr FROM messages WHERE mailbox_idnr = %llu",uid);

  if (db_query(query) == -1)
    return -1;

  for (PQcounter = 0; PQcounter < PQntuples (res); PQcounter ++)
    {
      msgid = strtoull(PQgetvalue (res, PQcounter, 0), NULL, 10);

      snprintf(query, DEF_QUERYSIZE, "DELETE FROM messageblks WHERE message_idnr = %llu",msgid);
      if (db_query(query) == -1)
	return -1;

      snprintf(query, DEF_QUERYSIZE, "DELETE FROM messages WHERE message_idnr = %llu",msgid);
      if (db_query(query) == -1)
	return -1;
    }

  snprintf(query, DEF_QUERYSIZE, "DELETE FROM mailboxes WHERE mailbox_idnr = %llu",uid);
  return db_query(query);
}

int db_disconnect()
{
  my_free(query);
  query = NULL;

  PQfinish(conn);
  return 0;
}


/*
 * db_imap_append_msg()
 *
 * inserts a message
 *
 * returns: 
 * -1 serious dbase/memory error
 *  0 ok
 *  1 invalid msg
 *  2 mail quotum exceeded
 *
 */
int db_imap_append_msg(char *msgdata, u64_t datalen, u64_t mboxid, u64_t uid)
{
  char timestr[30];
  time_t td;
  struct tm tm;
  u64_t msgid,cnt;
  int result;
  char savechar;
  char unique_id[UID_SIZE]; /* unique id */

  time(&td);              /* get time */
  tm = *localtime(&td);   /* get components */
  strftime(timestr, sizeof(timestr), "%G-%m-%d %H:%M:%S", &tm);

  /* create a msg 
   * status and seen_flag are set to 001, which means the message has been read 
   */
  snprintf(query, DEF_QUERYSIZE, "INSERT INTO messages "
	   "(mailbox_idnr,messagesize,unique_id,internal_date,status,"
	   " seen_flag) VALUES (%llu, 0, '', '%s',001,1)",
	   mboxid, timestr);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_imap_append_msg(): could not create message\n");
      my_free(query);
      return -1;
    }

  /* fetch the id of the new message */
  msgid = db_insert_result("message_idnr");

  result = db_check_sizelimit(datalen, msgid, &uid);
  if (result == -1 || result == -2)
    {     
      trace(TRACE_ERROR, "db_imap_append_msg(): dbase error checking size limit\n");
      return -1;
    }

  if (result)
    {     
      trace(TRACE_INFO, "db_imap_append_msg(): user %llu would exceed quotum\n",uid);
      return 2;
    }

  /* ok insert blocks */
  /* first the header: scan until double newline */
  for (cnt=1; cnt<datalen; cnt++)
    if (msgdata[cnt-1] == '\n' && msgdata[cnt] == '\n')
      break;

  if (cnt == datalen)
    {
      trace(TRACE_INFO, "db_imap_append_msg(): no double newline found [invalid msg]\n");
      snprintf(query, DEF_QUERYSIZE, "DELETE FROM messages WHERE message_idnr = %llu", msgid);
      if (db_query(query) == -1)
	trace(TRACE_ERROR, "db_imap_append_msg(): could not delete message id [%llu], "
	      "dbase invalid now..\n", msgid);


      return 1;
    }

  if (cnt == datalen-1)
    {
      /* msg consists of a single header */
      trace(TRACE_INFO, "db_imap_append_msg(): msg only contains a header\n");

      if (db_insert_message_block(msgdata, msgid) == -1 || 
	  db_insert_message_block(" \n", msgid)   == -1)
        {
	  trace(TRACE_ERROR, "db_imap_append_msg(): could not insert msg block\n");

	  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messages WHERE message_idnr = %llu", msgid);
	  if (db_query(query) == -1)
	    trace(TRACE_ERROR, "db_imap_append_msg(): could not delete message id [%llu], "
		  "dbase could be invalid now, run dbmail-maintenance\n", msgid);

	  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messageblks WHERE message_idnr = %llu", msgid);
	  if (db_query(query) == -1)
	    trace(TRACE_ERROR, "db_imap_append_msg(): could not delete messageblks for msg id [%llu], "
		  "dbase could be invalid now run dbmail-maintenance\n", msgid);


	  return -1;
        }

    }
  else
    {
      /* output header */
      cnt++;
      savechar = msgdata[cnt];                        /* remember char */
      msgdata[cnt] = 0;                               /* terminate string */
      if (db_insert_message_block(msgdata, msgid) == -1)
        {
	  trace(TRACE_ERROR, "db_imap_append_msg(): could not insert msg block\n");

	  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messages WHERE message_idnr = %llu", msgid);
	  if (db_query(query) == -1)
	    trace(TRACE_ERROR, "db_imap_append_msg(): could not delete message id [%llu], "
		  "dbase invalid now, run dbmail-maintenance\n", msgid);

	  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messageblks WHERE message_idnr = %llu", msgid);
	  if (db_query(query) == -1)
	    trace(TRACE_ERROR, "db_imap_append_msg(): could not delete messageblks for msg id [%llu], "
		  "dbase could be invalid now, run dbmail-maintenance\n", msgid);


	  return -1;
        }

      msgdata[cnt] = savechar;                        /* restore */

      /* output message */
      while ((datalen - cnt) > READ_BLOCK_SIZE)
        {
	  savechar = msgdata[cnt + READ_BLOCK_SIZE];        /* remember char */
	  msgdata[cnt + READ_BLOCK_SIZE] = 0;               /* terminate string */

	  if (db_insert_message_block(&msgdata[cnt], msgid) == -1)
            {
	      trace(TRACE_ERROR, "db_imap_append_msg(): could not insert msg block\n");

	      snprintf(query, DEF_QUERYSIZE, "DELETE FROM messages WHERE message_idnr = %llu", msgid);
	      if (db_query(query) == -1)
		trace(TRACE_ERROR, "db_imap_append_msg(): could not delete message id [%llu], "
		      "dbase invalid now run dbmail-maintenance\n", msgid);

	      snprintf(query, DEF_QUERYSIZE, "DELETE FROM messageblks WHERE message_idnr = %llu", msgid);
	      if (db_query(query) == -1)
		trace(TRACE_ERROR, "db_imap_append_msg(): could not delete messageblks "
		      "for msg id [%llu], dbase could be invalid now run dbmail-maintenance\n", msgid);
	      return -1;
            }

	  msgdata[cnt + READ_BLOCK_SIZE] = savechar;        /* restore */

	  cnt += READ_BLOCK_SIZE;
        }


      if (db_insert_message_block(&msgdata[cnt], msgid) == -1)
        {
	  trace(TRACE_ERROR, "db_imap_append_msg(): could not insert msg block\n");

	  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messages WHERE message_idnr = %llu", msgid);
	  if (db_query(query) == -1)
	    trace(TRACE_ERROR, "db_imap_append_msg(): could not delete message id [%llu], "
		  "dbase invalid now run dbmail-maintance\n", msgid);

	  snprintf(query, DEF_QUERYSIZE, "DELETE FROM messageblks WHERE message_idnr = %llu", msgid);
	  if (db_query(query) == -1)
	    trace(TRACE_ERROR, "db_imap_append_msg(): could not delete messageblks "
		  "for msg id [%llu], dbase could be invalid now run dbmail-maintenance\n", msgid);


	  return -1;
        }

    }  

  /* create a unique id */
  snprintf (unique_id,UID_SIZE,"%lluA%lu",msgid,td);

  /* set info on message */
  db_update_message (&msgid, unique_id, datalen);

  return 0;
}



/*
 * db_findmailbox()
 *
 * checks wheter the mailbox designated by 'name' exists for user 'useridnr'
 *
 * returns 0 if the mailbox is not found, 
 * (unsigned)(-1) on error,
 * or the UID of the mailbox otherwise.
 */
u64_t db_findmailbox(const char *name, u64_t useridnr)
{
  u64_t id;

  snprintf(query, DEF_QUERYSIZE, "SELECT mailbox_idnr FROM mailboxes WHERE name ~* '%s' "
	   "AND owner_idnr=%llu",
	   name, useridnr);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR,"db_findmailbox(): could not select mailbox '%s'\n",name);
      return (u64_t)(-1);
    }

  if (PQntuples(res)>0)
    {
      value = PQgetvalue(res, 0, 0);
      id = value ? strtoull(value, NULL, 10) : 0;
    }
  else
    id=0;

  PQclear(res);

  return id;
}


/*
 * db_findmailbox_by_regex()
 *
 * finds all the mailboxes owned by ownerid who match the regex pattern pattern.
 */
int db_findmailbox_by_regex(u64_t ownerid, const char *pattern, 
			    u64_t **children, unsigned *nchildren, int only_subscribed)
{
  *children = NULL;

  if (only_subscribed)
    snprintf(query, DEF_QUERYSIZE, "SELECT mailbox_idnr FROM mailboxes WHERE "
	     "owner_idnr=%llu AND is_subscribed != 0 AND name ~* '%s'", ownerid, pattern);
  else
    snprintf(query, DEF_QUERYSIZE, "SELECT mailbox_idnr FROM mailboxes WHERE "
	     "owner_idnr=%llu AND name ~* '%s'", ownerid, pattern);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR,"db_findmailbox_by_regex(): error during mailbox query\r\n");
      return (-1);
    }


  *nchildren = PQntuples(res);

  if (*nchildren == 0)
    {
      /* none exist, none matched */
      return 0;
    }

  /* alloc mem */
  *children = (u64_t *)my_malloc(sizeof(u64_t) * PQntuples(res));
  if (!(*children))
    {
      trace(TRACE_ERROR,"db_findmailbox_by_regex(): not enough memory\n");
      return (-1);
    }

  /* store matches */
  for (PQcounter = 0; PQcounter < PQntuples(res); PQcounter++)
    {
      (*children)[PQcounter] = strtoull(PQgetvalue(res, PQcounter, 0), NULL, 10);
    }

  PQclear(res);

  return 0;
}


/*
 * db_getmailbox()
 * 
 * gets mailbox info from dbase, builds the message sequence number list
 *
 * returns 
 *  -1  error
 *   0  success
 */
int db_getmailbox(mailbox_t *mb, u64_t userid)
{
  u64_t i;

  /* free existing MSN list */
  if (mb->seq_list)
    {
      my_free(mb->seq_list);
      mb->seq_list = NULL;
    }

  mb->flags = 0;
  mb->exists = 0;
  mb->unseen = 0;
  mb->recent = 0;
  mb->msguidnext = 0;

  /* select mailbox */
  snprintf(query, DEF_QUERYSIZE, 
	   "SELECT permission,"
	   "seen_flag,"
	   "answered_flag,"
	   "deleted_flag,"
	   "flagged_flag,"
	   "recent_flag,"
	   "draft_flag "
	   " FROM mailboxes WHERE mailbox_idnr = %llu", mb->uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_getmailbox(): could not select mailbox\n");
      return -1;
    }

  if (PQntuples(res)==0)
    {
      trace(TRACE_ERROR,"db_getmailbox(): invalid mailbox id specified\n");
      return -1;
    }

  mb->permission = atoi(PQgetvalue(res,0,0));

  if (PQgetvalue(res,0,1)) mb->flags |= IMAPFLAG_SEEN;
  if (PQgetvalue(res,0,2)) mb->flags |= IMAPFLAG_ANSWERED;
  if (PQgetvalue(res,0,3)) mb->flags |= IMAPFLAG_DELETED;
  if (PQgetvalue(res,0,4)) mb->flags |= IMAPFLAG_FLAGGED;
  if (PQgetvalue(res,0,5)) mb->flags |= IMAPFLAG_RECENT;
  if (PQgetvalue(res,0,6)) mb->flags |= IMAPFLAG_DRAFT;

  PQclear(res);

  /* select messages */
  snprintf(query, DEF_QUERYSIZE, "SELECT message_idnr, seen_flag, recent_flag "
	   "FROM messages WHERE mailbox_idnr = %llu "
	   "AND status<2 AND unique_id!='' ORDER BY message_idnr ASC", mb->uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_getmailbox(): could not retrieve messages\n");
      return -1;
    }

  mb->exists = PQntuples(res);

  /* alloc mem */
  mb->seq_list = (u64_t*)my_malloc(sizeof(u64_t) * mb->exists);
  if (!mb->seq_list)
    {
      /* out of mem */
      PQclear(res);
      return -1;
    }

  i=0;
  for (PQcounter = 0; PQcounter < PQntuples(res); PQcounter++)
    {
      if (PQgetvalue(res, PQcounter, 1)[0] == '0') mb->unseen++;
      if (PQgetvalue(res, PQcounter, 2)[0] == '1') mb->recent++;

      mb->seq_list[i++] = strtoull(PQgetvalue(res, PQcounter, 0),NULL,10);
    }

  PQclear(res);


  /* now determine the next message UID */
  /*
   * NOTE expunged messages are selected as well in order to be able to restore them 
   */

  snprintf(query, DEF_QUERYSIZE, "SELECT message_idnr FROM messages WHERE unique_id!='' "
	   "ORDER BY message_idnr DESC LIMIT 1");

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_getmailbox(): could not determine highest message ID\n");

      my_free(mb->seq_list);
      mb->seq_list = NULL;

      return -1;
    }

  if (PQntuples(res)>0)
    {
      value = PQgetvalue(res, 0, 0);
      mb->msguidnext = value ? strtoull(value, NULL, 10)+1 : 1;
    }

  PQclear(res);


  /* done */
  return 0;
}


/*
 * db_createmailbox()
 *
 * creates a mailbox for the specified user
 * does not perform hierarchy checks
 * 
 * returns -1 on error, 0 on succes
 */
int db_createmailbox(const char *name, u64_t ownerid)
{
  snprintf(query, DEF_QUERYSIZE, "INSERT INTO mailboxes (name, owner_idnr,"
	   "seen_flag, answered_flag, deleted_flag, flagged_flag, recent_flag, draft_flag, permission)"
	   " VALUES ('%s', %llu, 1, 1, 1, 1, 1, 1, 2)", name,ownerid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_createmailbox(): could not create mailbox\n");

      return -1;
    }


  return 0;
}


/*
 * db_listmailboxchildren()
 *
 * produces a list containing the UID's of the specified mailbox' children 
 * matching the search criterion
 * 
 * the filter contains '%' as a wildcard which will be replaced in this function
 * by '.*' as regex searching is used
 *
 * returns -1 on error, 0 on succes
 */
int db_listmailboxchildren(u64_t uid, u64_t useridnr, 
			   u64_t **children, int *nchildren, 
			   const char *filter)
{

  int i,j;
  char *row = 0;
  char *pgsql_filter;

  /* alloc mem */
  pgsql_filter = (char*)my_malloc(strlen(filter) * 2 +1);
  if (!pgsql_filter)
    {
      trace(TRACE_ERROR, "db_listmailboxchildren(): out of memory\n");
      return -1;
    }

  /* build regex filter */
  for (i=0,j=0; filter[i]; i++,j++)
    {
      if (filter[i] == '%')
        {
	  pgsql_filter[j++] = '.';
	  pgsql_filter[j] = '*';
        }
      else
	pgsql_filter[j] = filter[i];
    }
  pgsql_filter[j] = filter[i]; /* terminate string */


  /* retrieve the name of this mailbox */
  snprintf(query, DEF_QUERYSIZE, "SELECT name FROM mailboxes WHERE"
	   " mailbox_idnr = %llu AND owner_idnr = %llu", uid, useridnr);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_listmailboxchildren(): could not retrieve mailbox name\n");
      my_free(pgsql_filter);
      return -1;
    }

  if (PQntuples(res)>0)
    {
      row = PQgetvalue (res, 0, 0);
      if (row)
	snprintf(query, DEF_QUERYSIZE, "SELECT mailbox_idnr FROM mailboxes WHERE name ~* '%s/%s'"
		 " AND owner_idnr = %llu",
		 row,pgsql_filter,useridnr);
      else
	snprintf(query, DEF_QUERYSIZE, "SELECT mailbox_idnr FROM mailboxes WHERE name ~* '%s'"
		 " AND owner_idnr = %llu",pgsql_filter,useridnr);

    }

  PQclear(res);

  /* now find the children */
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_listmailboxchildren(): could not retrieve mailbox name\n");
      my_free(pgsql_filter);
      return -1;
    }

  /* free mem */
  my_free(pgsql_filter);
  pgsql_filter = NULL;


  if ((*nchildren = PQntuples(res))>0)
    {
      row = PQgetvalue(res, 0, 0);
      if (!row)
        {
	  /* empty set */
	  *children = NULL;
	  *nchildren = 0;
	  PQclear(res);
	  return 0;
        }
    }
  else
    {
      *children = NULL;
      return 0;
    }
  *children = (u64_t*)my_malloc(sizeof(u64_t) * (*nchildren));

  if (!(*children))
    {
      /* out of mem */
      trace(TRACE_ERROR,"db_listmailboxchildren(): out of memory\n");
      PQclear(res);

      return -1;
    }

  i = 0;
  PQcounter = 0;
  do
    {
      if (i == *nchildren)
        {
	  /*  big fatal */
	  my_free(*children);
	  *children = NULL;
	  *nchildren = 0;
	  PQclear(res);
	  trace(TRACE_ERROR, "db_listmailboxchildren(): data when none expected.\n");

	  return -1;
        }

      (*children)[i++] = strtoull(row, NULL, 10);
      row = PQgetvalue (res, PQcounter, 0);
      PQcounter ++;
    }
  while (PQcounter < PQntuples (res));

  PQclear(res);


  return 0; /* success */
}


/*
 * db_removemailbox()
 *
 * removes the mailbox indicated by UID/ownerid and all the messages associated with it
 * the mailbox SHOULD NOT have any children but no checks are performed
 *
 * returns -1 on failure, 0 on succes
 */
int db_removemailbox(u64_t uid, u64_t ownerid)
{
  if (db_removemsg(uid) == -1) /* remove all msg */
    {
      return -1;
    }

  /* now remove mailbox */
  snprintf(query, DEF_QUERYSIZE, "DELETE FROM mailboxes WHERE mailbox_idnr = %llu", uid);
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_removemailbox(): could not remove mailbox\n");
      return -1;
    }

  /* done */
  return 0;
}


/*
 * db_isselectable()
 *
 * returns 1 if the specified mailbox is selectable, 0 if not and -1 on failure
 */  
int db_isselectable(u64_t uid)
{
  snprintf(query, DEF_QUERYSIZE, "SELECT no_select FROM mailboxes WHERE mailbox_idnr = %llu",uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_isselectable(): could not retrieve select-flag\n");
      return -1;
    }

  if (PQntuples(res)>0)
    {
      value = PQgetvalue(res, 0, 0);

      if (!value)
        {
	  /* empty set, mailbox does not exist */
	  PQclear(res);
	  return 0;
        }

      if (atoi(value) == 0)
        {    
	  PQclear(res);
	  return 1;
        }
    }

  PQclear(res);
  return 0;
}


/*
 * db_noinferiors()
 *
 * checks if mailbox has no_inferiors flag set
 *
 * returns
 *   1  flag is set
 *   0  flag is not set
 *  -1  error
 */
int db_noinferiors(u64_t uid)
{
  char *row;

  snprintf(query, DEF_QUERYSIZE, "SELECT no_inferiors FROM mailboxes WHERE mailbox_idnr = %llu",uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_noinferiors(): could not retrieve noinferiors-flag\n");
      return -1;
    }

  if (PQntuples(res)>0)
    {
      row = PQgetvalue (res, 0, 0);

      if (!row)
        {
	  /* empty set, mailbox does not exist */
	  PQclear(res);
	  return 0;
        }

      if (atoi(row) == 1)
        {    
	  PQclear(res);
	  return 1;
        }
    }

  PQclear(res);
  return 0;
}


/*
 * db_setselectable()
 *
 * set the noselect flag of a mailbox on/off
 * returns 0 on success, -1 on failure
 */
int db_setselectable(u64_t uid, int val)
{


  snprintf(query, DEF_QUERYSIZE, "UPDATE mailboxes SET no_select = %d WHERE mailbox_idnr = %llu",
	   (!value), uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_setselectable(): could not set noselect-flag\n");
      return -1;
    }

  return 0;
}


/*
 * db_removemsg()
 *
 * removes ALL messages from a mailbox
 * removes by means of setting status to 3
 *
 * returns -1 on failure, 0 on success
 */
int db_removemsg(u64_t uid)
{


  /* update messages belonging to this mailbox: mark as deleted (status 3) */
  snprintf(query, DEF_QUERYSIZE, "UPDATE messages SET status=3 WHERE"
	   " mailbox_idnr = %llu", uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_removemsg(): could not update messages in mailbox\n");
      return -1;
    }

  return 0; /* success */
}


/*
 * db_expunge()
 *
 * removes all messages from a mailbox with delete-flag
 * removes by means of setting status to 3
 * makes a list of delete msg UID's 
 *
 * allocated memory should be freed by client; if msgids and/or nmsgs are NULL 
 * no list of deleted msg UID's will be made
 *
 * returns -1 on failure, 0 on success
 */
int db_expunge(u64_t uid,u64_t **msgids,u64_t *nmsgs)
{
  u64_t i;

  if (nmsgs && msgids)
    {
      /* first select msg UIDs */
      snprintf(query, DEF_QUERYSIZE, "SELECT message_idnr FROM messages WHERE"
	       " mailbox_idnr = %llu AND deleted_flag=1 AND status<2 ORDER BY message_idnr DESC", uid);

      if (db_query(query) == -1)
        {
	  trace(TRACE_ERROR, "db_expunge(): could not select messages in mailbox\n");
	  return -1;
        }

      /* now alloc mem */
      *nmsgs = PQntuples(res);
      *msgids = (u64_t *)my_malloc(sizeof(u64_t) * (*nmsgs));
      if (!(*msgids))
        {
	  /* out of mem */
	  *nmsgs = 0;
	  PQclear(res);
	  return -1;
        }

      /* save ID's in array */
      i = 0;
      PQcounter = 0;
      while ((PQcounter < PQntuples (res)) && i<*nmsgs)
        {
	  (*msgids)[i++] = strtoull(PQgetvalue(res,PQcounter,0), NULL, 10);
	  PQcounter++;
        }
      PQclear(res);
    }

  /* update messages belonging to this mailbox: mark as expunged (status 3) */
  snprintf(query, DEF_QUERYSIZE, "UPDATE messages SET status=3 WHERE"
	   " mailbox_idnr = %llu AND deleted_flag=1 AND status<2", uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_expunge(): could not update messages in mailbox\n");
      if (msgids)
	my_free(*msgids);

      if (nmsgs)
	*nmsgs = 0;

      return -1;
    }

  return 0; /* success */
}


/*
 * db_movemsg()
 *
 * moves all msgs from one mailbox to another
 * returns -1 on error, 0 on success
 */
int db_movemsg(u64_t to, u64_t from)
{
  snprintf(query, DEF_QUERYSIZE, "UPDATE messages SET mailbox_idnr=%llu WHERE"
	   " mailbox_idnr = %llu", to, from);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_movemsg(): could not update messages in mailbox\n");
      return -1;
    }

  return 0; /* success */
}


/*
 * db_copymsg()
 *
 * copies a msg to a specified mailbox
 * returns 0 on success, -1 on failure
 */
int db_copymsg(u64_t msgid, u64_t destmboxid)
{
  u64_t newmsgid;
  time_t td;

  time(&td);              /* get time */

  /* start transaction */
  if (db_query("BEGIN WORK") == -1)
    {
      trace(TRACE_ERROR, "db_copymsg(): could not start transaction\n");
      return -1;
    }

  /* copy message info */
  snprintf(query, DEF_QUERYSIZE, "INSERT INTO messages (mailbox_idnr, messagesize, status, "
	   "deleted_flag, seen_flag, answered_flag, draft_flag, flagged_flag, recent_flag,"
	   " unique_id, internal_date) "
	   "SELECT %llu, messagesize, status, deleted_flag, seen_flag, answered_flag, "
	   "draft_flag, flagged_flag, recent_flag, '', internal_date "
	   "FROM messages WHERE message_idnr = %llu",
	   destmboxid, msgid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_copymsg(): could not copy message\n");

      db_query("ROLLBACK WORK"); /* close transaction */

      return -1;
    }

  newmsgid = db_insert_result("message_idnr");

  /* copy message blocks */
  snprintf(query, DEF_QUERYSIZE, "INSERT INTO messageblks (message_idnr, messageblk, blocksize) "
	   "SELECT %llu, messageblk, blocksize FROM messageblks "
	   "WHERE message_idnr = %llu ORDER BY messageblk_idnr", newmsgid, msgid);
  
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_copymsg(): could not insert message blocks\n");

      db_query("ROLLBACK WORK"); /* close transaction */

      return -1;
    }

  /* all done, validate new msg by creating a new unique id for the copied msg */
  snprintf(query, DEF_QUERYSIZE, "UPDATE messages SET unique_id=\'%lluA%lu\' "
	   "WHERE message_idnr=%llu", newmsgid, td, newmsgid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_copymsg(): could not set unique ID for copied msg\n");

      db_query("ROLLBACK WORK"); /* close transaction */

      return -1;
    }

  /* commit transaction */
  if (db_query("COMMIT WORK") == -1)
    {
      trace(TRACE_ERROR, "db_copymsg(): could not commit transaction\n");
      return -1;
    }
  
  return 0; /* success */
}




/*
 * db_getmailboxname()
 *
 * retrieves the name of a specified mailbox
 * *name should be large enough to contain the name (IMAP_MAX_MAILBOX_NAMELEN)
 * returns -1 on error, 0 on success
 */
int db_getmailboxname(u64_t uid, char *name)
{

  char *row;

  snprintf(query, DEF_QUERYSIZE, "SELECT name FROM mailboxes WHERE mailbox_idnr = %llu",uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_getmailboxname(): could not retrieve name\n");
      return -1;
    }

  if (PQntuples(res)>0)
    {

      row = PQgetvalue (res, 0, 0);

      if (!row)
        {
	  /* empty set, mailbox does not exist */
	  PQclear(res);
	  *name = '\0';
	  return 0;
        }

      strncpy(name, row, IMAP_MAX_MAILBOX_NAMELEN);
    }

  PQclear(res);
  return 0;
}


/*
 * db_setmailboxname()
 *
 * sets the name of a specified mailbox
 * returns -1 on error, 0 on success
 */
int db_setmailboxname(u64_t uid, const char *name)
{


  snprintf(query, DEF_QUERYSIZE, "UPDATE mailboxes SET name = '%s' WHERE mailbox_idnr = %llu",
	   name, uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_setmailboxname(): could not set name\n");
      return -1;
    }

  return 0;
}


/*
 * db_first_unseen()
 *
 * return the message UID of the first unseen msg or -1 on error
 */
u64_t db_first_unseen(u64_t uid)
{
  char *row;
  u64_t id;

  snprintf(query, DEF_QUERYSIZE, "SELECT message_idnr FROM messages WHERE mailbox_idnr = %llu "
	   "AND status<2 AND seen_flag = 0 AND unique_id != '' "
	   "ORDER BY message_idnr ASC LIMIT 1", uid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_first_unseen(): could not select messages\n");
      return (u64_t)(-1);
    }

  if (PQntuples(res)>0)
    {
      row = PQgetvalue(res,0,0);
      if (row)
	id = strtoull(row,NULL,10);
      else
	id = 0; /* none found */
    }
  else
    id = 0;

  PQclear(res);
  return id;
}


/*
 * db_subscribe()
 *
 * subscribes to a certain mailbox
 */
int db_subscribe(u64_t mboxid)
{
  snprintf(query, DEF_QUERYSIZE, "UPDATE mailboxes SET is_subscribed = 1 WHERE mailbox_idnr = %llu", 
	   mboxid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_subscribe(): could not update mailbox\n");
      return (-1);
    }

  return 0;
}


/*
 * db_unsubscribe()
 *
 * unsubscribes to a certain mailbox
 */
int db_unsubscribe(u64_t mboxid)
{


  snprintf(query, DEF_QUERYSIZE, "UPDATE mailboxes SET is_subscribed = 0 WHERE mailbox_idnr = %llu", 
	   mboxid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_unsubscribe(): could not update mailbox\n");
      return (-1);
    }

  return 0;
}


/*
 * db_get_msgflag()
 *
 * gets a flag value specified by 'name' (i.e. 'seen' would check the Seen flag)
 *
 * returns:
 *  -1  error
 *   0  flag not set
 *   1  flag set
 */
int db_get_msgflag(const char *name, u64_t msguid, u64_t mailboxuid)
{

  char flagname[DEF_QUERYSIZE/2]; /* should be sufficient ;) */
  int val=0;
  char *row;

  /* determine flag */
  if (strcasecmp(name,"seen") == 0)
    snprintf(flagname, DEF_QUERYSIZE/2, "seen_flag");
  else if (strcasecmp(name,"deleted") == 0)
    snprintf(flagname, DEF_QUERYSIZE/2, "deleted_flag");
  else if (strcasecmp(name,"answered") == 0)
    snprintf(flagname, DEF_QUERYSIZE/2, "answered_flag");
  else if (strcasecmp(name,"flagged") == 0)
    snprintf(flagname, DEF_QUERYSIZE/2, "flagged_flag");
  else if (strcasecmp(name,"recent") == 0)
    snprintf(flagname, DEF_QUERYSIZE/2, "recent_flag");
  else if (strcasecmp(name,"draft") == 0)
    snprintf(flagname, DEF_QUERYSIZE/2, "draft_flag");
  else
    return 0; /* non-existent flag is not set */

 snprintf(query, DEF_QUERYSIZE, "SELECT %s FROM messages WHERE "
	  "message_idnr = %llu AND status<2 AND unique_id != '' "
	  "AND mailbox_idnr = %llu", flagname, msguid, mailboxuid);
 
 if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_msgflag(): could not select message\n");
      return (-1);
    }

  if (PQntuples(res)>0)
    {
      row = PQgetvalue (res, 0, 0);
      if (row)
	val = atoi(row);
      else
	val = 0; /* none found */
    }

  PQclear(res);
  return val;
}


/*
 * db_get_msgflag_all()
 *
 * gets all flags for a specified message
 *
 * flags are placed in *flags, an array build up according to IMAP_FLAGS 
 * (see top of imapcommands.c)
 *
 * returns:
 *  -1  error
 *   0  success
 */
int db_get_msgflag_all(u64_t msguid, u64_t mailboxuid, int *flags)
{
  char *row;
  int i;

  memset(flags, 0, sizeof(int) * IMAP_NFLAGS);

  snprintf(query, DEF_QUERYSIZE, "SELECT seen_flag, answered_flag, deleted_flag, "
	   "flagged_flag, draft_flag, recent_flag FROM messages WHERE "
	   "message_idnr = %llu AND status<2 AND unique_id != '' "
	   "AND mailbox_idnr = %llu", msguid, mailboxuid);
 
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_msgflag_all(): could not select message\n");
      return (-1);
    }

  if (PQntuples(res)>0)
    {
      for (i=0; i<IMAP_NFLAGS; i++)
	{
	  row = PQgetvalue(res, 0, i);
	  if (row && row[0] != '0') flags[i] = 1;
	}
    }
 
   PQclear(res);
   return 0;
}


/*
 * db_get_msgflag_all_range()
 * 
 * as db_get_msgflag_all() but queries for a range of messages.
 * *flags should be freed by the client.
 *
 * upon success, *flags is an array containing resultsetlen*IMAP_NFLAGS items
 * (a lineair dump of all the flags)
 *
 * for example, to retrieve the flags for the 2nd message you should index like
 *
 * flags[(2-1) * IMAP_NFLAGS + ___flag you want, range 0-5___]
 * remember the minus 1: we start counting at zero
 * 
 * returns 0 on success, -1 on dbase error, -2 on memory error
 */
int db_get_msgflag_all_range(u64_t msguidlow, u64_t msguidhigh, u64_t mailboxuid, 
			     int **flags, unsigned *resultsetlen)
{
  unsigned nrows, i, j;
  char *row;

  *flags = 0;
  *resultsetlen = 0;

  snprintf(query, DEF_QUERYSIZE, "SELECT seen_flag, answered_flag, deleted_flag, "
	   "flagged_flag, draft_flag, recent_flag FROM messages WHERE "
	   "message_idnr >= %llu AND message_idnr <= %llu AND status<2 AND unique_id != '' "
	   "AND mailbox_idnr = %llu", msguidlow, msguidhigh, mailboxuid);
 
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_msgflag_all_range(): could not select message\n");
      return (-1);
    }

  if ((nrows = PQntuples(res)) == 0)
    {
      return 0;
    }

  *flags = (int*)my_malloc(nrows * IMAP_NFLAGS * sizeof(int));
  if (! (*flags))
    {
      trace(TRACE_ERROR, "db_get_msgflag_all_range(): out of mem\n");
      PQclear(res);
      return -2;
    }

  for (i=0; i<nrows; i++)
    {
      for (j=0; j<IMAP_NFLAGS; j++)
	{
	  row = PQgetvalue(res, i, j);
	  (*flags)[i * IMAP_NFLAGS +j] = (row && row[0] != '0') ? 1 : 0;
	}
    }

  PQclear(res);
  *resultsetlen = nrows;
  return 0;
}


/*
 * db_set_msgflag()
 *
 * sets flags for a message
 * Flag set is specified in *flags; indexed as follows (see top of imapcommands.c file)
 * [0] "Seen", 
 * [1] "Answered", 
 * [2] "Deleted", 
 * [3] "Flagged", 
 * [4] "Draft", 
 * [5] "Recent"
 *
 * a value of zero represents 'off', a value of one "sets" the flag
 *
 * action_type can be one of the IMAP_FLAG_ACTIONS:
 *
 * IMAPFA_REPLACE  new set will be exactly as *flags with '1' to set, '0' to clear
 * IMAPFA_ADD      set all flags which have '1' as value in *flags
 * IMAPFA_REMOVE   clear all flags which have '1' as value in *flags
 * 
 * returns:
 *  -1  error
 *   0  success 
 */
int db_set_msgflag(u64_t msguid, u64_t mailboxuid, int *flags, int action_type)
{
  int i,placed=0;
  int left = DEF_QUERYSIZE - sizeof("answered_flag=1,");

  snprintf(query, DEF_QUERYSIZE, "UPDATE messages SET ");
  
  for (i=0; i<IMAP_NFLAGS; i++)
    {
      switch (action_type)
	{
	case IMAPFA_ADD:
	  if (flags[i] > 0)
	    {
	      strncat(query, db_flag_desc[i], left);
	      left -= sizeof("answered_flag");
	      strncat(query, "=1,", left);
	      left -= sizeof(" =1, ");
	      placed = 1;
	    }
	  break;

	case IMAPFA_REMOVE:
	  if (flags[i] > 0)
	    {
	      strncat(query, db_flag_desc[i], left);
	      left -= sizeof("answered_flag");
	      strncat(query, "=0,", left);
	      left -= sizeof("=0,");
	      placed = 1;
	    }
	  break;

	case IMAPFA_REPLACE:
	  strncat(query, db_flag_desc[i], left);
	  left -= sizeof("answered_flag");

	  if (flags[i] == 0)
	    strncat(query, "=0,", left);
	  else
	    strncat(query, "=1,", left);

	  left -= sizeof("=1,");
	  placed = 1;

	  break;
	}
    }

  if (!placed) 
    return 0; /* nothing to update */
  
  
  /* last character in string is comma, replace it --> strlen()-1 */
  snprintf(&query[strlen(query)-1], left, " WHERE "
	   "message_idnr = %llu AND status<2 "
	   "AND mailbox_idnr = %llu", msguid, mailboxuid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_set_msgflag(): could not set flag\n");
      return (-1);
    }

  return 0;
}



/*
 * db_set_msgflag_range()
 *
 * as db_set_msgflag() but acts on a range of messages
 * 
 * returns:
 *  -1  error
 *   0  success 
 */
int db_set_msgflag_range(u64_t msguidlow, u64_t msguidhigh, u64_t mailboxuid, 
		   int *flags, int action_type)
{
  int i,placed=0;
  int left = DEF_QUERYSIZE - sizeof("answered_flag=1,");

  snprintf(query, DEF_QUERYSIZE, "UPDATE messages SET ");
  
  for (i=0; i<IMAP_NFLAGS; i++)
    {
      switch (action_type)
	{
	case IMAPFA_ADD:
	  if (flags[i] > 0)
	    {
	      strncat(query, db_flag_desc[i], left);
	      left -= sizeof("answered_flag");
	      strncat(query, "=1,", left);
	      left -= sizeof(" =1, ");
	      placed = 1;
	    }
	  break;

	case IMAPFA_REMOVE:
	  if (flags[i] > 0)
	    {
	      strncat(query, db_flag_desc[i], left);
	      left -= sizeof("answered_flag");
	      strncat(query, "=0,", left);
	      left -= sizeof("=0,");
	      placed = 1;
	    }
	  break;

	case IMAPFA_REPLACE:
	  strncat(query, db_flag_desc[i], left);
	  left -= sizeof("answered_flag");

	  if (flags[i] == 0)
	    strncat(query, "=0,", left);
	  else
	    strncat(query, "=1,", left);

	  left -= sizeof("=1,");
	  placed = 1;

	  break;
	}
    }

  if (!placed) 
    return 0; /* nothing to update */
  
  
  /* last character in string is comma, replace it --> strlen()-1 */
  snprintf(&query[strlen(query)-1], left, " WHERE "
	   "message_idnr >= %llu AND message_idnr <= %llu AND "
	   "status<2 AND mailbox_idnr = %llu", msguidlow, msguidhigh, mailboxuid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_set_msgflag_range(): could not set flag\n");
      return (-1);
    }

  return 0;
}



/*
 * db_get_msgdate()
 *
 * retrieves msg internal date; 'date' should be large enough (IMAP_INTERNALDATE_LEN)
 * returns -1 on error, 0 on success
 */
int db_get_msgdate(u64_t mailboxuid, u64_t msguid, char *date)
{

  char *row;

  snprintf(query, DEF_QUERYSIZE, "SELECT internal_date FROM messages WHERE mailbox_idnr = %llu "
	   "AND message_idnr = %llu AND unique_id!=''", mailboxuid, msguid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_msgdate(): could not get message\n");
      return (-1);
    }

  if (PQntuples(res)>0)
    {
      row = PQgetvalue (res, 0, 0);
      if (row)
        {
	  strncpy(date, row, IMAP_INTERNALDATE_LEN);
	  date[IMAP_INTERNALDATE_LEN - 1] = '\0';
        }
      else
        {
	  /* no date ? let's say eelco's birthdate :) */
	  strncpy(date, "1976-09-11 06:45:00", IMAP_INTERNALDATE_LEN);
	  date[IMAP_INTERNALDATE_LEN - 1] = '\0';
        }
    }

  PQclear(res);
  return 0;
}


/*
 * db_set_rfcsize()
 *
 * sets the RFCSIZE field for a message.
 *
 * returns -1 on failure, 0 on success
 */
int db_set_rfcsize(u64_t size, u64_t msguid, u64_t mailboxuid)
{
  snprintf(query, DEF_QUERYSIZE, "UPDATE messages SET rfcsize = %llu "
	   "WHERE message_idnr = %llu AND mailbox_idnr = %llu",
	   size, msguid, mailboxuid);
  
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_set_rfcsize(): could not insert RFC size into table\n");
      return -1;
    }

  return 0;
}


u64_t db_get_rfcsize(u64_t msguid, u64_t mailboxuid)
{
  u64_t size;

  snprintf(query, DEF_QUERYSIZE, "SELECT rfcsize FROM messages WHERE message_idnr = %llu "
	   "AND status<2 AND unique_id != '' AND mailbox_idnr = %llu", msguid, mailboxuid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_rfcsize(): could not fetch RFC size from table\n");
      return -1;
    }
  
  if (PQntuples(res) < 1)
    {
      trace(TRACE_ERROR, "db_get_rfcsize(): message not found\n");
      return -1;
    }

  size = strtoull( PQgetvalue(res, 0, 0), NULL, 10);
  return size;
}



/*
 * db_get_msginfo_range()
 *
 * retrieves message info in a single query for a range of messages.
 *
 * returns 0 on succes, -1 on dbase error, -2 on memory error
 *
 * caller should free *result
 */
int db_get_msginfo_range(u64_t msguidlow, u64_t msguidhigh, u64_t mailboxuid,
			 int getflags, int getinternaldate, int getsize, int getuid,
			 msginfo_t **result, unsigned *resultsetlen)
{
  unsigned nrows, i, j;
  char *row;

  *result = 0;
  *resultsetlen = 0;

  snprintf(query, DEF_QUERYSIZE, "SELECT seen_flag, answered_flag, deleted_flag, "
	   "flagged_flag, draft_flag, recent_flag, internal_date, rfcsize, message_idnr "
	   "FROM messages WHERE "
	   "message_idnr >= %llu AND message_idnr <= %llu AND mailbox_idnr = %llu"
	   "AND status<2 AND unique_id != '' "
	   "ORDER BY message_idnr ASC", msguidlow, msguidhigh, mailboxuid);
 
  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_msginfo_range(): could not select message\n");
      return (-1);
    }

   if ((nrows = PQntuples(res)) == 0)
    {
      return 0;
    }

   *result = (msginfo_t*)my_malloc(nrows * sizeof(msginfo_t));
   if (! (*result)) 
     {
      trace(TRACE_ERROR,"db_get_msginfo_range(): out of memory\n");
      PQclear(res);
      return -2;
     }

   memset(*result, 0, nrows * sizeof(msginfo_t));

   for (i=0; i<nrows; i++)
     {
       if (getflags)
	 {
	   for (j=0; j<IMAP_NFLAGS; j++)
	     {
	       row = PQgetvalue(res, i, j);
	       (*result)[i].flags[j] = (row && row[0] != '0') ? 1 : 0;
	     }
	 }

       if (getinternaldate)
	 {
	   row = PQgetvalue(res, i, IMAP_NFLAGS);
	   strncpy((*result)[i].internaldate, 
		   row ? row : "2001-17-02 23:59:59", 
		   IMAP_INTERNALDATE_LEN);

	 }

       if (getsize)
	 {
	   row = PQgetvalue(res, i, IMAP_NFLAGS+1);
	   (*result)[i].rfcsize = strtoull(row ? row : "0", NULL, 10);
	 }

       if (getuid)
	 {
	   row = PQgetvalue(res, i, IMAP_NFLAGS+2);
	   if (!row)
	     {
	       my_free(*result);
	       *result = 0;
	       PQclear(res);

	       trace(TRACE_ERROR,"db_get_msginfo_range(): message has no UID??");
	       return -1;
	     }
	   (*result)[i].uid = strtoull(row, NULL, 10);
	 }
     }

   PQclear(res);
   *resultsetlen = i; /* should _always_ be equal to nrows */

   return 0;
}



/*
 * db_get_main_header()
 *
 * builds a list containing the fields of the main header (== the first) of
 * a message
 *
 * returns:
 * 0   success
 * -1  dbase error
 * -2  out of memory
 * -3  parse error
 */
int db_get_main_header(u64_t msguid, struct list *hdrlist)
{
  u64_t dummy = 0, sizedummy = 0;
  int result;
  char *row=0;

  if (!hdrlist)
    return 0;

  if (hdrlist->start)
    list_freelist(&hdrlist->start);

  list_init(hdrlist);

  snprintf(query, DEF_QUERYSIZE, "SELECT messageblk FROM messageblks WHERE "
	   "message_idnr = %llu ORDER BY messageblk_idnr LIMIT 1", msguid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_get_main_header(): could not get message header\n");
      return (-1);
    }

  if (PQntuples(res)>0)
    {
      row = PQgetvalue (res, 0, 0);
      if (!row)
        {
	  /* no header for this message ?? */
	  trace (TRACE_ERROR,"db_get_main_header(): error, no header for this message?");
	  PQclear(res);
	  return -1;
        }
    }
  else
    {
      /* no blocks? */
      trace (TRACE_ERROR,"db_get_main_header(): error, no msgblocks from select");
      PQclear(res);
      return -1;
    }

  result = mime_readheader(row, &dummy, hdrlist, &sizedummy);

  PQclear(res);

  if (result == -1)
    {
      /* parse error */
      trace(TRACE_ERROR,"db_get_main_header(): error parsing header of message %llu\n",msguid);
      if (hdrlist->start)
        {
	  list_freelist(&hdrlist->start);
	  list_init(hdrlist);
        }

      return -3;
    }

  if (result == -2)
    {
      /* out of memory */
      trace(TRACE_ERROR,"db_get_main_header(): out of memory\n");
      if (hdrlist->start)
        {
	  list_freelist(&hdrlist->start);
	  list_init(hdrlist);
        }

      return -2;
    }

  /* success ! */
  return 0;
}




/*
 * searches the given range within a msg for key
 */
int db_search_range(db_pos_t start, db_pos_t end, const char *key, u64_t msguid)
{
  int i,startpos,endpos,j;
  int distance;

  char *row;

  if (start.block > end.block)
    {
      trace(TRACE_ERROR,"db_search_range(): bad range specified\n");
      return 0;
    }

  if (start.block == end.block && start.pos > end.pos)
    {
      trace(TRACE_ERROR,"db_search_range(): bad range specified\n");
      return 0;
    }

  snprintf(query, DEF_QUERYSIZE, "SELECT messageblk FROM messageblks WHERE message_idnr = %llu"
	   " ORDER BY messageblk_idnr", 
	   msguid);

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_search_range(): could not get message\n");
      return 0;
    }

  if (PQntuples(res)>0)
    {
      trace (TRACE_ERROR,"db_search_range(): bad range specified\n");
      PQclear(res);
      return 0;
    }
        
  row = PQgetvalue (res, start.block, 0);

  if (!row)
    {
      trace(TRACE_ERROR,"db_search_range(): bad range specified\n");
      PQclear(res);
      return 0;
    }

  /* just one block? */
  if (start.block == end.block)
    {
      for (i=start.pos; i<=end.pos-strlen(key); i++)
        {
	  if (strncasecmp(&row[i], key, strlen(key)) == 0)
            {
	      PQclear(res);
	      return 1;
            }
        }

      PQclear(res);
      return 0;
    }


  /* 
   * multiple block range specified
   */

  for (i=start.block; i<=end.block; i++)
    {
      if (!row)
        {
	  trace(TRACE_ERROR,"db_search_range(): bad range specified\n");
	  PQclear(res);
	  return 0;
        }

      startpos = (i == start.block) ? start.pos : 0;
      endpos   = (i == end.block) ? end.pos+1 : PQgetlength(res,i,0);

      distance = endpos - startpos;

      for (j=0; j<distance-strlen(key); j++)
        {
	  if (strncasecmp(&row[i], key, strlen(key)) == 0)
            {
	      PQclear(res);
	      return 1;
            }
        }

      row = PQgetvalue (res, i, 0); /* fetch next row */
    }

  PQclear(res);

  return 0;
}


/*
 * db_mailbox_msg_match()
 *
 * checks if a msg belongs to a mailbox 
 */
int db_mailbox_msg_match(u64_t mailboxuid, u64_t msguid)
{

  int val;

  snprintf(query, DEF_QUERYSIZE, "SELECT message_idnr FROM messages WHERE message_idnr = %llu AND "
	   "mailbox_idnr = %llu AND status<002 AND unique_id!=''", msguid, mailboxuid); 

  if (db_query(query) == -1)
    {
      trace(TRACE_ERROR, "db_mailbox_msg_match(): could not get message\n");
      return (-1);
    }

  val = PQntuples(res);
  PQclear(res);

  return val;
}

