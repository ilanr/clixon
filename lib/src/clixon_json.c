/*
 *
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  CLIXON is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  CLIXON is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CLIXON; see the file LICENSE.  If not, see
  <http://www.gnu.org/licenses/>.

 * JSON support functions.
 * JSON syntax is according to:
 * http://www.ecma-international.org/publications/files/ECMA-ST/ECMA-404.pdf
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <fnmatch.h>
#include <stdint.h>
#include <syslog.h>
#include <assert.h>

/* cligen */
#include <cligen/cligen.h>

/* clixon */
#include "clixon_err.h"
#include "clixon_log.h"
#include "clixon_xml.h"
#include "clixon_json.h"
#include "clixon_json_parse.h"

#define JSON_INDENT 2 /* maybe we should set this programmatically? */

/* Let xml2json_cbuf_vec() return json array: [a,b].
   ALternative is to create a pseudo-object and return that: {top:{a,b}}
*/
#define VEC_ARRAY 1

enum array_element_type{
    NO_ARRAY=0,
    FIRST_ARRAY,
    MIDDLE_ARRAY,
    LAST_ARRAY,
    BODY_ARRAY
};

enum childtype{
    NULL_CHILD=0, /* eg <a/> no children */
    BODY_CHILD,   /* eg one child which is a body, eg <a>1</a> */
    ANY_CHILD,    /* eg <a><b/></a> or <a><b/><c/></a> */
};

/*! x is element and has exactly one child which in turn has none 
 * Clone from clixon_xml_map.c
 */
static enum childtype
childtype(cxobj *x)
{
    cxobj *xc1; /* the only child of x */

    if (xml_type(x) != CX_ELMNT)
	return -1; /* n/a */
    if (xml_child_nr(x) == 0)
    	return NULL_CHILD;
    if (xml_child_nr(x) > 1)
	return ANY_CHILD;
    xc1 = xml_child_i(x, 0); /* From here exactly one child */
    if (xml_child_nr(xc1) == 0 && xml_type(xc1)==CX_BODY)
	return BODY_CHILD;
    else
	return ANY_CHILD;
}

static char*
childtype2str(enum childtype lt)
{
    switch(lt){
    case NULL_CHILD:
	return "null";
	break;
    case BODY_CHILD:
	return "body";
	break;
    case ANY_CHILD:
	return "any";
	break;
    }
    return "";
}

static char*
arraytype2str(enum array_element_type lt)
{
    switch(lt){
    case NO_ARRAY:
	return "no";
	break;
    case FIRST_ARRAY:
	return "first";
	break;
    case MIDDLE_ARRAY:
	return "middle";
	break;
    case LAST_ARRAY:
	return "last";
	break;
    case BODY_ARRAY:
	return "body";
	break;
    }
    return "";
}

static enum array_element_type
array_eval(cxobj *xprev, 
	  cxobj *x, 
	  cxobj *xnext)
{
    enum array_element_type array = NO_ARRAY;
    int                    eqprev=0;
    int                    eqnext=0;

    if (xml_type(x)!=CX_ELMNT){
	array=BODY_ARRAY;
	goto done;
    }
    if (xnext && 
	xml_type(xnext)==CX_ELMNT &&
	strcmp(xml_name(x),xml_name(xnext))==0)
	    eqnext++;
    if (xprev &&
	xml_type(xprev)==CX_ELMNT &&
	strcmp(xml_name(x),xml_name(xprev))==0)
	eqprev++;
    if (eqprev && eqnext)
	array = MIDDLE_ARRAY;
    else if (eqprev)
	array = LAST_ARRAY;
    else if (eqnext)
	array = FIRST_ARRAY;
    else
	array = NO_ARRAY;
 done:
    return array;
}

