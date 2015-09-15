/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 2000-2015, University of Amsterdam
			      VU University Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <SWI-Stream.h>
#include <pthread.h>
#include "bdb4pl.h"
#include <sys/types.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "error.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>

#ifdef O_DEBUG
#define DEBUG(g) g
#else
#define DEBUG(g) (void)0
#endif

static atom_t ATOM_read;
static atom_t ATOM_update;
static atom_t ATOM_true;
static atom_t ATOM_false;
static atom_t ATOM_btree;
static atom_t ATOM_hash;
static atom_t ATOM_recno;
static atom_t ATOM_unknown;
static atom_t ATOM_duplicates;
static atom_t ATOM_mp_mmapsize;
static atom_t ATOM_mp_size;
static atom_t ATOM_home;
static atom_t ATOM_config;
static atom_t ATOM_type;
static atom_t ATOM_database;
static atom_t ATOM_key;
static atom_t ATOM_value;
static atom_t ATOM_term;
static atom_t ATOM_atom;
static atom_t ATOM_c_blob;
static atom_t ATOM_c_string;
static atom_t ATOM_c_long;
static atom_t ATOM_server;
static atom_t ATOM_server_timeout;
static atom_t ATOM_client_timeout;

static functor_t FUNCTOR_type1;

static DB_ENV   *db_env;		/* default environment */
static u_int32_t db_flags;		/* Create flag for db_env */

#define mkfunctor(n, a) PL_new_functor(PL_new_atom(n), a)

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
TBD: Thread-safe version

	- Deal with transactions and threads
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#if 1
#define NOSIG(code) { code; }
#else
#define NOSIG(code) \
	{ sigset_t new, old; \
	  sigemptyset(&new); \
	  sigaddset(&new, SIGINT); \
	  sigprocmask(SIG_BLOCK, &new, &old); \
	  code; \
	  sigprocmask(SIG_SETMASK, &old, NULL); \
	}
#endif

#define TheTXN current_transaction()

static void
initConstants(void)
{ ATOM_read	      =	PL_new_atom("read");
  ATOM_update	      =	PL_new_atom("update");
  ATOM_true	      =	PL_new_atom("true");
  ATOM_false	      =	PL_new_atom("false");
  ATOM_btree	      =	PL_new_atom("btree");
  ATOM_hash	      =	PL_new_atom("hash");
  ATOM_recno	      =	PL_new_atom("recno");
  ATOM_unknown	      =	PL_new_atom("unknown");
  ATOM_duplicates     =	PL_new_atom("duplicates");
  ATOM_mp_size	      =	PL_new_atom("mp_size");
  ATOM_mp_mmapsize    =	PL_new_atom("mp_mmapsize");
  ATOM_home	      =	PL_new_atom("home");
  ATOM_config	      =	PL_new_atom("config");
  ATOM_type	      =	PL_new_atom("type");
  ATOM_database	      =	PL_new_atom("database");
  ATOM_key	      =	PL_new_atom("key");
  ATOM_value	      =	PL_new_atom("value");
  ATOM_term	      =	PL_new_atom("term");
  ATOM_atom	      =	PL_new_atom("atom");
  ATOM_c_blob	      =	PL_new_atom("c_blob");
  ATOM_c_string	      =	PL_new_atom("c_string");
  ATOM_c_long	      =	PL_new_atom("c_long");
  ATOM_server	      =	PL_new_atom("server");
  ATOM_server_timeout =	PL_new_atom("server_timeout");
  ATOM_client_timeout =	PL_new_atom("client_timeout");

  FUNCTOR_type1	      =	mkfunctor("type", 1);
}

static void cleanup(void);


		 /*******************************
		 *	  SYMBOL WRAPPER	*
		 *******************************/

static void
acquire_db(atom_t symbol)
{ dbh *db = PL_blob_data(symbol, NULL, NULL);
  db->symbol = symbol;
}


static int
release_db(atom_t symbol)
{ dbh *db = PL_blob_data(symbol, NULL, NULL);
  DB *d;

  if ( (d=db->db) )
  { db->db = NULL;
    d->close(d, 0);
  }

  PL_free(db);

  return TRUE;
}

static int
compare_dbs(atom_t a, atom_t b)
{ dbh *ara = PL_blob_data(a, NULL, NULL);
  dbh *arb = PL_blob_data(b, NULL, NULL);

  return ( ara > arb ?  1 :
	   ara < arb ? -1 : 0
	 );
}

static int
write_db(IOSTREAM *s, atom_t symbol, int flags)
{ dbh *db = PL_blob_data(symbol, NULL, NULL);

  Sfprintf(s, "<db>(%p)", db);

  return TRUE;
}

static PL_blob_t db_blob =
{ PL_BLOB_MAGIC,
  PL_BLOB_NOCOPY,
  "db",
  release_db,
  compare_dbs,
  write_db,
  acquire_db
};


