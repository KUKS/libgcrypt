/* dynload.c - load cipher extensions
 *	Copyright (C) 1998 Free Software Foundation, Inc.
 *
 * This file is part of GNUPG.
 *
 * GNUPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GNUPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_DL_DLOPEN
  #include <dlfcn.h>
#endif
#include "util.h"
#include "cipher.h"
#include "dynload.h"

typedef struct ext_list {
    struct ext_list *next;
    void *handle; /* handle from dlopen() */
    int  failed;  /* already tried but failed */
    void * (*enumfunc)(int, int*, int*, int*);
    char *hintstr; /* pointer into name */
    char name[1];
} *EXTLIST;

static EXTLIST extensions;

typedef struct {
    EXTLIST r;
    int seq1;
    int seq2;
    void *sym;
} ENUMCONTEXT;

/****************
 * Register an extension module.  The last registered module will
 * be loaded first.  A name may have a list of classes
 * appended; e.g:
 *	mymodule.so(1:17,3:20,3:109)
 * means that this module provides digest algorithm 17 and public key
 * algorithms 20 and 109.  This is only a hint but if it is there the
 * loader may decide to only load a module which claims to have a
 * requested algorithm.
 */
void
register_cipher_extension( const char *fname )
{
    EXTLIST r, el;
    char *p, *pe;

    if( *fname != '/' ) { /* do tilde expansion etc */
	char *p ;

	if( strchr(fname, '/') )
	    p = make_filename(fname, NULL);
	else
	    p = make_filename(GNUPG_LIBDIR, fname, NULL);
	el = m_alloc_clear( sizeof *el + strlen(p) );
	strcpy(el->name, p );
	m_free(p);
    }
    else {
	el = m_alloc_clear( sizeof *el + strlen(fname) );
	strcpy(el->name, fname );
    }
    /* check whether we have a class hint */
    if( (p=strchr(el->name,'(')) && (pe=strchr(p+1,')')) && !pe[1] ) {
	*p = *pe = 0;
	el->hintstr = p+1;
    }
    else
	el->hintstr = NULL;

    /* check that it is not already registered */
    for(r = extensions; r; r = r->next )
	if( !compare_filenames(r->name, el->name) ) {
	    log_info("extension '%s' already registered\n", el->name );
	    m_free(el);
	    return;
	}
    /* and register */
    el->next = extensions;
    extensions = el;
}


static int
load_extension( EXTLIST el )
{
  #ifdef USE_DYNAMIC_LINKING
    char **name;
    void *sym;
    const char *err;
    int seq = 0;
    int class, vers;

    /* make sure we are not setuid */
    if( getuid() != geteuid() )
	log_bug("trying to load an extension while still setuid\n");

    /* now that we are not setuid anymore, we can safely load modules */
    el->handle = dlopen(el->name, RTLD_NOW);
    if( !el->handle ) {
	log_error("%s: error loading extension: %s\n", el->name, dlerror() );
	goto failure;
    }
    name = (char**)dlsym(el->handle, "gnupgext_version");
    if( (err=dlerror()) ) {
	log_error("%s: not a gnupg extension: %s\n", el->name, err );
	goto failure;
    }

    if( g10_opt_verbose )
	log_info("%s: %s%s%s%s\n", el->name, *name,
		  el->hintstr? " (":"",
		  el->hintstr? el->hintstr:"",
		  el->hintstr? ")":"");

    sym = dlsym(el->handle, "gnupgext_enum_func");
    if( (err=dlerror()) ) {
	log_error("%s: invalid gnupg extension: %s\n", el->name, err );
	goto failure;
    }
    el->enumfunc = (void *(*)(int,int*,int*,int*))sym;

    if( g10_opt_verbose > 1 ) {
	/* list the contents of the module */
	while( (sym = (*el->enumfunc)(0, &seq, &class, &vers)) ) {
	    if( vers != 1 ) {
		log_info("%s: ignoring func with version %d\n",el->name,vers);
		continue;
	    }
	    switch( class ) {
	      case 11:
	      case 21:
	      case 31:
		log_info("%s: provides %s algorithm %d\n", el->name,
				class == 11? "md"     :
				class == 21? "cipher" : "pubkey",
						       *(int*)sym);
		break;
	      default:
		/*log_debug("%s: skipping class %d\n", el->name, class);*/
		break;
	    }
	}
    }
    return 0;

  failure:
    if( el->handle ) {
	dlclose(el->handle);
	el->handle = NULL;
    }
    el->failed = 1;
  #endif /*USE_DYNAMIC_LINKING*/
    return -1;
}



int
enum_gnupgext_digests( void **enum_context,
	    int *algo,
	    const char *(**r_get_info)( int, size_t*,byte**, int*, int*,
				       void (**)(void*),
				       void (**)(void*,byte*,size_t),
				       void (**)(void*),byte *(**)(void*)) )
{
    EXTLIST r;
    ENUMCONTEXT *ctx;

    if( !*enum_context ) { /* init context */
	ctx = m_alloc_clear( sizeof( *ctx ) );
	ctx->r = extensions;
	*enum_context = ctx;
    }
    else if( !algo ) { /* release the context */
	m_free(*enum_context);
	*enum_context = NULL;
	return 0;
    }
    else
	ctx = *enum_context;

    for( r = ctx->r; r; r = r->next )  {
	int class, vers;

	if( r->failed )
	    continue;
	if( !r->handle && load_extension(r) )
	    continue;
	/* get a digest info function */
	if( ctx->sym )
	    goto inner_loop;
	while( (ctx->sym = (*r->enumfunc)(10, &ctx->seq1, &class, &vers)) ) {
	    void *sym;
	    /* must check class because enumfunc may be wrong coded */
	    if( vers != 1 || class != 10 )
		continue;
	  inner_loop:
	    *r_get_info = ctx->sym;
	    while( (sym = (*r->enumfunc)(11, &ctx->seq2, &class, &vers)) ) {
		if( vers != 1 || class != 11 )
		    continue;
		*algo = *(int*)sym;
		ctx->r = r;
		return 1;
	    }
	    ctx->seq2 = 0;
	}
	ctx->seq1 = 0;
    }
    ctx->r = r;
    return 0;
}