/*! Do the actual work of translating XML to JSON 
 * @param[out]   cb       Cligen text buffer containing json on exit
 * @param[in]    x        XML tree structure containing XML to translate
 * @param[in]    arraytype Does x occur in a array (of its parent) and how?
 * @param[in]    level    Indentation level
 * @param[in]    pretty   Pretty-print output (2 means debug)
 *
 * The following matrix explains how the mapping is done.
 * You need to understand what arraytype means (no/first/middle/last)
 * and what childtype is (null,body,any)
+---------+--------------+--------------+--------------+
|array,leaf| null         | body         | any          |
+---------+--------------+--------------+--------------+
|no       | <a/>         |<a>1</a>      |<a><b/></a>   |
|         |              |              |              |
|  json:  |\ta:null      |\ta:          |\ta:{\n       |
|         |              |              |\n}           |
+---------+--------------+--------------+--------------+
|first    |<a/><a..      |<a>1</a><a..  |<a><b/></a><a.|
|         |              |              |              |
|  json:  |\ta:[\n\tnull |\ta:[\n\t     |\ta:[\n\t{\n  |
|         |              |              |\n\t}         |
+---------+--------------+--------------+--------------+
|middle   |..a><a/><a..  |.a><a>1</a><a.|              |
|         |              |              |              |
|  json:  |\tnull        |\t            |\t{a          |
|         |              |              |\n\t}         |
+---------+--------------+--------------+--------------+
|last     |..a></a>      |..a><a>1</a>  |              |
|         |              |              |              |
|  json:  |\tnull        |\t            |\t{a          |
|         |\n\t]         |\n\t]         |\n\t}\t]      |
+---------+--------------+--------------+--------------+
 */
static int 
xml2json1_cbuf(cbuf                  *cb,
	       cxobj                 *x,
	       enum array_element_type arraytype,
	       int                    level,
	       int                    pretty)
{
    int             retval = -1;
    int             i;
    cxobj          *xc;
    enum childtype   childt;

    childt = childtype(x);
    if (pretty==2)
	cprintf(cb, "#%s_array, %s_child ", 
		arraytype2str(arraytype),
		childtype2str(childt));
    switch(arraytype){
    case BODY_ARRAY:
	assert(xml_value(x));
	cprintf(cb, "\"%s\"", xml_value(x));
	break;
    case NO_ARRAY:
	cprintf(cb, "%*s\"%s\": ", 
		pretty?(level*JSON_INDENT):0, "", 
		xml_name(x));
	switch (childt){
	case NULL_CHILD:
	    cprintf(cb, "null");
	    break;
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "{%s", pretty?"\n":"");
	    break;
	default:
	    break;
	}
	break;
    case FIRST_ARRAY:
	cprintf(cb, "%*s\"%s\": ", 
		pretty?(level*JSON_INDENT):0, "", 
		xml_name(x));
	level++;
	cprintf(cb, "[%s%*s", 
		pretty?"\n":"",
		pretty?(level*JSON_INDENT):0, "");
	switch (childt){
	case NULL_CHILD:
	    cprintf(cb, "null");
	    break;
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "{%s", pretty?"\n":"");
	    break;
	default:
	    break;
	}
	break;
    case MIDDLE_ARRAY:
    case LAST_ARRAY:
	level++;
	cprintf(cb, "%*s", 
		pretty?(level*JSON_INDENT):0, "");
	switch (childt){
	case NULL_CHILD:
	    cprintf(cb, "null");
	    break;
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "{ %s", pretty?"\n":"");
	    break;
	default:
	    break;
	}
	break;
    default:
	break;
    }
    for (i=0; i<xml_child_nr(x); i++){
	enum array_element_type xc_arraytype;
	xc = xml_child_i(x, i);
	xc_arraytype = array_eval(i?xml_child_i(x,i-1):NULL, 
				xc, 
				xml_child_i(x, i+1));
	if (xml2json1_cbuf(cb, 
			   xc, 
			   xc_arraytype,
			   level+1, pretty) < 0)
	    goto done;
	if (i<xml_child_nr(x)-1)
	    cprintf(cb, ",%s", pretty?"\n":"");
    }
    switch (arraytype){
    case BODY_ARRAY:
	break;
    case NO_ARRAY:
	switch (childt){
	case NULL_CHILD:
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "%s%*s}", 
		    pretty?"\n":"",
		    pretty?(level*JSON_INDENT):0, "");
	    break;
	default:
	    break;
	}
	level--;
	break;
    case FIRST_ARRAY:
    case MIDDLE_ARRAY:
	switch (childt){
	case NULL_CHILD:
	case BODY_CHILD:
	    break;
	case ANY_CHILD:
	    cprintf(cb, "%s%*s}", 
		    pretty?"\n":"",
		    pretty?(level*JSON_INDENT):0, "");
	    level--;
	    break;
	default:
	    break;
	}
	break;
    case LAST_ARRAY:
	switch (childt){
	case NULL_CHILD:
	case BODY_CHILD:
	    cprintf(cb, "%s",pretty?"\n":"");
	    break;
	case ANY_CHILD:
	    cprintf(cb, "%s%*s}", 
		    pretty?"\n":"",
		    pretty?(level*JSON_INDENT):0, "");
	    cprintf(cb, "%s",pretty?"\n":"");
	    level--;
	    break;
	default:
	    break;
	}
	cprintf(cb, "%*s]",
		pretty?(level*JSON_INDENT):0,"");
	break;
    default:
	break;
    }
    retval = 0;
 done:
    return retval;
}

