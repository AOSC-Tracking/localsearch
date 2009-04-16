/* -*- Mode: c; c-basic-offset: 2 -*-
 *
 * rasqal_query_results.c - Rasqal RDF Query Results
 *
 * Copyright (C) 2003-2008, David Beckett http://www.dajobe.org/
 * Copyright (C) 2003-2005, University of Bristol, UK http://www.bristol.ac.uk/
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


/**
 *
 * Query Results Class Internals
 *
 * This class provides the abstraction for query results in different
 * forms.  The forms can be either a sequence of variable bindings,
 * set of RDF triples, boolean value or a syntax.
 *
 * Query results can be created as a result of a #rasqal_query
 * execution using rasqal_query_execute() or as an independent result
 * set constructed from a query results syntax such as the SPARQL XML
 * results format via the #rasqal_query_results_formatter class.
 *
 * The query results constructor rasqal_new_query_results() takes
 * a world to use, an optional query, the type of result as well
 * as a variable table to operate on.  If the query is given, then
 * that is used to handle limit, offset and triple construction,
 * otherwise the result set is standalone and not associated with
 * a query.
 *
 * The variables table is used for the variables that will appear in
 * the result rows in the result set.  The query results module does
 * not own any variable information, all API calls are delegated to
 * the variables table.
 * 
 * If the rasqal_new_query_results_from_query_execution() is used to
 * make a query results from a query structure via executing the
 * query, it initialises a execution engine via the
 * #rasqal_query_execution_factory 'execute_init' factory method.
 * This method also determines whether the entire results need to be
 * (or a requested to be) obtained in one go, and if so, they are
 * done during construction.
 *
 * The user API to getting query results is primarily to get variable
 * bindings - a sequence of variable:value (also called #rasqal_row
 * internally), RDF triples, a boolean value or a syntax.  
 *
 * The variable bindings are generated from the execution engine by
 * retrieving #rasqal_row either one-by-one using the get_row method
 * or getting the entire result at once with the get_all_rows method.
 *
 * In the case of getting the entire result the rows are stored as a
 * sqeuence inside the #rasqal_query_results and returned one-by-one
 * from there, respecting any limit and offset.
 *
 * The RDF triples and boolean value results are generated from the
 * variable bindings (#rasqal_row) inside this class.  The underlying
 * execution engine only knows about rows.
 *
 * The class also handles several other results-specific methods such
 * as getting variable binding names, values by name, counts of
 * number of results, writing a query results as a syntax (in a
 * simple fashion), read a query results from a syntax.
 */

static int rasqal_query_results_execute_and_store_results(rasqal_query_results* query_results);
static void rasqal_query_results_update_bindings(rasqal_query_results* query_results);


/*
 * A query result for some query
 */
struct rasqal_query_results_s {
  rasqal_world* world;

  /* type of query result (bindings, boolean, graph or syntax) */
  rasqal_query_results_type type;
  
  /* non-0 if have read all (variable binding) results */
  int finished;

  /* non-0 if query has been executed */
  int executed;

  /* non 0 if query had fatal error and cannot return results */
  int failed;

  /* query that this was executed over */
  rasqal_query* query;

  /* how many (variable bindings) results found so far */
  int result_count;

  /* execution data for execution engine. owned by this object */
  void* execution_data;

  /* current row of results */
  rasqal_row* row;

  /* boolean ASK result >0 true, 0 false or -1 uninitialised */
  int ask_result;

  /* boolean: non-0 to store query results rather than lazy eval */
  int store_results;

  /* current triple in the sequence of triples 'constructs' or -1 */
  int current_triple_result;

  /* constructed triple result - shared and updated for each triple */
  raptor_statement result_triple;

  /* triple used to store references to literals for triple subject,
   * predicate, object.  never returned or used otherwise.
   */
  rasqal_triple* triple;
  
  /* sequence of stored results */
  raptor_sequence* results_sequence;

  /* size of result row fields:
   * row->results, row->values
   */
  int size;

  /* Execution engine used here */
  const rasqal_query_execution_factory* execution_factory;

  /* Variables table for variables in result rows */
  rasqal_variables_table* vars_table;
};
    

int
rasqal_init_query_results(void)
{
  return 0;
}


void
rasqal_finish_query_results(void)
{
}


/**
 * rasqal_new_query_results:
 * @world: rasqal world object
 * @query: query object (or NULL)
 * @type: query results (expected) type
 * @vars_table: variables table
 * 
 * INTERNAL -  create a query result set
 *
 * The @query may be NULL for result set objects that are standalone
 * and not attached to any particular query
 *
 * Return value: a new query result object or NULL on failure
 **/
