/* -*- Mode: c; c-basic-offset: 2 -*-
 *
 * rasqal_graph_pattern.c - Rasqal graph pattern class
 *
 * Copyright (C) 2004-2008, David Beckett http://www.dajobe.org/
 * Copyright (C) 2004-2005, University of Bristol, UK http://www.bristol.ac.uk/
 * 
 * This package is Free Software and part of Redland http://librdf.org/
 * 
 * It is licensed under the following three licenses as alternatives:
 *   1. GNU Lesser General Public License (LGPL) V2.1 or any newer version
 *   2. GNU General Public License (GPL) V2 or any newer version
 *   3. Apache License, V2.0 or any newer version
 * 
 * You may not use this file except in compliance with at least one of
 * the above three licenses.
 * 
 * See LICENSE.html or LICENSE.txt at the top of this package for the
 * complete terms and further detail along with the license texts for
 * the licenses in COPYING.LIB, COPYING and LICENSE-2.0.txt respectively.
 * 
 * 
 */

#ifdef HAVE_CONFIG_H
#include <rasqal_config.h>
#endif

#ifdef WIN32
#include <win32_rasqal_config.h>
#endif

#include <stdio.h>
#include <string.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#include <stdarg.h>

#include "rasqal.h"
#include "rasqal_internal.h"


/*
 * rasqal_new_graph_pattern:
 * @query: #rasqal_graph_pattern query object
 * @op: enum #rasqal_graph_pattern_operator operator
 *
 * INTERNAL - Create a new graph pattern object.
 * 
 * NOTE: This does not initialise the graph pattern completely
 * but relies on other operations.  The empty graph pattern
 * has no triples and no sub-graphs.
 *
 * Return value: a new #rasqal_graph_pattern object or NULL on failure
 **/
static rasqal_graph_pattern*
rasqal_new_graph_pattern(rasqal_query* query, 
                         rasqal_graph_pattern_operator op)
{
  rasqal_graph_pattern* gp;

  if(!query)
    return NULL;
  
  gp=(rasqal_graph_pattern*)RASQAL_CALLOC(rasqal_graph_pattern, 1, sizeof(rasqal_graph_pattern));
  if(!gp)
    return NULL;

  gp->op = op;
  
  gp->query=query;
  gp->triples= NULL;
  gp->start_column= -1;
  gp->end_column= -1;
  /* This is initialised by rasqal_query_prepare_count_graph_patterns() inside
   * rasqal_query_prepare()
   */
  gp->gp_index= -1;

  return gp;
}


/*
 * rasqal_new_basic_graph_pattern:
 * @query: #rasqal_graph_pattern query object
 * @triples: triples sequence containing the graph pattern
 * @start_column: first triple in the pattern
 * @end_column: last triple in the pattern
 *
 * INTERNAL - Create a new graph pattern object over triples.
 * 
 * Return value: a new #rasqal_graph_pattern object or NULL on failure
 **/
rasqal_graph_pattern*
rasqal_new_basic_graph_pattern(rasqal_query* query,
                               raptor_sequence *triples,
                               int start_column, int end_column)
{
  rasqal_graph_pattern* gp;

  if(!triples)
    return NULL;
  
  gp=rasqal_new_graph_pattern(query, RASQAL_GRAPH_PATTERN_OPERATOR_BASIC);
  if(!gp)
    return NULL;

  gp->triples=triples;
  gp->start_column=start_column;
  gp->end_column=end_column;

  return gp;
}


/*
 * rasqal_new_graph_pattern_from_sequence:
 * @query: #rasqal_graph_pattern query object
 * @graph_patterns: sequence containing the graph patterns
 * @operator: enum #rasqal_graph_pattern_operator such as
 * RASQAL_GRAPH_PATTERN_OPERATOR_OPTIONAL
 *
 * INTERNAL - Create a new graph pattern from a sequence of graph_patterns.
 * 
 * Return value: a new #rasqal_graph_pattern object or NULL on failure
 **/
