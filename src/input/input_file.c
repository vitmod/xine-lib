/* 
 * Copyright (C) 2000 the xine project
 * 
 * This file is part of xine, a unix video player.
 * 
 * xine is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * xine is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
 *
 * $Id: input_file.c,v 1.19 2001/08/28 22:44:10 jcdutton Exp $
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>	/*PATH_MAX*/

#include "xine_internal.h"
#include "monitor.h"
#include "input_plugin.h"

extern int errno;

static uint32_t xine_debug;

#define MAXFILES      65535

#ifndef NAME_MAX
#define NAME_MAX 256
#endif
#ifndef PATH_MAX
#define PATH_MAX 768
#endif

#ifndef S_ISLNK
#define S_ISLNK(mode)  0
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(mode) 0
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(mode) 0
#endif
#ifndef S_ISCHR
#define S_ISCHR(mode)  0
#endif
#ifndef S_ISBLK
#define S_ISBLK(mode)  0
#endif
#ifndef S_ISREG
#define S_ISREG(mode)  0
#endif
#if !S_IXUGO
#define S_IXUGO        (S_IXUSR | S_IXGRP | S_IXOTH)
#endif

typedef struct {
  input_plugin_t    input_plugin;
  
  int               fh;
  char             *mrl;
  config_values_t  *config;

  int               mrls_allocated_entries;
  mrl_t           **mrls;
  
} file_input_plugin_t;


/* ***************************************************************************
 *                            PRIVATES FUNCTIONS
 */

/*
 * Sorting function, it comes from GNU fileutils package.
 */
#define S_N        0x0
#define S_I        0x4
#define S_F        0x8
#define S_Z        0xC
#define CMP          2
#define LEN          3
#define ISDIGIT(c)   ((unsigned) (c) - '0' <= 9)
static int strverscmp(const char *s1, const char *s2) {
  const unsigned char *p1 = (const unsigned char *) s1;
  const unsigned char *p2 = (const unsigned char *) s2;
  unsigned char c1, c2;
  int state;
  int diff;
  static const unsigned int next_state[] = {
    S_N, S_I, S_Z, S_N,
    S_N, S_I, S_I, S_I,
    S_N, S_F, S_F, S_F,
    S_N, S_F, S_Z, S_Z
  };
  static const int result_type[] = {
    CMP, CMP, CMP, CMP, CMP, LEN, CMP, CMP,
    CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
    CMP,  -1,  -1, CMP,   1, LEN, LEN, CMP,
      1, LEN, LEN, CMP, CMP, CMP, CMP, CMP,
    CMP, CMP, CMP, CMP, CMP, LEN, CMP, CMP,
    CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
    CMP,   1,   1, CMP,  -1, CMP, CMP, CMP,
     -1, CMP, CMP, CMP
  };

  if(p1 == p2)
    return 0;

  c1 = *p1++;
  c2 = *p2++;

  state = S_N | ((c1 == '0') + (ISDIGIT(c1) != 0));

  while((diff = c1 - c2) == 0 && c1 != '\0') {
    state = next_state[state];
    c1 = *p1++;
    c2 = *p2++;
    state |= (c1 == '0') + (ISDIGIT(c1) != 0);
  }
  
  state = result_type[state << 2 | ((c2 == '0') + (ISDIGIT(c2) != 0))];
  
  switch(state) {
  case CMP:
    return diff;
    
  case LEN:
    while(ISDIGIT(*p1++))
      if(!ISDIGIT(*p2++))
	return 1;
    
    return ISDIGIT(*p2) ? -1 : diff;
    
  default:
    return state;
  }
}

/*
 * Wrapper to strverscmp() for qsort() calls, which sort mrl_t type array.
 */
static int _sortfiles_default(const mrl_t *s1, const mrl_t *s2) {
  return(strverscmp(s1->mrl, s2->mrl));
}

/*
 * Return the type (OR'ed) of the given file *fully named*
 */
