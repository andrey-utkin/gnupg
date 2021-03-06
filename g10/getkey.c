/* getkey.c -  Get a key from the database
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006,
 *               2007, 2008, 2010  Free Software Foundation, Inc.
 * Copyright (C) 2015 g10 Code GmbH
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "gpg.h"
#include "util.h"
#include "packet.h"
#include "iobuf.h"
#include "keydb.h"
#include "options.h"
#include "main.h"
#include "trustdb.h"
#include "i18n.h"
#include "keyserver-internal.h"
#include "call-agent.h"
#include "host2net.h"
#include "mbox-util.h"

#define MAX_PK_CACHE_ENTRIES   PK_UID_CACHE_SIZE
#define MAX_UID_CACHE_ENTRIES  PK_UID_CACHE_SIZE

#if MAX_PK_CACHE_ENTRIES < 2
#error We need the cache for key creation
#endif

struct getkey_ctx_s
{
  /* Part of the search criteria: whether the search is an exact
     search or not.  A search that is exact requires that a key or
     subkey meet all of the specified criteria.  A search that is not
     exact allows selecting a different key or subkey from the
     keyblock that matched the critera.  Further, an exact search
     returns the key or subkey that matched whereas a non-exact search
     typically returns the primary key.  See finish_lookup for
     details.  */
  int exact;

  /* Part of the search criteria: Whether the caller only wants keys
     with an available secret key.  This is used by getkey_next to get
     the next result with the same initial criteria.  */
  int want_secret;

  /* Part of the search criteria: The type of the requested key.  A
     mask of PUBKEY_USAGE_SIG, PUBKEY_USAGE_ENC and PUBKEY_USAGE_CERT.
     If non-zero, then for a key to match, it must implement one of
     the required uses.  */
  int req_usage;

  /* The database handle.  */
  KEYDB_HANDLE kr_handle;

  /* Whether we should call xfree() on the context when the context is
     released using getkey_end()).  */
  int not_allocated;

  /* This variable is used as backing store for strings which have
     their address used in ITEMS.  */
  strlist_t extra_list;

  /* Part of the search criteria: The low-level search specification
     as passed to keydb_search.  */
  int nitems;
  /* This must be the last element in the structure.  When we allocate
     the structure, we allocate it so that ITEMS can hold NITEMS.  */
  KEYDB_SEARCH_DESC items[1];
};

#if 0
static struct
{
  int any;
  int okay_count;
  int nokey_count;
  int error_count;
} lkup_stats[21];
#endif

typedef struct keyid_list
{
  struct keyid_list *next;
  char fpr[MAX_FINGERPRINT_LEN];
  u32 keyid[2];
} *keyid_list_t;


#if MAX_PK_CACHE_ENTRIES
typedef struct pk_cache_entry
{
  struct pk_cache_entry *next;
  u32 keyid[2];
  PKT_public_key *pk;
} *pk_cache_entry_t;
static pk_cache_entry_t pk_cache;
static int pk_cache_entries;	/* Number of entries in pk cache.  */
static int pk_cache_disabled;
#endif

#if MAX_UID_CACHE_ENTRIES < 5
#error we really need the userid cache
#endif
typedef struct user_id_db
{
  struct user_id_db *next;
  keyid_list_t keyids;
  int len;
  char name[1];
} *user_id_db_t;
static user_id_db_t user_id_db;
static int uid_cache_entries;	/* Number of entries in uid cache. */

static void merge_selfsigs (kbnode_t keyblock);
static int lookup (getkey_ctx_t ctx,
		   kbnode_t *ret_keyblock, kbnode_t *ret_found_key,
		   int want_secret);

#if 0
static void
print_stats ()
{
  int i;
  for (i = 0; i < DIM (lkup_stats); i++)
    {
      if (lkup_stats[i].any)
	es_fprintf (es_stderr,
		 "lookup stats: mode=%-2d  ok=%-6d  nokey=%-6d  err=%-6d\n",
		 i,
		 lkup_stats[i].okay_count,
		 lkup_stats[i].nokey_count, lkup_stats[i].error_count);
    }
}
#endif


/* For documentation see keydb.h.  */
void
cache_public_key (PKT_public_key * pk)
{
#if MAX_PK_CACHE_ENTRIES
  pk_cache_entry_t ce, ce2;
  u32 keyid[2];

  if (pk_cache_disabled)
    return;

  if (pk->flags.dont_cache)
    return;

  if (is_ELGAMAL (pk->pubkey_algo)
      || pk->pubkey_algo == PUBKEY_ALGO_DSA
      || pk->pubkey_algo == PUBKEY_ALGO_ECDSA
      || pk->pubkey_algo == PUBKEY_ALGO_EDDSA
      || pk->pubkey_algo == PUBKEY_ALGO_ECDH
      || is_RSA (pk->pubkey_algo))
    {
      keyid_from_pk (pk, keyid);
    }
  else
    return; /* Don't know how to get the keyid.  */

  for (ce = pk_cache; ce; ce = ce->next)
    if (ce->keyid[0] == keyid[0] && ce->keyid[1] == keyid[1])
      {
	if (DBG_CACHE)
	  log_debug ("cache_public_key: already in cache\n");
	return;
      }

  if (pk_cache_entries >= MAX_PK_CACHE_ENTRIES)
    {
      int n;

      /* Remove the last 50% of the entries.  */
      for (ce = pk_cache, n = 0; ce && n < pk_cache_entries/2; n++)
        ce = ce->next;
      if (ce != pk_cache && ce->next)
        {
          ce2 = ce->next;
          ce->next = NULL;
          ce = ce2;
          for (; ce; ce = ce2)
            {
              ce2 = ce->next;
              free_public_key (ce->pk);
              xfree (ce);
              pk_cache_entries--;
            }
        }
      assert (pk_cache_entries < MAX_PK_CACHE_ENTRIES);
    }
  pk_cache_entries++;
  ce = xmalloc (sizeof *ce);
  ce->next = pk_cache;
  pk_cache = ce;
  ce->pk = copy_public_key (NULL, pk);
  ce->keyid[0] = keyid[0];
  ce->keyid[1] = keyid[1];
#endif
}


/* Return a const utf-8 string with the text "[User ID not found]".
   This function is required so that we don't need to switch gettext's
   encoding temporary.  */
static const char *
user_id_not_found_utf8 (void)
{
  static char *text;

  if (!text)
    text = native_to_utf8 (_("[User ID not found]"));
  return text;
}



/* Return the user ID from the given keyblock.
 * We use the primary uid flag which has been set by the merge_selfsigs
 * function.  The returned value is only valid as long as the given
 * keyblock is not changed.  */
static const char *
get_primary_uid (KBNODE keyblock, size_t * uidlen)
{
  KBNODE k;
  const char *s;

  for (k = keyblock; k; k = k->next)
    {
      if (k->pkt->pkttype == PKT_USER_ID
	  && !k->pkt->pkt.user_id->attrib_data
	  && k->pkt->pkt.user_id->is_primary)
	{
	  *uidlen = k->pkt->pkt.user_id->len;
	  return k->pkt->pkt.user_id->name;
	}
    }
  s = user_id_not_found_utf8 ();
  *uidlen = strlen (s);
  return s;
}


static void
release_keyid_list (keyid_list_t k)
{
  while (k)
    {
      keyid_list_t k2 = k->next;
      xfree (k);
      k = k2;
    }
}

/****************
 * Store the association of keyid and userid
 * Feed only public keys to this function.
 */
static void
cache_user_id (KBNODE keyblock)
{
  user_id_db_t r;
  const char *uid;
  size_t uidlen;
  keyid_list_t keyids = NULL;
  KBNODE k;

  for (k = keyblock; k; k = k->next)
    {
      if (k->pkt->pkttype == PKT_PUBLIC_KEY
	  || k->pkt->pkttype == PKT_PUBLIC_SUBKEY)
	{
	  keyid_list_t a = xmalloc_clear (sizeof *a);
	  /* Hmmm: For a long list of keyids it might be an advantage
	   * to append the keys.  */
          fingerprint_from_pk (k->pkt->pkt.public_key, a->fpr, NULL);
	  keyid_from_pk (k->pkt->pkt.public_key, a->keyid);
	  /* First check for duplicates.  */
	  for (r = user_id_db; r; r = r->next)
	    {
	      keyid_list_t b = r->keyids;
	      for (b = r->keyids; b; b = b->next)
		{
		  if (!memcmp (b->fpr, a->fpr, MAX_FINGERPRINT_LEN))
		    {
		      if (DBG_CACHE)
			log_debug ("cache_user_id: already in cache\n");
		      release_keyid_list (keyids);
		      xfree (a);
		      return;
		    }
		}
	    }
	  /* Now put it into the cache.  */
	  a->next = keyids;
	  keyids = a;
	}
    }
  if (!keyids)
    BUG (); /* No key no fun.  */


  uid = get_primary_uid (keyblock, &uidlen);

  if (uid_cache_entries >= MAX_UID_CACHE_ENTRIES)
    {
      /* fixme: use another algorithm to free some cache slots */
      r = user_id_db;
      user_id_db = r->next;
      release_keyid_list (r->keyids);
      xfree (r);
      uid_cache_entries--;
    }
  r = xmalloc (sizeof *r + uidlen - 1);
  r->keyids = keyids;
  r->len = uidlen;
  memcpy (r->name, uid, r->len);
  r->next = user_id_db;
  user_id_db = r;
  uid_cache_entries++;
}


/* For documentation see keydb.h.  */
void
getkey_disable_caches ()
{
#if MAX_PK_CACHE_ENTRIES
  {
    pk_cache_entry_t ce, ce2;

    for (ce = pk_cache; ce; ce = ce2)
      {
	ce2 = ce->next;
	free_public_key (ce->pk);
	xfree (ce);
      }
    pk_cache_disabled = 1;
    pk_cache_entries = 0;
    pk_cache = NULL;
  }
#endif
  /* fixme: disable user id cache ? */
}


static void
pk_from_block (GETKEY_CTX ctx, PKT_public_key * pk, KBNODE keyblock,
	       KBNODE found_key)
{
  KBNODE a = found_key ? found_key : keyblock;

  (void) ctx;

  assert (a->pkt->pkttype == PKT_PUBLIC_KEY
	  || a->pkt->pkttype == PKT_PUBLIC_SUBKEY);

  copy_public_key (pk, a->pkt->pkt.public_key);
}


/* For documentation see keydb.h.  */
int
get_pubkey (PKT_public_key * pk, u32 * keyid)
{
  int internal = 0;
  int rc = 0;

#if MAX_PK_CACHE_ENTRIES
  if (pk)
    {
      /* Try to get it from the cache.  We don't do this when pk is
         NULL as it does not guarantee that the user IDs are
         cached. */
      pk_cache_entry_t ce;
      for (ce = pk_cache; ce; ce = ce->next)
	{
	  if (ce->keyid[0] == keyid[0] && ce->keyid[1] == keyid[1])
	    /* XXX: We don't check PK->REQ_USAGE here, but if we don't
	       read from the cache, we do check it!  */
	    {
	      copy_public_key (pk, ce->pk);
	      return 0;
	    }
	}
    }
#endif
  /* More init stuff.  */
  if (!pk)
    {
      pk = xmalloc_clear (sizeof *pk);
      internal++;
    }


  /* Do a lookup.  */
  {
    struct getkey_ctx_s ctx;
    KBNODE kb = NULL;
    KBNODE found_key = NULL;
    memset (&ctx, 0, sizeof ctx);
    ctx.exact = 1; /* Use the key ID exactly as given.  */
    ctx.not_allocated = 1;
    ctx.kr_handle = keydb_new ();
    ctx.nitems = 1;
    ctx.items[0].mode = KEYDB_SEARCH_MODE_LONG_KID;
    ctx.items[0].u.kid[0] = keyid[0];
    ctx.items[0].u.kid[1] = keyid[1];
    ctx.req_usage = pk->req_usage;
    rc = lookup (&ctx, &kb, &found_key, 0);
    if (!rc)
      {
	pk_from_block (&ctx, pk, kb, found_key);
      }
    getkey_end (&ctx);
    release_kbnode (kb);
  }
  if (!rc)
    goto leave;

  rc = GPG_ERR_NO_PUBKEY;

leave:
  if (!rc)
    cache_public_key (pk);
  if (internal)
    free_public_key (pk);
  return rc;
}