/*! Translate an XML tree to JSON in a CLIgen buffer
 *
 * @param[in,out] cb     Cligen buffer to write to
 * @param[in]     x      XML tree to translate from
 * @param[in]     pretty Set if output is pretty-printed
 * @param[in]  top    By default only children are printed, set if include top
 * @retval        0      OK
 * @retval       -1      Error
 *
 * @code
 * cbuf *cb;
 * cb = cbuf_new();
 * if (xml2json_cbuf(cb, xn, 0, 1) < 0)
 *   goto err;
 * cbuf_free(cb);
 * @endcode
 * See also xml2json
 */
int 
xml2json_cbuf(cbuf  *cb, 
	      cxobj *x, 
	      int    pretty)
{
    int    retval = 1;
    int    level = 0;

    if (pretty)
	cprintf(cb, "%*s{\n", level*JSON_INDENT,"");
    else
	cprintf(cb, "{");
    if (xml2json1_cbuf(cb, 
		       x, 
		       NO_ARRAY,
		       level+1, pretty) < 0)
	goto done;
    if (pretty)
	cprintf(cb, "\n%*s}\n", level*JSON_INDENT,"");
    else
	cprintf(cb, "}"); 
    retval = 0;
 done:
    return retval;
}

/*! Translate a vectro of xml objects to xml by adding a top pseudo-object.
 * example: <b/><c/> --> <a><b/><c/></a> --> {"a" : {"b" : null,"c" : null}}
 */
int 
xml2json_cbuf_vec(cbuf   *cb, 
		  cxobj **vec,
		  size_t  veclen,
		  int     pretty)
{
    int    retval = -1;
    int    level = 0;
    int    i;
    cxobj *xc;
    enum array_element_type arraytype;

#ifdef VEC_ARRAY
    cprintf(cb, "[%s", pretty?"\n":" ");
    level++;
#else /* pseudo object */
    /* Note: We add a pseudo-object on top of the vector.
     * This object is array_element_type == NO_ARRAY in xml2json1_cbuf 
     * and child_type == ANY_CHILD
     */
    cprintf(cb, "{%s", 
	    pretty?"\n":" ");
    level++;
    cprintf(cb, "%*s\"top\": ", /* NO_ARRAY */
	    pretty?(level*JSON_INDENT):0, "");
    cprintf(cb, "{%s", pretty?"\n":"");  /* ANY_CHILD */
#endif
    for (i=0; i<veclen; i++){
	xc = vec[i];
	arraytype = array_eval(i?vec[i-1]:NULL,
			     xc,
			     i<veclen-1?vec[i+1]:NULL);
	if (xml2json1_cbuf(cb, 
			   xc, 
			   arraytype,
			   level, pretty) < 0)
	    goto done;
	if (i<veclen-1){
	    cprintf(cb, ",");
	    if (pretty)
		cprintf(cb, "\n");
	}
    }
    level--;
#ifdef VEC_ARRAY
    cprintf(cb, "%s]%s", 
	    pretty?"\n":"",
	    pretty?"\n":""); /* top object */
#else /* pseudo object */
    cprintf(cb, "%s%*s}", 
	    pretty?"\n":"",
	    pretty?(level*JSON_INDENT):0, "");
    cprintf(cb, "%s}%s", 
	    pretty?"\n":"",
	    pretty?"\n":""); /* top object */
#endif
    retval = 0;
 done:
    return retval;

}