static int
get_db(term_t t, dbh **db)
{ PL_blob_t *type;
  void *data;

  if ( PL_get_blob(t, &data, NULL, &type) && type == &db_blob)
  { dbh *p = data;

    if ( p->symbol )
    { *db = p;

      return TRUE;
    }

    PL_permission_error("access", "closed_db", t);
    return FALSE;
  }

  return PL_type_error("db", t);
}




static int
unify_db(term_t t, dbh *db)
{ return PL_unify_blob(t, db, sizeof(*db), &db_blob);
}

		 /*******************************
		 *	   DATA EXCHANGE	*
		 *******************************/

static int
unify_dbt(term_t t, dtype type, DBT *dbt)
{ switch( type )
  { case D_TERM:
    { term_t r = PL_new_term_ref();

      PL_recorded_external(dbt->data, r);
      return PL_unify(t, r);
    }
    case D_ATOM:
      return PL_unify_chars(t, PL_ATOM|REP_UTF8, dbt->size, dbt->data);
    case D_CBLOB:
      return PL_unify_chars(t, PL_STRING|REP_ISO_LATIN_1, dbt->size, dbt->data);
    case D_CSTRING:
      return PL_unify_chars(t, PL_ATOM|REP_UTF8, (size_t)-1, dbt->data);
    case D_CLONG:
    { long *v = dbt->data;
      return PL_unify_integer(t, *v);
    }
  }
  assert(0);
  return FALSE;
}


static int
get_dbt(term_t t, dtype type, DBT *dbt)
{ memset(dbt, 0, sizeof(*dbt));

  switch(type)
  { case D_TERM:
    { size_t len;

      dbt->data = PL_record_external(t, &len);
      dbt->size = len;
      return TRUE;
    }
    case D_ATOM:
    { size_t len;
      char *s;

      if ( PL_get_nchars(t, &len, &s,
			 CVT_ATOM|CVT_EXCEPTION|REP_UTF8|BUF_MALLOC) )
      { dbt->data = s;
	dbt->size = len;

	return TRUE;
      } else
	return FALSE;
    }
    case D_CBLOB:
    { size_t len;
      char *s;

      if ( PL_get_nchars(t, &len, &s,
			 CVT_ATOM|CVT_STRING|CVT_EXCEPTION|
			 REP_ISO_LATIN_1|BUF_MALLOC) )
      { dbt->data = s;
	dbt->size = len;

	return TRUE;
      } else
	return FALSE;
    }
    case D_CSTRING:
    { size_t len;
      char *s;

      if ( PL_get_nchars(t, &len, &s,
			 CVT_ATOM|CVT_STRING|CVT_EXCEPTION|REP_UTF8|BUF_MALLOC) )
      { dbt->data = s;
	dbt->size = len+1;		/* account for terminator */

	return TRUE;
      } else
	return FALSE;
    }
    case D_CLONG:
    { long v;

      if ( PL_get_long_ex(t, &v) )
      {	long *d = malloc(sizeof(long));

	*d = v;
	dbt->data = d;
	dbt->size = sizeof(long);

	return TRUE;
      } else
	return FALSE;
    }
  }
  assert(0);
  return FALSE;
}


static void
free_dbt(DBT *dbt, dtype type)
{ switch ( type )
  { case D_TERM:
      PL_erase_external(dbt->data);
      break;
    case D_ATOM:
    case D_CBLOB:
    case D_CSTRING:
      PL_free(dbt->data);
      break;
    case D_CLONG:
      free(dbt->data);
  }
}


static void
free_result_dbt(DBT *dbt)
{ if ( dbt->flags & DB_DBT_MALLOC )
    free(dbt->data);
}


int
db_status(int rval)
{ switch( rval )
  { case 0:
      return TRUE;
    case DB_LOCK_DEADLOCK:
      Sdprintf("Throwing deadlock exception\n");
      return pl_error(ERR_PACKAGE_ID, "db", "deadlock", db_strerror(rval));
    case DB_RUNRECOVERY:
      Sdprintf("Need recovery\n");
      return pl_error(ERR_PACKAGE_ID, "db", "run_recovery", db_strerror(rval));
  }

  if ( rval < 0 )
  { DEBUG(Sdprintf("DB error: %s\n", db_strerror(rval)));
    return FALSE;			/* normal failure */
  }

  DEBUG(Sdprintf("Throwing error: %s\n", db_strerror(rval)));
  return pl_error(ERR_PACKAGE_INT, "db", rval, db_strerror(rval));
}


static int
db_type(term_t t, int *type)
{ term_t tail = PL_copy_term_ref(t);
  term_t head = PL_new_term_ref();

  while( PL_get_list(tail, head, tail) )
  { if ( PL_is_functor(head, FUNCTOR_type1) )
    { term_t a0 = PL_new_term_ref();
      atom_t tp;

      _PL_get_arg(1, head, a0);

      if ( !PL_get_atom_ex(a0, &tp) )
	return FALSE;
      if ( tp == ATOM_btree )
	*type = DB_BTREE;
      else if ( tp == ATOM_hash )
	*type = DB_HASH;
      else if ( tp == ATOM_recno )
	*type = DB_RECNO;
      else if ( tp == ATOM_unknown )
	*type = DB_UNKNOWN;
      else
	return PL_domain_error("db_type", a0);

      return TRUE;
    }
  }

  return TRUE;
}