rasqal_graph_pattern*
rasqal_new_graph_pattern_from_sequence(rasqal_query* query,
                                       raptor_sequence *graph_patterns, 
                                       rasqal_graph_pattern_operator op)
{
  rasqal_graph_pattern* gp;

  gp=rasqal_new_graph_pattern(query, op);
  if(!gp) {
  	if(graph_patterns)
      raptor_free_sequence(graph_patterns);
    return NULL;
  }
  
  gp->graph_patterns=graph_patterns;
  return gp;
}


/*
 * rasqal_new_filter_graph_pattern:
 * @query: #rasqal_graph_pattern query object
 * @expr: expression
 *
 * INTERNAL - Create a new graph pattern from a sequence of graph_patterns.
 * 
 * Return value: a new #rasqal_graph_pattern object or NULL on failure
 **/
rasqal_graph_pattern*
rasqal_new_filter_graph_pattern(rasqal_query* query,
                                rasqal_expression* expr)
{
  rasqal_graph_pattern* gp;

  gp=rasqal_new_graph_pattern(query, RASQAL_GRAPH_PATTERN_OPERATOR_FILTER);
  if(!gp) {
    rasqal_free_expression(expr);
    return NULL;
  }
  
  if(rasqal_graph_pattern_set_filter_expression(gp, expr)) {
    rasqal_free_graph_pattern(gp);
    gp=NULL;
  }

  return gp;
}


/*
 * rasqal_free_graph_pattern:
 * @gp: #rasqal_graph_pattern object
 *
 * INTERNAL - Free a graph pattern object.
 * 
 **/
void
rasqal_free_graph_pattern(rasqal_graph_pattern* gp)
{
  RASQAL_ASSERT_OBJECT_POINTER_RETURN(gp, rasqal_graph_pattern);
  
  if(gp->graph_patterns)
    raptor_free_sequence(gp->graph_patterns);
  
  if(gp->filter_expression)
    rasqal_free_expression(gp->filter_expression);

  if(gp->constraints)
    raptor_free_sequence(gp->constraints);

  if(gp->origin)
    rasqal_free_literal(gp->origin);

  RASQAL_FREE(rasqal_graph_pattern, gp);
}


/*
 * rasqal_graph_pattern_adjust:
 * @gp: #rasqal_graph_pattern graph pattern
 * @offset: adjustment
 *
 * INTERNAL - Adjust the column in a graph pattern by the offset.
 * 
 **/
void
rasqal_graph_pattern_adjust(rasqal_graph_pattern* gp, int offset)
{
  gp->start_column += offset;
  gp->end_column += offset;
}


/**
 * rasqal_graph_pattern_set_filter_expression:
 * @gp: #rasqal_graph_pattern query object
 * @expr: #rasqal_expression expr - ownership taken
 *
 * Set a filter graph pattern constraint expression
 *
 * Return value: non-0 on failure
 **/
int
rasqal_graph_pattern_set_filter_expression(rasqal_graph_pattern* gp,
                                           rasqal_expression* expr)
{
  if(gp->filter_expression)
    rasqal_free_expression(gp->filter_expression);
  gp->filter_expression = expr;
  return 0;
}


/**
 * rasqal_graph_pattern_get_filter_expression:
 * @gp: #rasqal_graph_pattern query object
 *
 * Get a filter graph pattern's constraint expression
 *
 * Return value: expression or NULL on failure
 **/
rasqal_expression*
rasqal_graph_pattern_get_filter_expression(rasqal_graph_pattern* gp)
{
  return gp->filter_expression;
}


/**
 * rasqal_graph_pattern_add_constraint:
 * @gp: #rasqal_graph_pattern query object
 * @expr: #rasqal_expression expr - ownership taken
 *
 * Add a constraint expression to the graph_pattern.
 *
 * @deprecated: Use rasqal_graph_pattern_set_filter_expression()
 *
 * Return value: non-0 on failure
 **/
