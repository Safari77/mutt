/* taken from XFCE's Xarchiver, made to work without glib for mutt */

#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "mutt.h"
#include "protos.h"

/* mkdtemp function for systems which don't have one */
char *mkdtemp(char *tmpl)
{
  int               len;
  int               i;

  len = strlen(tmpl);
  if (len < 6 || strcmp(&tmpl[len - 6], "XXXXXX") != 0)
  {
    errno = EINVAL;
    return NULL;
  }

  for (i = 0; i < 7 ; ++i)
  {
    /* fill in the random bits */
    mutt_random_base32_string(&tmpl[len - 6], 6);

    /* try to create the directory */
    if (mkdir(tmpl, 0700) == 0)
      return tmpl;
    else if (errno != EEXIST)
      return NULL;
  }

  errno = EEXIST;
  return NULL;
}