/* For documentation see keydb.h.  */
int
get_pubkey_fast (PKT_public_key * pk, u32 * keyid)
{
  int rc = 0;
  KEYDB_HANDLE hd;
  KBNODE keyblock;
  u32 pkid[2];

  assert (pk);
#if MAX_PK_CACHE_ENTRIES
  {
    /* Try to get it from the cache */
    pk_cache_entry_t ce;

    for (ce = pk_cache; ce; ce = ce->next)
      {
	if (ce->keyid[0] == keyid[0] && ce->keyid[1] == keyid[1]
	    /* Only consider primary keys.  */
	    && ce->pk->keyid[0] == ce->pk->main_keyid[0]
	    && ce->pk->keyid[1] == ce->pk->main_keyid[1])
	  {
	    if (pk)
	      copy_public_key (pk, ce->pk);
	    return 0;
	  }
      }
  }
#endif

  hd = keydb_new ();
  rc = keydb_search_kid (hd, keyid);
  if (gpg_err_code (rc) == GPG_ERR_NOT_FOUND)
    {
      keydb_release (hd);
      return GPG_ERR_NO_PUBKEY;
    }
  rc = keydb_get_keyblock (hd, &keyblock);
  keydb_release (hd);
  if (rc)
    {
      log_error ("keydb_get_keyblock failed: %s\n", gpg_strerror (rc));
      return GPG_ERR_NO_PUBKEY;
    }

  assert (keyblock && keyblock->pkt
          && keyblock->pkt->pkttype == PKT_PUBLIC_KEY);

  /* We return the primary key.  If KEYID matched a subkey, then we
     return an error.  */
  keyid_from_pk (keyblock->pkt->pkt.public_key, pkid);
  if (keyid[0] == pkid[0] && keyid[1] == pkid[1])
    copy_public_key (pk, keyblock->pkt->pkt.public_key);
  else
    rc = GPG_ERR_NO_PUBKEY;

  release_kbnode (keyblock);

  /* Not caching key here since it won't have all of the fields
     properly set. */

  return rc;
}


/* For documentation see keydb.h.  */
KBNODE
get_pubkeyblock (u32 * keyid)
{
  struct getkey_ctx_s ctx;
  int rc = 0;
  KBNODE keyblock = NULL;

  memset (&ctx, 0, sizeof ctx);
  /* No need to set exact here because we want the entire block.  */
  ctx.not_allocated = 1;
  ctx.kr_handle = keydb_new ();
  ctx.nitems = 1;
  ctx.items[0].mode = KEYDB_SEARCH_MODE_LONG_KID;
  ctx.items[0].u.kid[0] = keyid[0];
  ctx.items[0].u.kid[1] = keyid[1];
  rc = lookup (&ctx, &keyblock, NULL, 0);
  getkey_end (&ctx);

  return rc ? NULL : keyblock;
}


/* For documentation see keydb.h.  */
gpg_error_t
get_seckey (PKT_public_key *pk, u32 *keyid)
{
  gpg_error_t err;
  struct getkey_ctx_s ctx;
  kbnode_t keyblock = NULL;
  kbnode_t found_key = NULL;

  memset (&ctx, 0, sizeof ctx);
  ctx.exact = 1; /* Use the key ID exactly as given.  */
  ctx.not_allocated = 1;
  ctx.kr_handle = keydb_new ();
  ctx.nitems = 1;
  ctx.items[0].mode = KEYDB_SEARCH_MODE_LONG_KID;
  ctx.items[0].u.kid[0] = keyid[0];
  ctx.items[0].u.kid[1] = keyid[1];
  ctx.req_usage = pk->req_usage;
  err = lookup (&ctx, &keyblock, &found_key, 1);
  if (!err)
    {
      pk_from_block (&ctx, pk, keyblock, found_key);
    }
  getkey_end (&ctx);
  release_kbnode (keyblock);

  if (!err)
    {
      err = agent_probe_secret_key (/*ctrl*/NULL, pk);
      if (err)
	release_public_key_parts (pk);
    }

  return err;
}


/* Skip unusable keys.  A key is unusable if it is revoked, expired or
   disabled or if the selected user id is revoked or expired.  */
static int
skip_unusable (void *dummy, u32 * keyid, int uid_no)
{
  int unusable = 0;
  KBNODE keyblock;
  PKT_public_key *pk;

  (void) dummy;

  keyblock = get_pubkeyblock (keyid);
  if (!keyblock)
    {
      log_error ("error checking usability status of %s\n", keystr (keyid));
      goto leave;
    }

  pk = keyblock->pkt->pkt.public_key;

  /* Is the key revoked or expired?  */
  if (pk->flags.revoked || pk->has_expired)
    unusable = 1;

  /* Is the user ID in question revoked or expired? */
  if (!unusable && uid_no)
    {
      KBNODE node;
      int uids_seen = 0;

      for (node = keyblock; node; node = node->next)
	{
	  if (node->pkt->pkttype == PKT_USER_ID)
	    {
	      PKT_user_id *user_id = node->pkt->pkt.user_id;

	      uids_seen ++;
	      if (uids_seen != uid_no)
		continue;

	      if (user_id->is_revoked || user_id->is_expired)
		unusable = 1;

	      break;
	    }
	}

      /* If UID_NO is non-zero, then the keyblock better have at least
	 that many UIDs.  */
      assert (uids_seen == uid_no);
    }

  if (!unusable)
    unusable = pk_is_disabled (pk);

leave:
  release_kbnode (keyblock);
  return unusable;
}


/* Search for keys matching some criteria.

   If RETCTX is not NULL, then the constructed context is returned in
   *RETCTX so that getpubkey_next can be used to get subsequent
   results.  In this case, getkey_end() must be used to free the
   search context.  If RETCTX is not NULL, then RET_KDBHD must be
   NULL.

   If NAMELIST is not NULL, then a search query is constructed using
   classify_user_id on each of the strings in the list.  (Recall: the
   database does an OR of the terms, not an AND.)  If NAMELIST is
   NULL, then all results are returned.

   If PK is not NULL, the public key of the first result is returned
   in *PK.  Note: PK->REQ_USAGE must be valid!!!  If PK->REQ_USAGE is
   set, it is used to filter the search results.  See the
   documentation for finish_lookup to understand exactly how this is
   used.  Note: The self-signed data has already been merged into the
   public key using merge_selfsigs.  Free *PK by calling
   release_public_key_parts (or, if PK was allocated using xfree, you
   can use free_public_key, which calls release_public_key_parts(PK)
   and then xfree(PK)).

   If WANT_SECRET is set, then only keys with an available secret key
   (either locally or via key registered on a smartcard) are returned.

   If INCLUDE_UNUSABLE is set, then unusable keys (see the
   documentation for skip_unusable for an exact definition) are
   skipped unless they are looked up by key id or by fingerprint.

   If RET_KB is not NULL, the keyblock is returned in *RET_KB.  This
   should be freed using release_kbnode().

   If RET_KDBHD is not NULL, then the new database handle used to
   conduct the search is returned in *RET_KDBHD.  This can be used to
   get subsequent results using keydb_search_next.  Note: in this
   case, no advanced filtering is done for subsequent results (e.g.,
   WANT_SECRET and PK->REQ_USAGE are not respected).

   This function returns 0 on success.  Otherwise, an error code is
   returned.  In particular, GPG_ERR_NO_PUBKEY or GPG_ERR_NO_SECKEY
   (if want_secret is set) is returned if the key is not found.  */
static int
key_byname (GETKEY_CTX *retctx, strlist_t namelist,
	    PKT_public_key *pk,
	    int want_secret, int include_unusable,
	    KBNODE * ret_kb, KEYDB_HANDLE * ret_kdbhd)
{
  int rc = 0;
  int n;
  strlist_t r;
  GETKEY_CTX ctx;
  KBNODE help_kb = NULL;
  KBNODE found_key = NULL;

  if (retctx)
    {
      /* Reset the returned context in case of error.  */
      assert (!ret_kdbhd); /* Not allowed because the handle is stored
			      in the context.  */
      *retctx = NULL;
    }
  if (ret_kdbhd)
    *ret_kdbhd = NULL;

  if (!namelist)
    /* No search terms: iterate over the whole DB.  */
    {
      ctx = xmalloc_clear (sizeof *ctx);
      ctx->nitems = 1;
      ctx->items[0].mode = KEYDB_SEARCH_MODE_FIRST;
      if (!include_unusable)
	ctx->items[0].skipfnc = skip_unusable;
    }
  else
    {
      /* Build the search context.  */
      for (n = 0, r = namelist; r; r = r->next)
	n++;

      /* CTX has space for a single search term at the end.  Thus, we
	 need to allocate sizeof *CTX plus (n - 1) sizeof
	 CTX->ITEMS.  */
      ctx = xmalloc_clear (sizeof *ctx + (n - 1) * sizeof ctx->items);
      ctx->nitems = n;

      for (n = 0, r = namelist; r; r = r->next, n++)
	{
	  gpg_error_t err;

	  err = classify_user_id (r->d, &ctx->items[n], 1);

	  if (ctx->items[n].exact)
	    ctx->exact = 1;
	  if (err)
	    {
	      xfree (ctx);
	      return gpg_err_code (err); /* FIXME: remove gpg_err_code.  */
	    }
	  if (!include_unusable
	      && ctx->items[n].mode != KEYDB_SEARCH_MODE_SHORT_KID
	      && ctx->items[n].mode != KEYDB_SEARCH_MODE_LONG_KID
	      && ctx->items[n].mode != KEYDB_SEARCH_MODE_FPR16
	      && ctx->items[n].mode != KEYDB_SEARCH_MODE_FPR20
	      && ctx->items[n].mode != KEYDB_SEARCH_MODE_FPR)
	    ctx->items[n].skipfnc = skip_unusable;
	}
    }

  ctx->want_secret = want_secret;
  ctx->kr_handle = keydb_new ();
  if (!ret_kb)
    ret_kb = &help_kb;

  if (pk)
    {
      ctx->req_usage = pk->req_usage;
    }

  rc = lookup (ctx, ret_kb, &found_key, want_secret);
  if (!rc && pk)
    {
      pk_from_block (ctx, pk, *ret_kb, found_key);
    }

  release_kbnode (help_kb);

  if (retctx) /* Caller wants the context.  */
    *retctx = ctx;
  else
    {
      if (ret_kdbhd)
	{
	  *ret_kdbhd = ctx->kr_handle;
	  ctx->kr_handle = NULL;
	}
      getkey_end (ctx);
    }

  return rc;
}