int
rasqal_graph_pattern_add_constraint(rasqal_graph_pattern* gp,
                                    rasqal_expression* expr)
{
  if(!gp->constraints) {
    gp->constraints=raptor_new_sequence(NULL, 
                                        (raptor_sequence_print_handler*)rasqal_expression_print);
    if(!gp->constraints) {
      rasqal_free_expression(expr);
      return 1;
    }
  }
  raptor_sequence_set_at(gp->constraints, 0, expr);

  return rasqal_graph_pattern_set_filter_expression(gp, expr);
}


/**
 * rasqal_graph_pattern_get_constraint_sequence:
 * @gp: #rasqal_graph_pattern object
 *
 * Get the sequence of constraints expressions in the query.
 *
 * @deprecated: always returns a sequence with one expression.
 * Use rasqal_graph_pattern_get_filter_expression() to return it.
 *
 * Return value: NULL
 **/
raptor_sequence*
rasqal_graph_pattern_get_constraint_sequence(rasqal_graph_pattern* gp)
{
  return gp->constraints;
}


/**
 * rasqal_graph_pattern_get_constraint:
 * @gp: #rasqal_graph_pattern query object
 * @idx: index into the sequence (0 or larger)
 *
 * Get a constraint in the sequence of constraint expressions in the query.
 *
 * @deprecated: A FILTER graph pattern always has one expression
 * that can be returned with rasqal_graph_pattern_get_filter_expression()
 *
 * Return value: a #rasqal_expression pointer or NULL if out of the sequence range
 **/
rasqal_expression*
rasqal_graph_pattern_get_constraint(rasqal_graph_pattern* gp, int idx)
{
  return rasqal_graph_pattern_get_filter_expression(gp);
}


/**
 * rasqal_graph_pattern_get_operator:
 * @graph_pattern: #rasqal_graph_pattern graph pattern object
 *
 * Get the graph pattern operator .
 * 
 * The operator for the given graph pattern. See also
 * rasqal_graph_pattern_operator_as_string().
 *
 * Return value: graph pattern operator
 **/
rasqal_graph_pattern_operator
rasqal_graph_pattern_get_operator(rasqal_graph_pattern* graph_pattern)
{
  return graph_pattern->op;
}


static const char* const rasqal_graph_pattern_operator_labels[RASQAL_GRAPH_PATTERN_OPERATOR_LAST+1]={
  "UNKNOWN",
  "Basic",
  "Optional",
  "Union",
  "Group",
  "Graph",
  "Filter"
};


/**
 * rasqal_graph_pattern_operator_as_string:
 * @op: the #rasqal_graph_pattern_operator verb of the query
 *
 * Get a string for the query verb.
 * 
 * Return value: pointer to a shared string label for the query verb
 **/
const char*
rasqal_graph_pattern_operator_as_string(rasqal_graph_pattern_operator op)
{
  if(op <= RASQAL_GRAPH_PATTERN_OPERATOR_UNKNOWN || 
     op > RASQAL_GRAPH_PATTERN_OPERATOR_LAST)
    op=RASQAL_GRAPH_PATTERN_OPERATOR_UNKNOWN;

  return rasqal_graph_pattern_operator_labels[(int)op];
}
  

#ifdef RASQAL_DEBUG
#define DO_INDENTING 0
#else
#define DO_INDENTING -1
#endif

#define SPACES_LENGTH 80
static const char spaces[SPACES_LENGTH+1]="                                                                                ";

static void
rasqal_graph_pattern_write_indent(raptor_iostream *iostr, int indent) 
{
  while(indent > 0) {
    int sp=(indent > SPACES_LENGTH) ? SPACES_LENGTH : indent;
    raptor_iostream_write_bytes(iostr, spaces, sizeof(char), sp);
    indent -= sp;
  }
}


