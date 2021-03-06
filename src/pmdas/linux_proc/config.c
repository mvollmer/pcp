/*
 * Copyright (c) 1995 Silicon Graphics, Inc.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/stat.h>

#define _REGEX_RE_COMP
#include <sys/types.h>
#include <regex.h>

#include "pmapi.h"
#include "impl.h"

#include "gram_node.h"
#include "gram.tab.h"
#include "config.h"

char *conf_buffer;	/* contains config text */
char *pred_buffer;	/* contains parsed predicate */

static bool_node *the_tree;
static config_vars *the_vars;

/* internal functions */
static int eval_predicate(bool_node *);
static int eval_comparison(bool_node *);
static int eval_num_comp(N_tag, bool_node *, bool_node *);
static int eval_str_comp(N_tag, bool_node *, bool_node *);
static int eval_match_comp(N_tag, bool_node *, bool_node *);
static char* get_strvalue(bool_node *);
static double get_numvalue(bool_node *);
static void eval_error(char *);

extern int parse_predicate(bool_node **);
char *hotproc_configfile;
extern FILE *yyin;

void
set_conf_buffer(char *buf)
{
    conf_buffer = strdup(buf);
}

char *
get_conf_buffer(void)
{
    return conf_buffer;
}

FILE *
open_config(char configfile[])
{
    FILE *conf;

    hotproc_configfile = strdup(configfile);

    if ((conf = fopen(hotproc_configfile, "r")) == NULL) {
	if (pmDebug & DBG_TRACE_APPL0) {
	    fprintf(stderr, "%s: Cannot open configuration file \"%s\": %s\n",
		    pmProgname, hotproc_configfile, osstrerror());
	}
	return NULL;
    }
    return conf;
}

int
parse_config(bool_node **tree)
{
    int sts;
    FILE *file = NULL;
    char tmpname[] = "/var/tmp/pcp.XXXXXX";
    int fid = -1;
    struct stat stat_buf;
    long size;
    char *ptr;

    if ((sts = parse_predicate(tree)) != 0) {
	fprintf(stderr, "%s: Failed to parse configuration file\n", pmProgname);
	return sts;
    }

    /* --- dump to tmp file & read to buffer --- */
    if ((fid = mkstemp(tmpname)) == -1 ||
	(file = fdopen(fid, "w+")) == NULL) {
	sts = -oserror();
	fprintf(stderr, "%s: parse_config: failed to create \"%s\": %s\n",
	    pmProgname, tmpname, strerror(-sts));
	goto error;
    }
    if (unlink(tmpname) == -1) {
	sts = -oserror();
	fprintf(stderr, "%s: parse_config: failed to unlink \"%s\": %s\n",
	    pmProgname, tmpname, strerror(-sts));
	goto error;
    }
    dump_predicate(file, *tree);
    fflush(file);
    if (fstat(fileno(file), &stat_buf) < 0) {
	sts = -oserror();
	fprintf(stderr, "%s: parse_config: failed to stat \"%s\": %s\n",
	    pmProgname, tmpname, strerror(-sts));
	goto error;
    }
    size = (long)stat_buf.st_size;
    ptr = malloc(size+1);
    if (ptr == NULL) {
	sts = -oserror();
	fprintf(stderr, "%s: parse_config: failed to malloc: %s\n",
	    pmProgname, strerror(-sts));
	goto error;
    }
    rewind(file);
    if (fread(ptr, size, 1, file) != 1) {
	clearerr(file);
	fprintf(stderr, "%s: parse_config: failed to fread \"%s\"\n",
	    pmProgname, tmpname);
	sts = -1;
	goto error;
    }
    (void)fclose(file);

    if (pred_buffer != NULL)
	free(pred_buffer);
    pred_buffer = ptr; 
    pred_buffer[size] = '\0';
    return 0;

error:
    if (file != NULL)
	(void)fclose(file);
    return sts;
}

void
new_tree(bool_node *tree)
{
    /* free_tree will delete the tree we just constructed if NULL is passed in.  Not sure why */
    if (the_tree != NULL )
	free_tree(the_tree);

    the_tree = tree;
}

int
read_config(FILE *conf)
{
    struct stat stat_buf;
    long size;
    int sts;
    size_t nread;

    /* get length of file */
    sts = fstat(fileno(conf), &stat_buf);
    if (sts < 0) {
	fprintf(stderr, "%s: Failure to stat configuration file \"%s\": %s\n",
	    pmProgname, hotproc_configfile, osstrerror());
	return 0;
    }
    size = (long)stat_buf.st_size;

    /* create buffer */
    conf_buffer = (char*)malloc(size+1*sizeof(char));
    if (conf_buffer == NULL) {
	fprintf(stderr, "%s: Cannot create buffer configuration file \"%s\"\n",
	    pmProgname, hotproc_configfile);
	return 0;
    }

    /* read whole file into buffer */
    nread = fread(conf_buffer, sizeof(char), size, conf);
    if (nread != size) {
	fprintf(stderr, "%s: Failure to fread \"%s\" file into buffer\n",
	    pmProgname, hotproc_configfile);
	return 0;
    }
    conf_buffer[size] = '\0'; /* terminate the buffer */

    if (parse_config(&the_tree) != 0)
        return 0;

    return 1;
}