/* For documentation see keydb.h.  */
int
get_pubkey_byname (ctrl_t ctrl, GETKEY_CTX * retctx, PKT_public_key * pk,
		   const char *name, KBNODE * ret_keyblock,
		   KEYDB_HANDLE * ret_kdbhd, int include_unusable, int no_akl)
{
  int rc;
  strlist_t namelist = NULL;
  struct akl *akl;
  int is_mbox;
  int nodefault = 0;
  int anylocalfirst = 0;

  if (retctx)
    *retctx = NULL;

  /* Does NAME appear to be a mailbox (mail address)?  */
  is_mbox = is_valid_mailbox (name);

  /* The auto-key-locate feature works as follows: there are a number
     of methods to look up keys.  By default, the local keyring is
     tried first.  Then, each method listed in the --auto-key-locate is
     tried in the order it appears.

     This can be changed as follows:

       - if nodefault appears anywhere in the list of options, then
         the local keyring is not tried first, or,

       - if local appears anywhere in the list of options, then the
         local keyring is not tried first, but in the order in which
         it was listed in the --auto-key-locate option.

     Note: we only save the search context in RETCTX if the local
     method is the first method tried (either explicitly or
     implicitly).  */
  if (!no_akl)
    /* auto-key-locate is enabled.  */
    {
      /* nodefault is true if "nodefault" or "local" appear.  */
      for (akl = opt.auto_key_locate; akl; akl = akl->next)
	if (akl->type == AKL_NODEFAULT || akl->type == AKL_LOCAL)
	  {
	    nodefault = 1;
	    break;
	  }
      /* anylocalfirst is true if "local" appears before any other
	 search methods (except "nodefault").  */
      for (akl = opt.auto_key_locate; akl; akl = akl->next)
	if (akl->type != AKL_NODEFAULT)
	  {
	    if (akl->type == AKL_LOCAL)
	      anylocalfirst = 1;
	    break;
	  }
    }

  if (!nodefault)
    /* "nodefault" didn't occur.  Thus, "local" is implicitly the
       first method to try.  */
    anylocalfirst = 1;

  if (nodefault && is_mbox)
    /* Either "nodefault" or "local" (explicitly) appeared in the auto
       key locate list and NAME appears to be an email address.  Don't
       try the local keyring.  */
    {
      rc = GPG_ERR_NO_PUBKEY;
    }
  else
    /* Either "nodefault" and "local" don't appear in the auto key
       locate list (in which case we try the local keyring first) or
       NAME does not appear to be an email address (in which case we
       only try the local keyring).  In this case, lookup NAME in the
       local keyring.  */
    {
      add_to_strlist (&namelist, name);
      rc = key_byname (retctx, namelist, pk, 0,
		       include_unusable, ret_keyblock, ret_kdbhd);
    }

  /* If the requested name resembles a valid mailbox and automatic
     retrieval has been enabled, we try to import the key. */
  if (gpg_err_code (rc) == GPG_ERR_NO_PUBKEY && !no_akl && is_mbox)
    /* NAME wasn't present in the local keyring (or we didn't try the
       local keyring).  Since the auto key locate feature is enabled
       and NAME appears to be an email address, try the auto locate
       feature.  */
    {
      for (akl = opt.auto_key_locate; akl; akl = akl->next)
	{
	  unsigned char *fpr = NULL;
	  size_t fpr_len;
	  int did_key_byname = 0;
	  int no_fingerprint = 0;
	  const char *mechanism = "?";

	  switch (akl->type)
	    {
	    case AKL_NODEFAULT:
	      /* This is a dummy mechanism.  */
	      mechanism = "None";
	      rc = GPG_ERR_NO_PUBKEY;
	      break;

	    case AKL_LOCAL:
	      mechanism = "Local";
	      did_key_byname = 1;
	      if (retctx)
		{
		  getkey_end (*retctx);
		  *retctx = NULL;
		}
	      add_to_strlist (&namelist, name);
	      rc = key_byname (anylocalfirst ? retctx : NULL,
			       namelist, pk, 0,
			       include_unusable, ret_keyblock, ret_kdbhd);
	      break;

	    case AKL_CERT:
	      mechanism = "DNS CERT";
	      glo_ctrl.in_auto_key_retrieve++;
	      rc = keyserver_import_cert (ctrl, name, 0, &fpr, &fpr_len);
	      glo_ctrl.in_auto_key_retrieve--;
	      break;

	    case AKL_PKA:
	      mechanism = "PKA";
	      glo_ctrl.in_auto_key_retrieve++;
	      rc = keyserver_import_pka (ctrl, name, &fpr, &fpr_len);
	      glo_ctrl.in_auto_key_retrieve--;
	      break;

	    case AKL_DANE:
	      mechanism = "DANE";
	      glo_ctrl.in_auto_key_retrieve++;
	      rc = keyserver_import_cert (ctrl, name, 1, &fpr, &fpr_len);
	      glo_ctrl.in_auto_key_retrieve--;
	      break;

	    case AKL_LDAP:
	      mechanism = "LDAP";
	      glo_ctrl.in_auto_key_retrieve++;
	      rc = keyserver_import_ldap (ctrl, name, &fpr, &fpr_len);
	      glo_ctrl.in_auto_key_retrieve--;
	      break;

	    case AKL_KEYSERVER:
	      /* Strictly speaking, we don't need to only use a valid
	         mailbox for the getname search, but it helps cut down
	         on the problem of searching for something like "john"
	         and getting a whole lot of keys back. */
	      if (opt.keyserver)
		{
		  mechanism = opt.keyserver->uri;
		  glo_ctrl.in_auto_key_retrieve++;
		  rc = keyserver_import_name (ctrl, name, &fpr, &fpr_len,
                                              opt.keyserver);
		  glo_ctrl.in_auto_key_retrieve--;
		}
	      else
		{
		  mechanism = "Unconfigured keyserver";
		  rc = GPG_ERR_NO_PUBKEY;
		}
	      break;

	    case AKL_SPEC:
	      {
		struct keyserver_spec *keyserver;

		mechanism = akl->spec->uri;
		keyserver = keyserver_match (akl->spec);
		glo_ctrl.in_auto_key_retrieve++;
		rc = keyserver_import_name (ctrl,
                                            name, &fpr, &fpr_len, keyserver);
		glo_ctrl.in_auto_key_retrieve--;
	      }
	      break;
	    }

	  /* Use the fingerprint of the key that we actually fetched.
	     This helps prevent problems where the key that we fetched
	     doesn't have the same name that we used to fetch it.  In
	     the case of CERT and PKA, this is an actual security
	     requirement as the URL might point to a key put in by an
	     attacker.  By forcing the use of the fingerprint, we
	     won't use the attacker's key here. */
	  if (!rc && fpr)
	    {
	      char fpr_string[MAX_FINGERPRINT_LEN * 2 + 1];

	      assert (fpr_len <= MAX_FINGERPRINT_LEN);

	      free_strlist (namelist);
	      namelist = NULL;

	      bin2hex (fpr, fpr_len, fpr_string);

	      if (opt.verbose)
		log_info ("auto-key-locate found fingerprint %s\n",
			  fpr_string);

	      add_to_strlist (&namelist, fpr_string);
	    }
	  else if (!rc && !fpr && !did_key_byname)
	    /* The acquisition method said no failure occurred, but it
	       didn't return a fingerprint.  That's a failure.  */
	    {
	      no_fingerprint = 1;
	      rc = GPG_ERR_NO_PUBKEY;
	    }
	  xfree (fpr);
	  fpr = NULL;

	  if (!rc && !did_key_byname)
	    /* There was no error and we didn't do a local lookup.
	       This means that we imported a key into the local
	       keyring.  Try to read the imported key from the
	       keyring.  */
	    {
	      if (retctx)
		{
		  getkey_end (*retctx);
		  *retctx = NULL;
		}
	      rc = key_byname (anylocalfirst ? retctx : NULL,
			       namelist, pk, 0,
			       include_unusable, ret_keyblock, ret_kdbhd);
	    }
	  if (!rc)
	    {
	      /* Key found.  */
	      log_info (_("automatically retrieved '%s' via %s\n"),
			name, mechanism);
	      break;
	    }
	  if (gpg_err_code (rc) != GPG_ERR_NO_PUBKEY
              || opt.verbose || no_fingerprint)
	    log_info (_("error retrieving '%s' via %s: %s\n"),
		      name, mechanism,
		      no_fingerprint ? _("No fingerprint") : gpg_strerror (rc));
	}
    }


  if (rc && retctx)
    {
      getkey_end (*retctx);
      *retctx = NULL;
    }

  if (retctx && *retctx)
    {
      assert (!(*retctx)->extra_list);
      (*retctx)->extra_list = namelist;
    }
  else
    free_strlist (namelist);

  return rc;
}


/* For documentation see keydb.h.

   FIXME: We should replace this with the _byname function.  This can
   be done by creating a userID conforming to the unified fingerprint
   style.  */
int
get_pubkey_byfprint (PKT_public_key *pk, kbnode_t *r_keyblock,
		     const byte * fprint, size_t fprint_len)
{
  int rc;

  if (r_keyblock)
    *r_keyblock = NULL;

  if (fprint_len == 20 || fprint_len == 16)
    {
      struct getkey_ctx_s ctx;
      KBNODE kb = NULL;
      KBNODE found_key = NULL;

      memset (&ctx, 0, sizeof ctx);
      ctx.exact = 1;
      ctx.not_allocated = 1;
      ctx.kr_handle = keydb_new ();
      ctx.nitems = 1;
      ctx.items[0].mode = fprint_len == 16 ? KEYDB_SEARCH_MODE_FPR16
	: KEYDB_SEARCH_MODE_FPR20;
      memcpy (ctx.items[0].u.fpr, fprint, fprint_len);
      rc = lookup (&ctx, &kb, &found_key, 0);
      if (!rc && pk)
	pk_from_block (&ctx, pk, kb, found_key);
      if (!rc && r_keyblock)
	{
	  *r_keyblock = kb;
	  kb = NULL;
	}
      release_kbnode (kb);
      getkey_end (&ctx);
    }
  else
    rc = GPG_ERR_GENERAL; /* Oops */
  return rc;
}


/* For documentation see keydb.h.  */
int
get_pubkey_byfprint_fast (PKT_public_key * pk,
			  const byte * fprint, size_t fprint_len)
{
  int rc = 0;
  KEYDB_HANDLE hd;
  KBNODE keyblock;
  byte fprbuf[MAX_FINGERPRINT_LEN];
  int i;

  for (i = 0; i < MAX_FINGERPRINT_LEN && i < fprint_len; i++)
    fprbuf[i] = fprint[i];
  while (i < MAX_FINGERPRINT_LEN)
    fprbuf[i++] = 0;

  hd = keydb_new ();
  rc = keydb_search_fpr (hd, fprbuf);
  if (gpg_err_code (rc) == GPG_ERR_NOT_FOUND)
    {
      keydb_release (hd);
      return GPG_ERR_NO_PUBKEY;
    }
  rc = keydb_get_keyblock (hd, &keyblock);
  keydb_release (hd);
  if (rc)
    {
      log_error ("keydb_get_keyblock failed: %s\n", gpg_strerror (rc));
      return GPG_ERR_NO_PUBKEY;
    }

  assert (keyblock->pkt->pkttype == PKT_PUBLIC_KEY
	  || keyblock->pkt->pkttype == PKT_PUBLIC_SUBKEY);
  if (pk)
    copy_public_key (pk, keyblock->pkt->pkt.public_key);
  release_kbnode (keyblock);

  /* Not caching key here since it won't have all of the fields
     properly set. */

  return 0;
}

const char *
parse_def_secret_key (ctrl_t ctrl)
{
  KEYDB_HANDLE hd = NULL;
  strlist_t t;
  static int warned;

  for (t = opt.def_secret_key; t; t = t->next)
    {
      gpg_error_t err;
      KEYDB_SEARCH_DESC desc;
      KBNODE kb;

      err = classify_user_id (t->d, &desc, 1);
      if (err)
        {
          log_error (_("Invalid value ('%s') for --default-key.\n"),
                     t->d);
          continue;
        }

      if (! hd)
        hd = keydb_new ();
      else
        keydb_search_reset (hd);

      err = keydb_search (hd, &desc, 1, NULL);
      if (gpg_err_code (err) == GPG_ERR_NOT_FOUND)
        continue;

      if (err)
        {
          log_error (_("Error reading from keyring: %s.\n"),
                     gpg_strerror (err));
          t = NULL;
          break;
        }

      err = keydb_get_keyblock (hd, &kb);
      if (err)
        {
          log_error (_("error reading keyblock: %s\n"),
                     gpg_strerror (err));
          continue;
        }

      err = agent_probe_secret_key (ctrl, kb->pkt->pkt.public_key);
      release_kbnode (kb);
      if (! err)
        {
          if (! warned)
            log_debug (_("Using %s as default secret key.\n"), t->d);
          break;
        }
    }

  warned = 1;

  if (hd)
    keydb_release (hd);

  if (t)
    return t->d;
  return NULL;
}

/* For documentation see keydb.h.  */
gpg_error_t
get_seckey_default (ctrl_t ctrl, PKT_public_key *pk)
{
  gpg_error_t err;
  strlist_t namelist = NULL;
  int include_unusable = 1;


  const char *def_secret_key = parse_def_secret_key (ctrl);
  if (def_secret_key)
    add_to_strlist (&namelist, def_secret_key);
  else
    include_unusable = 0;

  err = key_byname (NULL, namelist, pk, 1, include_unusable, NULL, NULL);

  free_strlist (namelist);

  return err;
}