static void
rasqal_graph_pattern_write_plurals(raptor_iostream *iostr,
                                   const char* label, int value)
{
  raptor_iostream_write_decimal(iostr, value);
  raptor_iostream_write_byte(iostr, ' ');
  raptor_iostream_write_string(iostr, label);
  if(value != 1)
    raptor_iostream_write_byte(iostr, 's');
}


/**
 * rasqal_graph_pattern_print_indent:
 * @gp: the #rasqal_graph_pattern object
 * @iostr: the iostream to write to
 * @indent: the current indent level, <0 for no indenting
 *
 * INTERNAL - Print a #rasqal_graph_pattern in a debug format with indenting
 * 
 **/
static int
rasqal_graph_pattern_write_internal(rasqal_graph_pattern* gp,
                                    raptor_iostream* iostr, int indent)
{
  int pending_nl=0;
  
  raptor_iostream_write_counted_string(iostr, "graph pattern", 13);
  if(gp->gp_index >= 0) {
    raptor_iostream_write_byte(iostr, '[');
    raptor_iostream_write_decimal(iostr, gp->gp_index);
    raptor_iostream_write_byte(iostr, ']');
  }
  raptor_iostream_write_byte(iostr, ' ');
  raptor_iostream_write_string(iostr, 
                               rasqal_graph_pattern_operator_as_string(gp->op));
  raptor_iostream_write_byte(iostr, '(');

  if(indent >= 0)
    indent+= 2;

  if(gp->triples) {
    int size=gp->end_column - gp->start_column +1;
    int i;
    raptor_iostream_write_counted_string(iostr, "over ", 5);
    rasqal_graph_pattern_write_plurals(iostr, "triple", size);
    raptor_iostream_write_byte(iostr, '[');

    if(indent >= 0) {
      raptor_iostream_write_byte(iostr, '\n');
      indent+= 2;
      rasqal_graph_pattern_write_indent(iostr, indent);
    }
    
    for(i=gp->start_column; i <= gp->end_column; i++) {
      rasqal_triple *t=(rasqal_triple*)raptor_sequence_get_at(gp->triples, i);
      if(i > gp->start_column) {
        raptor_iostream_write_counted_string(iostr, " ,", 2);

        if(indent >= 0) {
          raptor_iostream_write_byte(iostr, '\n');
          rasqal_graph_pattern_write_indent(iostr, indent);
        }
      }
      rasqal_triple_write(t, iostr);
    }
    if(indent >= 0) {
      raptor_iostream_write_byte(iostr, '\n');
      indent-= 2;
      rasqal_graph_pattern_write_indent(iostr, indent);
    }
    raptor_iostream_write_byte(iostr, ']');

    pending_nl=1;
  }

  if(gp->origin) {
    if(pending_nl) {
      raptor_iostream_write_counted_string(iostr, " ,", 2);

      if(indent >= 0) {
        raptor_iostream_write_byte(iostr, '\n');
        rasqal_graph_pattern_write_indent(iostr, indent);
      }
    }

    raptor_iostream_write_counted_string(iostr, "origin ", 7);

    rasqal_literal_write(gp->origin, iostr);

    pending_nl=1;
  }

  if(gp->graph_patterns) {
    int size=raptor_sequence_size(gp->graph_patterns);
    int i;

    if(pending_nl) {
      raptor_iostream_write_counted_string(iostr, " ,", 2);
      if(indent >= 0) {
        raptor_iostream_write_byte(iostr, '\n');
        rasqal_graph_pattern_write_indent(iostr, indent);
      }
    }
    
    raptor_iostream_write_counted_string(iostr, "over ", 5);
    rasqal_graph_pattern_write_plurals(iostr, "graph pattern", size);
    raptor_iostream_write_byte(iostr, '[');

    if(indent >= 0) {
      raptor_iostream_write_byte(iostr, '\n');
      indent+= 2;
      rasqal_graph_pattern_write_indent(iostr, indent);
    }
    
    for(i=0; i< size; i++) {
      rasqal_graph_pattern* sgp;
      sgp=(rasqal_graph_pattern*)raptor_sequence_get_at(gp->graph_patterns, i);
      if(i) {
        raptor_iostream_write_counted_string(iostr, " ,", 2);
        if(indent >= 0) {
          raptor_iostream_write_byte(iostr, '\n');
          rasqal_graph_pattern_write_indent(iostr, indent);
        }
      }
      if(sgp)
        rasqal_graph_pattern_write_internal(sgp, iostr, indent);
      else
        raptor_iostream_write_counted_string(iostr, "(empty)", 7);
    }
    if(indent >= 0) {
      raptor_iostream_write_byte(iostr, '\n');
      indent-= 2;
      rasqal_graph_pattern_write_indent(iostr, indent);
    }
    raptor_iostream_write_byte(iostr, ']');

    pending_nl=1;
  }

  if(gp->filter_expression) {
    if(pending_nl) {
      raptor_iostream_write_counted_string(iostr, " ,", 2);

      if(indent >= 0) {
        raptor_iostream_write_byte(iostr, '\n');
        rasqal_graph_pattern_write_indent(iostr, indent);
      }
    }
    
    if(gp->triples || gp->graph_patterns)
      raptor_iostream_write_counted_string(iostr, "with ", 5);
  
    if(indent >= 0) {
      raptor_iostream_write_byte(iostr, '\n');
      indent+= 2;
      rasqal_graph_pattern_write_indent(iostr, indent);
    }

    rasqal_expression_write(gp->filter_expression, iostr);
    if(indent >= 0)
      indent-= 2;
    
    pending_nl=1;
  }

  if(indent >= 0)
    indent-= 2;

  if(pending_nl) {
    if(indent >= 0) {
      raptor_iostream_write_byte(iostr, '\n');
      rasqal_graph_pattern_write_indent(iostr, indent);
    }
  }
  
  raptor_iostream_write_byte(iostr, ')');

  return 0;
}