static uint32_t get_file_type(char *filepathname, char *origin) {
  struct stat  pstat;
  int          mode;
  uint32_t     file_type = 0;
  char         buf[PATH_MAX + NAME_MAX + 1];
  
  if((lstat(filepathname, &pstat)) < 0) {
    sprintf(buf, "%s/%s", origin, filepathname);
    if((lstat(buf, &pstat)) < 0) {
      printf("lstat failed for %s{%s}\n", filepathname, origin);
      file_type |= mrl_unknown;
      return file_type;
    }
  }
  
  file_type |= mrl_file;
  
  mode = pstat.st_mode;
  
  if(S_ISLNK(mode))
    file_type |= mrl_file_symlink;
  else if(S_ISDIR(mode))
    file_type |= mrl_file_directory;
  else if(S_ISCHR(mode))
    file_type |= mrl_file_chardev;
  else if(S_ISBLK(mode))
    file_type |= mrl_file_blockdev;
  else if(S_ISFIFO(mode))
    file_type |= mrl_file_fifo;
  else if(S_ISSOCK(mode))
    file_type |= mrl_file_sock;
  else {
    if(S_ISREG(mode)) {
      file_type |= mrl_file_normal;
    }
    if(mode & S_IXUGO)
      file_type |= mrl_file_exec;
  }
  
  if(filepathname[strlen(filepathname) - 1] == '~')
    file_type |= mrl_file_backup;
  
  return file_type;
}

/*
 * Return the file size of the given file *fully named*
 */
static off_t get_file_size(char *filepathname, char *origin) {
  struct stat  pstat;
  char         buf[PATH_MAX + NAME_MAX + 1];

  if((lstat(filepathname, &pstat)) < 0) {
    sprintf(buf, "%s/%s", origin, filepathname);
    if((lstat(buf, &pstat)) < 0)
      return (off_t) 0;
  }

  return pstat.st_size;
}
/*
 *                              END OF PRIVATES
 *****************************************************************************/

/*
 *
 */
static uint32_t file_plugin_get_capabilities (input_plugin_t *this_gen) {
  return INPUT_CAP_SEEKABLE | INPUT_CAP_GET_DIR;
}

/*
 *
 */
static int file_plugin_open (input_plugin_t *this_gen, char *mrl) {

  char                *filename;
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  this->mrl = mrl;

  if (!strncasecmp (mrl, "file:",5))
    filename = &mrl[5];
  else
    filename = mrl;

  xprintf (VERBOSE|INPUT, "Opening >%s<\n",filename);

  this->fh = open (filename, O_RDONLY);

  if (this->fh == -1) {
    return 0;
  }

  return 1;
}

/*
 *
 */
static off_t file_plugin_read (input_plugin_t *this_gen, char *buf, off_t len) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  return read (this->fh, buf, len);
}

/*
 *
 */
static buf_element_t *file_plugin_read_block (input_plugin_t *this_gen, fifo_buffer_t *fifo, off_t todo) {

  off_t                 num_bytes, total_bytes;
  file_input_plugin_t  *this = (file_input_plugin_t *) this_gen;
  buf_element_t        *buf = fifo->buffer_pool_alloc (fifo);

  buf->content = buf->mem;
  total_bytes = 0;

  while (total_bytes < todo) {
    num_bytes = read (this->fh, buf->mem + total_bytes, todo-total_bytes);
    total_bytes += num_bytes;
    if (!num_bytes) {
      buf->free_buffer (buf);
      return NULL;
    }
  }

  buf->size = total_bytes;

  return buf;
}

/*
 *
 */
static off_t file_plugin_seek (input_plugin_t *this_gen, off_t offset, int origin) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  return lseek (this->fh, offset, origin);
}

/*
 *
 */
static off_t file_plugin_get_current_pos (input_plugin_t *this_gen){
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  return lseek (this->fh, 0, SEEK_CUR);
}

/*
 *
 */
static off_t file_plugin_get_length (input_plugin_t *this_gen) {

  struct stat          buf ;
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  if (fstat (this->fh, &buf) == 0) {
    return buf.st_size;
  } else
    perror ("system call fstat");
  return 0;
}

/*
 *
 */
static uint32_t file_plugin_get_blocksize (input_plugin_t *this_gen) {
  return 0;
}

/*
 * Return 1 is filepathname is a directory, otherwise 0
 */
static int is_a_dir(char *filepathname) {
  struct stat  pstat;
  
  stat(filepathname, &pstat);

  return (S_ISDIR(pstat.st_mode));
}

/*
 *
 */
