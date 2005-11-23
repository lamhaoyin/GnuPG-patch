/* qualified.c - Routines related to qualified signatures
 * Copyright (C) 2005 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
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
#include <stdarg.h>
#include <assert.h>
#include <errno.h>
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif
#ifdef HAVE_LANGINFO_CODESET
#include <langinfo.h>
#endif

#include "gpgsm.h"
#include "i18n.h"
#include <ksba.h>


/* We open the file only once and keep the open file pointer as well
   as the name of the file here.  Note that, a listname not equal to
   NULL indicates that this module has been intialized and if the
   LISTFP is also NULL, no list of qualified signatures exists. */
static char *listname;
static FILE *listfp;


/* Read the trustlist and return entry by entry.  KEY must point to a
   buffer of at least 41 characters. COUNTRY shall be a buffer of at
   least 3 characters to receive the country code of that qualified
   signature (i.e. "de" for German and "be" for Belgium).

   Reading a valid entry returns 0, EOF is indicated by GPG_ERR_EOF
   and any other error condition is indicated by the appropriate error
   code. */
static gpg_error_t
read_list (char *key, char *country, int *lnr)
{
  gpg_error_t err;
  int c, i, j;
  char *p, line[256];

  *key = 0;
  *country = 0;

  if (!listname)
    {
      listname = make_filename (GNUPG_DATADIR, "qualified.txt", NULL);
      listfp = fopen (listname, "r");
      if (!listfp && errno != ENOENT)
        {
          err = gpg_error_from_errno (errno);
          log_error (_("can't open `%s': %s\n"), listname, gpg_strerror (err));
          return err;
        }
    }

  if (!listfp)
    return gpg_error (GPG_ERR_EOF);

  do
    {
      if (!fgets (line, DIM(line)-1, listfp) )
        {
          if (feof (listfp))
            return gpg_error (GPG_ERR_EOF);
          return gpg_error_from_errno (errno);
        }

      if (!*line || line[strlen(line)-1] != '\n')
        {
          /* Eat until end of line. */
          while ( (c=getc (listfp)) != EOF && c != '\n')
            ;
          return gpg_error (*line? GPG_ERR_LINE_TOO_LONG
                                 : GPG_ERR_INCOMPLETE_LINE);
        }
      ++*lnr;
      
      /* Allow for empty lines and spaces */
      for (p=line; spacep (p); p++)
        ;
    }
  while (!*p || *p == '\n' || *p == '#');
  
  for (i=j=0; (p[i] == ':' || hexdigitp (p+i)) && j < 40; i++)
    if ( p[i] != ':' )
      key[j++] = p[i] >= 'a'? (p[i] & 0xdf): p[i];
  key[j] = 0;
  if (j != 40 || !(spacep (p+i) || p[i] == '\n'))
    {
      log_error (_("invalid formatted fingerprint in `%s', line %d\n"),
                 listname, *lnr);
      return gpg_error (GPG_ERR_BAD_DATA);
    }
  assert (p[i]);
  i++;
  while (spacep (p+i))
    i++;
  if ( p[i] >= 'a' && p[i] <= 'z' 
       && p[i+1] >= 'a' && p[i+1] <= 'z' 
       && (spacep (p+i+2) || p[i+2] == '\n'))
    {
      country[0] = p[i];
      country[1] = p[i+1];
      country[2] = 0;
    }
  else
    {
      log_error (_("invalid country code in `%s', line %d\n"), listname, *lnr);
      return gpg_error (GPG_ERR_BAD_DATA);
    }

  return 0;
}




/* Check whether the certificate CERT is included in the list of
   qualified certificates.  This list is similar to the "trustlist.txt"
   as maintained by gpg-agent and includes fingerprints of root
   certificates to be used for qualified (legally binding like
   handwritten) signatures.  We keep this list system wide and not
   per user because it is not a decision of the user. 

   Returns: 0 if the certificate is included.  GPG_ERR_NOT_FOUND if it
   is not in the liost or any other error (e.g. if no list of
   qualified signatures is available. */
gpg_error_t
gpgsm_is_in_qualified_list (ctrl_t ctrl, ksba_cert_t cert)
{
  gpg_error_t err;
  char *fpr;
  char key[41];
  char country[2];
  int lnr = 0;

  fpr = gpgsm_get_fingerprint_hexstring (cert, GCRY_MD_SHA1);
  if (!fpr)
    return gpg_error (GPG_ERR_GENERAL);

  if (listfp)
    rewind (listfp);
  while (!(err = read_list (key, country, &lnr)))
    {
      if (!strcmp (key, fpr))
        break;
    }
  if (gpg_err_code (err) == GPG_ERR_EOF)
    err = gpg_error (GPG_ERR_NOT_FOUND);

  xfree (fpr);
  return err;
}


/* We know that CERT is a qualified certificate.  Ask the user for
   consent to actually create a signature using this certificate.
   Returns: 0 for yes, GPG_ERR_CANCEL for no or any otehr error
   code. */