rasqal_query_results*  
rasqal_new_query_results(rasqal_world* world,
                         rasqal_query* query,
                         rasqal_query_results_type type,
                         rasqal_variables_table* vars_table)
{
  rasqal_query_results* query_results;
    
  query_results = (rasqal_query_results*)RASQAL_CALLOC(rasqal_query_results, 1, sizeof(rasqal_query_results));
  if(!query_results)
    return NULL;
  
  query_results->world = world;
  query_results->type = type;
  query_results->finished = 0;
  query_results->executed = 0;
  query_results->failed = 0;
  query_results->query = query;
  query_results->result_count = 0;
  query_results->execution_data = NULL;
  query_results->row = NULL;
  query_results->ask_result = -1; 
  query_results->store_results = 0; 
  query_results->current_triple_result = -1;
  /* query_results->result_triple is static */
  query_results->triple = NULL;
  query_results->results_sequence = NULL;
  query_results->size = 0;
  query_results->vars_table = rasqal_new_variables_table_from_variables_table(vars_table);

  return query_results;
}


/**
 * rasqal_query_results_execute_with_engine:
 * @query: the #rasqal_query object
 * @engine: execution factory
 *
 * INTERNAL - Create a new query results set executing a prepared query with the given execution engine
 *
 * return value: a #rasqal_query_results structure or NULL on failure.
 **/
rasqal_query_results*
rasqal_query_results_execute_with_engine(rasqal_query* query,
                                         const rasqal_query_execution_factory* engine)
{
  rasqal_query_results *query_results = NULL;
  int rc = 0;
  size_t ex_data_size;
  rasqal_query_results_type type = RASQAL_QUERY_RESULTS_BINDINGS;

  if(!query)
    return NULL;
  
  if(query->failed)
    return NULL;

  if(query->query_results_formatter_name)
    type = RASQAL_QUERY_RESULTS_SYNTAX;
  else
    switch(query->verb) {
      case RASQAL_QUERY_VERB_SELECT:
        type = RASQAL_QUERY_RESULTS_BINDINGS;
        break;
      case RASQAL_QUERY_VERB_ASK:
        type = RASQAL_QUERY_RESULTS_BOOLEAN;
        break;
      case RASQAL_QUERY_VERB_CONSTRUCT:
      case RASQAL_QUERY_VERB_DESCRIBE:
        type = RASQAL_QUERY_RESULTS_GRAPH;
        break;
        
      case RASQAL_QUERY_VERB_UNKNOWN:
      case RASQAL_QUERY_VERB_DELETE:
      case RASQAL_QUERY_VERB_INSERT:
      default:
        return NULL;
    }
  
  query_results = rasqal_new_query_results(query->world, query, type,
                                           query->vars_table);
  if(!query_results)
    return NULL;

  query_results->execution_factory = engine;
  
  /* set executed flag early to enable cleanup on error */
  query_results->executed = 1;

  query_results->store_results = (query->store_results || 
                                  query->order_conditions_sequence ||
                                  query->distinct);
  
  ex_data_size = query_results->execution_factory->execution_data_size;
  if(ex_data_size > 0) {
    query_results->execution_data = RASQAL_CALLOC(data, 1, ex_data_size);
    if(!query_results->execution_data) {
      rasqal_free_query_results(query_results);
      return NULL;
    }
  } else
    query_results->execution_data = NULL;

  if(query_results->execution_factory->execute_init) {
    rasqal_engine_error execution_error = RASQAL_ENGINE_OK;
    int execution_flags = 0;
    if(query_results->store_results)
      execution_flags |= 1;

    rc = query_results->execution_factory->execute_init(query_results->execution_data, query, query_results, execution_flags, &execution_error);
    if(execution_error != RASQAL_ENGINE_OK) {
      query_results->failed = 1;
      rasqal_free_query_results(query_results);
      return NULL;
    }
  }

  /* Choose either to execute all now and store OR do it on demand (lazy) */
  if(query_results->store_results)
    rc = rasqal_query_results_execute_and_store_results(query_results);

  return query_results;
}


/**
 * rasqal_free_query_results:
 * @query_results: #rasqal_query_results object
 *
 * Destructor - destroy a rasqal_query_results.
 *
 **/
void
rasqal_free_query_results(rasqal_query_results* query_results)
{
  rasqal_query* query;

  RASQAL_ASSERT_OBJECT_POINTER_RETURN(query_results, rasqal_query_result);

  query = query_results->query;

  if(query_results->executed) {
    if(query_results->execution_factory->execute_finish) {
      rasqal_engine_error execution_error = RASQAL_ENGINE_OK;

      query_results->execution_factory->execute_finish(query_results->execution_data, &execution_error);
      /* ignoring failure of execute_finish */
    }
  }

  if(query_results->execution_data)
    RASQAL_FREE(rasqal_engine_execution_data, query_results->execution_data);

  if(query_results->row)
    rasqal_free_row(query_results->row);

  if(query_results->results_sequence)
    raptor_free_sequence(query_results->results_sequence);

  if(query_results->triple)
    rasqal_free_triple(query_results->triple);

  if(query_results->vars_table)
    rasqal_free_variables_table(query_results->vars_table);

  if(query)
    rasqal_query_remove_query_result(query, query_results);

  RASQAL_FREE(rasqal_query_results, query_results);
}