static int
get_dtype(term_t t, dtype *type)
{ atom_t a;

  if ( !PL_get_atom_ex(t, &a) )
    return FALSE;
  if ( a == ATOM_term )
    *type = D_TERM;
  else if ( a == ATOM_atom )
    *type = D_ATOM;
  else if ( a == ATOM_c_blob )
    *type = D_CBLOB;
  else if ( a == ATOM_c_string )
    *type = D_CSTRING;
  else if ( a == ATOM_c_long )
    *type = D_CLONG;
  else
    return PL_domain_error("type", t);

  return TRUE;
}


static int
db_options(term_t t, dbh *dbh, char **subdb)
{ term_t tail = PL_copy_term_ref(t);
  term_t head = PL_new_term_ref();
  int flags = 0;

  dbh->key_type   = D_TERM;
  dbh->value_type = D_TERM;

  while( PL_get_list(tail, head, tail) )
  { atom_t name;
    int arity;

    if ( PL_get_name_arity(head, &name, &arity) )
    { if ( arity == 1 )
      { term_t a0 = PL_new_term_ref();

	_PL_get_arg(1, head, a0);
	if ( name == ATOM_duplicates )
	{ int v;

	  if ( !PL_get_bool_ex(a0, &v) )
	    return FALSE;

	  if ( v )
	  { flags |= DB_DUP;
	    dbh->duplicates = TRUE;
	  }
	} else if ( name == ATOM_database )
	{ if ( !PL_get_chars(a0, subdb,
			     CVT_ATOM|CVT_STRING|CVT_EXCEPTION|REP_UTF8) )
	    return FALSE;
	} else if ( name == ATOM_key )
	{ if ( !get_dtype(a0, &dbh->key_type) )
	    return FALSE;
	} else if ( name == ATOM_value )
	{ if ( !get_dtype(a0, &dbh->value_type) )
	    return FALSE;
	} else if ( name == ATOM_type )
	    ;  /* skip [ ... type(_) ... ]  because it's handled by db_type */
	else
	    return PL_domain_error("db_option", head);
      } else
	  return PL_domain_error("db_option", head);
    }
  }

  if ( !PL_get_nil_ex(tail) )
    return FALSE;

  if ( flags )
  { int rval;

    if ( (rval=dbh->db->set_flags(dbh->db, flags)) )
      return db_status(rval);
  }


  return TRUE;
}


static foreign_t
pl_bdb_open(term_t file, term_t mode, term_t handle, term_t options)
{ char *fname;
  int flags;
  int m = 0666;
  int type = DB_BTREE;
  dbh *dbh;
  atom_t a;
  int rval;
  char *subdb = NULL;

  if ( !PL_get_file_name(file, &fname, PL_FILE_OSPATH) )
    return FALSE;

  if ( !PL_get_atom_ex(mode, &a) )		/* process mode */
    return FALSE;
  if ( a == ATOM_read )
    flags = DB_RDONLY;
  else if ( a == ATOM_update )
    flags = DB_CREATE;
  else
    return PL_domain_error("io_mode", mode);

  dbh = calloc(1, sizeof(*dbh));
  dbh->magic = DBH_MAGIC;
  NOSIG(rval=db_create(&dbh->db, db_env, 0));
  if ( rval )
    return db_status(rval);

  DEBUG(Sdprintf("New DB at %p\n", dbh->db));

  if ( !db_type(options, &type) ||
       !db_options(options, dbh, &subdb) )
  { dbh->db->close(dbh->db, 0);
    return FALSE;
  }

#ifdef DB41
  if ( (db_flags&DB_INIT_TXN) )
    flags |= DB_AUTO_COMMIT;
  NOSIG(rval=dbh->db->open(dbh->db, NULL, fname, subdb, type, flags, m));
#else
  NOSIG(rval=dbh->db->open(dbh->db, fname, subdb, type, flags, m));
#endif

  if ( rval )
  { dbh->db->close(dbh->db, 0);
    return db_status(rval);
  }

  return unify_db(handle, dbh);
}


static foreign_t
pl_bdb_close(term_t handle)
{ dbh *db;

  if ( get_db(handle, &db) )
  { int rval;

    DEBUG(Sdprintf("Close DB at %p\n", db->db));
    NOSIG(rval = db->db->close(db->db, 0);
	  db->symbol = 0);

    return db_status(rval);
  }

  return FALSE;
}

static foreign_t
pl_bdb_is_open(term_t t)
{ PL_blob_t *type;
  void *data;

  if ( PL_get_blob(t, &data, NULL, &type) && type == &db_blob)
  { dbh *p = data;

    if ( p->symbol )
      return TRUE;

    return FALSE;
  }

  return PL_type_error("db", t);
}


		 /*******************************
		 *	   TRANSACTIONS		*
		 *******************************/