/* For documentation see keydb.h.  */
gpg_error_t
getkey_bynames (getkey_ctx_t *retctx, PKT_public_key *pk,
                strlist_t names, int want_secret, kbnode_t *ret_keyblock)
{
  return key_byname (retctx, names, pk, want_secret, 1,
                     ret_keyblock, NULL);
}


/* For documentation see keydb.h.  */
gpg_error_t
getkey_byname (ctrl_t ctrl, getkey_ctx_t *retctx, PKT_public_key *pk,
               const char *name, int want_secret, kbnode_t *ret_keyblock)
{
  gpg_error_t err;
  strlist_t namelist = NULL;
  int with_unusable = 1;
  const char *def_secret_key = NULL;

  if (want_secret && !name)
    def_secret_key = parse_def_secret_key (ctrl);

  if (want_secret && !name && def_secret_key)
    add_to_strlist (&namelist, def_secret_key);
  else if (name)
    add_to_strlist (&namelist, name);
  else
    with_unusable = 0;

  err = key_byname (retctx, namelist, pk, want_secret, with_unusable,
                    ret_keyblock, NULL);

  /* FIXME: Check that we really return GPG_ERR_NO_SECKEY if
     WANT_SECRET has been used.  */

  free_strlist (namelist);

  return err;
}


/* For documentation see keydb.h.  */
gpg_error_t
getkey_next (getkey_ctx_t ctx, PKT_public_key *pk, kbnode_t *ret_keyblock)
{
  int rc; /* Fixme:  Make sure this is proper gpg_error */
  KBNODE found_key = NULL;

  /* We need to disable the caching so that for an exact key search we
     won't get the result back from the cache and thus end up in an
     endless loop.  The endless loop can occur, because the cache is
     used without respecting the current file pointer!  */
  keydb_disable_caching (ctx->kr_handle);

  rc = lookup (ctx, ret_keyblock, &found_key, ctx->want_secret);
  if (!rc && pk && ret_keyblock)
    pk_from_block (ctx, pk, *ret_keyblock, found_key);

  return rc;
}


/* For documentation see keydb.h.  */
void
getkey_end (getkey_ctx_t ctx)
{
  if (ctx)
    {
      keydb_release (ctx->kr_handle);
      free_strlist (ctx->extra_list);
      if (!ctx->not_allocated)
	xfree (ctx);
    }
}



/************************************************
 ************* Merging stuff ********************
 ************************************************/

/* For documentation see keydb.h.  */
void
setup_main_keyids (kbnode_t keyblock)
{
  u32 kid[2], mainkid[2];
  kbnode_t kbctx, node;
  PKT_public_key *pk;

  if (keyblock->pkt->pkttype != PKT_PUBLIC_KEY)
    BUG ();
  pk = keyblock->pkt->pkt.public_key;

  keyid_from_pk (pk, mainkid);
  for (kbctx=NULL; (node = walk_kbnode (keyblock, &kbctx, 0)); )
    {
      if (!(node->pkt->pkttype == PKT_PUBLIC_KEY
            || node->pkt->pkttype == PKT_PUBLIC_SUBKEY))
        continue;
      pk = node->pkt->pkt.public_key;
      keyid_from_pk (pk, kid); /* Make sure pk->keyid is set.  */
      if (!pk->main_keyid[0] && !pk->main_keyid[1])
        {
          pk->main_keyid[0] = mainkid[0];
          pk->main_keyid[1] = mainkid[1];
        }
    }
}


/* For documentation see keydb.h.  */
void
merge_keys_and_selfsig (KBNODE keyblock)
{
  if (!keyblock)
    ;
  else if (keyblock->pkt->pkttype == PKT_PUBLIC_KEY)
    merge_selfsigs (keyblock);
  else
    log_debug ("FIXME: merging secret key blocks is not anymore available\n");
}


static int
parse_key_usage (PKT_signature * sig)
{
  int key_usage = 0;
  const byte *p;
  size_t n;
  byte flags;

  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_KEY_FLAGS, &n);
  if (p && n)
    {
      /* First octet of the keyflags.  */
      flags = *p;

      if (flags & 1)
	{
	  key_usage |= PUBKEY_USAGE_CERT;
	  flags &= ~1;
	}

      if (flags & 2)
	{
	  key_usage |= PUBKEY_USAGE_SIG;
	  flags &= ~2;
	}

      /* We do not distinguish between encrypting communications and
         encrypting storage. */
      if (flags & (0x04 | 0x08))
	{
	  key_usage |= PUBKEY_USAGE_ENC;
	  flags &= ~(0x04 | 0x08);
	}

      if (flags & 0x20)
	{
	  key_usage |= PUBKEY_USAGE_AUTH;
	  flags &= ~0x20;
	}

      if (flags)
	key_usage |= PUBKEY_USAGE_UNKNOWN;

      if (!key_usage)
	key_usage |= PUBKEY_USAGE_NONE;
    }
  else if (p) /* Key flags of length zero.  */
    key_usage |= PUBKEY_USAGE_NONE;

  /* We set PUBKEY_USAGE_UNKNOWN to indicate that this key has a
     capability that we do not handle.  This serves to distinguish
     between a zero key usage which we handle as the default
     capabilities for that algorithm, and a usage that we do not
     handle.  Likewise we use PUBKEY_USAGE_NONE to indicate that
     key_flags have been given but they do not specify any usage.  */

  return key_usage;
}


/* Apply information from SIGNODE (which is the valid self-signature
 * associated with that UID) to the UIDNODE:
 * - weather the UID has been revoked
 * - assumed creation date of the UID
 * - temporary store the keyflags here
 * - temporary store the key expiration time here
 * - mark whether the primary user ID flag hat been set.
 * - store the preferences
 */
static void
fixup_uidnode (KBNODE uidnode, KBNODE signode, u32 keycreated)
{
  PKT_user_id *uid = uidnode->pkt->pkt.user_id;
  PKT_signature *sig = signode->pkt->pkt.signature;
  const byte *p, *sym, *hash, *zip;
  size_t n, nsym, nhash, nzip;

  sig->flags.chosen_selfsig = 1;/* We chose this one. */
  uid->created = 0;		/* Not created == invalid. */
  if (IS_UID_REV (sig))
    {
      uid->is_revoked = 1;
      return; /* Has been revoked.  */
    }
  else
    uid->is_revoked = 0;

  uid->expiredate = sig->expiredate;

  if (sig->flags.expired)
    {
      uid->is_expired = 1;
      return; /* Has expired.  */
    }
  else
    uid->is_expired = 0;

  uid->created = sig->timestamp; /* This one is okay. */
  uid->selfsigversion = sig->version;
  /* If we got this far, it's not expired :) */
  uid->is_expired = 0;

  /* Store the key flags in the helper variable for later processing.  */
  uid->help_key_usage = parse_key_usage (sig);

  /* Ditto for the key expiration.  */
  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_KEY_EXPIRE, NULL);
  if (p && buf32_to_u32 (p))
    uid->help_key_expire = keycreated + buf32_to_u32 (p);
  else
    uid->help_key_expire = 0;

  /* Set the primary user ID flag - we will later wipe out some
   * of them to only have one in our keyblock.  */
  uid->is_primary = 0;
  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_PRIMARY_UID, NULL);
  if (p && *p)
    uid->is_primary = 2;

  /* We could also query this from the unhashed area if it is not in
   * the hased area and then later try to decide which is the better
   * there should be no security problem with this.
   * For now we only look at the hashed one.  */

  /* Now build the preferences list.  These must come from the
     hashed section so nobody can modify the ciphers a key is
     willing to accept.  */
  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_PREF_SYM, &n);
  sym = p;
  nsym = p ? n : 0;
  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_PREF_HASH, &n);
  hash = p;
  nhash = p ? n : 0;
  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_PREF_COMPR, &n);
  zip = p;
  nzip = p ? n : 0;
  if (uid->prefs)
    xfree (uid->prefs);
  n = nsym + nhash + nzip;
  if (!n)
    uid->prefs = NULL;
  else
    {
      uid->prefs = xmalloc (sizeof (*uid->prefs) * (n + 1));
      n = 0;
      for (; nsym; nsym--, n++)
	{
	  uid->prefs[n].type = PREFTYPE_SYM;
	  uid->prefs[n].value = *sym++;
	}
      for (; nhash; nhash--, n++)
	{
	  uid->prefs[n].type = PREFTYPE_HASH;
	  uid->prefs[n].value = *hash++;
	}
      for (; nzip; nzip--, n++)
	{
	  uid->prefs[n].type = PREFTYPE_ZIP;
	  uid->prefs[n].value = *zip++;
	}
      uid->prefs[n].type = PREFTYPE_NONE; /* End of list marker  */
      uid->prefs[n].value = 0;
    }

  /* See whether we have the MDC feature.  */
  uid->flags.mdc = 0;
  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_FEATURES, &n);
  if (p && n && (p[0] & 0x01))
    uid->flags.mdc = 1;

  /* And the keyserver modify flag.  */
  uid->flags.ks_modify = 1;
  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_KS_FLAGS, &n);
  if (p && n && (p[0] & 0x80))
    uid->flags.ks_modify = 0;
}

static void
sig_to_revoke_info (PKT_signature * sig, struct revoke_info *rinfo)
{
  rinfo->date = sig->timestamp;
  rinfo->algo = sig->pubkey_algo;
  rinfo->keyid[0] = sig->keyid[0];
  rinfo->keyid[1] = sig->keyid[1];
}


/* Given a keyblock, parse the key block and extract various pieces of
   information and save them with the primary key packet and the user
   id packets.  For instance, some information is stored in signature
   packets.  We find the latest such valid packet (since the user can
   change that information) and copy its contents into the
   PKT_public_key.

   Note that R_REVOKED may be set to 0, 1 or 2.

   This function fills in the following fields in the primary key's
   keyblock:

     main_keyid          (computed)
     revkey / numrevkeys (derived from self signed key data)
     flags.valid         (whether we have at least 1 self-sig)
     flags.maybe_revoked (whether a designed revoked the key, but
                          we are missing the key to check the sig)
     selfsigversion      (highest version of any valid self-sig)
     pubkey_usage        (derived from most recent self-sig or most
                          recent user id)
     has_expired         (various sources)
     expiredate          (various sources)

  See the documentation for fixup_uidnode for how the user id packets
  are modified.  In addition to that the primary user id's is_primary
  field is set to 1 and the other user id's is_primary are set to
  0.  */
static void
merge_selfsigs_main (KBNODE keyblock, int *r_revoked,
		     struct revoke_info *rinfo)
{
  PKT_public_key *pk = NULL;
  KBNODE k;
  u32 kid[2];
  u32 sigdate, uiddate, uiddate2;
  KBNODE signode, uidnode, uidnode2;
  u32 curtime = make_timestamp ();
  unsigned int key_usage = 0;
  u32 keytimestamp = 0;
  u32 key_expire = 0;
  int key_expire_seen = 0;
  byte sigversion = 0;

  *r_revoked = 0;
  memset (rinfo, 0, sizeof (*rinfo));

  /* Section 11.1 of RFC 4880 determines the order of packets within a
     message.  There are three sections, which must occur in the
     following order: the public key, the user ids and user attributes
     and the subkeys.  Within each section, each primary packet (e.g.,
     a user id packet) is followed by one or more signature packets,
     which modify that packet.  */

  /* According to Section 11.1 of RFC 4880, the public key must be the
     first packet.  */
  if (keyblock->pkt->pkttype != PKT_PUBLIC_KEY)
    /* parse_keyblock_image ensures that the first packet is the
       public key.  */
    BUG ();
  pk = keyblock->pkt->pkt.public_key;
  keytimestamp = pk->timestamp;

  keyid_from_pk (pk, kid);
  pk->main_keyid[0] = kid[0];
  pk->main_keyid[1] = kid[1];

  if (pk->version < 4)
    {
      /* Before v4 the key packet itself contains the expiration date
       * and there was no way to change it, so we start with the one
       * from the key packet.  */
      key_expire = pk->max_expiredate;
      key_expire_seen = 1;
    }

  /* First pass:

      - Find the latest direct key self-signature.  We assume that the
        newest one overrides all others.

      - Determine whether the key has been revoked.

      - Gather all revocation keys (unlike other data, we don't just
        take them from the latest self-signed packet).

      - Determine max (sig[...]->version).
   */