/**
 * rasqal_query_results_get_query:
 * @query_results: #rasqal_query_results object
 *
 * Get thq query associated with this query result
 * 
 * Return value: shared pointer to query object
 **/
rasqal_query*
rasqal_query_results_get_query(rasqal_query_results* query_results)
{
  return query_results->query;
}


/**
 * rasqal_query_results_is_bindings:
 * @query_results: #rasqal_query_results object
 *
 * Test if rasqal_query_results is variable bindings format.
 * 
 * Return value: non-0 if true
 **/
int
rasqal_query_results_is_bindings(rasqal_query_results* query_results)
{
  return (query_results->type == RASQAL_QUERY_RESULTS_BINDINGS);
}


/**
 * rasqal_query_results_is_boolean:
 * @query_results: #rasqal_query_results object
 *
 * Test if rasqal_query_results is boolean format.
 * 
 * Return value: non-0 if true
 **/
int
rasqal_query_results_is_boolean(rasqal_query_results* query_results)
{
  return (query_results->type == RASQAL_QUERY_RESULTS_BOOLEAN);
}
 

/**
 * rasqal_query_results_is_graph:
 * @query_results: #rasqal_query_results object
 *
 * Test if rasqal_query_results is RDF graph format.
 * 
 * Return value: non-0 if true
 **/
int
rasqal_query_results_is_graph(rasqal_query_results* query_results)
{
  return (query_results->type == RASQAL_QUERY_RESULTS_GRAPH);
}


/**
 * rasqal_query_results_is_syntax:
 * @query_results: #rasqal_query_results object
 *
 * Test if the rasqal_query_results is a syntax.
 *
 * Many of the query results may be formatted as a syntax using the
 * #rasqal_query_formatter class however this function returns true
 * if a syntax result was specifically requested.
 * 
 * Return value: non-0 if true
 **/
int
rasqal_query_results_is_syntax(rasqal_query_results* query_results)
{
  return (query_results->type == RASQAL_QUERY_RESULTS_SYNTAX);
}


/**
 * rasqal_query_results_check_limit_offset:
 * @query_results: query results object
 *
 * INTERNAL - Check the query result count is in the limit and offset range if any.
 *
 * Return value: before range -1, in range 0, after range 1
 */
int
rasqal_query_results_check_limit_offset(rasqal_query_results* query_results)
{
  rasqal_query* query = query_results->query;
  int limit;

  if(!query)
    return 0;

  limit = query->limit;

  /* Ensure ASK queries never do more than one result */
  if(query->verb == RASQAL_QUERY_VERB_ASK)
    limit = 1;

  if(query->offset > 0) {
    /* offset */
    if(query_results->result_count <= query->offset)
      return -1;
    
    if(limit >= 0) {
      /* offset and limit */
      if(query_results->result_count > (query->offset + limit)) {
        query_results->finished = 1;
      }
    }
    
  } else if(limit >= 0) {
    /* limit */
    if(query_results->result_count > limit) {
      query_results->finished = 1;
    }
  }

  return query_results->finished;
}


/**
 * rasqal_query_results_get_row_from_saved:
 * @query_results: Query results to execute
 *
 * INTERNAL - Get next result row from a stored query result sequence
 *
 * Return value: result row or NULL if finished or failed
 */
static rasqal_row*
rasqal_query_results_get_row_from_saved(rasqal_query_results* query_results)
{
  rasqal_query* query = query_results->query;
  int size;
  rasqal_row* row = NULL;
  
  size = raptor_sequence_size(query_results->results_sequence);
  
  while(1) {
    if(query_results->result_count >= size) {
      query_results->finished = 1;
      break;
    }
    
    query_results->result_count++;
    
    /* finished if beyond result range */
    if(rasqal_query_results_check_limit_offset(query_results) > 0) {
      query_results->result_count--;
      break;
    }
    
    /* continue if before start of result range */
    if(rasqal_query_results_check_limit_offset(query_results) < 0)
      continue;
    
    /* else got result or finished */
    row = (rasqal_row*)raptor_sequence_delete_at(query_results->results_sequence,
                                                 query_results->result_count-1);
    
    if(row) {
      /* stored results may not be canonicalized yet - do it lazily */
      rasqal_row_to_nodes(row);

      if(query && query->constructs)
        rasqal_query_results_update_bindings(query_results);
    }
    break;
  }
  
  return row;
}


/**
 * rasqal_query_results_ensure_have_row_internal:
 * @query_results: #rasqal_query_results query_results
 *
 * INTERNAL - Ensure there is a row in the query results by getting it from the generator/stored list
 *
 * If one already is held, nothing is done.  It is assumed
 * that @query_results is not NULL and the query is neither finished
 * nor failed.
 *
 * Return value: non-0 if failed or results exhausted
 **/