/**
 * rasqal_graph_pattern_print:
 * @gp: the #rasqal_graph_pattern object
 * @fh: the #FILE* handle to print to
 *
 * Print a #rasqal_graph_pattern in a debug format.
 * 
 * The print debug format may change in any release.
 * 
 **/
void
rasqal_graph_pattern_print(rasqal_graph_pattern* gp, FILE* fh)
{
  raptor_iostream* iostr;
  iostr=raptor_new_iostream_to_file_handle(fh);
  rasqal_graph_pattern_write_internal(gp, iostr, DO_INDENTING);
  raptor_free_iostream(iostr);
}


/**
 * rasqal_graph_pattern_visit:
 * @query: #rasqal_query to operate on
 * @gp: #rasqal_graph_pattern graph pattern
 * @fn: pointer to function to apply that takes user data and graph pattern parameters
 * @user_data: user data for applied function 
 * 
 * Visit a user function over a #rasqal_graph_pattern
 *
 * If the user function @fn returns 0, the visit is truncated.
 *
 * Return value: 0 if the visit was truncated.
 **/
int
rasqal_graph_pattern_visit(rasqal_query *query,
                           rasqal_graph_pattern* gp,
                           rasqal_graph_pattern_visit_fn fn,
                           void *user_data)
{
  raptor_sequence *seq;
  int result;
  
  result=fn(query, gp, user_data);
  if(result)
    return result;
  
  seq=rasqal_graph_pattern_get_sub_graph_pattern_sequence(gp);
  if(seq && raptor_sequence_size(seq) > 0) {
    int gp_index=0;
    while(1) {
      rasqal_graph_pattern* sgp;
      sgp=rasqal_graph_pattern_get_sub_graph_pattern(gp, gp_index);
      if(!sgp)
        break;
      
      result=rasqal_graph_pattern_visit(query, sgp, fn, user_data);
      if(result)
        return result;
      gp_index++;
    }
  }

  return 0;
}