  /* Reset this in case this key was already merged. */
  xfree (pk->revkey);
  pk->revkey = NULL;
  pk->numrevkeys = 0;

  signode = NULL;
  sigdate = 0; /* Helper variable to find the latest signature.  */

  /* According to Section 11.1 of RFC 4880, the public key comes first
     and is immediately followed by any signature packets that modify
     it.  */
  for (k = keyblock;
       k && k->pkt->pkttype != PKT_USER_ID
	 && k->pkt->pkttype != PKT_ATTRIBUTE
	 && k->pkt->pkttype != PKT_PUBLIC_SUBKEY;
       k = k->next)
    {
      if (k->pkt->pkttype == PKT_SIGNATURE)
	{
	  PKT_signature *sig = k->pkt->pkt.signature;
	  if (sig->keyid[0] == kid[0] && sig->keyid[1] == kid[1])
	    /* Self sig.  */
	    {
	      if (check_key_signature (keyblock, k, NULL))
		; /* Signature did not verify.  */
	      else if (IS_KEY_REV (sig))
		{
		  /* Key has been revoked - there is no way to
		   * override such a revocation, so we theoretically
		   * can stop now.  We should not cope with expiration
		   * times for revocations here because we have to
		   * assume that an attacker can generate all kinds of
		   * signatures.  However due to the fact that the key
		   * has been revoked it does not harm either and by
		   * continuing we gather some more info on that
		   * key.  */
		  *r_revoked = 1;
		  sig_to_revoke_info (sig, rinfo);
		}
	      else if (IS_KEY_SIG (sig))
		{
		  /* Add the indicated revocations keys from all
		     signatures not just the latest.  We do this
		     because you need multiple 1F sigs to properly
		     handle revocation keys (PGP does it this way, and
		     a revocation key could be sensitive and hence in
		     a different signature). */
		  if (sig->revkey)
		    {
		      int i;

		      pk->revkey =
			xrealloc (pk->revkey, sizeof (struct revocation_key) *
				  (pk->numrevkeys + sig->numrevkeys));

		      for (i = 0; i < sig->numrevkeys; i++)
			memcpy (&pk->revkey[pk->numrevkeys++],
				&sig->revkey[i],
				sizeof (struct revocation_key));
		    }

		  if (sig->timestamp >= sigdate)
		    /* This is the latest signature so far.  */
		    {
		      if (sig->flags.expired)
			; /* Signature has expired - ignore it.  */
		      else
			{
			  sigdate = sig->timestamp;
			  signode = k;
			  if (sig->version > sigversion)
			    sigversion = sig->version;

			}
		    }
		}
	    }
	}
    }

  /* Remove dupes from the revocation keys.  */
  if (pk->revkey)
    {
      int i, j, x, changed = 0;

      for (i = 0; i < pk->numrevkeys; i++)
	{
	  for (j = i + 1; j < pk->numrevkeys; j++)
	    {
	      if (memcmp (&pk->revkey[i], &pk->revkey[j],
			  sizeof (struct revocation_key)) == 0)
		{
		  /* remove j */

		  for (x = j; x < pk->numrevkeys - 1; x++)
		    pk->revkey[x] = pk->revkey[x + 1];

		  pk->numrevkeys--;
		  j--;
		  changed = 1;
		}
	    }
	}

      if (changed)
	pk->revkey = xrealloc (pk->revkey,
			       pk->numrevkeys *
			       sizeof (struct revocation_key));
    }

  if (signode)
    /* SIGNODE is the 1F signature packet with the latest creation
       time.  Extract some information from it.  */
    {
      /* Some information from a direct key signature take precedence
       * over the same information given in UID sigs.  */
      PKT_signature *sig = signode->pkt->pkt.signature;
      const byte *p;

      key_usage = parse_key_usage (sig);

      p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_KEY_EXPIRE, NULL);
      if (p && buf32_to_u32 (p))
	{
	  key_expire = keytimestamp + buf32_to_u32 (p);
	  key_expire_seen = 1;
	}

      /* Mark that key as valid: One direct key signature should
       * render a key as valid.  */
      pk->flags.valid = 1;
    }

  /* Pass 1.5: Look for key revocation signatures that were not made
     by the key (i.e. did a revocation key issue a revocation for
     us?).  Only bother to do this if there is a revocation key in the
     first place and we're not revoked already.  */

  if (!*r_revoked && pk->revkey)
    for (k = keyblock; k && k->pkt->pkttype != PKT_USER_ID; k = k->next)
      {
	if (k->pkt->pkttype == PKT_SIGNATURE)
	  {
	    PKT_signature *sig = k->pkt->pkt.signature;

	    if (IS_KEY_REV (sig) &&
		(sig->keyid[0] != kid[0] || sig->keyid[1] != kid[1]))
	      {
		int rc = check_revocation_keys (pk, sig);
		if (rc == 0)
		  {
		    *r_revoked = 2;
		    sig_to_revoke_info (sig, rinfo);
		    /* Don't continue checking since we can't be any
		       more revoked than this.  */
		    break;
		  }
		else if (gpg_err_code (rc) == GPG_ERR_NO_PUBKEY)
		  pk->flags.maybe_revoked = 1;

		/* A failure here means the sig did not verify, was
		   not issued by a revocation key, or a revocation
		   key loop was broken.  If a revocation key isn't
		   findable, however, the key might be revoked and
		   we don't know it.  */

		/* TODO: In the future handle subkey and cert
		   revocations?  PGP doesn't, but it's in 2440. */
	      }
	  }
      }

  /* Second pass: Look at the self-signature of all user IDs.  */

  /* According to RFC 4880 section 11.1, user id and attribute packets
     are in the second section, after the public key packet and before
     the subkey packets.  */
  signode = uidnode = NULL;
  sigdate = 0; /* Helper variable to find the latest signature in one UID. */
  for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY; k = k->next)
    {
      if (k->pkt->pkttype == PKT_USER_ID || k->pkt->pkttype == PKT_ATTRIBUTE)
	/* New user id packet.  */
	{
	  if (uidnode && signode)
	    /* Apply the data from the most recent self-signed packet
	       to the preceding user id packet.  */
	    {
	      fixup_uidnode (uidnode, signode, keytimestamp);
	      pk->flags.valid = 1;
	    }
	  /* Clear SIGNODE.  The only relevant self-signed data for
	     UIDNODE follows it.  */
	  if (k->pkt->pkttype == PKT_USER_ID)
	    uidnode = k;
	  else
	    uidnode = NULL;
	  signode = NULL;
	  sigdate = 0;
	}
      else if (k->pkt->pkttype == PKT_SIGNATURE && uidnode)
	{
	  PKT_signature *sig = k->pkt->pkt.signature;
	  if (sig->keyid[0] == kid[0] && sig->keyid[1] == kid[1])
	    {
	      if (check_key_signature (keyblock, k, NULL))
		;		/* signature did not verify */
	      else if ((IS_UID_SIG (sig) || IS_UID_REV (sig))
		       && sig->timestamp >= sigdate)
		{
		  /* Note: we allow to invalidate cert revocations
		   * by a newer signature.  An attacker can't use this
		   * because a key should be revoked with a key revocation.
		   * The reason why we have to allow for that is that at
		   * one time an email address may become invalid but later
		   * the same email address may become valid again (hired,
		   * fired, hired again).  */

		  sigdate = sig->timestamp;
		  signode = k;
		  signode->pkt->pkt.signature->flags.chosen_selfsig = 0;
		  if (sig->version > sigversion)
		    sigversion = sig->version;
		}
	    }
	}
    }
  if (uidnode && signode)
    {
      fixup_uidnode (uidnode, signode, keytimestamp);
      pk->flags.valid = 1;
    }

  /* If the key isn't valid yet, and we have
     --allow-non-selfsigned-uid set, then force it valid. */
  if (!pk->flags.valid && opt.allow_non_selfsigned_uid)
    {
      if (opt.verbose)
	log_info (_("Invalid key %s made valid by"
		    " --allow-non-selfsigned-uid\n"), keystr_from_pk (pk));
      pk->flags.valid = 1;
    }

  /* The key STILL isn't valid, so try and find an ultimately
     trusted signature. */
  if (!pk->flags.valid)
    {
      uidnode = NULL;

      for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY;
	   k = k->next)
	{
	  if (k->pkt->pkttype == PKT_USER_ID)
	    uidnode = k;
	  else if (k->pkt->pkttype == PKT_SIGNATURE && uidnode)
	    {
	      PKT_signature *sig = k->pkt->pkt.signature;

	      if (sig->keyid[0] != kid[0] || sig->keyid[1] != kid[1])
		{
		  PKT_public_key *ultimate_pk;

		  ultimate_pk = xmalloc_clear (sizeof (*ultimate_pk));

		  /* We don't want to use the full get_pubkey to
		     avoid infinite recursion in certain cases.
		     There is no reason to check that an ultimately
		     trusted key is still valid - if it has been
		     revoked the user should also remove the
		     ultimate trust flag.  */
		  if (get_pubkey_fast (ultimate_pk, sig->keyid) == 0
		      && check_key_signature2 (keyblock, k, ultimate_pk,
					       NULL, NULL, NULL, NULL) == 0
		      && get_ownertrust (ultimate_pk) == TRUST_ULTIMATE)
		    {
		      free_public_key (ultimate_pk);
		      pk->flags.valid = 1;
		      break;
		    }

		  free_public_key (ultimate_pk);
		}
	    }
	}
    }

  /* Record the highest selfsig version so we know if this is a v3
     key through and through, or a v3 key with a v4 selfsig
     somewhere.  This is useful in a few places to know if the key
     must be treated as PGP2-style or OpenPGP-style.  Note that a
     selfsig revocation with a higher version number will also raise
     this value.  This is okay since such a revocation must be
     issued by the user (i.e. it cannot be issued by someone else to
     modify the key behavior.) */

  pk->selfsigversion = sigversion;

  /* Now that we had a look at all user IDs we can now get some information
   * from those user IDs.
   */

  if (!key_usage)
    {
      /* Find the latest user ID with key flags set. */
      uiddate = 0; /* Helper to find the latest user ID.  */
      for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY;
	   k = k->next)
	{
	  if (k->pkt->pkttype == PKT_USER_ID)
	    {
	      PKT_user_id *uid = k->pkt->pkt.user_id;
	      if (uid->help_key_usage && uid->created > uiddate)
		{
		  key_usage = uid->help_key_usage;
		  uiddate = uid->created;
		}
	    }
	}
    }
  if (!key_usage)
    {
      /* No key flags at all: get it from the algo.  */
      key_usage = openpgp_pk_algo_usage (pk->pubkey_algo);
    }
  else
    {
      /* Check that the usage matches the usage as given by the algo.  */
      int x = openpgp_pk_algo_usage (pk->pubkey_algo);
      if (x) /* Mask it down to the actual allowed usage.  */
	key_usage &= x;
    }

  /* Whatever happens, it's a primary key, so it can certify. */
  pk->pubkey_usage = key_usage | PUBKEY_USAGE_CERT;

  if (!key_expire_seen)
    {
      /* Find the latest valid user ID with a key expiration set
       * Note, that this may be a different one from the above because
       * some user IDs may have no expiration date set.  */
      uiddate = 0;
      for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY;
	   k = k->next)
	{
	  if (k->pkt->pkttype == PKT_USER_ID)
	    {
	      PKT_user_id *uid = k->pkt->pkt.user_id;
	      if (uid->help_key_expire && uid->created > uiddate)
		{
		  key_expire = uid->help_key_expire;
		  uiddate = uid->created;
		}
	    }
	}
    }

  /* Currently only v3 keys have a maximum expiration date, but I'll
     bet v5 keys get this feature again. */
  if (key_expire == 0
      || (pk->max_expiredate && key_expire > pk->max_expiredate))
    key_expire = pk->max_expiredate;

  pk->has_expired = key_expire >= curtime ? 0 : key_expire;
  pk->expiredate = key_expire;

  /* Fixme: we should see how to get rid of the expiretime fields  but
   * this needs changes at other places too. */

  /* And now find the real primary user ID and delete all others.  */
  uiddate = uiddate2 = 0;
  uidnode = uidnode2 = NULL;
  for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY; k = k->next)
    {
      if (k->pkt->pkttype == PKT_USER_ID && !k->pkt->pkt.user_id->attrib_data)
	{
	  PKT_user_id *uid = k->pkt->pkt.user_id;
	  if (uid->is_primary)
	    {
	      if (uid->created > uiddate)
		{
		  uiddate = uid->created;
		  uidnode = k;
		}
	      else if (uid->created == uiddate && uidnode)
		{
		  /* The dates are equal, so we need to do a
		     different (and arbitrary) comparison.  This
		     should rarely, if ever, happen.  It's good to
		     try and guarantee that two different GnuPG
		     users with two different keyrings at least pick
		     the same primary. */
		  if (cmp_user_ids (uid, uidnode->pkt->pkt.user_id) > 0)
		    uidnode = k;
		}
	    }
	  else
	    {
	      if (uid->created > uiddate2)
		{
		  uiddate2 = uid->created;
		  uidnode2 = k;
		}
	      else if (uid->created == uiddate2 && uidnode2)
		{
		  if (cmp_user_ids (uid, uidnode2->pkt->pkt.user_id) > 0)
		    uidnode2 = k;
		}
	    }
	}
    }
  if (uidnode)
    {
      for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY;
	   k = k->next)
	{
	  if (k->pkt->pkttype == PKT_USER_ID &&
	      !k->pkt->pkt.user_id->attrib_data)
	    {
	      PKT_user_id *uid = k->pkt->pkt.user_id;
	      if (k != uidnode)
		uid->is_primary = 0;
	    }
	}
    }
  else if (uidnode2)
    {
      /* None is flagged primary - use the latest user ID we have,
         and disambiguate with the arbitrary packet comparison. */
      uidnode2->pkt->pkt.user_id->is_primary = 1;
    }
  else
    {
      /* None of our uids were self-signed, so pick the one that
         sorts first to be the primary.  This is the best we can do
         here since there are no self sigs to date the uids. */

      uidnode = NULL;

      for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY;
	   k = k->next)
	{
	  if (k->pkt->pkttype == PKT_USER_ID
	      && !k->pkt->pkt.user_id->attrib_data)
	    {
	      if (!uidnode)
		{
		  uidnode = k;
		  uidnode->pkt->pkt.user_id->is_primary = 1;
		  continue;
		}
	      else
		{
		  if (cmp_user_ids (k->pkt->pkt.user_id,
				    uidnode->pkt->pkt.user_id) > 0)
		    {
		      uidnode->pkt->pkt.user_id->is_primary = 0;
		      uidnode = k;
		      uidnode->pkt->pkt.user_id->is_primary = 1;
		    }
		  else
		    k->pkt->pkt.user_id->is_primary = 0;	/* just to be
								   safe */
		}
	    }
	}
    }
}