static int
rasqal_query_results_ensure_have_row_internal(rasqal_query_results* query_results)
{
  /* already have row */
  if(query_results->row)
    return 0;
  
  if(query_results->results_sequence) {
    query_results->row = rasqal_query_results_get_row_from_saved(query_results);
  } else if(query_results->execution_factory &&
            query_results->execution_factory->get_row) {
    rasqal_engine_error execution_error = RASQAL_ENGINE_OK;

    query_results->row = query_results->execution_factory->get_row(query_results->execution_data, &execution_error);
    if(execution_error == RASQAL_ENGINE_FAILED)
      query_results->failed = 1;
    else if(execution_error == RASQAL_ENGINE_OK)
      query_results->result_count++;
  }
  
  if(query_results->row) {
    rasqal_row_to_nodes(query_results->row);
    query_results->size = query_results->row->size;
  } else
    query_results->finished = 1;

  return (query_results->row == NULL);
}


/**
 * rasqal_query_results_get_current_row:
 * @query_results: query results object
 *
 * INTERNAL - Get the current query result as a row of values
 *
 * The returned row is shared and owned by query_results
 *
 * Return value: result row or NULL on failure
 */
static rasqal_row*
rasqal_query_results_get_current_row(rasqal_query_results* query_results)
{
  if(!query_results || query_results->failed || query_results->finished)
    return NULL;
  
  if(!rasqal_query_results_is_bindings(query_results))
    return NULL;

  /* ensure we have a row */
  rasqal_query_results_ensure_have_row_internal(query_results);

  return query_results->row;
}


/**
 * rasqal_query_results_get_count:
 * @query_results: #rasqal_query_results query_results
 *
 * Get number of bindings so far.
 * 
 * Return value: number of bindings found so far or < 0 on failure
 **/
int
rasqal_query_results_get_count(rasqal_query_results* query_results)
{
  rasqal_query* query;

  if(!query_results || query_results->failed)
    return -1;

  if(!rasqal_query_results_is_bindings(query_results))
    return -1;
  
  query = query_results->query;
  if(query && query->offset > 0)
    return query_results->result_count - query->offset;
  return query_results->result_count;
}


/**
 * rasqal_query_results_next:
 * @query_results: #rasqal_query_results query_results
 *
 * Move to the next result.
 * 
 * Return value: non-0 if failed or results exhausted
 **/
int
rasqal_query_results_next(rasqal_query_results* query_results)
{
  if(!query_results || query_results->failed || query_results->finished)
    return 1;
  
  if(!rasqal_query_results_is_bindings(query_results))
    return 1;

  /* Remove any current row */
  if(query_results->row) {
    rasqal_free_row(query_results->row);
    query_results->row = NULL;
  }

  /* Now try to get a new one */
  return rasqal_query_results_ensure_have_row_internal(query_results);
}


/**
 * rasqal_query_results_finished:
 * @query_results: #rasqal_query_results query_results
 *
 * Find out if binding results are exhausted.
 * 
 * Return value: non-0 if results are finished or query failed
 **/
int
rasqal_query_results_finished(rasqal_query_results* query_results)
{
  if(!query_results || query_results->failed || query_results->finished)
    return 1;
  
  if(!rasqal_query_results_is_bindings(query_results))
    return 1;

  /* need to have at least tried to get a row once */
  if(!query_results->failed && !query_results->finished)
    rasqal_query_results_ensure_have_row_internal(query_results);
  
  return (query_results->failed || query_results->finished);
}


/**
 * rasqal_query_results_get_bindings:
 * @query_results: #rasqal_query_results query_results
 * @names: pointer to an array of binding names (or NULL)
 * @values: pointer to an array of binding value #rasqal_literal (or NULL)
 *
 * Get all binding names, values for current result.
 * 
 * If names is not NULL, it is set to the address of a shared array
 * of names of the bindings (an output parameter).  These names
 * are shared and must not be freed by the caller
 *
 * If values is not NULL, it is set to the address of a shared array
 * of #rasqal_literal* binding values.  These values are shaerd
 * and must not be freed by the caller.
 * 
 * Return value: non-0 if the assignment failed
 **/
int
rasqal_query_results_get_bindings(rasqal_query_results* query_results,
                                  const unsigned char ***names, 
                                  rasqal_literal ***values)
{
  if(!query_results)
    return 1;
  
  if(!rasqal_query_results_is_bindings(query_results))
    return 1;
  
  if(names)
    *names = rasqal_variables_table_get_names(query_results->vars_table);
  
  if(values) {
    rasqal_row* row;

    row = rasqal_query_results_get_current_row(query_results);
    if(row)
      *values = row->values;
    else
      query_results->finished = 1;
  }
    
  return 0;
}