static pthread_key_t transaction_key;

typedef struct transaction
{ DB_TXN *tid;				/* transaction id */
  struct transaction *parent;		/* parent id */
} transaction;

typedef struct transaction_stack
{ transaction *top;
} transaction_stack;

static transaction_stack *
my_tr_stack(void)
{ transaction_stack *stack;

  if ( (stack=pthread_getspecific(transaction_key)) )
    return stack;

  if ( (stack=calloc(1,sizeof(*stack))) )
  { pthread_setspecific(transaction_key, stack);
    return stack;
  }

  PL_resource_error("memory");
  return NULL;
}

static void
free_transaction_stack(void *ptr)
{ transaction_stack *stack = ptr;

  assert(stack->top == NULL);
  free(stack);
}


static int
begin_transaction(transaction *t)
{ if ( db_env && (db_flags&DB_INIT_TXN) )
  { int rval;
    DB_TXN *pid, *tid;
    transaction_stack *stack;

    if ( !(stack=my_tr_stack()) )
      return FALSE;

    if ( stack->top )
      pid = stack->top->tid;
    else
      pid = NULL;

    if ( (rval=db_env->txn_begin(db_env, pid, &tid, 0)) )
      return db_status(rval);

    t->parent = stack->top;
    t->tid = tid;
    stack->top = t;

    return TRUE;
  }

  return pl_error(ERR_PACKAGE_INT, "db", 0,
		  "Not initialized for transactions");
}


static int
commit_transaction(transaction *t)
{ transaction_stack *stack = my_tr_stack();
  int rval;

  assert(stack);
  assert(stack->top == t);

  stack->top = t->parent;

  if ( (rval=t->tid->commit(t->tid, 0)) )
    return db_status(rval);

  return TRUE;
}


static int
abort_transaction(transaction *t)
{ transaction_stack *stack = my_tr_stack();
  int rval;

  assert(stack);
  assert(stack->top == t);

  stack->top = t->parent;

  if ( (rval=t->tid->abort(t->tid)) )
    return db_status(rval);

  return TRUE;
}


static DB_TXN *
current_transaction(void)
{ transaction_stack *stack;

  if ( (stack=pthread_getspecific(transaction_key)) &&
       stack->top )
    return stack->top->tid;

  return NULL;
}


static foreign_t
pl_bdb_transaction(term_t goal)
{ static predicate_t call1;
  qid_t qid;
  int rval;
  struct transaction tr;

  if ( !call1 )
    call1 = PL_predicate("call", 1, "user");

  NOSIG(rval=begin_transaction(&tr));
  if ( !rval )
    return FALSE;

  qid = PL_open_query(NULL, PL_Q_PASS_EXCEPTION, call1, goal);
  rval = PL_next_solution(qid);
  if ( rval )
  { PL_cut_query(qid);
    NOSIG(rval=commit_transaction(&tr));
    return rval;
  } else
  { PL_cut_query(qid);

    NOSIG(rval=abort_transaction(&tr));
    if ( !rval )
      return FALSE;

    return FALSE;
  }
}


		 /*******************************
		 *	     DB ACCESS		*
		 *******************************/

static foreign_t
pl_bdb_put(term_t handle, term_t key, term_t value)
{ DBT k, v;
  dbh *db;
  int flags = 0;
  int rval;

  if ( !get_db(handle, &db) )
    return FALSE;

  if ( !get_dbt(key, db->key_type, &k) ||
       !get_dbt(value, db->value_type, &v) )
    return FALSE;

  NOSIG(rval = db_status(db->db->put(db->db, TheTXN, &k, &v, flags)));
  free_dbt(&k, db->key_type);
  free_dbt(&v, db->value_type);

  return rval;
}


static foreign_t
pl_bdb_del2(term_t handle, term_t key)
{ DBT k;
  dbh *db;
  int flags = 0;			/* current no flags in DB */
  int rval;

  if ( !get_db(handle, &db) )
    return FALSE;

  if ( !get_dbt(key, db->key_type, &k) )
    return FALSE;

  NOSIG(rval = db_status(db->db->del(db->db, TheTXN, &k, flags)));
  free_dbt(&k, db->key_type);

  return rval;
}


static int
equal_dbt(DBT *a, DBT *b)
{ if ( a->size == b->size )
  { if ( a->data == b->data )
      return TRUE;
    if ( memcmp(a->data, b->data, a->size) == 0 )
      return TRUE;
  }

  return FALSE;
}