static mrl_t **file_plugin_get_dir (input_plugin_t *this_gen, 
				    char *filename, int *nFiles) {
  file_input_plugin_t  *this = (file_input_plugin_t *) this_gen;
  struct dirent        *pdirent;
  DIR                  *pdir;
  mrl_t                *hide_files, *dir_files, *norm_files;
  char                  current_dir[PATH_MAX + 1];
  char                  current_dir_slashed[PATH_MAX + 1];
  char                  fullfilename[PATH_MAX + NAME_MAX + 1];
  int                   num_hide_files  = 0;
  int                   num_dir_files   = 0;
  int                   num_norm_files  = 0;
  int                   num_files       = -1;
  int                 (*func) ()        = _sortfiles_default;

  *nFiles = 0;
  memset(current_dir, 0, sizeof current_dir);

  /* 
   * No origin location, so got the content of the current directory
   */
  if(!filename) {
    char *pwd;
    
    if((pwd = getenv("PWD")) == NULL)
      snprintf(current_dir, 1, "%s", ".");
    else
      snprintf(current_dir, PATH_MAX, "%s", pwd);
  }
  else {

    /* Remove exceed '/' */
    while((filename[strlen(filename) - 1] == '/') && strlen(filename) > 1)
      filename[strlen(filename) - 1] = '\0';
    
    snprintf(current_dir, PATH_MAX, "%s", filename);
    
  }
  
  if(strcasecmp(current_dir, "/"))
    sprintf(current_dir_slashed, "%s/", current_dir);
  else
    sprintf(current_dir_slashed, "/");
  
  /*
   * Ooch!
   */
  if((pdir = opendir(current_dir)) == NULL)
    return NULL;
  
  dir_files  = (mrl_t *) xmalloc(sizeof(mrl_t) * MAXFILES);
  hide_files = (mrl_t *) xmalloc(sizeof(mrl_t) * MAXFILES);
  norm_files = (mrl_t *) xmalloc(sizeof(mrl_t) * MAXFILES);
  
  while((pdirent = readdir(pdir)) != NULL) {
    
    memset(fullfilename, 0, sizeof fullfilename);
    sprintf(fullfilename, "%s/%s", current_dir, pdirent->d_name);
    
    if(is_a_dir(fullfilename)) {

      dir_files[num_dir_files].mrl    = (char *) 
	xmalloc(strlen(current_dir_slashed) + 1 + strlen(pdirent->d_name) + 1);

      dir_files[num_dir_files].origin = strdup(current_dir);
      sprintf(dir_files[num_dir_files].mrl, "%s%s", 
	      current_dir_slashed, pdirent->d_name);
      dir_files[num_dir_files].link   = NULL;
      dir_files[num_dir_files].type   = get_file_type(fullfilename, current_dir);
      dir_files[num_dir_files].size   = get_file_size(fullfilename, current_dir);
      
      /* The file is a link, follow it */
      if(dir_files[num_dir_files].type & mrl_file_symlink) {
	char linkbuf[PATH_MAX + NAME_MAX + 1];
	int linksize;
	
	memset(linkbuf, 0, sizeof linkbuf);
	linksize = readlink(fullfilename, linkbuf, PATH_MAX + NAME_MAX);
	
	if(linksize < 0) {
	  fprintf(stderr, "%s(%d): readlink() failed: %s\n", 
		  __FUNCTION__, __LINE__, strerror(errno));
	}
	else {
	  dir_files[num_dir_files].link = (char *) xmalloc(linksize + 1);
	  strncpy(dir_files[num_dir_files].link, linkbuf, linksize);
	  dir_files[num_dir_files].type |= get_file_type(dir_files[num_dir_files].link, current_dir);
	}
      }
      
      num_dir_files++;
    } /* Hmmmm, an hidden file ? */
    else if((strlen(pdirent->d_name) > 1)
	    && (pdirent->d_name[0] == '.' &&  pdirent->d_name[1] != '.')) {

      hide_files[num_hide_files].mrl    = (char *) 
	xmalloc(strlen(current_dir_slashed) + 1 + strlen(pdirent->d_name) + 1);

      hide_files[num_hide_files].origin = strdup(current_dir);
      sprintf(hide_files[num_hide_files].mrl, "%s%s", 
	      current_dir_slashed, pdirent->d_name);
      hide_files[num_hide_files].link   = NULL;
      hide_files[num_hide_files].type   = get_file_type(fullfilename, current_dir);
      hide_files[num_hide_files].size   = get_file_size(fullfilename, current_dir);
      
      /* The file is a link, follow it */
      if(hide_files[num_hide_files].type & mrl_file_symlink) {
	char linkbuf[PATH_MAX + NAME_MAX + 1];
	int linksize;
	
	memset(linkbuf, 0, sizeof linkbuf);
	linksize = readlink(fullfilename, linkbuf, PATH_MAX + NAME_MAX);
	
	if(linksize < 0) {
	  fprintf(stderr, "%s(%d): readlink() failed: %s\n", 
		  __FUNCTION__, __LINE__, strerror(errno));
	}
	else {
	  hide_files[num_hide_files].link = (char *) 
	    xmalloc(linksize + 1);
	  strncpy(hide_files[num_hide_files].link, linkbuf, linksize);
	  hide_files[num_hide_files].type |= get_file_type(hide_files[num_hide_files].link, current_dir);
	}
      }
      
      num_hide_files++;
    } /* So a *normal* one. */
    else {

      norm_files[num_norm_files].mrl    = (char *) 
	xmalloc(strlen(current_dir_slashed) + 1 + strlen(pdirent->d_name) + 1);

      norm_files[num_norm_files].origin = strdup(current_dir);
      sprintf(norm_files[num_norm_files].mrl, "%s%s", 
	      current_dir_slashed, pdirent->d_name);
      norm_files[num_norm_files].link   = NULL;
      norm_files[num_norm_files].type   = get_file_type(fullfilename, current_dir);
      norm_files[num_norm_files].size   = get_file_size(fullfilename, current_dir);
      
      /* The file is a link, follow it */
      if(norm_files[num_norm_files].type & mrl_file_symlink) {
	char linkbuf[PATH_MAX + NAME_MAX + 1];
	int linksize;
	
	memset(linkbuf, 0, sizeof linkbuf);
	linksize = readlink(fullfilename, linkbuf, PATH_MAX + NAME_MAX);
	
	if(linksize < 0) {
	  fprintf(stderr, "%s(%d): readlink() failed: %s\n", 
		  __FUNCTION__, __LINE__, strerror(errno));
	}
	else {
	  norm_files[num_norm_files].link = (char *) 
	    xmalloc(linksize + 1);
	  strncpy(norm_files[num_norm_files].link, linkbuf, linksize);
	  norm_files[num_norm_files].type |= get_file_type(norm_files[num_norm_files].link, current_dir);
	}
      }
      
      num_norm_files++;
    }
    
    num_files++;
  }
  
  closedir(pdir);
  
  /*
   * Ok, there are some files here, so sort
   * them then store them into global mrls array.
   */
  if(num_files > 0) {
    int i;

    num_files = 0;

    /*
     * Sort arrays
     */
    if(num_dir_files)
      qsort(dir_files, num_dir_files, sizeof(mrl_t), func);
    
    if(num_hide_files)
      qsort(hide_files, num_hide_files, sizeof(mrl_t), func);
    
    if(num_norm_files)
      qsort(norm_files, num_norm_files, sizeof(mrl_t), func);
    
    /*
     * Add directories entries
     */
    for(i = 0; i < num_dir_files; i++) {
      
      if(num_files >= this->mrls_allocated_entries
	 || this->mrls_allocated_entries == 0) {
	this->mrls[num_files] = (mrl_t *) xmalloc(sizeof(mrl_t));
      }
      else
	memset(this->mrls[num_files], 0, sizeof(mrl_t));
      
      MRL_DUPLICATE(&dir_files[i], this->mrls[num_files]); 

      num_files++;
    }

    /*
     * Add hidden files entries
     */
    for(i = 0; i < num_hide_files; i++) {
      
      if(num_files >= this->mrls_allocated_entries
	 || this->mrls_allocated_entries == 0) {
	this->mrls[num_files] = (mrl_t *) xmalloc(sizeof(mrl_t));
      }
      else
	memset(this->mrls[num_files], 0, sizeof(mrl_t));
      
      MRL_DUPLICATE(&hide_files[i], this->mrls[num_files]); 

      num_files++;
    }
    
    /* 
     * Add other files entries
     */
    for(i = 0; i < num_norm_files; i++) {

      if(num_files >= this->mrls_allocated_entries
	 || this->mrls_allocated_entries == 0) {
	this->mrls[num_files] = (mrl_t *) xmalloc(sizeof(mrl_t));
      }
      else
	memset(this->mrls[num_files], 0, sizeof(mrl_t));

      MRL_DUPLICATE(&norm_files[i], this->mrls[num_files]); 

      num_files++;
    }
    
    /* Some cleanups before leaving */
    for(i = num_dir_files; i == 0; i--)
      MRL_ZERO(&dir_files[i]);
    free(dir_files);
    
    for(i = num_hide_files; i == 0; i--)
      MRL_ZERO(&hide_files[i]);
    free(hide_files);
    
    for(i = num_norm_files; i == 0; i--)
      MRL_ZERO(&norm_files[i]);
    free(norm_files);
    
  }
  else 
    return NULL;
  
  /*
   * Inform caller about files found number.
   */
  *nFiles = num_files;
  
  /*
   * Freeing exceeded mrls if exists.
   */
  if(num_files > this->mrls_allocated_entries)
    this->mrls_allocated_entries = num_files;
  else if(this->mrls_allocated_entries > num_files) {
    while(this->mrls_allocated_entries > num_files) {
      MRL_ZERO(this->mrls[this->mrls_allocated_entries - 1]);
      free(this->mrls[this->mrls_allocated_entries--]);
    }
  }
  
  /*
   * This is useful to let UI know where it should stops ;-).
   */
  this->mrls[num_files] = NULL;

  /*
   * Some debugging info
   */
  /*
  {
    int j = 0;
    while(this->mrls[j]) {
      printf("mrl[%d] = '%s'\n", j, this->mrls[j]->mrl);
      j++;
    }
  }
  */

  return this->mrls;
}