/**
 * rasqal_query_results_get_binding_value:
 * @query_results: #rasqal_query_results query_results
 * @offset: offset of binding name into array of known names
 *
 * Get one binding value for the current result.
 * 
 * Return value: a pointer to a shared #rasqal_literal binding value or NULL on failure
 **/
rasqal_literal*
rasqal_query_results_get_binding_value(rasqal_query_results* query_results, 
                                       int offset)
{
  rasqal_row* row;
  
  if(!query_results)
    return NULL;
  
  if(!rasqal_query_results_is_bindings(query_results))
    return NULL;
  
  if(offset < 0 || offset > query_results->size-1)
    return NULL;

  row = rasqal_query_results_get_current_row(query_results);
  if(row)
    return row->values[offset];

  query_results->finished = 1;
  return NULL;
}


/**
 * rasqal_query_results_get_binding_name:
 * @query_results: #rasqal_query_results query_results
 * @offset: offset of binding name into array of known names
 *
 * Get binding name for the current result.
 * 
 * Return value: a pointer to a shared copy of the binding name or NULL on failure
 **/
const unsigned char*
rasqal_query_results_get_binding_name(rasqal_query_results* query_results, 
                                      int offset)
{
  rasqal_variable* v;

  if(!query_results)
    return NULL;
  
  if(!rasqal_query_results_is_bindings(query_results)) 
    return NULL;
  
  v = rasqal_variables_table_get(query_results->vars_table, offset);
  if(!v)
    return NULL;
  
  return v->name;
}


/**
 * rasqal_query_results_get_binding_value_by_name:
 * @query_results: #rasqal_query_results query_results
 * @name: variable name
 *
 * Get one binding value for a given name in the current result.
 * 
 * Return value: a pointer to a shared #rasqal_literal binding value or NULL on failure
 **/
rasqal_literal*
rasqal_query_results_get_binding_value_by_name(rasqal_query_results* query_results,
                                               const unsigned char *name)
{
  rasqal_row* row;
  rasqal_variable* v;

  if(!query_results)
    return NULL;
  
  if(!rasqal_query_results_is_bindings(query_results))
    return NULL;
  
  row = rasqal_query_results_get_current_row(query_results);
  if(!row)
    return NULL;
  
  v = rasqal_variables_table_get_by_name(query_results->vars_table, name);
  if(!v)
    return NULL;

  return row->values[v->offset];
}


/**
 * rasqal_query_results_get_bindings_count:
 * @query_results: #rasqal_query_results query_results
 *
 * Get the number of bound variables in the result.
 * 
 * Return value: <0 if failed or results exhausted
 **/
int
rasqal_query_results_get_bindings_count(rasqal_query_results* query_results)
{
  if(!query_results || query_results->failed)
    return -1;
  
  if(!rasqal_query_results_is_bindings(query_results))
    return -1;
  
  return query_results->size;
}


static unsigned char*
rasqal_prefix_id(int prefix_id, unsigned char *string)
{
  int tmpid = prefix_id;
  unsigned char* buffer;
  size_t length = strlen((const char*)string)+4;  /* "r" +... + "_" +... \0 */

  while(tmpid /= 10)
    length++;
  
  buffer = (unsigned char*)RASQAL_MALLOC(cstring, length);
  if(!buffer)
    return NULL;
  
  sprintf((char*)buffer, "r%d_%s", prefix_id, string);
  
  return buffer;
}


/**
 * rasqal_query_results_get_triple:
 * @query_results: #rasqal_query_results query_results
 *
 * Get the current triple in the result.
 *
 * The return value is a shared #raptor_statement.
 * 
 * Return value: #raptor_statement or NULL if failed or results exhausted
 **/