void
dump_tree(FILE *f)
{
    dump_bool_tree(f, the_tree);
}

int
eval_tree(config_vars *vars)
{
    the_vars = vars;
    return eval_predicate(the_tree);
}

/*
 * do predicate testing for qa
 */

#define QA_LINE 512

/*
 * Return convention
 * EOF = finished line or file
 * 0 = error in input
 * 1 = successful and continue
 */

/*
 * Read test vars of form: "var=value|var=value|var=value"
 */

static int
read_test_var(char *line, config_vars *vars)
{
    const char EQUALS = '=';
    const char DIVIDER = '|';
    char var[QA_LINE];
    char value[QA_LINE];
    static char *c;
    int i = 0;

    /* if line is NULL then continue where left off */
    if (line != NULL)
	c = line;

    if (*c == '\n')
	return EOF;

    /* --- get variable name --- */
    i = 0;
    while(*c != EQUALS && *c != '\n') {
        var[i++] = *c++;
    }
    var[i] = '\0';

    if (*c == '\n') {
	fprintf(stderr, "%s: Error reading test variable, "
			"looking for \"%c\"\n", pmProgname, EQUALS);
	return 0;
    }

    c++; /* skip over EQUALS */

    /* --- get value --- */
    i = 0;
    while(*c != DIVIDER && *c != '\n') {
        value[i++] = *c++;
    }
    value[i] = '\0';

    if (*c == DIVIDER) /* skip over DIVIDER */
        c++;

    /* --- var = value --- */
    if (strcmp(var, "uid") == 0) {
        vars->uid = atoi(value);
    }
    else if (strcmp(var, "uname") == 0) {
        if ((strcpy(vars->uname, value)) == NULL)
            goto failure;
    } 
    else if (strcmp(var, "gid") == 0) {
        vars->gid = atoi(value);
    } 
    else if (strcmp(var, "gname") == 0) {
        if ((strcpy(vars->gname, value)) == NULL)
            goto failure;
    } 
    else if (strcmp(var, "fname") == 0) {
        (void)strcpy(vars->fname, value);
    } 
    else if (strcmp(var, "psargs") == 0) {
        (void)strcpy(vars->psargs, value);
    } 
    else if (strcmp(var, "cpuburn") == 0) {
        vars->cpuburn = atof(value);
    } 
    /*else if (strcmp(var, "syscalls") == 0) {
        vars->preds.syscalls = atof(value);
    } 
    else if (strcmp(var, "pu_sysc") == 0) {
        vars->pu_sysc = atol(value);
    } 
    */
    else if (strcmp(var, "ctxswitch") == 0) {
        vars->preds.ctxswitch = atof(value);
    } 
    /*
    else if (strcmp(var, "pu_vctx") == 0) {
        vars->pu_vctx = atol(value);
    } 
    else if (strcmp(var, "pu_ictx") == 0) {
        vars->pu_ictx = atol(value);
    }*/ 
    else if (strcmp(var, "virtualsize") == 0) {
        vars->preds.virtualsize = atof(value);
    } 
    /*else if (strcmp(var, "pr_size") == 0) {
        vars->pr_size = atol(value);
    }
    else if (strcmp(var, "residentsize") == 0) {
        vars->preds.residentsize = atof(value);
    } 
    else if (strcmp(var, "pr_rssize") == 0) {
        vars->pr_rssize = atol(value);
    }*/
    else if (strcmp(var, "iodemand") == 0) {
        vars->preds.iodemand = atof(value);
    } 
    /*else if (strcmp(var, "pu_gbread") == 0) {
        vars->pu_gbread = atol(value);
    } 
    else if (strcmp(var, "pu_bread") == 0) {
        vars->pu_bread = atol(value);
    } 
    else if (strcmp(var, "pu_gbwrit") == 0) {
        vars->pu_gbwrit = atol(value);
    } 
    else if (strcmp(var, "pu_bwrit") == 0) {
        vars->pu_bwrit = atol(value);
    } 
    else if (strcmp(var, "iowait") == 0) {
        vars->preds.iowait = atof(value);
    } 
    else if (strcmp(var, "ac_bwtime") == 0) {
        vars->ac_bwtime = atoll(value);
    } 
    else if (strcmp(var, "ac_rwtime") == 0) {
        vars->ac_rwtime = atoll(value);
    }*/
    else if (strcmp(var, "schedwait") == 0) {
        vars->preds.schedwait = atof(value);
    } 
    /*else if (strcmp(var, "ac_qwtime") == 0) {
        vars->ac_qwtime = atoll(value);
    }*/
    else {
	fprintf(stderr, "%s: Error unrecognised test variable: \"%s\"\n", 
                pmProgname, var);
	return 0;
    }

    return 1;

failure:
    fprintf(stderr, "%s: malloc failed for read_test_var()\n", pmProgname); 
    exit(1);
}