static foreign_t
pl_bdb_getall(term_t handle, term_t key, term_t value)
{ DBT k, v;
  dbh *db;
  int rval;

  if ( !get_db(handle, &db) )
    return FALSE;

  if ( !get_dbt(key, db->key_type, &k) )
    return FALSE;
  memset(&v, 0, sizeof(v));

  if ( db->duplicates )			/* must use a cursor */
  { DBC *cursor;
    term_t tail = PL_copy_term_ref(value);
    term_t head = PL_new_term_ref();

    NOSIG(rval=db->db->cursor(db->db, TheTXN, &cursor, 0));
    if ( rval )
      return db_status(rval);

    NOSIG(rval=cursor->c_get(cursor, &k, &v, DB_SET));
    if ( rval == 0 )
    { DBT k2;

      if ( !PL_unify_list(tail, head, tail) ||
	   !unify_dbt(head, db->value_type, &v) )
      { cursor->c_close(cursor);
	return FALSE;
      }

      memset(&k2, 0, sizeof(k2));
      for(;;)
      { NOSIG(rval=cursor->c_get(cursor, &k2, &v, DB_NEXT));

	if ( rval == 0 && equal_dbt(&k, &k2) )
	{ if ( PL_unify_list(tail, head, tail) &&
	       unify_dbt(head, db->value_type, &v) )
	    continue;
	}

	NOSIG(cursor->c_close(cursor);
	      free_dbt(&k, db->key_type));

	if ( rval <= 0 )		/* normal failure */
	{ return PL_unify_nil(tail);
	} else				/* error failure */
	{ return db_status(rval);
	}
      }
    } else if ( rval == DB_NOTFOUND )
    { free_dbt(&k, db->key_type);
      return FALSE;
    } else
    { free_dbt(&k, db->key_type);
      return db_status(rval);
    }
  } else
  { NOSIG(rval=db->db->get(db->db, TheTXN, &k, &v, 0));

    if ( !rval )
    { term_t t = PL_new_term_ref();
      term_t tail = PL_copy_term_ref(value);
      term_t head = PL_new_term_ref();

      free_dbt(&k, db->key_type);
      PL_recorded_external(v.data, t);
      if ( PL_unify_list(tail, head, tail) &&
	   PL_unify(head, t) &&
	   PL_unify_nil(tail) )
	return TRUE;

      return FALSE;
    } else
      return db_status(rval);
  }
}


typedef struct _dbget_ctx
{ dbh *db;				/* the database */
  DBC *cursor;				/* the cursor */
  DBT key;				/* the key */
  DBT k2;				/* secondary key */
  DBT value;				/* the value */
} dbget_ctx;


static foreign_t
pl_bdb_enum(term_t handle, term_t key, term_t value, control_t ctx)
{ DBT k, v;
  dbh *db;
  int rval = 0;
  dbget_ctx *c = NULL;
  fid_t fid = 0;

  memset(&k, 0, sizeof(k));
  memset(&v, 0, sizeof(v));

  switch( PL_foreign_control(ctx) )
  { case PL_FIRST_CALL:
      if ( !get_db(handle, &db) )
	return FALSE;
      c = calloc(1, sizeof(*c));

      c->db = db;
      if ( (rval=db->db->cursor(db->db, TheTXN, &c->cursor, 0)) )
      { free(c);
	return db_status(rval);
      }
      DEBUG(Sdprintf("Created cursor at %p\n", c->cursor));

      rval = c->cursor->c_get(c->cursor, &c->key, &c->value, DB_FIRST);
      if ( rval == 0 )
      { fid = PL_open_foreign_frame();

	if ( unify_dbt(key, db->key_type, &c->key) &&
	     unify_dbt(value, db->value_type, &c->value) )
	{ PL_close_foreign_frame(fid);
	  PL_retry_address(c);
	}

	PL_rewind_foreign_frame(fid);
	goto retry;
      }
      goto out;
    case PL_REDO:
      c = PL_foreign_context_address(ctx);
      db = c->db;

    retry:
      for(;;)
      { rval = c->cursor->c_get(c->cursor, &c->k2, &c->value, DB_NEXT);

	if ( rval == 0 )
	{ if ( !fid )
	    fid = PL_open_foreign_frame();
	  if ( unify_dbt(key, db->key_type, &c->k2) &&
	       unify_dbt(value, db->value_type, &c->value) )
	  { PL_close_foreign_frame(fid);
	    PL_retry_address(c);
	  }
	  PL_rewind_foreign_frame(fid);
	  continue;
	}
	break;
      }
      break;
    case PL_PRUNED:
      c = PL_foreign_context_address(ctx);
      db = c->db;
      break;
  }

out:
  if ( c )
  { if ( rval == 0 )
      rval = c->cursor->c_close(c->cursor);
    else
      c->cursor->c_close(c->cursor);
    free(c);
  }
  if ( fid )
    PL_close_foreign_frame(fid);

  db_status(rval);
  return FALSE;				/* also on rval = 0! */
}


#define DO_DEL \
	if ( del ) \
	{ do \
	  { if ( (rval=c->cursor->c_del(c->cursor, 0)) != 0 ) \
	      return db_status(rval); \
	  } while(0); \
	}