raptor_statement*
rasqal_query_results_get_triple(rasqal_query_results* query_results)
{
  rasqal_query* query;
  int rc;
  rasqal_triple *t;
  rasqal_literal *s, *p, *o;
  raptor_statement *rs = NULL;
  unsigned char *nodeid;
  int skipped;
  
 if(!query_results || query_results->failed || query_results->finished)
    return NULL;
  
  if(!rasqal_query_results_is_graph(query_results))
    return NULL;
  
  query = query_results->query;
  if(!query)
    return NULL;
  
  if(query->verb == RASQAL_QUERY_VERB_DESCRIBE)
    return NULL;

 
  /* ensure we have a row to work on */
  if(rasqal_query_results_ensure_have_row_internal(query_results))
    return NULL;

  skipped = 0;
  while(1) {
    if(skipped) {
      rc = rasqal_query_results_next(query_results);
      if(rc) {
        rs = NULL;
        break;
      }
      query_results->current_triple_result = -1;
    }
    
    if(query_results->current_triple_result < 0)
      query_results->current_triple_result = 0;

    t = (rasqal_triple*)raptor_sequence_get_at(query->constructs,
                                               query_results->current_triple_result);

    rs = &query_results->result_triple;

    s = rasqal_literal_as_node(t->subject);
    if(!s) {
      rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_WARNING,
                              &query->locator,
                              "Triple with unbound subject skipped");
      skipped = 1;
      continue;
    }
    switch(s->type) {
      case RASQAL_LITERAL_URI:
        rs->subject = s->value.uri;
        rs->subject_type = RAPTOR_IDENTIFIER_TYPE_RESOURCE;
        break;

      case RASQAL_LITERAL_BLANK:
        nodeid = rasqal_prefix_id(query_results->result_count,
                                  (unsigned char*)s->string);
        rasqal_free_literal(s);
        if(!nodeid) {
          rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_FATAL,
                                  &query->locator,
                                  "Could not prefix subject blank identifier");
          return NULL;
        }
        s = rasqal_new_simple_literal(query_results->world, RASQAL_LITERAL_BLANK,
                                      nodeid);
        if(!s) {
          rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_FATAL,
                                  &query->locator,
                                  "Could not create a new subject blank literal");
          return NULL;
        }
        rs->subject = nodeid;
        rs->subject_type = RAPTOR_IDENTIFIER_TYPE_ANONYMOUS;
        break;

      case RASQAL_LITERAL_QNAME:
      case RASQAL_LITERAL_PATTERN:
      case RASQAL_LITERAL_BOOLEAN:
      case RASQAL_LITERAL_INTEGER:
      case RASQAL_LITERAL_DOUBLE:
      case RASQAL_LITERAL_FLOAT:
      case RASQAL_LITERAL_VARIABLE:
      case RASQAL_LITERAL_DECIMAL:
      case RASQAL_LITERAL_DATETIME:
        /* QNames should be gone by the time expression eval happens
         * Everything else is removed by rasqal_literal_as_node() above. 
         */

      case RASQAL_LITERAL_STRING:
        /* string [literal] subjects are not RDF */

      case RASQAL_LITERAL_UNKNOWN:
      default:
        /* case RASQAL_LITERAL_STRING: */
        rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_WARNING,
                                &query->locator,
                                "Triple with non-URI/blank node subject skipped");
        skipped = 1;
        break;
    }
    if(skipped) {
      if(s)
        rasqal_free_literal(s);
      continue;
    }
    

    p = rasqal_literal_as_node(t->predicate);
    if(!p) {
      rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_WARNING,
                              &query->locator,
                              "Triple with unbound predicate skipped");
      rasqal_free_literal(s);
      skipped = 1;
      continue;
    }
    switch(p->type) {
      case RASQAL_LITERAL_URI:
        rs->predicate = p->value.uri;
        rs->predicate_type = RAPTOR_IDENTIFIER_TYPE_RESOURCE;
        break;

      case RASQAL_LITERAL_QNAME:
      case RASQAL_LITERAL_PATTERN:
      case RASQAL_LITERAL_BOOLEAN:
      case RASQAL_LITERAL_INTEGER:
      case RASQAL_LITERAL_DOUBLE:
      case RASQAL_LITERAL_FLOAT:
      case RASQAL_LITERAL_VARIABLE:
      case RASQAL_LITERAL_DECIMAL:
      case RASQAL_LITERAL_DATETIME:
        /* QNames should be gone by the time expression eval happens
         * Everything else is removed by rasqal_literal_as_node() above. 
         */

      case RASQAL_LITERAL_BLANK:
      case RASQAL_LITERAL_STRING:
        /* blank node or string [literal] predicates are not RDF */

      case RASQAL_LITERAL_UNKNOWN:
      default:
        rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_WARNING,
                                &query->locator,
                                "Triple with non-URI predicate skipped");
        skipped = 1;
        break;
    }
    if(skipped) {
      rasqal_free_literal(s);
      if(p)
        rasqal_free_literal(p);
      continue;
    }

    o = rasqal_literal_as_node(t->object);
    if(!o) {
      rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_WARNING,
                              &query->locator,
                              "Triple with unbound object skipped");
      rasqal_free_literal(s);
      rasqal_free_literal(p);
      skipped = 1;
      continue;
    }
    switch(o->type) {
      case RASQAL_LITERAL_URI:
        rs->object = o->value.uri;
        rs->object_type = RAPTOR_IDENTIFIER_TYPE_RESOURCE;
        break;

      case RASQAL_LITERAL_BLANK:
        nodeid = rasqal_prefix_id(query_results->result_count,
                                  (unsigned char*)o->string);
        rasqal_free_literal(o);
        if(!nodeid) {
          rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_FATAL,
                                  &query->locator,
                                  "Could not prefix blank identifier");
          rasqal_free_literal(s);
          rasqal_free_literal(p);
          return NULL;
        }
        o = rasqal_new_simple_literal(query_results->world, RASQAL_LITERAL_BLANK,
                                      nodeid);
        if(!o) {
          rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_FATAL,
                                  &query->locator,
                                  "Could not create a new subject blank literal");
          rasqal_free_literal(s);
          rasqal_free_literal(p);
          return NULL;
        }
        rs->object = nodeid;
        rs->object_type = RAPTOR_IDENTIFIER_TYPE_ANONYMOUS;
        break;

      case RASQAL_LITERAL_STRING:
        rs->object = o->string;
        rs->object_literal_language = (const unsigned char*)o->language;
        rs->object_literal_datatype = o->datatype;
        rs->object_type = RAPTOR_IDENTIFIER_TYPE_LITERAL;
        break;

      case RASQAL_LITERAL_QNAME:
      case RASQAL_LITERAL_PATTERN:
      case RASQAL_LITERAL_BOOLEAN:
      case RASQAL_LITERAL_INTEGER:
      case RASQAL_LITERAL_DOUBLE:
      case RASQAL_LITERAL_FLOAT:
      case RASQAL_LITERAL_VARIABLE:
      case RASQAL_LITERAL_DECIMAL:
      case RASQAL_LITERAL_DATETIME:
        /* QNames should be gone by the time expression eval happens
         * Everything else is removed by rasqal_literal_as_node() above. 
         */

      case RASQAL_LITERAL_UNKNOWN:
      default:
        rasqal_log_error_simple(query_results->world, RAPTOR_LOG_LEVEL_WARNING,
                                &query->locator,
                                "Triple with unknown object skipped");
        skipped = 1;
        break;
    }
    if(skipped) {
      rasqal_free_literal(s);
      rasqal_free_literal(p);
      if(o)
        rasqal_free_literal(o);
      continue;
    }
    
    /* dispose previous triple if any */
    if(query_results->triple) {
      rasqal_free_triple(query_results->triple);
      query_results->triple = NULL;
    }

    /* for saving s, p, o for later disposal */
    query_results->triple = rasqal_new_triple(s, p, o);

    /* got triple, return it */
    break;
  }
  
  return rs;
}