gpg_error_t
gpgsm_qualified_consent (ctrl_t ctrl, ksba_cert_t cert)
{
  gpg_error_t err;
  char *name, *subject, *buffer, *p;
  const char *s;
  char *orig_codeset = NULL;

  name = ksba_cert_get_subject (cert, 0);
  if (!name)
    return gpg_error (GPG_ERR_GENERAL);
  subject = gpgsm_format_name2 (name, 0);
  ksba_free (name); name = NULL;

#ifdef ENABLE_NLS
  /* The Assuan agent protocol requires us to transmit utf-8 strings */
  orig_codeset = bind_textdomain_codeset (PACKAGE_GT, NULL);
#ifdef HAVE_LANGINFO_CODESET
  if (!orig_codeset)
    orig_codeset = nl_langinfo (CODESET);
#endif
  if (orig_codeset)
    { /* We only switch when we are able to restore the codeset later.
         Note that bind_textdomain_codeset does only return on memory
         errors but not if a codeset is not available.  Thus we don't
         bother printing a diagnostic here. */
      orig_codeset = xstrdup (orig_codeset);
      if (!bind_textdomain_codeset (PACKAGE_GT, "utf-8"))
        orig_codeset = NULL; 
    }
#endif

  if (asprintf (&name,
                _("You are about to create a signature using your "
                  "certificate:\n"
                  "\"%s\"\n"
                  "This will create a qualified signature by law "
                  "equated to a handwritten signature.\n\n%s%s"
                  "Are you really sure that you want to do this?"),
                subject? subject:"?",
                opt.qualsig_approval? 
                "":
                "Note that this software is not officially approved "
                "to create or verify such signatures.\n",
                opt.qualsig_approval? "":"\n"
                ) < 0 )
    err = gpg_error_from_errno (errno);
  else
    err = 0;

#ifdef ENABLE_NLS
  if (orig_codeset)
    bind_textdomain_codeset (PACKAGE_GT, orig_codeset);
#endif
  xfree (orig_codeset);
  xfree (subject);

  if (err)
    return err;

  buffer = p = xtrymalloc (strlen (name) * 3 + 1);
  if (!buffer)
    {
      err = gpg_error_from_errno (errno);
      free (name);
      return err;
    }
  for (s=name; *s; s++)
    {
      if (*s < ' ' || *s == '+')
        {
          sprintf (p, "%%%02X", *(unsigned char *)s);
          p += 3;
        }
      else if (*s == ' ')
        *p++ = '+';
      else
        *p++ = *s;
    }
  *p = 0;
  free (name); 


  err = gpgsm_agent_get_confirmation (ctrl, buffer);

  xfree (buffer);
  return err;
}


/* Popup a prompt to inform the user that the signature created is not
   a qualified one.  This is of course only doen if we know that we
   have been approved. */
gpg_error_t
gpgsm_not_qualified_warning (ctrl_t ctrl, ksba_cert_t cert)
{
  gpg_error_t err;
  char *name, *subject, *buffer, *p;
  const char *s;
  char *orig_codeset = NULL;

  if (!opt.qualsig_approval)
    return 0;

  name = ksba_cert_get_subject (cert, 0);
  if (!name)
    return gpg_error (GPG_ERR_GENERAL);
  subject = gpgsm_format_name2 (name, 0);
  ksba_free (name); name = NULL;


#ifdef ENABLE_NLS
  /* The Assuan agent protocol requires us to transmit utf-8 strings */
  orig_codeset = bind_textdomain_codeset (PACKAGE_GT, NULL);
#ifdef HAVE_LANGINFO_CODESET
  if (!orig_codeset)
    orig_codeset = nl_langinfo (CODESET);
#endif
  if (orig_codeset)
    { /* We only switch when we are able to restore the codeset later.
         Note that bind_textdomain_codeset does only return on memory
         errors but not if a codeset is not available.  Thus we don't
         bother printing a diagnostic here. */
      orig_codeset = xstrdup (orig_codeset);
      if (!bind_textdomain_codeset (PACKAGE_GT, "utf-8"))
        orig_codeset = NULL; 
    }
#endif

  if (asprintf (&name,
                _("You are about to create a signature using your "
                  "certificate:\n"
                  "\"%s\"\n"
                  "Note, that this certificate will NOT create a "
                  "qualified signature!"),
                subject? subject:"?") < 0 )
    err = gpg_error_from_errno (errno);
  else
    err = 0;

#ifdef ENABLE_NLS
  if (orig_codeset)
    bind_textdomain_codeset (PACKAGE_GT, orig_codeset);
#endif
  xfree (orig_codeset);
  xfree (subject);

  if (err)
    return err;

  buffer = p = xtrymalloc (strlen (name) * 3 + 1);
  if (!buffer)
    {
      err = gpg_error_from_errno (errno);
      free (name);
      return err;
    }
  for (s=name; *s; s++)
    {
      if (*s < ' ' || *s == '+')
        {
          sprintf (p, "%%%02X", *(unsigned char *)s);
          p += 3;
        }
      else if (*s == ' ')
        *p++ = '+';
      else
        *p++ = *s;
    }
  *p = 0;
  free (name); 


  err = gpgsm_agent_get_confirmation (ctrl, buffer);

  xfree (buffer);
  return err;
}