static foreign_t
pl_bdb_getdel(term_t handle, term_t key, term_t value, control_t ctx, int del)
{ dbh *db;
  int rval = 0;
  dbget_ctx *c = NULL;
  fid_t fid = 0;

  switch( PL_foreign_control(ctx) )
  { case PL_FIRST_CALL:
      if ( !get_db(handle, &db) )
	return FALSE;

      if ( db->duplicates )		/* DB with duplicates */
      { c = calloc(1, sizeof(*c));

	c->db = db;
	if ( (rval=db->db->cursor(db->db, TheTXN, &c->cursor, 0)) )
	{ free(c);
	  return db_status(rval);
	}
	DEBUG(Sdprintf("Created cursor at %p\n", c->cursor));
	if ( !get_dbt(key, db->key_type, &c->key) )
	  return FALSE;

	rval = c->cursor->c_get(c->cursor, &c->key, &c->value, DB_SET);
	if ( rval == 0 )
	{ fid = PL_open_foreign_frame();

	  if ( unify_dbt(value, db->value_type, &c->value) )
	  { DO_DEL;

	    PL_close_foreign_frame(fid);
	    PL_retry_address(c);
	  }

	  PL_rewind_foreign_frame(fid);
	  goto retry;
	}
	goto out;
      } else				/* Unique DB */
      { DBT k, v;
	int rc;

	if ( !get_dbt(key, db->key_type, &k) )
	  return FALSE;
	memset(&v, 0, sizeof(v));
	if ( (db_flags&DB_THREAD) )
	  v.flags = DB_DBT_MALLOC;

	if ( (rval=db->db->get(db->db, TheTXN, &k, &v, 0)) == 0 )
	{ rc = unify_dbt(value, db->value_type, &v);

	  free_result_dbt(&v);
	  if ( rc && del )
	  { int flags = 0;

	    rc = db_status(db->db->del(db->db, TheTXN, &k, flags));
	  }
	} else
	  rc = db_status(rval);

	free_dbt(&k, db->key_type);

	return rc;
      }
    case PL_REDO:
      c = PL_foreign_context_address(ctx);
      db = c->db;

    retry:
      for(;;)
      { rval = c->cursor->c_get(c->cursor, &c->k2, &c->value, DB_NEXT);

	if ( rval == 0 && equal_dbt(&c->key, &c->k2) )
	{ if ( !fid )
	    fid = PL_open_foreign_frame();
	  if ( unify_dbt(value, db->value_type, &c->value) )
	  { DO_DEL;
	    PL_close_foreign_frame(fid);
	    PL_retry_address(c);
	  }
	  PL_rewind_foreign_frame(fid);
	  continue;
	}
	break;
      }
      break;
    case PL_PRUNED:
      c = PL_foreign_context_address(ctx);
      db = c->db;
      break;
  }

out:
  if ( c )
  { if ( rval == 0 )
      rval = c->cursor->c_close(c->cursor);
    else
      c->cursor->c_close(c->cursor);
    DEBUG(Sdprintf("Destroyed cursor at %p\n", c->cursor));
    free_dbt(&c->key, db->key_type);
    free(c);
  }
  if ( fid )
    PL_close_foreign_frame(fid);

  db_status(rval);
  return FALSE;				/* also on rval = 0! */
}


static foreign_t
pl_bdb_get(term_t handle, term_t key, term_t value, control_t ctx)
{ int rval;

  NOSIG(rval = pl_bdb_getdel(handle, key, value, ctx, FALSE));

  return rval;
}


static foreign_t
pl_bdb_del3(term_t handle, term_t key, term_t value, control_t ctx)
{ int rval;

  NOSIG(rval=pl_bdb_getdel(handle, key, value, ctx, TRUE));

  return rval;
}


static void
cleanup(void)
{ if ( db_env )
  { int rval;

    if ( (rval=db_env->close(db_env, 0)) )
      Sdprintf("DB: ENV close failed: %s\n", db_strerror(rval));

    db_env   = NULL;
    db_flags = 0;
  }
}


		 /*******************************
		 *	     APPINIT		*
		 *******************************/

typedef struct _server_info
{ char *host;
  long cl_timeout;
  long sv_timeout;
  u_int32_t flags;
} server_info;


static void
#ifdef DB43
pl_bdb_error(const DB_ENV *dbenv, const char *prefix, const char *msg)
#else
pl_bdb_error(const char *prefix, char *msg)
#endif
{ Sdprintf("%s%s\n", prefix, msg);
}


#if defined(HAVE_SET_RPC_SERVER) || defined(HAVE_SET_SERVER)

