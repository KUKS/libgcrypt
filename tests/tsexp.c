/* tsexp.c  -  S-expression regression tests
 *	Copyright (C) 2001 Free Software Foundation, Inc.
 *
 * This file is part of Libgcrypt.
 *
 * Libgcrypt is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Libgcrypt is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../src/gcrypt.h"

#define PGMNAME "tsexp"

static int verbose;
static int error_count;

static void
info (const char *format, ...)
{
  va_list arg_ptr;

  if (verbose)
    {
      va_start( arg_ptr, format ) ;
      vfprintf (stderr, format, arg_ptr );
      va_end(arg_ptr);
    }
}

static void
fail ( const char *format, ... )
{
    va_list arg_ptr ;

    fputs (PGMNAME ": ", stderr);
    va_start( arg_ptr, format ) ;
    vfprintf (stderr, format, arg_ptr );
    va_end(arg_ptr);
    error_count++;
}


/* fixme: we need better tests */
static void
basic (void)
{
  GCRY_SEXP sexp;
  int idx;
  const char *string;
  static struct {
    const char *token;
    const char *parm;
  } values[] = {
    { "public-key", NULL },
    { "dsa", NULL },
    { "dsa", "p" },
    { "dsa", "y" },
    { "dsa", "q" },
    { "dsa", "g" },
    { NULL }
  };

  info ("doing some pretty pointless tests\n"); 
  string = ("(public-key (dsa (p #41424344#) (y this_is_y) "
            "(q #61626364656667#) (g %m)))");
     
  if ( gcry_sexp_build (&sexp, NULL, string, gcry_mpi_set_ui (NULL, 42)) )
    {
      fail (" scanning `%s' failed\n", string);
      return;
    }

  /* now find something */
  for (idx=0; values[idx].token; idx++)
    {
      const char *token = values[idx].token;
      const char *parm = values[idx].parm;
      GCRY_SEXP s1, s2;
      GCRY_MPI a;
      const char *p;
      size_t n;

      s1 = gcry_sexp_find_token (sexp, token, strlen(token) );
      if (!s1)
        {
          fail ("didn't found `%s'\n", token);
          continue;
        }

      p = gcry_sexp_nth_data (s1, 0, &n);
      if (!p)
        {
          fail ("no car for `%s'\n", token);
          continue;
        }
      info ("car=`%.*s'\n", (int)n, p);

      s2 = gcry_sexp_cdr (s1);
      if (!s2) 
        {
          fail ("no cdr for `%s'\n", token);
          continue;
        }

      p = gcry_sexp_nth_data (s2, 0, &n);
      if (p)
        {
          fail ("data at car of `%s'\n", token);
          continue;
        }

      if (parm)
        {
          s2 = gcry_sexp_find_token (s1, parm, strlen (parm));
          if (!s2)
	    {
              fail ("didn't found `%s'\n", parm);
              continue;
	    }
          p = gcry_sexp_nth_data (s2, 0, &n);
          if (!p) 
            {
              fail("no car for `%s'\n", parm );
              continue;
            }
          info ("car=`%.*s'\n", (int)n, p);
          p = gcry_sexp_nth_data (s2, 1, &n);
          if (!p) 
            {
              fail("no cdr for `%s'\n", parm );
              continue;
            }
          info ("cdr=`%.*s'\n", (int)n, p);
          
          a = gcry_sexp_nth_mpi (s2, 0, GCRYMPI_FMT_USG);
          if (!a)
	    {
              fail("failed to cdr the mpi for `%s'\n", parm);
              continue;
            }
        }
    }
}


static void
canon_len (void)
{
  static struct {
    size_t textlen; /* length of the buffer */
    size_t expected;/* expected length or 0 on error and then ... */
    size_t erroff;  /* ... and at this offset */
    int errcode;    /* ... with this error code */
    unsigned char *text; 
  } values[] = {
    { 13, 13, 0,  0, "10:abcdefghi" },
    { 14, 13, 0,  0, "10:abcdefghix" },
    { 12,  0, 2, -2, "10:abcdefghi" },
    { 13,  0, 0, -7, "010:abcdefghi" },
    {  1,  0, 1, -2, "1:"},
    {  2,  0, 1, -2, "1:"},
    {  3,  3, 0,  0, "1:x"},
    {  2,  2, 0,  0, "()"},
    {  4,  2, 0,  0, "()()"},
    {  4,  4, 0,  0, "(())"},
    {  3,  0, 3, -2, "(()"},
    {  3,  0, 1, -5, "( )"},
    {  9,  9, 0,  0, "(3:abc())"},
    { 10,  0, 6, -5, "(3:abc ())"},
    /* fixme: we need much more cases */
    { 0 },
  };
  int idx;
  int errcode;
  size_t n, erroff;

  info ("checking canoncial length test function\n");
  for (idx=0; values[idx].text; idx++)
    {
      n = gcry_sexp_canon_len (values[idx].text, values[idx].textlen, 
                               &erroff, &errcode);
      
      if (n && n == values[idx].expected)
        ; /* success */
      else if (!n && !values[idx].expected)
        { /* we expected an error - check that this is the right one */
          if (values[idx].erroff != erroff)
            fail ("canonical length test %d - wrong error offset %u\n",
                  idx, (unsigned int)erroff);
          if (values[idx].errcode != errcode)
            fail ("canonical length test %d - wrong error code %d\n",
                  idx, errcode);
        }
      else
        fail ("canonical length test %d failed - n=%u, off=%u, err=%d\n",
              idx, (unsigned int)n, (unsigned int)erroff, errcode);
    }
}


int
main (int argc, char **argv)
{
  if (argc > 1 && !strcmp (argv[1], "-v"))
    verbose = 1;

  basic ();
  canon_len ();
  
  return error_count? 1:0;
}