/*! Translate from xml tree to JSON and print to file
 * @param[in]  f      File to print to
 * @param[in]  x      XML tree to translate from
 * @param[in]  pretty Set if output is pretty-printed
 * @param[in]  top    By default only children are printed, set if include top
 * @retval     0      OK
 * @retval    -1      Error
 *
 * @code
 * if (xml2json(stderr, xn, 0) < 0)
 *   goto err;
 * @endcode
 */
int 
xml2json(FILE   *f, 
	     cxobj *x, 
	 int     pretty)
{
    int   retval = 1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) ==NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (xml2json_cbuf(cb, x, pretty) < 0)
	goto done;
    fprintf(f, "%s", cbuf_get(cb));
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

int 
xml2json_vec(FILE  *f, 
	     cxobj **vec,
	     size_t  veclen,
	     int    pretty)
{
    int   retval = 1;
    cbuf *cb = NULL;

    if ((cb = cbuf_new()) ==NULL){
	clicon_err(OE_XML, errno, "cbuf_new");
	goto done;
    }
    if (xml2json_cbuf_vec(cb, vec, veclen, pretty) < 0)
	goto done;
    fprintf(f, "%s", cbuf_get(cb));
    retval = 0;
 done:
    if (cb)
	cbuf_free(cb);
    return retval;
}

/*! Parse a string containing JSON and return an XML tree
 * @param[in]  str    Input string containing JSON
 * @param[in]  name   Log string, typically filename
 * @param[out] xt     XML top of tree typically w/o children on entry (but created)
 */
static int 
json_parse(char       *str, 
	   const char *name, 
	   cxobj      *xt)
{
    int                         retval = -1;
    struct clicon_json_yacc_arg jy = {0,};

    //    clicon_debug(1, "%s", __FUNCTION__);
    jy.jy_parse_string = str;
    jy.jy_name = name;
    jy.jy_linenum = 1;
    jy.jy_current = xt;
    if (json_scan_init(&jy) < 0)
	goto done;
    if (json_parse_init(&jy) < 0)
	goto done;
    if (clixon_json_parseparse(&jy) != 0) { /* yacc returns 1 on error */
	clicon_log(LOG_NOTICE, "JSON error: %s on line %d", name, jy.jy_linenum);
	if (clicon_errno == 0)
	    clicon_err(OE_XML, 0, "JSON parser error with no error code (should not happen)");
	goto done;
    }
    retval = 0;
 done:
    //    clicon_debug(1, "%s retval:%d", __FUNCTION__, retval);
    json_parse_exit(&jy);
    json_scan_exit(&jy);
    return retval; 
}

/*! Parse string containing JSON and return an XML tree
 *
 * @param[in]  str   String containing JSON
 * @param[out] xt    On success a top of XML parse tree is created with name 'top'
 * @retval  0  OK
 * @retval -1  Error with clicon_err called
 *
 * @code
 *  cxobj *cx = NULL;
 *  if (json_parse_str(str, &cx) < 0)
 *    err;
 *  xml_free(cx);
 * @endcode
 * @note  you need to free the xml parse tree after use, using xml_free()
 */
int 
json_parse_str(char   *str, 
	       cxobj **xt)
{
    if ((*xt = xml_new("top", NULL)) == NULL)
	return -1;
    return json_parse(str, "", *xt);
}