const char *
enum_gnupgext_ciphers( void **enum_context, int *algo,
		       size_t *keylen, size_t *blocksize, size_t *contextsize,
		       void (**setkey)( void *c, byte *key, unsigned keylen ),
		       void (**encrypt)( void *c, byte *outbuf, byte *inbuf ),
		       void (**decrypt)( void *c, byte *outbuf, byte *inbuf )
		     )
{
    EXTLIST r;
    ENUMCONTEXT *ctx;
    const char * (*finfo)(int, size_t*, size_t*, size_t*,
			  void (**)( void *, byte *, unsigned),
			  void (**)( void *, byte *, byte *),
			  void (**)( void *, byte *, byte *));

    if( !*enum_context ) { /* init context */
	ctx = m_alloc_clear( sizeof( *ctx ) );
	ctx->r = extensions;
	*enum_context = ctx;
    }
    else if( !algo ) { /* release the context */
	m_free(*enum_context);
	*enum_context = NULL;
	return NULL;
    }
    else
	ctx = *enum_context;

    for( r = ctx->r; r; r = r->next )  {
	int class, vers;

	if( r->failed )
	    continue;
	if( !r->handle && load_extension(r) )
	    continue;
	/* get a cipher info function */
	if( ctx->sym )
	    goto inner_loop;
	while( (ctx->sym = (*r->enumfunc)(20, &ctx->seq1, &class, &vers)) ) {
	    void *sym;
	    /* must check class because enumfunc may be wrong coded */
	    if( vers != 1 || class != 20 )
		continue;
	  inner_loop:
	    finfo = ctx->sym;
	    while( (sym = (*r->enumfunc)(21, &ctx->seq2, &class, &vers)) ) {
		const char *algname;
		if( vers != 1 || class != 21 )
		    continue;
		*algo = *(int*)sym;
		algname = (*finfo)( *algo, keylen, blocksize, contextsize,
				    setkey, encrypt, decrypt );
		if( algname ) {
		    ctx->r = r;
		    return algname;
		}
	    }
	    ctx->seq2 = 0;
	}
	ctx->seq1 = 0;
    }
    ctx->r = r;
    return NULL;
}

const char *
enum_gnupgext_pubkeys( void **enum_context, int *algo,
    int *npkey, int *nskey, int *nenc, int *nsig, int *usage,
    int (**generate)( int algo, unsigned nbits, MPI *skey, MPI **retfactors ),
    int (**check_secret_key)( int algo, MPI *skey ),
    int (**encrypt)( int algo, MPI *resarr, MPI data, MPI *pkey ),
    int (**decrypt)( int algo, MPI *result, MPI *data, MPI *skey ),
    int (**sign)( int algo, MPI *resarr, MPI data, MPI *skey ),
    int (**verify)( int algo, MPI hash, MPI *data, MPI *pkey,
		    int (*cmp)(void *, MPI), void *opaquev ),
    unsigned (**get_nbits)( int algo, MPI *pkey ) )
{
    EXTLIST r;
    ENUMCONTEXT *ctx;
    const char * (*finfo)( int, int *, int *, int *, int *, int *,
			   int (**)( int, unsigned, MPI *, MPI **),
			   int (**)( int, MPI * ),
			   int (**)( int, MPI *, MPI , MPI * ),
			   int (**)( int, MPI *, MPI *, MPI * ),
			   int (**)( int, MPI *, MPI , MPI * ),
			   int (**)( int, MPI , MPI *, MPI *,
					    int (*)(void*,MPI), void *),
			   unsigned (**)( int , MPI * ) );

    if( !*enum_context ) { /* init context */
	ctx = m_alloc_clear( sizeof( *ctx ) );
	ctx->r = extensions;
	*enum_context = ctx;
    }
    else if( !algo ) { /* release the context */
	m_free(*enum_context);
	*enum_context = NULL;
	return NULL;
    }
    else
	ctx = *enum_context;

    for( r = ctx->r; r; r = r->next )  {
	int class, vers;

	if( r->failed )
	    continue;
	if( !r->handle && load_extension(r) )
	    continue;
	/* get a pubkey info function */
	if( ctx->sym )
	    goto inner_loop;
	while( (ctx->sym = (*r->enumfunc)(30, &ctx->seq1, &class, &vers)) ) {
	    void *sym;
	    if( vers != 1 || class != 30 )
		continue;
	  inner_loop:
	    finfo = ctx->sym;
	    while( (sym = (*r->enumfunc)(31, &ctx->seq2, &class, &vers)) ) {
		const char *algname;
		if( vers != 1 || class != 31 )
		    continue;
		*algo = *(int*)sym;
		algname = (*finfo)( *algo, npkey, nskey, nenc, nsig, usage,
				    generate, check_secret_key, encrypt,
				    decrypt, sign, verify, get_nbits );
		if( algname ) {
		    ctx->r = r;
		    return algname;
		}
	    }
	    ctx->seq2 = 0;
	}
	ctx->seq1 = 0;
    }
    ctx->r = r;
    return NULL;
}