/* Convert a buffer to a signature.  Useful for 0x19 embedded sigs.
   Caller must free the signature when they are done. */
static PKT_signature *
buf_to_sig (const byte * buf, size_t len)
{
  PKT_signature *sig = xmalloc_clear (sizeof (PKT_signature));
  IOBUF iobuf = iobuf_temp_with_content (buf, len);
  int save_mode = set_packet_list_mode (0);

  if (parse_signature (iobuf, PKT_SIGNATURE, len, sig) != 0)
    {
      xfree (sig);
      sig = NULL;
    }

  set_packet_list_mode (save_mode);
  iobuf_close (iobuf);

  return sig;
}

/* Use the self-signed data to fill in various fields in subkeys.

   KEYBLOCK is the whole keyblock.  SUBNODE is the subkey to fill in.

   Sets the following fields on the subkey:

     main_keyid
     flags.valid        if the subkey has a valid self-sig binding
     flags.revoked
     flags.backsig
     pubkey_usage
     has_expired
     expired_date

   On this subkey's most revent valid self-signed packet, the
   following field is set:

     flags.chosen_selfsig
  */
static void
merge_selfsigs_subkey (KBNODE keyblock, KBNODE subnode)
{
  PKT_public_key *mainpk = NULL, *subpk = NULL;
  PKT_signature *sig;
  KBNODE k;
  u32 mainkid[2];
  u32 sigdate = 0;
  KBNODE signode;
  u32 curtime = make_timestamp ();
  unsigned int key_usage = 0;
  u32 keytimestamp = 0;
  u32 key_expire = 0;
  const byte *p;

  if (subnode->pkt->pkttype != PKT_PUBLIC_SUBKEY)
    BUG ();
  mainpk = keyblock->pkt->pkt.public_key;
  if (mainpk->version < 4)
    return;/* (actually this should never happen) */
  keyid_from_pk (mainpk, mainkid);
  subpk = subnode->pkt->pkt.public_key;
  keytimestamp = subpk->timestamp;

  subpk->flags.valid = 0;
  subpk->main_keyid[0] = mainpk->main_keyid[0];
  subpk->main_keyid[1] = mainpk->main_keyid[1];

  /* Find the latest key binding self-signature.  */
  signode = NULL;
  sigdate = 0; /* Helper to find the latest signature.  */
  for (k = subnode->next; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY;
       k = k->next)
    {
      if (k->pkt->pkttype == PKT_SIGNATURE)
	{
	  sig = k->pkt->pkt.signature;
	  if (sig->keyid[0] == mainkid[0] && sig->keyid[1] == mainkid[1])
	    {
	      if (check_key_signature (keyblock, k, NULL))
		; /* Signature did not verify.  */
	      else if (IS_SUBKEY_REV (sig))
		{
		  /* Note that this means that the date on a
		     revocation sig does not matter - even if the
		     binding sig is dated after the revocation sig,
		     the subkey is still marked as revoked.  This
		     seems ok, as it is just as easy to make new
		     subkeys rather than re-sign old ones as the
		     problem is in the distribution.  Plus, PGP (7)
		     does this the same way.  */
		  subpk->flags.revoked = 1;
		  sig_to_revoke_info (sig, &subpk->revoked);
		  /* Although we could stop now, we continue to
		   * figure out other information like the old expiration
		   * time.  */
		}
	      else if (IS_SUBKEY_SIG (sig) && sig->timestamp >= sigdate)
		{
		  if (sig->flags.expired)
		    ; /* Signature has expired - ignore it.  */
		  else
		    {
		      sigdate = sig->timestamp;
		      signode = k;
		      signode->pkt->pkt.signature->flags.chosen_selfsig = 0;
		    }
		}
	    }
	}
    }

  /* No valid key binding.  */
  if (!signode)
    return;

  sig = signode->pkt->pkt.signature;
  sig->flags.chosen_selfsig = 1; /* So we know which selfsig we chose later.  */

  key_usage = parse_key_usage (sig);
  if (!key_usage)
    {
      /* No key flags at all: get it from the algo.  */
      key_usage = openpgp_pk_algo_usage (subpk->pubkey_algo);
    }
  else
    {
      /* Check that the usage matches the usage as given by the algo.  */
      int x = openpgp_pk_algo_usage (subpk->pubkey_algo);
      if (x) /* Mask it down to the actual allowed usage.  */
	key_usage &= x;
    }

  subpk->pubkey_usage = key_usage;

  p = parse_sig_subpkt (sig->hashed, SIGSUBPKT_KEY_EXPIRE, NULL);
  if (p && buf32_to_u32 (p))
    key_expire = keytimestamp + buf32_to_u32 (p);
  else
    key_expire = 0;
  subpk->has_expired = key_expire >= curtime ? 0 : key_expire;
  subpk->expiredate = key_expire;

  /* Algo doesn't exist.  */
  if (openpgp_pk_test_algo (subpk->pubkey_algo))
    return;

  subpk->flags.valid = 1;

  /* Find the most recent 0x19 embedded signature on our self-sig. */
  if (!subpk->flags.backsig)
    {
      int seq = 0;
      size_t n;
      PKT_signature *backsig = NULL;

      sigdate = 0;

      /* We do this while() since there may be other embedded
         signatures in the future.  We only want 0x19 here. */

      while ((p = enum_sig_subpkt (sig->hashed,
				   SIGSUBPKT_SIGNATURE, &n, &seq, NULL)))
	if (n > 3
	    && ((p[0] == 3 && p[2] == 0x19) || (p[0] == 4 && p[1] == 0x19)))
	  {
	    PKT_signature *tempsig = buf_to_sig (p, n);
	    if (tempsig)
	      {
		if (tempsig->timestamp > sigdate)
		  {
		    if (backsig)
		      free_seckey_enc (backsig);

		    backsig = tempsig;
		    sigdate = backsig->timestamp;
		  }
		else
		  free_seckey_enc (tempsig);
	      }
	  }

      seq = 0;

      /* It is safe to have this in the unhashed area since the 0x19
         is located on the selfsig for convenience, not security. */

      while ((p = enum_sig_subpkt (sig->unhashed, SIGSUBPKT_SIGNATURE,
				   &n, &seq, NULL)))
	if (n > 3
	    && ((p[0] == 3 && p[2] == 0x19) || (p[0] == 4 && p[1] == 0x19)))
	  {
	    PKT_signature *tempsig = buf_to_sig (p, n);
	    if (tempsig)
	      {
		if (tempsig->timestamp > sigdate)
		  {
		    if (backsig)
		      free_seckey_enc (backsig);

		    backsig = tempsig;
		    sigdate = backsig->timestamp;
		  }
		else
		  free_seckey_enc (tempsig);
	      }
	  }

      if (backsig)
	{
	  /* At this point, backsig contains the most recent 0x19 sig.
	     Let's see if it is good. */

	  /* 2==valid, 1==invalid, 0==didn't check */
	  if (check_backsig (mainpk, subpk, backsig) == 0)
	    subpk->flags.backsig = 2;
	  else
	    subpk->flags.backsig = 1;

	  free_seckey_enc (backsig);
	}
    }
}


/* Merge information from the self-signatures with the public key,
   subkeys and user ids to make using them more easy.

   See documentation for merge_selfsigs_main, merge_selfsigs_subkey
   and fixup_uidnode for exactly which fields are updated.  */
static void
merge_selfsigs (KBNODE keyblock)
{
  KBNODE k;
  int revoked;
  struct revoke_info rinfo;
  PKT_public_key *main_pk;
  prefitem_t *prefs;
  unsigned int mdc_feature;

  if (keyblock->pkt->pkttype != PKT_PUBLIC_KEY)
    {
      if (keyblock->pkt->pkttype == PKT_SECRET_KEY)
	{
	  log_error ("expected public key but found secret key "
		     "- must stop\n");
	  /* We better exit here because a public key is expected at
	     other places too.  FIXME: Figure this out earlier and
	     don't get to here at all */
	  g10_exit (1);
	}
      BUG ();
    }

  merge_selfsigs_main (keyblock, &revoked, &rinfo);

  /* Now merge in the data from each of the subkeys.  */
  for (k = keyblock; k; k = k->next)
    {
      if (k->pkt->pkttype == PKT_PUBLIC_SUBKEY)
	{
	  merge_selfsigs_subkey (keyblock, k);
	}
    }

  main_pk = keyblock->pkt->pkt.public_key;
  if (revoked || main_pk->has_expired || !main_pk->flags.valid)
    {
      /* If the primary key is revoked, expired, or invalid we
       * better set the appropriate flags on that key and all
       * subkeys.  */
      for (k = keyblock; k; k = k->next)
	{
	  if (k->pkt->pkttype == PKT_PUBLIC_KEY
	      || k->pkt->pkttype == PKT_PUBLIC_SUBKEY)
	    {
	      PKT_public_key *pk = k->pkt->pkt.public_key;
	      if (!main_pk->flags.valid)
		pk->flags.valid = 0;
	      if (revoked && !pk->flags.revoked)
		{
		  pk->flags.revoked = revoked;
		  memcpy (&pk->revoked, &rinfo, sizeof (rinfo));
		}
	      if (main_pk->has_expired)
		pk->has_expired = main_pk->has_expired;
	    }
	}
      return;
    }

  /* Set the preference list of all keys to those of the primary real
   * user ID.  Note: we use these preferences when we don't know by
   * which user ID the key has been selected.
   * fixme: we should keep atoms of commonly used preferences or
   * use reference counting to optimize the preference lists storage.
   * FIXME: it might be better to use the intersection of
   * all preferences.
   * Do a similar thing for the MDC feature flag.  */
  prefs = NULL;
  mdc_feature = 0;
  for (k = keyblock; k && k->pkt->pkttype != PKT_PUBLIC_SUBKEY; k = k->next)
    {
      if (k->pkt->pkttype == PKT_USER_ID
	  && !k->pkt->pkt.user_id->attrib_data
	  && k->pkt->pkt.user_id->is_primary)
	{
	  prefs = k->pkt->pkt.user_id->prefs;
	  mdc_feature = k->pkt->pkt.user_id->flags.mdc;
	  break;
	}
    }
  for (k = keyblock; k; k = k->next)
    {
      if (k->pkt->pkttype == PKT_PUBLIC_KEY
	  || k->pkt->pkttype == PKT_PUBLIC_SUBKEY)
	{
	  PKT_public_key *pk = k->pkt->pkt.public_key;
	  if (pk->prefs)
	    xfree (pk->prefs);
	  pk->prefs = copy_prefs (prefs);
	  pk->flags.mdc = mdc_feature;
	}
    }
}