static int
get_server(term_t options, server_info *info)
{ term_t l = PL_copy_term_ref(options);
  term_t h = PL_new_term_ref();

  while( PL_get_list(l, h, l) )
  { atom_t name;
    int arity;

    if ( PL_get_name_arity(h, &name, &arity) && name == ATOM_server )
    { info->cl_timeout = 0;
      info->sv_timeout = 0;
      info->flags      = 0;

      if ( arity >= 1 )			/* server(host) */
      { term_t a = PL_new_term_ref();

	_PL_get_arg(1, h, a);
	if ( !PL_get_chars(a, &info->host,
			   CVT_ATOM|CVT_STRING|REP_MB|CVT_EXCEPTION) )
	  return FALSE;
      }
      if ( arity == 2 )			/* server(host, options) */
      { term_t a = PL_new_term_ref();

	_PL_get_arg(2, h, l);
	while( PL_get_list(l, h, l) )
	{ atom_t name;
	  int arity;

	  if ( PL_get_name_arity(h, &name, &arity) && arity == 1 )
	  { _PL_get_arg(1, h, a);

	    if ( name == ATOM_server_timeout )
	    { if ( !PL_get_long_ex(a, &info->sv_timeout) )
		return FALSE;
	    } else if ( name == ATOM_client_timeout )
	    { if ( !PL_get_long_ex(a, &info->cl_timeout) )
		return FALSE;
	    } else
	      return PL_domain_error("server_option", a);
	  } else
	    return PL_domain_error("server_option", a);
	}
	if ( !PL_get_nil_ex(l) )
	  return FALSE;
      }

      return TRUE;
    }
  }

  return FALSE;
}


#if defined(DB_CLIENT) && !defined(DB_RPCCLIENT)
#define DB_RPCCLIENT DB_CLIENT
#endif

#endif


#define MAXCONFIG 20
typedef struct db_flag
{ char	   *name;
  u_int32_t flags;
  atom_t    aname;
} db_flag;

static db_flag db_dlags[] =
{ { "init_lock",	DB_INIT_LOCK },
  { "init_log",		DB_INIT_LOG  },
  { "init_mpool",	DB_INIT_MPOOL },
  { "init_rep",		DB_INIT_REP|DB_INIT_TXN|DB_INIT_LOCK },
  { "init_txn",		DB_INIT_TXN|DB_INIT_LOG },
  { "recover",		DB_RECOVER|DB_CREATE|DB_INIT_TXN },
  { "recover_fatal",	DB_RECOVER_FATAL|DB_CREATE|DB_INIT_TXN },
  { "use_environ",	DB_USE_ENVIRON },
  { "use_environ_root",	DB_USE_ENVIRON_ROOT },
  { "create",		DB_CREATE },
  { "lockdown",		DB_LOCKDOWN },
  { "failchk",		DB_FAILCHK },
  { "private",		DB_PRIVATE },
  { "register",		DB_REGISTER },
  { "system_mem",	DB_SYSTEM_MEM },
  { "thread",		DB_THREAD },
  { (char*)NULL,	0 }
};

#define F_ERROR       ((u_int32_t)-1)
#define F_UNPROCESSED ((u_int32_t)-2)

static u_int32_t
lookup_flag(atom_t name, term_t arg)
{ db_flag *fp;

  for(fp=db_dlags; fp->name; fp++)
  { if ( !fp->aname )
      fp->aname = PL_new_atom(fp->name);

    if ( fp->aname == name )
    { int v;

      if ( !PL_get_bool_ex(arg, &v) )
	return F_ERROR;
      return v ? fp->flags : 0;
    }
  }

  return F_UNPROCESSED;
}


