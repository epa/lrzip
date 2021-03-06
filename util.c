/* 
   Copyright (C) Andrew Tridgell 1998
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
  Utilities used in rzip

  tridge, June 1996
  */

/*
 * Realloc removed
 * Functions added
 *    read_config()
 * Peter Hyman, December 2008
 */

#include "rzip.h"

void err_msg(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

void fatal(const char *format, ...)
{
	va_list ap;

	if (format) {
		va_start(ap, format);
		vfprintf(stderr, format, ap);
		va_end(ap);
	}

	/* Delete temporary files generated for testing or faking stdio */
	if (control.flags & (FLAG_TEST_ONLY | FLAG_STDOUT))
		unlink(control.outfile);

	if (control.flags & FLAG_STDIN)
		unlink(control.infile);

	fprintf(stderr, "Fatal error - exiting\n");
	exit(1);
}

void sighandler()
{
	/* Delete temporary files generated for testing or faking stdio */
	if (control.flags & (FLAG_TEST_ONLY | FLAG_STDOUT))
		unlink(control.outfile);

	if (control.flags & FLAG_STDIN)
		unlink(control.infile);

	exit(0);
}

void read_config( struct rzip_control *control )
{
	/* check for lrzip.conf in ., $HOME/.lrzip and /etc/lrzip */

	FILE *fp;
	char *parameter;
	char *parametervalue;
	char *line, *s;
	char *HOME, *homeconf;

	line = malloc(255);
	homeconf = malloc(255);
	if (line == NULL || homeconf == NULL)
		fatal("Fatal Memory Error in read_config");

	fp = fopen("lrzip.conf", "r");
	if (fp)
		fprintf(control->msgout, "Using configuration file ./lrzip.conf\n");
	if (fp == NULL) {
		fp = fopen("/etc/lrzip/lrzip.conf", "r");
		if (fp)
			fprintf(control->msgout, "Using configuration file /etc/lrzip/lrzip.conf\n");
	}
	if (fp == NULL) {
		HOME=getenv("HOME");
		if (HOME) {
			strcpy(homeconf, HOME);
			strcat(homeconf,"/.lrzip/lrzip.conf");
			fp = fopen(homeconf, "r");
			if (fp)
				fprintf(control->msgout, "Using configuration file %s\n", homeconf);
		}
	}
	if (fp == NULL)
		return;

	/* if we get here, we have a file. read until no more. */

	while ((s = fgets(line, 255, fp)) != NULL) {
		if (strlen(line))
			line[strlen(line) - 1] = '\0';
		parameter = strtok(line, " =");
		if (parameter == NULL)
			continue;
		/* skip if whitespace or # */
		if (isspace(*parameter))
			continue;
		if (*parameter == '#')
			continue;

		parametervalue = strtok(NULL, " =");
		if (parametervalue == NULL)
			continue;

		/* have valid parameter line, now assign to control */

		if (!strcasecmp(parameter, "window"))
			control->window = atoi(parametervalue);
		else if (!strcasecmp(parameter, "compressionlevel")) {
			control->compression_level = atoi(parametervalue);
			if ( control->compression_level < 1 || control->compression_level > 9 )
				fatal("CONF.FILE error. Compression Level must between 1 and 9");
		} else if (!strcasecmp(parameter, "compressionmethod")) {
			/* valid are rzip, gzip, bzip2, lzo, lzma (default) */
			if (control->flags & FLAG_NOT_LZMA)
				fatal("CONF.FILE error. Can only specify one compression method");

			if (!strcasecmp(parametervalue, "bzip2"))
				control->flags |= FLAG_BZIP2_COMPRESS;
			else if (!strcasecmp(parametervalue, "gzip"))
				control->flags |= FLAG_ZLIB_COMPRESS;
			else if (!strcasecmp(parametervalue, "lzo"))
				control->flags |= FLAG_LZO_COMPRESS;
			else if (!strcasecmp(parametervalue, "rzip"))
				control->flags |= FLAG_NO_COMPRESS;
			else if (!strcasecmp(parametervalue, "zpaq"))
				control->flags |= FLAG_ZPAQ_COMPRESS;
			else if (strcasecmp(parametervalue, "lzma"))
				fatal("CONF.FILE error. Invalid compression method %s specified",parametervalue);
		} else if (!strcasecmp(parameter, "testthreshold")) {
			control->threshold = atoi(parametervalue);
			if (control->threshold < 1 || control->threshold > 10)
				fatal("CONF.FILE error. Threshold value out of range %d", parametervalue);
			control->threshold = 1.05-control->threshold / 20;
		} else if (!strcasecmp(parameter, "outputdirectory")) {
			control->outdir = malloc(strlen(parametervalue) + 2);
			if (!control->outdir)
				fatal("Fatal Memory Error in read_config");
			strcpy(control->outdir, parametervalue);
			if (strcmp(parametervalue + strlen(parametervalue) - 1, "/"))
				strcat(control->outdir, "/");
		} else if (!strcasecmp(parameter,"verbosity")) {
			if (control->flags & FLAG_VERBOSE)
				fatal("CONF.FILE error. Verbosity already defined.");

			if (!strcasecmp(parametervalue, "true") || !strcasecmp(parametervalue, "1"))
				control->flags |= FLAG_VERBOSITY;
			else if (!strcasecmp(parametervalue,"max") || !strcasecmp(parametervalue, "2"))
				control->flags |= FLAG_VERBOSITY_MAX;
		} else if (!strcasecmp(parameter,"nice")) {
			control->nice_val = atoi(parametervalue);
			if (control->nice_val < -20 || control->nice_val > 19)
				fatal("CONF.FILE error. Nice must be between -20 and 19");
		} else if (!strcasecmp(parameter, "showprogress")) {
			/* true by default */
			if (!strcasecmp(parametervalue, "false") || !strcasecmp(parametervalue," 0"))
				control->flags &= ~FLAG_SHOW_PROGRESS;
		} else if (!strcmp(parameter, "DELETEFILES")) {
				/* delete files must be case sensitive */
					if (!strcmp(parametervalue, "YES"))
				control->flags &= ~FLAG_KEEP_FILES;
		} else if (!strcmp(parameter, "REPLACEFILE")) {
			/* replace lrzip file must be case sensitive */
			if (!strcmp(parametervalue, "YES"))
				control->flags |= FLAG_FORCE_REPLACE;
		}
	}

	/* clean up */
	free(line);
	free(homeconf);

/*	fprintf(stderr, "\nWindow = %d \
		\nCompression Level = %d \
		\nThreshold = %1.2f \
		\nOutput Directory = %s \
		\nFlags = %d\n", control->window,control->compression_level, control->threshold, control->outdir, control->flags);
*/
}