int
read_test_values(FILE *file, config_vars *vars)
{
    static char line[QA_LINE];
    int sts;
    int i;

    if (fgets(line, QA_LINE-1, file) == NULL)
        return EOF;

    if (strlen(line) == QA_LINE-1) {
	fprintf(stderr, "%s: line limit exceeded\n", pmProgname);
        return 0;
    }

    /* note that line must end in '\n' */

    /* reset all values */
    memset(vars, 0, sizeof(*vars));

    /* read each var=value pair for a line */
    for (i=0;/*forever*/;i++) {
        sts = read_test_var((i==0?line:NULL), vars); 
        if (sts == EOF) 
	    return 1;
        if (sts == 0)
	    return 0;
    }
}

static void 
eval_error(char *msg)
{
   fprintf(stderr, "%s: Internal error : %s\n", pmProgname, msg?msg:""); 
   exit(1);
}

static int
eval_predicate(bool_node *pred)
{
    bool_node *lhs, *rhs;

    switch (pred->tag) {
	case N_and:	
	    lhs = pred->data.children.left;
	    rhs = pred->data.children.right;
	    return eval_predicate(lhs) && eval_predicate(rhs);	
	case N_or:	
	    lhs = pred->data.children.left;
	    rhs = pred->data.children.right;
	    return eval_predicate(lhs) || eval_predicate(rhs);	
	case N_not:	
	    lhs = pred->data.children.left;
	    return !eval_predicate(lhs);	
	case N_true:
	    return 1;
	case N_false:
	    return 0;
	default:
	    return eval_comparison(pred);
    }
}

static int
eval_comparison(bool_node *comp)
{
    bool_node *lhs = comp->data.children.left;
    bool_node *rhs = comp->data.children.right;

    switch (comp->tag) {
	case N_lt: case N_gt: case N_ge: case N_le:
        case N_eq: case N_neq:
	    return eval_num_comp(comp->tag, lhs, rhs); 
	case N_seq: case N_sneq:
	    return eval_str_comp(comp->tag, lhs, rhs);
	case N_match: case N_nmatch:
	    return eval_match_comp(comp->tag, lhs, rhs);
	default:
	    eval_error("comparison");
	    break;
    }
    return 0;
}

static int
eval_num_comp(N_tag tag, bool_node *lhs, bool_node *rhs)
{
    double x = get_numvalue(lhs);
    double y = get_numvalue(rhs);

    switch (tag) {
	case N_lt: return (x < y);
	case N_gt: return (x > y);
	case N_le: return (x <= y);
	case N_ge: return (x >= y);
	case N_eq: return (x == y);
	case N_neq: return (x != y);
	default:
	    eval_error("number comparison");
	    break;
    }
   return 0;
}

static double
get_numvalue(bool_node *n)
{
    switch(n->tag) {
	case N_number: return n->data.num_val;
	case N_cpuburn: return the_vars->cpuburn;
	/*case N_syscalls: return the_vars->preds.syscalls;*/
        case N_ctxswitch: return the_vars->preds.ctxswitch;
        case N_virtualsize: return the_vars->preds.virtualsize;
        case N_residentsize: return the_vars->preds.residentsize;
        case N_iodemand: return the_vars->preds.iodemand;
        case N_iowait: return the_vars->preds.iowait;
        case N_schedwait: return the_vars->preds.schedwait;
	case N_gid: return the_vars->gid;
	case N_uid: return the_vars->uid;
	default:
	    eval_error("number value");
	    break;
    }
    return 0;
}

static int
eval_str_comp(N_tag tag, bool_node *lhs, bool_node *rhs)
{
    char *x = get_strvalue(lhs);
    char *y = get_strvalue(rhs);

    switch (tag) {
	case N_seq: return (strcmp(x,y)==0?1:0);
	case N_sneq: return (strcmp(x,y)==0?0:1);
	default:
	    eval_error("string comparison");
	    break;
    }
    return 0;
}

static int
eval_match_comp(N_tag tag, bool_node *lhs, bool_node *rhs)
{
    int sts;
    char *res;
    char *str= get_strvalue(lhs);
    char *pat = get_strvalue(rhs);

    if (rhs->tag != N_pat) {
	eval_error("match");
    }

    res = re_comp(pat);
    if (res != NULL) {
	/* should have been checked at lex stage */
	/* => internal error */
	eval_error(res);
    }
    sts = re_exec(str);
    if (sts < 0) {
	eval_error("re_exec");
    }

    switch (tag) {
	case N_match: return sts;
	case N_nmatch: return !sts;
	default:
	    eval_error("match comparison");
	    break;
    }
    return 0;
}

static char *
get_strvalue(bool_node *n)
{
    switch (n->tag) {
	case N_str: 
	case N_pat:
		return n->data.str_val;
	case N_gname: 
		/*if (the_vars->gname != NULL)*/
		    return the_vars->gname;
		/*else
		    return get_gname_info(the_vars->gid);*/
	case N_uname: 
		/*if (the_vars->uname != NULL)*/
		    return the_vars->uname;
		/*else
		    return get_uname_info(the_vars->uid);*/
	case N_fname: return the_vars->fname; 
	case N_psargs: return the_vars->psargs; 
	default:
	    eval_error("string value");
	    break;
    }
    return 0;
}