static foreign_t
pl_bdb_init(term_t option_list)
{ int rval;
  term_t options = PL_copy_term_ref(option_list);
  u_int32_t flags = 0;
  term_t head = PL_new_term_ref();
  term_t a    = PL_new_term_ref();
  char *home = NULL;
  char *config[MAXCONFIG];
  int nconf = 0;

  if ( db_env )
    return pl_error(ERR_PACKAGE_INT, "db", 0, "Already initialized");

  config[0] = NULL;

  {
#if defined(HAVE_SET_RPC_SERVER) || defined(HAVE_SET_SERVER)
    server_info si;
    if ( get_server(option_list, &si) )
    { if ( (rval=db_env_create(&db_env, DB_RPCCLIENT)) )
	goto db_error;
#ifdef HAVE_SET_RPC_SERVER		/* >= 4.0; <= 5.0 */
      rval = db_env->set_rpc_server(db_env, 0, si.host,
				    si.cl_timeout, si.sv_timeout, si.flags);
#else
#ifdef HAVE_SET_SERVER
      rval = db_env->set_server(db_env, si.host,
			        si.cl_timeout, si.sv_timeout, si.flags);
#endif
#endif
      if ( rval )
	goto db_error;
    } else
#endif
    { if ( (rval=db_env_create(&db_env, 0)) )
	goto db_error;
    }
  }

  db_env->set_errpfx(db_env, "db4pl: ");
  db_env->set_errcall(db_env, pl_bdb_error);

  flags |= DB_INIT_MPOOL;		/* always needed? */

  while(PL_get_list(options, head, options))
  { atom_t name;
    int arity;

    if ( !PL_get_name_arity(head, &name, &arity) )
    { PL_type_error("option", head);
      goto pl_error;
    }

    if ( arity == 1 )
    { _PL_get_arg(1, head, a);

      if ( name == ATOM_mp_mmapsize )	/* mp_mmapsize */
      { size_t v;

	if ( !PL_get_size_ex(a, &v) )
	  return FALSE;
	db_env->set_mp_mmapsize(db_env, v);
	flags |= DB_INIT_MPOOL;
      } else if ( name == ATOM_mp_size ) /* mp_size */
      { size_t v;

	if ( !PL_get_size_ex(a, &v) )
	  return FALSE;
	db_env->set_cachesize(db_env, 0, v, 0);
	flags |= DB_INIT_MPOOL;
      } else if ( name == ATOM_home )	/* db_home */
      {	if ( !PL_get_chars(a, &home, CVT_ATOM|CVT_STRING|CVT_EXCEPTION|REP_MB) )
	  goto pl_error;
      } else if ( name == ATOM_config )	/* db_config */
      { term_t h = PL_new_term_ref();
	term_t a2 = PL_new_term_ref();

	while(PL_get_list(a, h, a))
	{ atom_t nm;
	  int ar;
	  const char *n;
	  char *v;

	  if ( !PL_get_name_arity(h, &nm, &ar) || ar !=	1 )
	  { PL_domain_error("db_config", h);
	    goto pl_error;
	  }
	  _PL_get_arg(1, h, a2);
	  if ( !PL_get_chars(a2, &v, CVT_ATOM|CVT_STRING|CVT_EXCEPTION) )
	    goto pl_error;
	  n = PL_atom_chars(nm);
	  if ( !(config[nconf] = malloc(strlen(n)+strlen(v)+2)) )
	  { PL_resource_error("memory");
	    goto pl_error;
	  }
	  strcpy(config[nconf], n);
	  strcat(config[nconf], " ");
	  strcat(config[nconf], v);
	  config[++nconf] = NULL;
	}
	if ( !PL_get_nil_ex(a) )
	  goto pl_error;
      } else
      { u_int32_t fv = lookup_flag(name, a);

	switch(fv)
	{ case F_ERROR:
	    goto pl_error;
	  case F_UNPROCESSED:
	    PL_domain_error("db_option", head);
	    goto pl_error;
	  default:
	    flags |= fv;
	}
      }
    } else
    { PL_type_error("db_option", head);
      goto pl_error;
    }
  }

  if ( !PL_get_nil_ex(options) )
    goto pl_error;

  if ( (rval=db_env->open(db_env, home, flags, 0666)) != 0 )
    goto db_error;
  db_flags = flags;

  if ( !rval )
    return TRUE;

pl_error:
  cleanup();
  return FALSE;

db_error:
  cleanup();
  return db_status(rval);
}

static foreign_t
pl_bdb_exit(void)
{ cleanup();

  return TRUE;
}


		 /*******************************
		 *     TMP ATOM PROLOG STUFF	*
		 *******************************/

static foreign_t
pl_bdb_atom(term_t handle, term_t atom, term_t id)
{ dbh *db;
  atom_t a;
  long lv;
  atomid_t aid;

  if ( !get_db(handle, &db) )
    return FALSE;

  if ( PL_get_atom(atom, &a) )
  { if ( !db_atom_id(db, a, &aid, DB4PL_ATOM_CREATE) )
      return FALSE;

    return PL_unify_integer(id, aid);
  } else if ( PL_get_long(id, &lv) )
  { aid = (atomid_t)lv;

    if ( !pl_atom_from_db(db, aid, &a) )
      return FALSE;

    return PL_unify_atom(atom, a);
  } else
    return pl_error(ERR_TYPE, "atom", atom);
}


install_t
install(void)
{ initConstants();

  PL_register_foreign("bdb_open",   4, pl_bdb_open,   0);
  PL_register_foreign("bdb_close",  1, pl_bdb_close,  0);
  PL_register_foreign("bdb_is_open",1, pl_bdb_is_open,0);
  PL_register_foreign("bdb_put",    3, pl_bdb_put,    0);
  PL_register_foreign("bdb_del",    2, pl_bdb_del2,   0);
  PL_register_foreign("bdb_del",    3, pl_bdb_del3,   PL_FA_NONDETERMINISTIC);
  PL_register_foreign("bdb_getall", 3, pl_bdb_getall, 0);
  PL_register_foreign("bdb_get",    3, pl_bdb_get,    PL_FA_NONDETERMINISTIC);
  PL_register_foreign("bdb_enum",   3, pl_bdb_enum,   PL_FA_NONDETERMINISTIC);
  PL_register_foreign("bdb_init",   1, pl_bdb_init,   0);
  PL_register_foreign("bdb_exit",   0, pl_bdb_exit,   0);
  PL_register_foreign("bdb_transaction", 1, pl_bdb_transaction,
						    PL_FA_TRANSPARENT);

  PL_register_foreign("bdb_atom",  3, pl_bdb_atom,  0);

  pthread_key_create(&transaction_key, free_transaction_stack);
}


install_t
uninstall(void)
{ if ( transaction_key )
  { pthread_key_delete(transaction_key);
    transaction_key = 0;
  }
  cleanup();
}