/* See whether the key satisfies any additional requirements specified
   in CTX.  If so, return 1 and set CTX->FOUND_KEY to an appropriate
   key or subkey.  Otherwise, return 0 if there was no appropriate
   key.

   In case the primary key is not required, select a suitable subkey.
   We need the primary key if PUBKEY_USAGE_CERT is set in
   CTX->REQ_USAGE or we are in PGP6 or PGP7 mode and PUBKEY_USAGE_SIG
   is set in CTX->REQ_USAGE.

   If any of PUBKEY_USAGE_SIG, PUBKEY_USAGE_ENC and PUBKEY_USAGE_CERT
   are set in CTX->REQ_USAGE, we filter by the key's function.
   Concretely, if PUBKEY_USAGE_SIG and PUBKEY_USAGE_CERT are set, then
   we only return a key if it is (at least) either a signing or a
   certification key.

   If CTX->REQ_USAGE is set, then we reject any keys that are not good
   (i.e., valid, not revoked, not expired, etc.).  This allows the
   getkey functions to be used for plain key listings.

   Sets the matched key's user id field (pk->user_id) to the user id
   that matched the low-level search criteria or NULL.


   This function needs to handle several different cases:

    1. No requested usage and no primary key requested
       Examples for this case are that we have a keyID to be used
       for decrytion or verification.
    2. No usage but primary key requested
       This is the case for all functions which work on an
       entire keyblock, e.g. for editing or listing
    3. Usage and primary key requested
       FXME
    4. Usage but no primary key requested
       FIXME

 */
static KBNODE
finish_lookup (GETKEY_CTX ctx, KBNODE keyblock)
{
  KBNODE k;

  /* If CTX->EXACT is set, the key or subkey that actually matched the
     low-level search criteria.  */
  KBNODE foundk = NULL;
  /* The user id (if any) that matched the low-level search criteria.  */
  PKT_user_id *foundu = NULL;

#define USAGE_MASK  (PUBKEY_USAGE_SIG|PUBKEY_USAGE_ENC|PUBKEY_USAGE_CERT)
  unsigned int req_usage = (ctx->req_usage & USAGE_MASK);

  /* Request the primary if we're certifying another key, and also
     if signing data while --pgp6 or --pgp7 is on since pgp 6 and 7
     do not understand signatures made by a signing subkey.  PGP 8
     does. */
  int req_prim = (ctx->req_usage & PUBKEY_USAGE_CERT) ||
    ((PGP6 || PGP7) && (ctx->req_usage & PUBKEY_USAGE_SIG));

  u32 curtime = make_timestamp ();

  u32 latest_date;
  KBNODE latest_key;

  assert (keyblock->pkt->pkttype == PKT_PUBLIC_KEY);

  if (ctx->exact)
    /* Get the key or subkey that matched the low-level search
       criteria.  */
    {
      for (k = keyblock; k; k = k->next)
	{
	  if ((k->flag & 1))
	    {
	      assert (k->pkt->pkttype == PKT_PUBLIC_KEY
		      || k->pkt->pkttype == PKT_PUBLIC_SUBKEY);
	      foundk = k;
	      break;
	    }
	}
    }

  /* Get the user id that matched that low-level search criteria.  */
  for (k = keyblock; k; k = k->next)
    {
      if ((k->flag & 2))
	{
	  assert (k->pkt->pkttype == PKT_USER_ID);
	  foundu = k->pkt->pkt.user_id;
	  break;
	}
    }

  if (DBG_LOOKUP)
    log_debug ("finish_lookup: checking key %08lX (%s)(req_usage=%x)\n",
	       (ulong) keyid_from_pk (keyblock->pkt->pkt.public_key, NULL),
	       foundk ? "one" : "all", req_usage);

  if (!req_usage)
    {
      latest_key = foundk ? foundk : keyblock;
      goto found;
    }

  latest_date = 0;
  latest_key = NULL;
  /* Set latest_key to the latest (the one with the most recent
     timestamp) good (valid, not revoked, not expired, etc.) subkey.

     Don't bother if we are only looking for a primary key or we need
     an exact match and the exact match is not a subkey.  */
  if (req_prim || (foundk && foundk->pkt->pkttype != PKT_PUBLIC_SUBKEY))
    ;
  else
    {
      KBNODE nextk;

      /* Either start a loop or check just this one subkey.  */
      for (k = foundk ? foundk : keyblock; k; k = nextk)
	{
	  PKT_public_key *pk;

	  if (foundk)
	    /* If FOUNDK is not NULL, then only consider that exact
	       key, i.e., don't iterate.  */
	    nextk = NULL;
	  else
	    nextk = k->next;

	  if (k->pkt->pkttype != PKT_PUBLIC_SUBKEY)
	    continue;

	  pk = k->pkt->pkt.public_key;
	  if (DBG_LOOKUP)
	    log_debug ("\tchecking subkey %08lX\n",
		       (ulong) keyid_from_pk (pk, NULL));
	  if (!pk->flags.valid)
	    {
	      if (DBG_LOOKUP)
		log_debug ("\tsubkey not valid\n");
	      continue;
	    }
	  if (pk->flags.revoked)
	    {
	      if (DBG_LOOKUP)
		log_debug ("\tsubkey has been revoked\n");
	      continue;
	    }
	  if (pk->has_expired)
	    {
	      if (DBG_LOOKUP)
		log_debug ("\tsubkey has expired\n");
	      continue;
	    }
	  if (pk->timestamp > curtime && !opt.ignore_valid_from)
	    {
	      if (DBG_LOOKUP)
		log_debug ("\tsubkey not yet valid\n");
	      continue;
	    }

	  if (!((pk->pubkey_usage & USAGE_MASK) & req_usage))
	    {
	      if (DBG_LOOKUP)
		log_debug ("\tusage does not match: want=%x have=%x\n",
			   req_usage, pk->pubkey_usage);
	      continue;
	    }

	  if (DBG_LOOKUP)
	    log_debug ("\tsubkey might be fine\n");
	  /* In case a key has a timestamp of 0 set, we make sure
	     that it is used.  A better change would be to compare
	     ">=" but that might also change the selected keys and
	     is as such a more intrusive change.  */
	  if (pk->timestamp > latest_date || (!pk->timestamp && !latest_date))
	    {
	      latest_date = pk->timestamp;
	      latest_key = k;
	    }
	}
    }

  /* Check if the primary key is ok (valid, not revoke, not expire,
     matches requested usage) if:

       - we didn't find an appropriate subkey and we're not doing an
         exact search,

       - we're doing an exact match and the exact match was the
         primary key, or,

       - we're just considering the primary key.  */
  if ((!latest_key && !ctx->exact) || foundk == keyblock || req_prim)
    {
      PKT_public_key *pk;
      if (DBG_LOOKUP && !foundk && !req_prim)
	log_debug ("\tno suitable subkeys found - trying primary\n");
      pk = keyblock->pkt->pkt.public_key;
      if (!pk->flags.valid)
	{
	  if (DBG_LOOKUP)
	    log_debug ("\tprimary key not valid\n");
	}
      else if (pk->flags.revoked)
	{
	  if (DBG_LOOKUP)
	    log_debug ("\tprimary key has been revoked\n");
	}
      else if (pk->has_expired)
	{
	  if (DBG_LOOKUP)
	    log_debug ("\tprimary key has expired\n");
	}
      else if (!((pk->pubkey_usage & USAGE_MASK) & req_usage))
	{
	  if (DBG_LOOKUP)
	    log_debug ("\tprimary key usage does not match: "
		       "want=%x have=%x\n", req_usage, pk->pubkey_usage);
	}
      else /* Okay.  */
	{
	  if (DBG_LOOKUP)
	    log_debug ("\tprimary key may be used\n");
	  latest_key = keyblock;
	  latest_date = pk->timestamp;
	}
    }

  if (!latest_key)
    {
      if (DBG_LOOKUP)
	log_debug ("\tno suitable key found -  giving up\n");
      return NULL; /* Not found.  */
    }

found:
  if (DBG_LOOKUP)
    log_debug ("\tusing key %08lX\n",
	       (ulong) keyid_from_pk (latest_key->pkt->pkt.public_key, NULL));

  if (latest_key)
    {
      PKT_public_key *pk = latest_key->pkt->pkt.public_key;
      if (pk->user_id)
	free_user_id (pk->user_id);
      pk->user_id = scopy_user_id (foundu);
    }

  if (latest_key != keyblock && opt.verbose)
    {
      char *tempkeystr =
	xstrdup (keystr_from_pk (latest_key->pkt->pkt.public_key));
      log_info (_("using subkey %s instead of primary key %s\n"),
		tempkeystr, keystr_from_pk (keyblock->pkt->pkt.public_key));
      xfree (tempkeystr);
    }

  cache_user_id (keyblock);

  return latest_key ? latest_key : keyblock; /* Found.  */
}


/* A high-level function to lookup keys.

   This function builds on top of the low-level keydb API.  It first
   searches the database using the description stored in CTX->ITEMS,
   then it filters the results using CTX and, finally, if WANT_SECRET
   is set, it ignores any keys for which no secret key is available.

   Unlike the low-level search functions, this function also merges
   all of the self-signed data into the keys, subkeys and user id
   packets (see the merge_selfsigs for details).

   On success the key's keyblock is stored at *RET_KEYBLOCK.  */
static int
lookup (getkey_ctx_t ctx, kbnode_t *ret_keyblock, kbnode_t *ret_found_key,
	int want_secret)
{
  int rc;
  int no_suitable_key = 0;
  KBNODE keyblock = NULL;
  KBNODE found_key = NULL;

  for (;;)
    {
      rc = keydb_search (ctx->kr_handle, ctx->items, ctx->nitems, NULL);
      if (rc)
        break;

      /* If we are iterating over the entire database, then we need to
	 change from KEYDB_SEARCH_MODE_FIRST, which does an implicit
	 reset, to KEYDB_SEARCH_MODE_NEXT, which gets the next
	 record.  */
      if (ctx->nitems && ctx->items->mode == KEYDB_SEARCH_MODE_FIRST)
	ctx->items->mode = KEYDB_SEARCH_MODE_NEXT;

      rc = keydb_get_keyblock (ctx->kr_handle, &keyblock);
      if (rc)
	{
	  log_error ("keydb_get_keyblock failed: %s\n", gpg_strerror (rc));
	  rc = 0;
	  goto skip;
	}

      if (want_secret && agent_probe_any_secret_key (NULL, keyblock))
        goto skip; /* No secret key available.  */

      /* Warning: node flag bits 0 and 1 should be preserved by
       * merge_selfsigs.  For secret keys, premerge transferred the
       * keys to the keyblock.  */
      merge_selfsigs (keyblock);
      found_key = finish_lookup (ctx, keyblock);
      if (found_key)
	{
	  no_suitable_key = 0;
	  goto found;
	}
      else
	no_suitable_key = 1;

    skip:
      /* Release resources and continue search. */
      release_kbnode (keyblock);
      keyblock = NULL;
      /* The keyblock cache ignores the current "file position".
         Thus, if we request the next result and the cache matches
         (and it will since it is what we just looked for), we'll get
         the same entry back!  We can avoid this infinite loop by
         disabling the cache.  */
      keydb_disable_caching (ctx->kr_handle);
    }

found:
  if (rc && gpg_err_code (rc) != GPG_ERR_NOT_FOUND)
    log_error ("keydb_search failed: %s\n", gpg_strerror (rc));

  if (!rc)
    {
      *ret_keyblock = keyblock; /* Return the keyblock.  */
      keyblock = NULL;
    }
  else if (gpg_err_code (rc) == GPG_ERR_NOT_FOUND && no_suitable_key)
    rc = want_secret? GPG_ERR_UNUSABLE_SECKEY : GPG_ERR_UNUSABLE_PUBKEY;
  else if (gpg_err_code (rc) == GPG_ERR_NOT_FOUND)
    rc = want_secret? GPG_ERR_NO_SECKEY : GPG_ERR_NO_PUBKEY;

  release_kbnode (keyblock);

  if (ret_found_key)
    {
      if (! rc)
	*ret_found_key = found_key;
      else
	*ret_found_key = NULL;
    }

  return rc;
}