/**
 * rasqal_graph_pattern_get_index:
 * @gp: #rasqal_graph_pattern graph pattern
 * 
 * Get the graph pattern absolute index in the array of graph patterns.
 * 
 * The graph pattern index is assigned when rasqal_query_prepare() is
 * run on a query containing a graph pattern.
 *
 * Return value: index or <0 if no index has been assigned yet
 **/
int
rasqal_graph_pattern_get_index(rasqal_graph_pattern* gp)
{
  return gp->gp_index;
}


/**
 * rasqal_graph_pattern_add_sub_graph_pattern:
 * @graph_pattern: graph pattern to add to
 * @sub_graph_pattern: graph pattern to add inside
 *
 * Add a sub graph pattern to a graph pattern.
 *
 * Return value: non-0 on failure
 **/
int
rasqal_graph_pattern_add_sub_graph_pattern(rasqal_graph_pattern* graph_pattern,
                                           rasqal_graph_pattern* sub_graph_pattern)
{
  if(!graph_pattern->graph_patterns) {
    graph_pattern->graph_patterns=raptor_new_sequence((raptor_sequence_free_handler*)rasqal_free_graph_pattern, (raptor_sequence_print_handler*)rasqal_graph_pattern_print);
    if(!graph_pattern->graph_patterns) {
      if(sub_graph_pattern)
        rasqal_free_graph_pattern(sub_graph_pattern);
      return 1;
    }
  }
  return raptor_sequence_push(graph_pattern->graph_patterns, sub_graph_pattern);
}


/**
 * rasqal_graph_pattern_get_triple:
 * @graph_pattern: #rasqal_graph_pattern graph pattern object
 * @idx: index into the sequence of triples in the graph pattern
 *
 * Get a triple inside a graph pattern.
 * 
 * Return value: #rasqal_triple or NULL if out of range
 **/
rasqal_triple*
rasqal_graph_pattern_get_triple(rasqal_graph_pattern* graph_pattern, int idx)
{
  if(!graph_pattern->triples)
    return NULL;

  idx += graph_pattern->start_column;

  if(idx > graph_pattern->end_column)
    return NULL;
  
  return (rasqal_triple*)raptor_sequence_get_at(graph_pattern->triples, idx);
}


/**
 * rasqal_graph_pattern_get_sub_graph_pattern_sequence:
 * @graph_pattern: #rasqal_graph_pattern graph pattern object
 *
 * Get the sequence of graph patterns inside a graph pattern .
 * 
 * Return value:  a #raptor_sequence of #rasqal_graph_pattern pointers.
 **/
raptor_sequence*
rasqal_graph_pattern_get_sub_graph_pattern_sequence(rasqal_graph_pattern* graph_pattern)
{
  return graph_pattern->graph_patterns;
}


/**
 * rasqal_graph_pattern_get_sub_graph_pattern:
 * @graph_pattern: #rasqal_graph_pattern graph pattern object
 * @idx: index into the sequence of sub graph_patterns in the graph pattern
 *
 * Get a sub-graph pattern inside a graph pattern.
 * 
 * Return value: #rasqal_graph_pattern or NULL if out of range
 **/
rasqal_graph_pattern*
rasqal_graph_pattern_get_sub_graph_pattern(rasqal_graph_pattern* graph_pattern, int idx)
{
  if(!graph_pattern->graph_patterns)
    return NULL;

  return (rasqal_graph_pattern*)raptor_sequence_get_at(graph_pattern->graph_patterns, idx);
}


/*
 * rasqal_graph_pattern_set_origin:
 * @graph_pattern: #rasqal_graph_pattern graph pattern object
 * @origin: #rasqal_literal variable or URI
 *
 * INTERNAL - Set the graph pattern triple origin.
 * 
 * All triples in this graph pattern or contained graph patterns are set
 * to have the given origin.
 **/