/**
 * rasqal_query_results_next_triple:
 * @query_results: #rasqal_query_results query_results
 *
 * Move to the next triple result.
 * 
 * Return value: non-0 if failed or results exhausted
 **/
int
rasqal_query_results_next_triple(rasqal_query_results* query_results)
{
  rasqal_query* query;
  int rc = 0;
  
  if(!query_results || query_results->failed || query_results->finished)
    return 1;
  
  if(!rasqal_query_results_is_graph(query_results))
    return 1;
  
  query = query_results->query;
  if(!query)
    return 1;

  if(query->verb == RASQAL_QUERY_VERB_DESCRIBE)
    return 1;
  
  if(query_results->triple) {
    rasqal_free_triple(query_results->triple);
    query_results->triple = NULL;
  }

  if(++query_results->current_triple_result >= raptor_sequence_size(query->constructs)) {
    /* Remove any current row */
    if(query_results->row) {
      rasqal_free_row(query_results->row);
      query_results->row = NULL;
    }
    
    /* Now try to get a new one */
    if(rasqal_query_results_ensure_have_row_internal(query_results))
      return 1;
    
    query_results->current_triple_result = -1;
  }

  return rc;
}


/**
 * rasqal_query_results_get_boolean:
 * @query_results: #rasqal_query_results query_results
 *
 * Get boolean query result.
 *
 * The return value is only meaningful if this is a boolean
 * query result - see rasqal_query_results_is_boolean()
 *
 * Return value: boolean query result - >0 is true, 0 is false, <0 on error
 */
int
rasqal_query_results_get_boolean(rasqal_query_results* query_results)
{
  if(!query_results || query_results->failed)
    return -1;
  
  if(!rasqal_query_results_is_boolean(query_results))
    return -1;
  
  if(query_results->ask_result >= 0)
    return query_results->ask_result;

  query_results->ask_result= (query_results->result_count > 0) ? 1 : 0;
  query_results->finished= 1;
  
  return query_results->ask_result;
}


/**
 * rasqal_query_results_write:
 * @iostr: #raptor_iostream to write the query to
 * @results: #rasqal_query_results query results format
 * @format_uri: #raptor_uri describing the format to write (or NULL for default)
 * @base_uri: #raptor_uri base URI of the output format
 *
 * Write the query results to an iostream in a format.
 * 
 * This uses the #rasqal_query_results_formatter class
 * and the rasqal_query_results_formatter_write() method
 * to perform the formatting. See
 * rasqal_query_results_formats_enumerate() 
 * for obtaining the supported format URIs at run time.
 *
 * Return value: non-0 on failure
 **/