/*
 *
 */
static int file_plugin_eject_media (input_plugin_t *this_gen) {
  return 1; /* doesn't make sense */
}

/*
 *
 */
static char* file_plugin_get_mrl (input_plugin_t *this_gen) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  return this->mrl;
}

/*
 *
 */
static void file_plugin_close (input_plugin_t *this_gen) {
  file_input_plugin_t *this = (file_input_plugin_t *) this_gen;

  xprintf (VERBOSE|INPUT, "closing input\n");

  close(this->fh);
  this->fh = -1;
}

/*
 *
 */
static void file_plugin_stop (input_plugin_t *this_gen) {

  xprintf (VERBOSE|INPUT, "stopping input\n");
  file_plugin_close(this_gen);
}

/*
 *
 */
static char *file_plugin_get_description (input_plugin_t *this_gen) {
  return "plain file input plugin as shipped with xine";
}

/*
 *
 */
static char *file_plugin_get_identifier (input_plugin_t *this_gen) {
  return "file";
}

/*
 *
 */
static int file_plugin_get_optional_data (input_plugin_t *this_gen, 
					  void *data, int data_type) {
  
  return INPUT_OPTIONAL_UNSUPPORTED;
}

/*
 *
 */
input_plugin_t *init_input_plugin (int iface, config_values_t *config) {
  file_input_plugin_t *this;

  xine_debug = config->lookup_int (config, "xine_debug", 0);

  if (iface != 3) {
    printf("file input plugin doesn't support plugin API version %d.\n"
	   "PLUGIN DISABLED.\n"
	   "This means there's a version mismatch between xine and this input"
	   "plugin.\nInstalling current input plugins should help.\n",
	   iface);
    return NULL;
  }

  this = (file_input_plugin_t *) xmalloc (sizeof (file_input_plugin_t));
  
  this->input_plugin.interface_version  = INPUT_PLUGIN_IFACE_VERSION;
  this->input_plugin.get_capabilities   = file_plugin_get_capabilities;
  this->input_plugin.open               = file_plugin_open;
  this->input_plugin.read               = file_plugin_read;
  this->input_plugin.read_block         = file_plugin_read_block;
  this->input_plugin.seek               = file_plugin_seek;
  this->input_plugin.get_current_pos    = file_plugin_get_current_pos;
  this->input_plugin.get_length         = file_plugin_get_length;
  this->input_plugin.get_blocksize      = file_plugin_get_blocksize;
  this->input_plugin.get_dir            = file_plugin_get_dir;
  this->input_plugin.eject_media        = file_plugin_eject_media;
  this->input_plugin.get_mrl            = file_plugin_get_mrl;
  this->input_plugin.close              = file_plugin_close;
  this->input_plugin.stop               = file_plugin_stop;
  this->input_plugin.get_description    = file_plugin_get_description;
  this->input_plugin.get_identifier     = file_plugin_get_identifier;
  this->input_plugin.get_autoplay_list  = NULL;
  this->input_plugin.get_optional_data  = file_plugin_get_optional_data;
  this->input_plugin.handle_input_event = NULL;
  this->input_plugin.is_branch_possible = NULL;

  this->fh                     = -1;
  this->mrl                    = NULL;
  this->config                 = config;
  
  this->mrls = (mrl_t **) xmalloc(sizeof(mrl_t));
  this->mrls_allocated_entries = 0;
  
  return (input_plugin_t *) this;
}