void
rasqal_graph_pattern_set_origin(rasqal_graph_pattern* graph_pattern,
                                rasqal_literal *origin)
{
  raptor_sequence* s;
  
  s=graph_pattern->triples;
  if(s) {
    int i;

    /* Flag all the triples in this graph pattern with origin */
    for(i= graph_pattern->start_column; i <= graph_pattern->end_column; i++) {
      rasqal_triple *t=(rasqal_triple*)raptor_sequence_get_at(s, i);
      rasqal_triple_set_origin(t, rasqal_new_literal_from_literal(origin));
    }
  }

  s=graph_pattern->graph_patterns;
  if(s) {
    int i;

    /* Flag all the triples in sub-graph patterns with origin */
    for(i=0; i < raptor_sequence_size(s); i++) {
      rasqal_graph_pattern *gp=(rasqal_graph_pattern*)raptor_sequence_get_at(s, i);
      rasqal_graph_pattern_set_origin(gp, origin);
    }
  }

}


/**
 * rasqal_new_basic_graph_pattern_from_formula:
 * @query: #rasqal_graph_pattern query object
 * @formula: triples sequence containing the graph pattern
 * @op: enum #rasqal_graph_pattern_operator operator
 *
 * INTERNAL - Create a new graph pattern object over a formula. This function
 * frees the formula passed in.
 * 
 * Return value: a new #rasqal_graph_pattern object or NULL on failure
 **/
rasqal_graph_pattern*
rasqal_new_basic_graph_pattern_from_formula(rasqal_query* query,
                                            rasqal_formula* formula)
{
  rasqal_graph_pattern* gp;
  raptor_sequence *triples=query->triples;
  raptor_sequence *formula_triples=formula->triples;
  int offset=raptor_sequence_size(triples);
  int triple_pattern_size=0;

  if(formula_triples) {
    /* Move formula triples to end of main triples sequence */
    triple_pattern_size=raptor_sequence_size(formula_triples);
    if(raptor_sequence_join(triples, formula_triples)) {
      rasqal_free_formula(formula);
      return NULL;
    }
  }

  rasqal_free_formula(formula);

  gp=rasqal_new_basic_graph_pattern(query, triples, 
                                    offset, 
                                    offset+triple_pattern_size-1);
  return gp;
}


/**
 * rasqal_new_2_group_graph_pattern:
 * @query: query object
 * @first_gp: first graph pattern
 * @second_gp: second graph pattern
 *
 * INTERNAL - Make a new group graph pattern from two graph patterns
 * of which either or both may be NULL, in which case a group
 * of 0 graph patterns is created.
 *
 * @first_gp and @second_gp if given, become owned by the new graph
 * pattern.
 *
 * Return value: new group graph pattern or NULL on failure
 */
rasqal_graph_pattern*
rasqal_new_2_group_graph_pattern(rasqal_query* query,
                                 rasqal_graph_pattern* first_gp,
                                 rasqal_graph_pattern* second_gp)
{
  raptor_sequence *seq;

  seq=raptor_new_sequence((raptor_sequence_free_handler*)rasqal_free_graph_pattern, (raptor_sequence_print_handler*)rasqal_graph_pattern_print);
  if(!seq) {
    if(first_gp)
      rasqal_free_graph_pattern(first_gp);
    if(second_gp)
      rasqal_free_graph_pattern(second_gp);
    return NULL;
  }

  if(first_gp && raptor_sequence_push(seq, first_gp)) {
    raptor_free_sequence(seq);
    if(second_gp)
      rasqal_free_graph_pattern(second_gp);
    return NULL;
  }
  if(second_gp && raptor_sequence_push(seq, second_gp)) {
    raptor_free_sequence(seq);
    return NULL;
  }

  return rasqal_new_graph_pattern_from_sequence(query, seq,
                                                RASQAL_GRAPH_PATTERN_OPERATOR_GROUP);
}