int
rasqal_query_results_write(raptor_iostream *iostr,
                           rasqal_query_results* results,
                           raptor_uri *format_uri,
                           raptor_uri *base_uri)
{
  rasqal_query_results_formatter *formatter;
  int status;
  
  if(!results || results->failed)
    return 1;

  formatter = rasqal_new_query_results_formatter(results->world, NULL,
                                                 format_uri);
  if(!formatter)
    return 1;

  status = rasqal_query_results_formatter_write(iostr, formatter,
                                                results, base_uri);

  rasqal_free_query_results_formatter(formatter);
  return status;
}


/**
 * rasqal_query_results_read:
 * @iostr: #raptor_iostream to read the query from
 * @results: #rasqal_query_results query results format
 * @format_uri: #raptor_uri describing the format to read (or NULL for default)
 * @base_uri: #raptor_uri base URI of the input format
 *
 * Read the query results from an iostream in a format.
 * 
 * This uses the #rasqal_query_results_formatter class
 * and the rasqal_query_results_formatter_read() method
 * to perform the formatting. See
 * rasqal_query_results_formats_enumerate() 
 * for obtaining the supported format URIs at run time.
 *
 * Return value: non-0 on failure
 **/
int
rasqal_query_results_read(raptor_iostream *iostr,
                          rasqal_query_results* results,
                          raptor_uri *format_uri,
                          raptor_uri *base_uri)
{
  rasqal_query_results_formatter *formatter;
  int status;
  
  if(!results || results->failed)
    return 1;

  formatter = rasqal_new_query_results_formatter(results->world, NULL,
                                                 format_uri);
  if(!formatter)
    return 1;

  status = rasqal_query_results_formatter_read(results->world, iostr, formatter,
                                               results, base_uri);

  rasqal_free_query_results_formatter(formatter);
  return status;
}


/**
 * rasqal_query_results_add_row:
 * @query_results: query results object
 * @row: query result row
 *
 * INTERNAL - Add a query result row to the sequence of result rows
 */
void
rasqal_query_results_add_row(rasqal_query_results* query_results,
                             rasqal_row* row)
{
  if(!query_results->results_sequence) {
    query_results->results_sequence = raptor_new_sequence((raptor_sequence_free_handler*)rasqal_free_row, (raptor_sequence_print_handler*)rasqal_row_print);
    query_results->result_count = 1;
  }

  row->offset = query_results->result_count-1;
  raptor_sequence_push(query_results->results_sequence, row);
}


/**
 * rasqal_query_results_execute_and_store_results:
 * @query_results: query results object
 *
 * INTERNAL - Store all query result (rows) immediately
 *
 * Return value: non-0 on finished or failure
 */
static int
rasqal_query_results_execute_and_store_results(rasqal_query_results* query_results)
{
  rasqal_query* query;
  raptor_sequence* seq = NULL;

  query = query_results->query;

  if(query_results->results_sequence)
     raptor_free_sequence(query_results->results_sequence);

  if(query_results->execution_factory->get_all_rows) {
    rasqal_engine_error execution_error = RASQAL_ENGINE_OK;

    seq = query_results->execution_factory->get_all_rows(query_results->execution_data, &execution_error);
    if(execution_error == RASQAL_ENGINE_FAILED)
      query_results->failed = 1;
  }

  query_results->results_sequence = seq;

  if(!seq) {
    query_results->finished = 1;
  } else {
    int size;

    size = raptor_sequence_size(seq);
    query_results->finished = (size == 0);
    
    if(query && !query->limit)
      query_results->finished = 1;
    
    if(!query_results->finished) {
      /* Reset to first result, index-1 into sequence of results */
      query_results->result_count = 0;
      
      /* skip past any OFFSET */
      if(query && query->offset > 0) {
        query_results->result_count += query->offset;
        if(query_results->result_count >= size)
          query_results->finished = 1;
      }
      
    }
    
    if(query_results->finished)
      query_results->result_count = 0;
    else {
      if(query && query->constructs)
        rasqal_query_results_update_bindings(query_results);
    }
  }

  return query_results->finished;
}


static void
rasqal_query_results_update_bindings(rasqal_query_results* query_results)
{
  int i;
  int size;

  /* bind the construct variables again if running through a sequence */
  size = rasqal_variables_table_get_named_variables_count(query_results->vars_table);
  for(i = 0; i< size; i++) {
    rasqal_variable* v;
    rasqal_literal* value;
    v = rasqal_variables_table_get(query_results->vars_table, i);
    value = rasqal_query_results_get_binding_value(query_results, i);
    rasqal_variable_set_value(v, rasqal_new_literal_from_literal(value));
  }
}


void
rasqal_query_results_remove_query_reference(rasqal_query_results* query_results)
{
  rasqal_query* query = query_results->query;
  query_results->query = NULL;

  rasqal_free_query(query);
}


rasqal_variables_table*
rasqal_query_results_get_variables_table(rasqal_query_results* query_results)
{
  return query_results->vars_table;
}