/* For documentation see keydb.h.  */
gpg_error_t
enum_secret_keys (ctrl_t ctrl, void **context, PKT_public_key *sk)
{
  gpg_error_t err = 0;
  const char *name;
  struct
  {
    int eof;
    int state;
    strlist_t sl;
    kbnode_t keyblock;
    kbnode_t node;
  } *c = *context;

  if (!c)
    {
      /* Make a new context.  */
      c = xtrycalloc (1, sizeof *c);
      if (!c)
        return gpg_error_from_syserror ();
      *context = c;
    }

  if (!sk)
    {
      /* Free the context.  */
      release_kbnode (c->keyblock);
      xfree (c);
      *context = NULL;
      return 0;
    }

  if (c->eof)
    return gpg_error (GPG_ERR_EOF);

  for (;;)
    {
      /* Loop until we have a keyblock.  */
      while (!c->keyblock)
        {
          /* Loop over the list of secret keys.  */
          do
            {
              name = NULL;
              switch (c->state)
                {
                case 0: /* First try to use the --default-key.  */
                  name = parse_def_secret_key (ctrl);
                  c->state = 1;
                  break;

                case 1: /* Init list of keys to try.  */
                  c->sl = opt.secret_keys_to_try;
                  c->state++;
                  break;

                case 2: /* Get next item from list.  */
                  if (c->sl)
                    {
                      name = c->sl->d;
                      c->sl = c->sl->next;
                    }
                  else
                    c->state++;
                  break;

                default: /* No more names to check - stop.  */
                  c->eof = 1;
                  return gpg_error (GPG_ERR_EOF);
                }
            }
          while (!name || !*name);

          err = getkey_byname (ctrl, NULL, NULL, name, 1, &c->keyblock);
          if (err)
            {
              /* getkey_byname might return a keyblock even in the
                 error case - I have not checked.  Thus better release
                 it.  */
              release_kbnode (c->keyblock);
              c->keyblock = NULL;
            }
          else
            c->node = c->keyblock;
        }

      /* Get the next key from the current keyblock.  */
      for (; c->node; c->node = c->node->next)
	{
	  if (c->node->pkt->pkttype == PKT_PUBLIC_KEY
              || c->node->pkt->pkttype == PKT_PUBLIC_SUBKEY)
	    {
	      copy_public_key (sk, c->node->pkt->pkt.public_key);
	      c->node = c->node->next;
	      return 0;	/* Found.  */
	    }
        }

      /* Dispose the keyblock and continue.  */
      release_kbnode (c->keyblock);
      c->keyblock = NULL;
    }
}


/*********************************************
 ***********  User ID printing helpers *******
 *********************************************/

/* Return a string with a printable representation of the user_id.
 * this string must be freed by xfree.   */
static char *
get_user_id_string (u32 * keyid, int mode, size_t *r_len)
{
  user_id_db_t r;
  keyid_list_t a;
  int pass = 0;
  char *p;

  /* Try it two times; second pass reads from the database.  */
  do
    {
      for (r = user_id_db; r; r = r->next)
	{
	  for (a = r->keyids; a; a = a->next)
	    {
	      if (a->keyid[0] == keyid[0] && a->keyid[1] == keyid[1])
		{
                  if (mode == 2)
                    {
                      /* An empty string as user id is possible.  Make
                         sure that the malloc allocates one byte and
                         does not bail out.  */
                      p = xmalloc (r->len? r->len : 1);
                      memcpy (p, r->name, r->len);
                      if (r_len)
                        *r_len = r->len;
                    }
                  else
                    {
                      if (mode)
                        p = xasprintf ("%08lX%08lX %.*s",
                                       (ulong) keyid[0], (ulong) keyid[1],
                                       r->len, r->name);
                      else
                        p = xasprintf ("%s %.*s", keystr (keyid),
                                       r->len, r->name);
                      if (r_len)
                        *r_len = strlen (p);
                    }

                  return p;
		}
	    }
	}
    }
  while (++pass < 2 && !get_pubkey (NULL, keyid));

  if (mode == 2)
    p = xstrdup (user_id_not_found_utf8 ());
  else if (mode)
    p = xasprintf ("%08lX%08lX [?]", (ulong) keyid[0], (ulong) keyid[1]);
  else
    p = xasprintf ("%s [?]", keystr (keyid));

  if (r_len)
    *r_len = strlen (p);
  return p;
}


char *
get_user_id_string_native (u32 * keyid)
{
  char *p = get_user_id_string (keyid, 0, NULL);
  char *p2 = utf8_to_native (p, strlen (p), 0);
  xfree (p);
  return p2;
}


char *
get_long_user_id_string (u32 * keyid)
{
  return get_user_id_string (keyid, 1, NULL);
}


/* Please try to use get_user_byfpr instead of this one.  */
char *
get_user_id (u32 * keyid, size_t * rn)
{
  return get_user_id_string (keyid, 2, rn);
}


/* Please try to use get_user_id_byfpr_native instead of this one.  */
char *
get_user_id_native (u32 * keyid)
{
  size_t rn;
  char *p = get_user_id (keyid, &rn);
  char *p2 = utf8_to_native (p, rn, 0);
  xfree (p);
  return p2;
}


/* Return the user id for a key designated by its fingerprint, FPR,
   which must be MAX_FINGERPRINT_LEN bytes in size.  Note: the
   returned string, which must be freed using xfree, may not be NUL
   terminated.  To determine the length of the string, you must use
   *RN.  */
char *
get_user_id_byfpr (const byte *fpr, size_t *rn)
{
  user_id_db_t r;
  char *p;
  int pass = 0;

  /* Try it two times; second pass reads from the database.  */
  do
    {
      for (r = user_id_db; r; r = r->next)
	{
	  keyid_list_t a;
	  for (a = r->keyids; a; a = a->next)
	    {
	      if (!memcmp (a->fpr, fpr, MAX_FINGERPRINT_LEN))
		{
                  /* An empty string as user id is possible.  Make
                     sure that the malloc allocates one byte and does
                     not bail out.  */
		  p = xmalloc (r->len? r->len : 1);
		  memcpy (p, r->name, r->len);
		  *rn = r->len;
		  return p;
		}
	    }
	}
    }
  while (++pass < 2
	 && !get_pubkey_byfprint (NULL, NULL, fpr, MAX_FINGERPRINT_LEN));
  p = xstrdup (user_id_not_found_utf8 ());
  *rn = strlen (p);
  return p;
}

/* Like get_user_id_byfpr, but convert the string to the native
   encoding.  The returned string needs to be freed.  Unlike
   get_user_id_byfpr, the returned string is NUL terminated.  */
char *
get_user_id_byfpr_native (const byte *fpr)
{
  size_t rn;
  char *p = get_user_id_byfpr (fpr, &rn);
  char *p2 = utf8_to_native (p, rn, 0);
  xfree (p);
  return p2;
}



/* For documentation see keydb.h.  */
KEYDB_HANDLE
get_ctx_handle (GETKEY_CTX ctx)
{
  return ctx->kr_handle;
}

static void
free_akl (struct akl *akl)
{
  if (! akl)
    return;

  if (akl->spec)
    free_keyserver_spec (akl->spec);

  xfree (akl);
}

void
release_akl (void)
{
  while (opt.auto_key_locate)
    {
      struct akl *akl2 = opt.auto_key_locate;
      opt.auto_key_locate = opt.auto_key_locate->next;
      free_akl (akl2);
    }
}

/* Returns false on error. */
int
parse_auto_key_locate (char *options)
{
  char *tok;

  while ((tok = optsep (&options)))
    {
      struct akl *akl, *check, *last = NULL;
      int dupe = 0;

      if (tok[0] == '\0')
	continue;

      akl = xmalloc_clear (sizeof (*akl));

      if (ascii_strcasecmp (tok, "clear") == 0)
	{
          xfree (akl);
          free_akl (opt.auto_key_locate);
          opt.auto_key_locate = NULL;
          continue;
        }
      else if (ascii_strcasecmp (tok, "nodefault") == 0)
	akl->type = AKL_NODEFAULT;
      else if (ascii_strcasecmp (tok, "local") == 0)
	akl->type = AKL_LOCAL;
      else if (ascii_strcasecmp (tok, "ldap") == 0)
	akl->type = AKL_LDAP;
      else if (ascii_strcasecmp (tok, "keyserver") == 0)
	akl->type = AKL_KEYSERVER;
#ifdef USE_DNS_CERT
      else if (ascii_strcasecmp (tok, "cert") == 0)
	akl->type = AKL_CERT;
#endif
      else if (ascii_strcasecmp (tok, "pka") == 0)
	akl->type = AKL_PKA;
      else if (ascii_strcasecmp (tok, "dane") == 0)
	akl->type = AKL_DANE;
      else if ((akl->spec = parse_keyserver_uri (tok, 1)))
	akl->type = AKL_SPEC;
      else
	{
	  free_akl (akl);
	  return 0;
	}

      /* We must maintain the order the user gave us */
      for (check = opt.auto_key_locate; check;
	   last = check, check = check->next)
	{
	  /* Check for duplicates */
	  if (check->type == akl->type
	      && (akl->type != AKL_SPEC
		  || (akl->type == AKL_SPEC
		      && strcmp (check->spec->uri, akl->spec->uri) == 0)))
	    {
	      dupe = 1;
	      free_akl (akl);
	      break;
	    }
	}

      if (!dupe)
	{
	  if (last)
	    last->next = akl;
	  else
	    opt.auto_key_locate = akl;
	}
    }

  return 1;
}


/* For documentation see keydb.h.  */
int
have_secret_key_with_kid (u32 *keyid)
{
  gpg_error_t err;
  KEYDB_HANDLE kdbhd;
  KEYDB_SEARCH_DESC desc;
  kbnode_t keyblock;
  kbnode_t node;
  int result = 0;

  kdbhd = keydb_new ();
  memset (&desc, 0, sizeof desc);
  desc.mode = KEYDB_SEARCH_MODE_LONG_KID;
  desc.u.kid[0] = keyid[0];
  desc.u.kid[1] = keyid[1];
  while (!result)
    {
      err = keydb_search (kdbhd, &desc, 1, NULL);
      if (err)
        break;

      err = keydb_get_keyblock (kdbhd, &keyblock);
      if (err)
        {
          log_error (_("error reading keyblock: %s\n"), gpg_strerror (err));
          break;
        }

      for (node = keyblock; node; node = node->next)
	{
          /* Bit 0 of the flags is set if the search found the key
             using that key or subkey.  Note: a search will only ever
             match a single key or subkey.  */
	  if ((node->flag & 1))
            {
              assert (node->pkt->pkttype == PKT_PUBLIC_KEY
                      || node->pkt->pkttype == PKT_PUBLIC_SUBKEY);

              if (agent_probe_secret_key (NULL, node->pkt->pkt.public_key) == 0)
		/* Not available.  */
		result = 1;
	      else
		result = 0;

	      break;
	    }
	}
      release_kbnode (keyblock);
    }

  keydb_release (kdbhd);
  return result;
}
