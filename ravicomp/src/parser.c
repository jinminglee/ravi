/******************************************************************************
 * Copyright (C) 2020-2021 Dibyendu Majumdar
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ******************************************************************************/
/* Portions Copyright (C) 1994-2019 Lua.org, PUC-Rio.*/

/*
A parser and syntax tree builder for Ravi.
Note that the overall structure of the parser is loosely based on the Lua 5.3 parser.

The parser retains the syntactic structure - including constant expressions and some redundant
syntax nodes because these are useful for testing and understanding.

A later pass simplifies the AST - see ast_simplify.c
*/

#include "fnv_hash.h"
#include <parser.h>

/* forward declarations */
static AstNode *parse_expression(struct parser_state *);
static void parse_statement_list(struct parser_state *, AstNodeList **list);
static AstNode *parse_statement(struct parser_state *);
static AstNode *new_function(struct parser_state *parser);
static AstNode *end_function(struct parser_state *parser);
static Scope *new_scope(struct parser_state *parser);
static void end_scope(struct parser_state *parser);
static AstNode *new_literal_expression(struct parser_state *parser, ravitype_t type);
static AstNode *generate_label(struct parser_state *parser, const StringObject *label);
static void add_local_symbol_to_current_scope(struct parser_state *parser, LuaSymbol *sym);

static void add_symbol(CompilerState *container, LuaSymbolList **list, LuaSymbol *sym)
{
	raviX_ptrlist_add((struct ptr_list **)list, sym, &container->ptrlist_allocator);
}

static void add_ast_node(CompilerState *container, AstNodeList **list, AstNode *node)
{
	raviX_ptrlist_add((struct ptr_list **)list, node, &container->ptrlist_allocator);
}

static AstNode *allocate_ast_node(struct parser_state *parser, enum AstNodeType type)
{
	AstNode *node = (AstNode *)raviX_allocator_allocate(&parser->container->ast_node_allocator, 0);
	node->type = type;
	node->line_number = parser->ls->lastline;
	return node;
}

static AstNode *allocate_expr_ast_node(struct parser_state *parser, enum AstNodeType type)
{
	AstNode *node = allocate_ast_node(parser, type);
	node->common_expr.truncate_results = 0;
	set_typecode(&node->common_expr.type, RAVI_TANY);
	return node;
}

static void error_expected(LexerState *ls, int token)
{
	raviX_token2str(token, &ls->container->error_message);
	raviX_buffer_add_string(&ls->container->error_message, " expected");
	longjmp(ls->container->env, 1);
}

static int testnext(LexerState *ls, int c)
{
	if (ls->t.token == c) {
		raviX_next(ls);
		return 1;
	} else
		return 0;
}

static void check(LexerState *ls, int c)
{
	if (ls->t.token != c)
		error_expected(ls, c);
}

static void checknext(LexerState *ls, int c)
{
	check(ls, c);
	raviX_next(ls);
}

/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/

/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
static int block_follow(LexerState *ls, int withuntil)
{
	switch (ls->t.token) {
	case TOK_else:
	case TOK_elseif:
	case TOK_end:
	case TOK_EOS:
		return 1;
	case TOK_until:
		return withuntil;
	default:
		return 0;
	}
}

static void check_match(LexerState *ls, int what, int who, int where)
{
	if (!testnext(ls, what)) {
		if (where == ls->linenumber)
			error_expected(ls, what);
		else {
			TextBuffer mb;
			raviX_buffer_init(&mb, 256);
			raviX_token2str(what, &mb);
			raviX_buffer_add_string(&mb, " expected (to close ");
			raviX_token2str(who, &mb);
			raviX_buffer_add_fstring(&mb, " at line %d)", where);
			char message[1024];
			raviX_string_copy(message, raviX_buffer_data(&mb), sizeof message);
			raviX_buffer_free(&mb);
			raviX_syntaxerror(ls, message);
		}
	}
}

/* Check that current token is a name, and advance */
static const StringObject *check_name_and_next(LexerState *ls)
{
	const StringObject *ts;
	check(ls, TOK_NAME);
	ts = ls->t.seminfo.ts;
	raviX_next(ls);
	return ts;
}

/* create a new local variable in function scope, and set the
 * variable type (RAVI - added type tt) */
static LuaSymbol *new_local_symbol(struct parser_state *parser, const StringObject *name, ravitype_t tt,
					   const StringObject *usertype)
{
	Scope *scope = parser->current_scope;
	LuaSymbol *symbol = raviX_allocator_allocate(&parser->container->symbol_allocator, 0);
	set_typename(&symbol->variable.value_type, tt, usertype);
	symbol->symbol_type = SYM_LOCAL;
	symbol->variable.block = scope;
	symbol->variable.var_name = name;
	symbol->variable.pseudo = NULL;
	symbol->variable.escaped = 0;
	return symbol;
}

/* create a new label */
static LuaSymbol *new_label(struct parser_state *parser, const StringObject *name)
{
	Scope *scope = parser->current_scope;
	assert(scope);
	LuaSymbol *symbol = raviX_allocator_allocate(&parser->container->symbol_allocator, 0);
	symbol->symbol_type = SYM_LABEL;
	symbol->label.block = scope;
	symbol->label.label_name = name;
	// Add to the end of the symbol list
	// Note that Lua allows multiple local declarations of the same name
	// so a new instance just gets added to the end
	add_symbol(parser->container, &scope->symbol_list, symbol);
	return symbol;
}

/* create a new local variable
 */
static LuaSymbol *new_localvarliteral_(struct parser_state *parser, const char *name, size_t sz)
{
	return new_local_symbol(parser, raviX_create_string(parser->container, name, (uint32_t)sz), RAVI_TANY, NULL);
}

/* create a new local variable
 */
#define new_localvarliteral(parser, name) new_localvarliteral_(parser, "" name, (sizeof(name) / sizeof(char)) - 1)

static LuaSymbol *search_for_variable_in_block(Scope *scope, const StringObject *varname)
{
	LuaSymbol *symbol;
	// Lookup in reverse order so that we discover the
	// most recently added local symbol - as Lua allows same
	// symbol to be declared local more than once in a scope
	// Should also work with nesting as the function when parsed
	// will only know about vars declared in parent function until
	// now.
	FOR_EACH_PTR_REVERSE(scope->symbol_list, symbol)
	{
		switch (symbol->symbol_type) {
		case SYM_LOCAL: {
			if (varname == symbol->variable.var_name) {
				return symbol;
			}
			break;
		}
		default:
			break;
		}
	}
	END_FOR_EACH_PTR_REVERSE(symbol);
	return NULL;
}

/* Each function has a list of upvalues, searches this list for given name
 */
static LuaSymbol *search_upvalue_in_function(AstNode *function, const StringObject *name)
{
	LuaSymbol *symbol;
	FOR_EACH_PTR(function->function_expr.upvalues, symbol)
	{
		switch (symbol->symbol_type) {
		case SYM_UPVALUE: {
			assert(symbol->upvalue.target_variable->symbol_type == SYM_LOCAL ||
			       symbol->upvalue.target_variable->symbol_type == SYM_ENV);
			if (name == symbol->upvalue.target_variable->variable.var_name) {
				return symbol;
			}
			break;
		}
		default:
			break;
		}
	}
	END_FOR_EACH_PTR(symbol);
	return NULL;
}

/* Each function has a list of upvalues, searches this list for given name, and adds it if not found.
 * Returns true if added, false means the function already has the upvalue.
 */
static bool add_upvalue_in_function(struct parser_state *parser, AstNode *function, LuaSymbol *sym)
{
	assert(sym->symbol_type == SYM_LOCAL || sym->symbol_type == SYM_ENV);
	LuaSymbol *symbol;
	FOR_EACH_PTR(function->function_expr.upvalues, symbol)
	{
		switch (symbol->symbol_type) {
		case SYM_UPVALUE: {
			assert(symbol->upvalue.target_variable->symbol_type == SYM_LOCAL ||
			       symbol->upvalue.target_variable->symbol_type == SYM_ENV);
			if (sym == symbol->upvalue.target_variable) {
				return false;
			}
			break;
		}
		default:
			break;
		}
	}
	END_FOR_EACH_PTR(symbol);
	LuaSymbol *upvalue = raviX_allocator_allocate(&parser->container->symbol_allocator, 0);
	upvalue->symbol_type = SYM_UPVALUE;
	upvalue->upvalue.target_variable = sym;
	upvalue->upvalue.target_function = function;
	upvalue->upvalue.upvalue_index = raviX_ptrlist_size(
	    (const struct ptr_list *)function->function_expr.upvalues); /* position of upvalue in function */
	copy_type(&upvalue->upvalue.value_type, &sym->variable.value_type);
	add_symbol(parser->container, &function->function_expr.upvalues, upvalue);
	if (sym->symbol_type == SYM_LOCAL) {
		sym->variable.escaped = 1;	     /* mark original variable as having escaped */
		sym->variable.block->need_close = 1; /* mark block containing variable as needing close operation */
		sym->variable.block->function->function_expr.need_close = 1;
	}
	return true;
}

/* Searches for a variable starting from current scope, going up the
 * scope chain within the current function. If the variable is not found in any scope of the function, then
 * search the function's upvalue list. Repeat the exercise in parent function until either
 * the symbol is found or we exhaust the search. NULL is returned if search was
 * exhausted.
 */
static LuaSymbol *search_for_variable(struct parser_state *parser, const StringObject *varname,
					      bool *is_local)
{
	*is_local = false;
	Scope *current_scope = parser->current_scope;
	AstNode *start_function = parser->current_function;
	assert(current_scope && current_scope->function == parser->current_function);
	while (current_scope) {
		AstNode *current_function = current_scope->function;
		while (current_scope && current_function == current_scope->function) {
			LuaSymbol *symbol = search_for_variable_in_block(current_scope, varname);
			if (symbol) {
				*is_local = (current_function == start_function);
				return symbol;
			}
			current_scope = current_scope->parent;
		}
		// search upvalues in the function
		LuaSymbol *symbol = search_upvalue_in_function(current_function, varname);
		if (symbol)
			return symbol;
		// try in parent function
	}
	return NULL;
}

/* Adds an upvalue to current_function and its parents until var_function; var_function being where the symbol
 * exists as a local or an upvalue. If the symbol is found in a function's upvalue list then there is no need to
 * check parent functions.
 */
static void add_upvalue_in_levels_upto(struct parser_state *parser, AstNode *current_function,
				       AstNode *var_function, LuaSymbol *symbol)
{
	// NOTE: var_function may be NULL in the case of _ENV
	// This is okay as it means we go up the whole call stack in that case
	assert(symbol->symbol_type == SYM_LOCAL || symbol->symbol_type == SYM_ENV);
	assert((symbol->symbol_type == SYM_ENV && var_function == NULL) || var_function != NULL);
	assert(current_function != var_function);
	while (current_function && current_function != var_function) {
		bool added = add_upvalue_in_function(parser, current_function, symbol);
		if (!added)
			// this function already has it so we are done
			break;
		current_function = current_function->function_expr.parent_function;
	}
}

/**
 * Adds an upvalue for _ENV.
 */
static void add_upvalue_for_ENV(struct parser_state *parser)
{
	bool is_local = false;
	LuaSymbol *symbol = search_for_variable(parser, parser->container->_ENV, &is_local);
	if (symbol == NULL) {
		// No definition of _ENV found
		// Create special symbol for _ENV - so that upvalues can reference it
		// Note that this symbol is not added to any scope, however upvalue created below will reference it
		symbol = raviX_allocator_allocate(&parser->container->symbol_allocator, 0);
		symbol->symbol_type = SYM_ENV;
		symbol->variable.var_name = parser->container->_ENV;
		symbol->variable.block = NULL;
		set_type(&symbol->variable.value_type, RAVI_TTABLE); // _ENV is by default a table
		// Create an upvalue for _ENV
		add_upvalue_in_levels_upto(parser, parser->current_function, NULL, symbol);
	} else if (!is_local && symbol->symbol_type == SYM_LOCAL) {
		// If _ENV occurred as a local symbol in a parent function then we
		// need to construct an upvalue. Lua requires that the upvalue be
		// added to all functions in the tree up to the function where the local
		// is defined.
		add_upvalue_in_levels_upto(parser, parser->current_function, symbol->variable.block->function, symbol);
	} else if (symbol->symbol_type == SYM_UPVALUE && symbol->upvalue.target_function != parser->current_function) {
		// We found an upvalue but it is not at the same level
		// Ensure all levels have the upvalue
		// Note that if the upvalue refers to special _ENV symbol then target function will be NULL
		add_upvalue_in_levels_upto(parser, parser->current_function, symbol->upvalue.target_function,
					   symbol->upvalue.target_variable);
	}
}

/* Creates a symbol reference to the name; the returned symbol reference
 * may be local, upvalue or global.
 */
static AstNode *new_symbol_reference(struct parser_state *parser, const StringObject *varname)
{
	bool is_local = false;
	LuaSymbol *symbol = search_for_variable(parser, varname, &is_local); // Search in all scopes
	if (symbol) {
		// TODO we had a bug here - see t013.lua
		// Need more test cases for this
		// we found a local or upvalue
		if (!is_local && symbol->symbol_type == SYM_LOCAL) {
			// If the local symbol occurred in a parent function then we
			// need to construct an upvalue. Lua requires that the upvalue be
			// added to all functions in the tree up to the function where the local
			// is defined.
			add_upvalue_in_levels_upto(parser, parser->current_function, symbol->variable.block->function,
						   symbol);
			// TODO Following search could be avoided if above returned the symbol
			symbol = search_upvalue_in_function(parser->current_function, varname);
		} else if (symbol->symbol_type == SYM_UPVALUE &&
			   symbol->upvalue.target_function != parser->current_function) {
			// We found an upvalue but it is not at the same level
			// Ensure all levels have the upvalue
			// Note that if the uvalue refers to special _ENV symbol then target function will be NULL
			add_upvalue_in_levels_upto(parser, parser->current_function, symbol->upvalue.target_function,
						   symbol->upvalue.target_variable);
			// TODO Following search could be avoided if above returned the symbol
			symbol = search_upvalue_in_function(parser->current_function, varname);
		}
	} else {
		// Return global symbol
		LuaSymbol *global = raviX_allocator_allocate(&parser->container->symbol_allocator, 0);
		global->symbol_type = SYM_GLOBAL;
		global->variable.var_name = varname;
		global->variable.block = NULL;
		set_type(&global->variable.value_type, RAVI_TANY); // Globals are always ANY type
		// We don't add globals to any scope so that they are
		// always looked up
		symbol = global;
		// Since we have a global reference we need to add upvalue for _ENV
		// At the parser level we do not try to model that the global reference will be
		// resolved by _ENV[name] - we leave that to the code generator to decide.
		// However adding an upvalue later is hard so we do it here.
		add_upvalue_for_ENV(parser);
		bool is_local;
		global->variable.env = search_for_variable(parser, parser->container->_ENV, &is_local);
		assert(global->variable.env);
	}
	AstNode *symbol_expr = allocate_expr_ast_node(parser, EXPR_SYMBOL);
	symbol_expr->symbol_expr.type = symbol->variable.value_type;
	symbol_expr->symbol_expr.var = symbol;
	return symbol_expr;
}

/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/

static AstNode *new_string_literal(struct parser_state *parser, const StringObject *ts)
{
	AstNode *node = allocate_expr_ast_node(parser, EXPR_LITERAL);
	set_type(&node->literal_expr.type, RAVI_TSTRING);
	node->literal_expr.u.ts = ts;
	return node;
}

static AstNode *new_field_selector(struct parser_state *parser, const StringObject *ts)
{
	AstNode *index = allocate_expr_ast_node(parser, EXPR_FIELD_SELECTOR);
	index->index_expr.expr = new_string_literal(parser, ts);
	set_type(&index->index_expr.type, RAVI_TANY);
	return index;
}

/*
 * Parse ['.' | ':'] NAME
 */
static AstNode *parse_field_selector(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* fieldsel -> ['.' | ':'] NAME */
	raviX_next(ls); /* skip the dot or colon */
	const StringObject *ts = check_name_and_next(ls);
	return new_field_selector(parser, ts);
}

/*
 * Parse '[' expr ']
 */
static AstNode *parse_yindex(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* index -> '[' expr ']' */
	raviX_next(ls); /* skip the '[' */
	AstNode *expr = parse_expression(parser);
	checknext(ls, ']');

	AstNode *index = allocate_expr_ast_node(parser, EXPR_Y_INDEX);
	index->index_expr.expr = expr;
	set_type(&index->index_expr.type, RAVI_TANY);
	return index;
}

/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

static AstNode *new_indexed_assign_expr(struct parser_state *parser, AstNode *key_expr,
						AstNode *value_expr)
{
	AstNode *set = allocate_expr_ast_node(parser, EXPR_TABLE_ELEMENT_ASSIGN);
	set->table_elem_assign_expr.key_expr = key_expr;
	set->table_elem_assign_expr.value_expr = value_expr;
	set->table_elem_assign_expr.type =
	    value_expr->common_expr.type; /* type of indexed assignment is same as the value*/
	return set;
}

static AstNode *parse_recfield(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* recfield -> (NAME | '['exp1']') = exp1 */
	AstNode *index_expr;
	if (ls->t.token == TOK_NAME) {
		const StringObject *ts = check_name_and_next(ls);
		index_expr = new_field_selector(parser, ts);
	} else /* ls->t.token == '[' */
		index_expr = parse_yindex(parser);
	checknext(ls, '=');
	AstNode *value_expr = parse_expression(parser);
	return new_indexed_assign_expr(parser, index_expr, value_expr);
}

static AstNode *parse_listfield(struct parser_state *parser)
{
	/* listfield -> exp */
	AstNode *value_expr = parse_expression(parser);
	return new_indexed_assign_expr(parser, NULL, value_expr);
}

static AstNode *parse_field(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* field -> listfield | recfield */
	switch (ls->t.token) {
	case TOK_NAME: {			/* may be 'listfield' or 'recfield' */
		if (raviX_lookahead(ls) != '=') /* expression? */
			return parse_listfield(parser);
		else
			return parse_recfield(parser);
		break;
	}
	case '[': {
		return parse_recfield(parser);
		break;
	}
	default: {
		return parse_listfield(parser);
		break;
	}
	}
	return NULL;
}

static AstNode *has_function_call(AstNode *expr)
{
	if (!expr)
		return NULL;
	if (expr->type == EXPR_FUNCTION_CALL)
		return expr;
	else if (expr->type == EXPR_SUFFIXED) {
		if (expr->suffixed_expr.suffix_list) {
			return has_function_call(
			    (AstNode *)raviX_ptrlist_last((struct ptr_list *)expr->suffixed_expr.suffix_list));
		} else {
			return has_function_call(expr->suffixed_expr.primary_expr);
		}
	} else {
		return NULL;
	}
}

/* If a call expr appears as the last in the expression list then mark it as multi-return (-1)
 * i.e. the caller wants all available returns.
 */
static void set_multireturn(struct parser_state *parser, AstNodeList *expr_list, bool in_table_constructor)
{
	AstNode *last_expr = (AstNode *)raviX_ptrlist_last((struct ptr_list *)expr_list);
	if (!last_expr)
		return;
	if (in_table_constructor) {
		if (last_expr->type == EXPR_TABLE_ELEMENT_ASSIGN &&
		    last_expr->table_elem_assign_expr.key_expr == NULL) {
			last_expr = last_expr->table_elem_assign_expr.value_expr;
		} else {
			return;
		}
	}
	AstNode *call_expr = has_function_call(last_expr);
	if (call_expr) {
		// Last expr so accept all available results
		call_expr->function_call_expr.num_results = -1;
	}
}

static AstNode *parse_table_constructor(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* constructor -> '{' [ field { sep field } [sep] ] '}'
	sep -> ',' | ';' */
	int line = ls->linenumber;
	checknext(ls, '{');
	AstNode *table_expr = allocate_expr_ast_node(parser, EXPR_TABLE_LITERAL);
	set_type(&table_expr->table_expr.type, RAVI_TTABLE);
	table_expr->table_expr.expr_list = NULL;
	do {
		if (ls->t.token == '}')
			break;
		AstNode *field_expr = parse_field(parser);
		add_ast_node(parser->container, &table_expr->table_expr.expr_list, field_expr);
	} while (testnext(ls, ',') || testnext(ls, ';'));
	set_multireturn(parser, table_expr->table_expr.expr_list, true);
	check_match(ls, '}', '{', line);
	return table_expr;
}

/* }====================================================================== */

/*
 * We would like to allow user defined types to contain the sequence
 * NAME [. NAME]+
 * The initial NAME is supplied.
 * Returns extended name.
 * Note that the returned string will be anchored in the Lexer and must
 * be anchored somewhere else by the time parsing finishes
 */
static const StringObject *parse_user_defined_type_name(LexerState *ls,
								const StringObject *typename)
{
	size_t len = 0;
	if (testnext(ls, '.')) {
		char buffer[256] = {0};
		const char *str = typename->str;
		len = strlen(str);
		if (len >= sizeof buffer) {
			raviX_syntaxerror(ls, "User defined type name is too long");
			return typename;
		}
		snprintf(buffer, sizeof buffer, "%s", str);
		do {
			typename = check_name_and_next(ls);
			str = typename->str;
			size_t newlen = len + strlen(str) + 1;
			if (newlen >= sizeof buffer) {
				raviX_syntaxerror(ls, "User defined type name is too long");
				return typename;
			}
			snprintf(buffer + len, sizeof buffer - len, ".%s", str);
			len = newlen;
		} while (testnext(ls, '.'));
		typename = raviX_create_string(ls->container, buffer, (uint32_t)strlen(buffer));
	}
	return typename;
}

/* RAVI Parse
 *   name : type
 *   where type is 'integer', 'integer[]',
 *                 'number', 'number[]'
 */
static LuaSymbol *parse_local_variable_declaration(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* assume a dynamic type */
	ravitype_t tt = RAVI_TANY;
	const StringObject *name = check_name_and_next(ls);
	const StringObject *pusertype = NULL;
	if (testnext(ls, ':')) {
		const StringObject *typename = check_name_and_next(ls); /* we expect a type name */
		const char *str = typename->str;
		/* following is not very nice but easy as
		 * the lexer doesn't need to be changed
		 */
		if (strcmp(str, "integer") == 0)
			tt = RAVI_TNUMINT;
		else if (strcmp(str, "number") == 0)
			tt = RAVI_TNUMFLT;
		else if (strcmp(str, "closure") == 0)
			tt = RAVI_TFUNCTION;
		else if (strcmp(str, "table") == 0)
			tt = RAVI_TTABLE;
		else if (strcmp(str, "string") == 0)
			tt = RAVI_TSTRING;
		else if (strcmp(str, "boolean") == 0)
			tt = RAVI_TBOOLEAN;
		else if (strcmp(str, "any") == 0)
			tt = RAVI_TANY;
		else {
			/* default is a userdata type */
			tt = RAVI_TUSERDATA;
			typename = parse_user_defined_type_name(ls, typename);
			// str = getstr(typename);
			pusertype = typename;
		}
		if (tt == RAVI_TNUMFLT || tt == RAVI_TNUMINT) {
			/* if we see [] then it is an array type */
			if (testnext(ls, '[')) {
				checknext(ls, ']');
				tt = (tt == RAVI_TNUMFLT) ? RAVI_TARRAYFLT : RAVI_TARRAYINT;
			}
		}
	}
	return new_local_symbol(parser, name, tt, pusertype);
}

static bool parse_parameter_list(struct parser_state *parser, LuaSymbolList **list)
{
	LexerState *ls = parser->ls;
	/* parlist -> [ param { ',' param } ] */
	int nparams = 0;
	bool is_vararg = false;
	if (ls->t.token != ')') { /* is 'parlist' not empty? */
		do {
			switch (ls->t.token) {
			case TOK_NAME: { /* param -> NAME */
					 /* RAVI change - add type */
				LuaSymbol *symbol = parse_local_variable_declaration(parser);
				symbol->variable.function_parameter = 1;
				add_symbol(parser->container, list, symbol);
				add_local_symbol_to_current_scope(parser, symbol);
				nparams++;
				break;
			}
			case TOK_DOTS: { /* param -> '...' */
				raviX_next(ls);
				is_vararg = true; /* declared vararg */
				break;
			}
			default:
				raviX_syntaxerror(ls, "<name> or '...' expected");
			}
		} while (!is_vararg && testnext(ls, ','));
	}
	return is_vararg;
}

static void parse_function_body(struct parser_state *parser, AstNode *func_ast, int ismethod, int line)
{
	LexerState *ls = parser->ls;
	/* body ->  '(' parlist ')' block END */
	checknext(ls, '(');
	if (ismethod) {
		LuaSymbol *symbol = new_localvarliteral(parser, "self"); /* create 'self' parameter */
		add_symbol(parser->container, &func_ast->function_expr.args, symbol);
	}
	bool is_vararg = parse_parameter_list(parser, &func_ast->function_expr.args);
	func_ast->function_expr.is_vararg = is_vararg;
	func_ast->function_expr.is_method = ismethod;
	checknext(ls, ')');
	parse_statement_list(parser, &func_ast->function_expr.function_statement_list);
	check_match(ls, TOK_end, TOK_function, line);
}

/* parse expression list */
static int parse_expression_list(struct parser_state *parser, AstNodeList **list)
{
	LexerState *ls = parser->ls;
	/* explist -> expr { ',' expr } */
	int n = 1; /* at least one expression */
	AstNode *expr = parse_expression(parser);
	add_ast_node(parser->container, list, expr);
	while (testnext(ls, ',')) {
		expr = parse_expression(parser);
		add_ast_node(parser->container, list, expr);
		n++;
	}
	return n;
}

/* parse function arguments */
static AstNode *parse_function_call(struct parser_state *parser, const StringObject *methodname,
					    int line)
{
	LexerState *ls = parser->ls;
	AstNode *call_expr = allocate_expr_ast_node(parser, EXPR_FUNCTION_CALL);
	call_expr->function_call_expr.method_name = methodname;
	call_expr->function_call_expr.arg_list = NULL;
	call_expr->function_call_expr.num_results = 1; /* By default we expect one arg */
	set_type(&call_expr->function_call_expr.type, RAVI_TANY);
	switch (ls->t.token) {
	case '(': { /* funcargs -> '(' [ explist ] ')' */
		raviX_next(ls);
		if (ls->t.token == ')') /* arg list is empty? */
			;
		else {
			parse_expression_list(parser, &call_expr->function_call_expr.arg_list);
			set_multireturn(parser, call_expr->function_call_expr.arg_list, false);
		}
		check_match(ls, ')', '(', line);
		break;
	}
	case '{': { /* funcargs -> constructor */
		AstNode *table_expr = parse_table_constructor(parser);
		add_ast_node(parser->container, &call_expr->function_call_expr.arg_list, table_expr);
		break;
	}
	case TOK_STRING: { /* funcargs -> STRING */
		AstNode *string_expr = new_literal_expression(parser, RAVI_TSTRING);
		string_expr->literal_expr.u.ts = ls->t.seminfo.ts;
		add_ast_node(parser->container, &call_expr->function_call_expr.arg_list, string_expr);
		raviX_next(ls);
		break;
	}
	default: {
		raviX_syntaxerror(ls, "function arguments expected");
	}
	}
	return call_expr;
}

/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

/* primary expression - name or subexpression */
static AstNode *parse_primary_expression(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	AstNode *primary_expr = NULL;
	/* primaryexp -> NAME | '(' expr ')' */
	switch (ls->t.token) {
	case '(': {
		int line = ls->linenumber;
		raviX_next(ls);
		primary_expr = parse_expression(parser);
		primary_expr->common_expr.truncate_results = 1; /* Lua requires that we truncate results to 1 */
		check_match(ls, ')', '(', line);
		break;
	}
	case TOK_NAME: {
		primary_expr = new_symbol_reference(parser, check_name_and_next(parser->ls));
		break;
	}
	default: {
		raviX_syntaxerror(ls, "unexpected symbol");
	}
	}
	assert(primary_expr);
	return primary_expr;
}

/* variable or field access or function call */
static AstNode *parse_suffixed_expression(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* suffixedexp ->
	primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
	int line = ls->linenumber;
	AstNode *suffixed_expr = allocate_expr_ast_node(parser, EXPR_SUFFIXED);
	suffixed_expr->suffixed_expr.primary_expr = parse_primary_expression(parser);
	suffixed_expr->suffixed_expr.type = suffixed_expr->suffixed_expr.primary_expr->common_expr.type;
	suffixed_expr->suffixed_expr.suffix_list = NULL;
	for (;;) {
		switch (ls->t.token) {
		case '.': { /* fieldsel */
			AstNode *suffix = parse_field_selector(parser);
			add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
			set_type(&suffixed_expr->suffixed_expr.type, RAVI_TANY);
			break;
		}
		case '[': { /* '[' exp1 ']' */
			AstNode *suffix = parse_yindex(parser);
			add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
			set_type(&suffixed_expr->suffixed_expr.type, RAVI_TANY);
			break;
		}
		case ':': { /* ':' NAME funcargs */
			raviX_next(ls);
			const StringObject *methodname = check_name_and_next(ls);
			AstNode *suffix = parse_function_call(parser, methodname, line);
			add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
			break;
		}
		case '(':
		case TOK_STRING:
		case '{': { /* funcargs */
			AstNode *suffix = parse_function_call(parser, NULL, line);
			add_ast_node(parser->container, &suffixed_expr->suffixed_expr.suffix_list, suffix);
			break;
		}
		default:
			return suffixed_expr;
		}
	}
}

static AstNode *new_literal_expression(struct parser_state *parser, ravitype_t type)
{
	AstNode *expr = allocate_expr_ast_node(parser, EXPR_LITERAL);
	set_type(&expr->literal_expr.type, type);
	expr->literal_expr.u.i = 0; /* initialize */
	return expr;
}

static AstNode *parse_simple_expression(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
	constructor | FUNCTION body | suffixedexp */
	AstNode *expr = NULL;
	switch (ls->t.token) {
	case TOK_FLT: {
		expr = new_literal_expression(parser, RAVI_TNUMFLT);
		expr->literal_expr.u.r = ls->t.seminfo.r;
		break;
	}
	case TOK_INT: {
		expr = new_literal_expression(parser, RAVI_TNUMINT);
		expr->literal_expr.u.i = ls->t.seminfo.i;
		break;
	}
	case TOK_STRING: {
		expr = new_literal_expression(parser, RAVI_TSTRING);
		expr->literal_expr.u.ts = ls->t.seminfo.ts;
		break;
	}
	case TOK_nil: {
		expr = new_literal_expression(parser, RAVI_TNIL);
		expr->literal_expr.u.i = -1;
		break;
	}
	case TOK_true: {
		expr = new_literal_expression(parser, RAVI_TBOOLEAN);
		expr->literal_expr.u.i = 1;
		break;
	}
	case TOK_false: {
		expr = new_literal_expression(parser, RAVI_TBOOLEAN);
		expr->literal_expr.u.i = 0;
		break;
	}
	case TOK_DOTS: { /* vararg */
		expr = new_literal_expression(parser, RAVI_TVARARGS);
		break;
	}
	case '{': { /* constructor */
		return parse_table_constructor(parser);
	}
	case TOK_function: {
		raviX_next(ls);
		AstNode *function_ast = new_function(parser);
		parse_function_body(parser, function_ast, 0, ls->linenumber);
		end_function(parser);
		return function_ast;
	}
	default: {
		return parse_suffixed_expression(parser);
	}
	}
	raviX_next(ls);
	return expr;
}

static UnaryOperatorType get_unary_opr(int op)
{
	switch (op) {
	case TOK_not:
		return UNOPR_NOT;
	case '-':
		return UNOPR_MINUS;
	case '~':
		return UNOPR_BNOT;
	case '#':
		return UNOPR_LEN;
	case TOK_TO_INTEGER:
		return UNOPR_TO_INTEGER;
	case TOK_TO_NUMBER:
		return UNOPR_TO_NUMBER;
	case TOK_TO_INTARRAY:
		return UNOPR_TO_INTARRAY;
	case TOK_TO_NUMARRAY:
		return UNOPR_TO_NUMARRAY;
	case TOK_TO_TABLE:
		return UNOPR_TO_TABLE;
	case TOK_TO_STRING:
		return UNOPR_TO_STRING;
	case TOK_TO_CLOSURE:
		return UNOPR_TO_CLOSURE;
	case '@':
		return UNOPR_TO_TYPE;
	default:
		return UNOPR_NOUNOPR;
	}
}

static BinaryOperatorType get_binary_opr(int op)
{
	switch (op) {
	case '+':
		return BINOPR_ADD;
	case '-':
		return BINOPR_SUB;
	case '*':
		return BINOPR_MUL;
	case '%':
		return BINOPR_MOD;
	case '^':
		return BINOPR_POW;
	case '/':
		return BINOPR_DIV;
	case TOK_IDIV:
		return BINOPR_IDIV;
	case '&':
		return BINOPR_BAND;
	case '|':
		return BINOPR_BOR;
	case '~':
		return BINOPR_BXOR;
	case TOK_SHL:
		return BINOPR_SHL;
	case TOK_SHR:
		return BINOPR_SHR;
	case TOK_CONCAT:
		return BINOPR_CONCAT;
	case TOK_NE:
		return BINOPR_NE;
	case TOK_EQ:
		return BINOPR_EQ;
	case '<':
		return BINOPR_LT;
	case TOK_LE:
		return BINOPR_LE;
	case '>':
		return BINOPR_GT;
	case TOK_GE:
		return BINOPR_GE;
	case TOK_and:
		return BINOPR_AND;
	case TOK_or:
		return BINOPR_OR;
	default:
		return BINOPR_NOBINOPR;
	}
}

static const struct {
	lu_byte left;  /* left priority for each binary operator */
	lu_byte right; /* right priority */
} priority[] = {
    /* ORDER OPR */
    {10, 10}, {10, 10},		/* '+' '-' */
    {11, 11}, {11, 11},		/* '*' '%' */
    {14, 13},			/* '^' (right associative) */
    {11, 11}, {11, 11},		/* '/' '//' */
    {6, 6},   {4, 4},	{5, 5}, /* '&' '|' '~' */
    {7, 7},   {7, 7},		/* '<<' '>>' */
    {9, 8},			/* '..' (right associative) */
    {3, 3},   {3, 3},	{3, 3}, /* ==, <, <= */
    {3, 3},   {3, 3},	{3, 3}, /* ~=, >, >= */
    {2, 2},   {1, 1}		/* and, or */
};

#define UNARY_PRIORITY 12 /* priority for unary operators */

/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
static AstNode *parse_sub_expression(struct parser_state *parser, int limit, BinaryOperatorType *untreated_op)
{
	LexerState *ls = parser->ls;
	BinaryOperatorType op;
	UnaryOperatorType uop;
	AstNode *expr = NULL;
	uop = get_unary_opr(ls->t.token);
	if (uop != UNOPR_NOUNOPR) {
		// RAVI change - get usertype if @<name>
		const StringObject *usertype = NULL;
		if (uop == UNOPR_TO_TYPE) {
			usertype = ls->t.seminfo.ts;
			raviX_next(ls);
			// Check and expand to extended name if necessary
			usertype = parse_user_defined_type_name(ls, usertype);
		} else {
			raviX_next(ls);
		}
		BinaryOperatorType ignored;
		AstNode *subexpr = parse_sub_expression(parser, UNARY_PRIORITY, &ignored);
		expr = allocate_expr_ast_node(parser, EXPR_UNARY);
		expr->unary_expr.expr = subexpr;
		expr->unary_expr.unary_op = uop;
		expr->unary_expr.type.type_name = usertype;
	} else {
		expr = parse_simple_expression(parser);
	}
	/* expand while operators have priorities higher than 'limit' */
	op = get_binary_opr(ls->t.token);
	while (op != BINOPR_NOBINOPR && priority[op].left > limit) {
		BinaryOperatorType nextop;
		raviX_next(ls);
		/* read sub-expression with higher priority */
		AstNode *exprright = parse_sub_expression(parser, priority[op].right, &nextop);

		AstNode *binexpr = allocate_expr_ast_node(parser, EXPR_BINARY);
		binexpr->binary_expr.expr_left = expr;
		binexpr->binary_expr.expr_right = exprright;
		binexpr->binary_expr.binary_op = op;
		expr = binexpr; // Becomes the left expr for next iteration
		op = nextop;
	}
	*untreated_op = op; /* return first untreated operator */
	return expr;
}

static AstNode *parse_expression(struct parser_state *parser)
{
	BinaryOperatorType ignored;
	return parse_sub_expression(parser, 0, &ignored);
}

/* }==================================================================== */

/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/

static void add_local_symbol_to_current_scope(struct parser_state *parser, LuaSymbol *sym)
{
	// Note that Lua allows multiple local declarations of the same name
	// so a new instance just gets added to the end
	add_symbol(parser->container, &parser->current_scope->symbol_list, sym);
	add_symbol(parser->container, &parser->current_scope->function->function_expr.locals, sym);
}

static Scope *parse_block(struct parser_state *parser, AstNodeList **statement_list)
{
	/* block -> statlist */
	Scope *scope = new_scope(parser);
	parse_statement_list(parser, statement_list);
	end_scope(parser);
	return scope;
}

/* parse condition in a repeat statement or an if control structure
 * called by repeatstat(), test_then_block()
 */
static AstNode *parse_condition(struct parser_state *parser)
{
	/* cond -> exp */
	return parse_expression(parser); /* read condition */
}

static AstNode *parse_goto_statment(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	const StringObject *label;
	int is_break = 0;
	if (testnext(ls, TOK_goto))
		label = check_name_and_next(ls);
	else {
		raviX_next(ls); /* skip break */
		label = raviX_create_string(ls->container, "break", sizeof "break");
		is_break = 1;
	}
	// Resolve labels in the end?
	AstNode *goto_stmt = allocate_ast_node(parser, STMT_GOTO);
	goto_stmt->goto_stmt.name = label;
	goto_stmt->goto_stmt.is_break = is_break;
	goto_stmt->goto_stmt.goto_scope = parser->current_scope;
	return goto_stmt;
}

/* skip no-op statements */
static void skip_noop_statements(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	while (ls->t.token == ';') //  || ls->t.token == TOK_DBCOLON)
		parse_statement(parser);
}

static AstNode *generate_label(struct parser_state *parser, const StringObject *label)
{
	LuaSymbol *symbol = new_label(parser, label);
	AstNode *label_stmt = allocate_ast_node(parser, STMT_LABEL);
	label_stmt->label_stmt.symbol = symbol;
	return label_stmt;
}

static AstNode *parse_label_statement(struct parser_state *parser, const StringObject *label, int line)
{
	(void)line;
	LexerState *ls = parser->ls;
	/* label -> '::' NAME '::' */
	checknext(ls, TOK_DBCOLON); /* skip double colon */
	/* create new entry for this label */
	AstNode *label_stmt = generate_label(parser, label);
	skip_noop_statements(parser); /* skip other no-op statements */
	return label_stmt;
}

static AstNode *parse_while_statement(struct parser_state *parser, int line)
{
	LexerState *ls = parser->ls;
	/* whilestat -> WHILE cond DO block END */
	raviX_next(ls); /* skip WHILE */
	AstNode *stmt = allocate_ast_node(parser, STMT_WHILE);
	stmt->while_or_repeat_stmt.loop_scope = NULL;
	stmt->while_or_repeat_stmt.loop_statement_list = NULL;
	stmt->while_or_repeat_stmt.condition = parse_condition(parser);
	checknext(ls, TOK_do);
	stmt->while_or_repeat_stmt.loop_scope = parse_block(parser, &stmt->while_or_repeat_stmt.loop_statement_list);
	check_match(ls, TOK_end, TOK_while, line);
	return stmt;
}

static AstNode *parse_repeat_statement(struct parser_state *parser, int line)
{
	LexerState *ls = parser->ls;
	/* repeatstat -> REPEAT block UNTIL cond */
	raviX_next(ls); /* skip REPEAT */
	AstNode *stmt = allocate_ast_node(parser, STMT_REPEAT);
	stmt->while_or_repeat_stmt.condition = NULL;
	stmt->while_or_repeat_stmt.loop_statement_list = NULL;
	stmt->while_or_repeat_stmt.loop_scope = new_scope(parser); /* scope block */
	parse_statement_list(parser, &stmt->while_or_repeat_stmt.loop_statement_list);
	check_match(ls, TOK_until, TOK_repeat, line);
	stmt->while_or_repeat_stmt.condition = parse_condition(parser); /* read condition (inside scope block) */
	end_scope(parser);
	return stmt;
}

/* parse a for loop body for both versions of the for loop */
static void parse_forbody(struct parser_state *parser, AstNode *stmt, int line, int nvars, int isnum)
{
	(void)line;
	(void)nvars;
	(void)isnum;
	LexerState *ls = parser->ls;
	/* forbody -> DO block */
	checknext(ls, TOK_do);
	stmt->for_stmt.for_body = parse_block(parser, &stmt->for_stmt.for_statement_list);
}

/* parse a numerical for loop */
static void parse_fornum_statement(struct parser_state *parser, AstNode *stmt,
				   const StringObject *varname, int line)
{
	LexerState *ls = parser->ls;
	/* fornum -> NAME = exp1,exp1[,exp1] forbody */
	LuaSymbol *local = new_local_symbol(parser, varname, RAVI_TANY, NULL);
	add_symbol(parser->container, &stmt->for_stmt.symbols, local);
	add_local_symbol_to_current_scope(parser, local);
	checknext(ls, '=');
	/* get the type of each expression */
	add_ast_node(parser->container, &stmt->for_stmt.expr_list, parse_expression(parser)); /* initial value */
	checknext(ls, ',');
	add_ast_node(parser->container, &stmt->for_stmt.expr_list, parse_expression(parser)); /* limit */
	if (testnext(ls, ',')) {
		add_ast_node(parser->container, &stmt->for_stmt.expr_list,
			     parse_expression(parser)); /* optional step */
	}
	parse_forbody(parser, stmt, line, 1, 1);
}

/* parse a generic for loop */
static void parse_for_list(struct parser_state *parser, AstNode *stmt, const StringObject *indexname)
{
	LexerState *ls = parser->ls;
	/* forlist -> NAME {,NAME} IN explist forbody */
	int nvars = 4; /* gen, state, control, plus at least one declared var */
	/* create declared variables */
	LuaSymbol *local = new_local_symbol(parser, indexname, RAVI_TANY, NULL);
	add_symbol(parser->container, &stmt->for_stmt.symbols, local);
	add_local_symbol_to_current_scope(parser, local);
	while (testnext(ls, ',')) {
		local = new_local_symbol(parser, check_name_and_next(ls), RAVI_TANY, NULL);
		add_symbol(parser->container, &stmt->for_stmt.symbols, local);
		add_local_symbol_to_current_scope(parser, local);
		nvars++;
	}
	checknext(ls, TOK_in);
	parse_expression_list(parser, &stmt->for_stmt.expr_list);
	int line = ls->linenumber;
	parse_forbody(parser, stmt, line, nvars - 3, 0);
}

/* initial parsing of a for loop - calls fornum() or forlist() */
static AstNode *parse_for_statement(struct parser_state *parser, int line)
{
	LexerState *ls = parser->ls;
	/* forstat -> FOR (fornum | forlist) END */
	const StringObject *varname;
	AstNode *stmt = allocate_ast_node(parser, AST_NONE);
	stmt->for_stmt.symbols = NULL;
	stmt->for_stmt.expr_list = NULL;
	stmt->for_stmt.for_body = NULL;
	stmt->for_stmt.for_statement_list = NULL;
	stmt->for_stmt.for_scope = new_scope(parser); // For the loop variables
	raviX_next(ls);				      /* skip 'for' */
	varname = check_name_and_next(ls);	      /* first variable name */
	switch (ls->t.token) {
	case '=':
		stmt->type = STMT_FOR_NUM;
		parse_fornum_statement(parser, stmt, varname, line);
		break;
	case ',':
	case TOK_in:
		stmt->type = STMT_FOR_IN;
		parse_for_list(parser, stmt, varname);
		break;
	default:
		raviX_syntaxerror(ls, "'=' or 'in' expected");
	}
	check_match(ls, TOK_end, TOK_for, line);
	end_scope(parser);
	return stmt;
}

/* parse if cond then block - called from parse_if_statement() */
static AstNode *parse_if_cond_then_block(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* test_then_block -> [IF | ELSEIF] cond THEN block */
	raviX_next(ls); /* skip IF or ELSEIF */
	AstNode *test_then_block =
	    allocate_ast_node(parser, STMT_TEST_THEN);			       // This is not an AST node on its own
	test_then_block->test_then_block.condition = parse_expression(parser); /* read condition */
	test_then_block->test_then_block.test_then_scope = NULL;
	test_then_block->test_then_block.test_then_statement_list = NULL;
	checknext(ls, TOK_then);
	if (ls->t.token == TOK_goto || ls->t.token == TOK_break) {
		test_then_block->test_then_block.test_then_scope = new_scope(parser);
		AstNode *stmt = parse_goto_statment(parser); /* handle goto/break */
		add_ast_node(parser->container, &test_then_block->test_then_block.test_then_statement_list, stmt);
		skip_noop_statements(parser); /* skip other no-op statements */
		if (block_follow(ls, 0)) {    /* 'goto' is the entire block? */
			end_scope(parser);
			return test_then_block; /* and that is it */
		} else {			/* must skip over 'then' part if condition is false */
			;
		}
	} else { /* regular case (not goto/break) */
		test_then_block->test_then_block.test_then_scope = new_scope(parser);
	}
	parse_statement_list(parser, &test_then_block->test_then_block.test_then_statement_list); /* 'then' part */
	end_scope(parser);
	return test_then_block;
}

static AstNode *parse_if_statement(struct parser_state *parser, int line)
{
	LexerState *ls = parser->ls;
	/* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
	AstNode *stmt = allocate_ast_node(parser, STMT_IF);
	stmt->if_stmt.if_condition_list = NULL;
	stmt->if_stmt.else_block = NULL;
	stmt->if_stmt.else_statement_list = NULL;
	AstNode *test_then_block = parse_if_cond_then_block(parser); /* IF cond THEN block */
	add_ast_node(parser->container, &stmt->if_stmt.if_condition_list, test_then_block);
	while (ls->t.token == TOK_elseif) {
		test_then_block = parse_if_cond_then_block(parser); /* ELSEIF cond THEN block */
		add_ast_node(parser->container, &stmt->if_stmt.if_condition_list, test_then_block);
	}
	if (testnext(ls, TOK_else))
		stmt->if_stmt.else_block = parse_block(parser, &stmt->if_stmt.else_statement_list); /* 'else' part */
	check_match(ls, TOK_end, TOK_if, line);
	return stmt;
}

static AstNode *parse_local_function_statement(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	LuaSymbol *symbol =
	    new_local_symbol(parser, check_name_and_next(ls), RAVI_TFUNCTION, NULL); /* new local variable */
	/* local function f ... is parsed as local f; f = function ... */
	add_local_symbol_to_current_scope(parser, symbol);
	AstNode *function_ast = new_function(parser);
	parse_function_body(parser, function_ast, 0, ls->linenumber); /* function created in next register */
	end_function(parser);
	AstNode *stmt = allocate_ast_node(parser, STMT_LOCAL);
	stmt->local_stmt.var_list = NULL;
	stmt->local_stmt.expr_list = NULL;
	add_symbol(parser->container, &stmt->local_stmt.var_list, symbol);
	add_ast_node(parser->container, &stmt->local_stmt.expr_list, function_ast);
	return stmt;
}

/**
 * If a call expression is at the end of a local or expression statement then
 * we need to check the number of return values that is expected.
 */
static void limit_function_call_results(struct parser_state *parser, int num_lhs, AstNodeList *expr_list)
{
	// FIXME probably doesn't handle var arg case
	AstNode *last_expr = (AstNode *)raviX_ptrlist_last((struct ptr_list *)expr_list);
	AstNode *call_expr = has_function_call(last_expr);
	if (!call_expr)
		return;
	int num_expr = raviX_ptrlist_size((const struct ptr_list *)expr_list);
	if (num_expr < num_lhs) {
		call_expr->function_call_expr.num_results = (num_lhs - num_expr) + 1;
	}
}

static AstNode *parse_local_statement(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* stat -> LOCAL NAME {',' NAME} ['=' explist] */
	AstNode *node = allocate_ast_node(parser, STMT_LOCAL);
	node->local_stmt.var_list = NULL;
	node->local_stmt.expr_list = NULL;
	int nvars = 0;
	do {
		/* local name : type = value */
		LuaSymbol *symbol = parse_local_variable_declaration(parser);
		add_symbol(parser->container, &node->local_stmt.var_list, symbol);
		nvars++;
		if (nvars >= MAXVARS)
			raviX_syntaxerror(ls, "too many local variables");
	} while (testnext(ls, ','));
	if (testnext(ls, '=')) /* nexps = */
		parse_expression_list(parser, &node->local_stmt.expr_list);
	else {
		/* nexps = 0; */
		;
	}
	limit_function_call_results(parser, nvars, node->local_stmt.expr_list);
	/* local symbols are only added to scope at the end of the local statement */
	LuaSymbol *sym = NULL;
	FOR_EACH_PTR(node->local_stmt.var_list, sym) { add_local_symbol_to_current_scope(parser, sym); }
	END_FOR_EACH_PTR(sym);
	return node;
}

/* parse a function name specification with base symbol, optional selectors and optional method name
 */
static AstNode *parse_function_name(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* funcname -> NAME {fieldsel} [':' NAME] */
	AstNode *function_stmt = allocate_ast_node(parser, STMT_FUNCTION);
	function_stmt->function_stmt.function_expr = NULL;
	function_stmt->function_stmt.method_name = NULL;
	function_stmt->function_stmt.selectors = NULL;
	function_stmt->function_stmt.name = new_symbol_reference(parser, check_name_and_next(parser->ls));
	while (ls->t.token == '.') {
		add_ast_node(parser->container, &function_stmt->function_stmt.selectors, parse_field_selector(parser));
	}
	if (ls->t.token == ':') {
		function_stmt->function_stmt.method_name = parse_field_selector(parser);
	}
	return function_stmt;
}

static AstNode *parse_function_statement(struct parser_state *parser, int line)
{
	LexerState *ls = parser->ls;
	/* funcstat -> FUNCTION funcname body */
	raviX_next(ls); /* skip FUNCTION */
	AstNode *function_stmt = parse_function_name(parser);
	int ismethod = function_stmt->function_stmt.method_name != NULL;
	AstNode *function_ast = new_function(parser);
	parse_function_body(parser, function_ast, ismethod, line);
	end_function(parser);
	function_stmt->function_stmt.function_expr = function_ast;
	return function_stmt;
}

/* parse function call with no returns or assignment statement */
static AstNode *parse_expression_statement(struct parser_state *parser)
{
	AstNode *stmt = allocate_ast_node(parser, STMT_EXPR);
	stmt->expression_stmt.var_expr_list = NULL;
	stmt->expression_stmt.expr_list = NULL;
	LexerState *ls = parser->ls;
	/* stat -> func | assignment */
	/* Until we see '=' we do not know if this is an assignment or expr list*/
	AstNodeList *current_list = NULL;
	add_ast_node(parser->container, &current_list, parse_suffixed_expression(parser));
	while (testnext(ls, ',')) { /* assignment -> ',' suffixedexp assignment */
		add_ast_node(parser->container, &current_list, parse_suffixed_expression(parser));
	}
	if (ls->t.token == '=') { /* stat -> assignment ? */
		checknext(ls, '=');
		stmt->expression_stmt.var_expr_list = current_list;
		current_list = NULL;
		parse_expression_list(parser, &current_list);
		limit_function_call_results(
		    parser, raviX_ptrlist_size((const struct ptr_list *)stmt->expression_stmt.var_expr_list), current_list);
	}
	stmt->expression_stmt.expr_list = current_list;
	// TODO Check that if not assignment then it is a function call
	return stmt;
}

static AstNode *parse_return_statement(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	/* stat -> RETURN [explist] [';'] */
	AstNode *return_stmt = allocate_ast_node(parser, STMT_RETURN);
	return_stmt->return_stmt.expr_list = NULL;
	if (block_follow(ls, 1) || ls->t.token == ';')
		/* nret = 0*/; /* return no values */
	else {
		/*nret = */
		parse_expression_list(parser, &return_stmt->return_stmt.expr_list); /* optional return values */
		set_multireturn(parser, return_stmt->return_stmt.expr_list, false);
	}
	testnext(ls, ';'); /* skip optional semicolon */
	return return_stmt;
}

static AstNode *parse_do_statement(struct parser_state *parser, int line)
{
	raviX_next(parser->ls); /* skip DO */
	AstNode *stmt = allocate_ast_node(parser, STMT_DO);
	stmt->do_stmt.do_statement_list = NULL;
	stmt->do_stmt.scope = parse_block(parser, &stmt->do_stmt.do_statement_list);
	check_match(parser->ls, TOK_end, TOK_do, line);
	return stmt;
}

/* parse a statement */
static AstNode *parse_statement(struct parser_state *parser)
{
	LexerState *ls = parser->ls;
	int line = ls->linenumber; /* may be needed for error messages */
	AstNode *stmt = NULL;
	switch (ls->t.token) {
	case ';': {		/* stat -> ';' (empty statement) */
		raviX_next(ls); /* skip ';' */
		break;
	}
	case TOK_if: { /* stat -> ifstat */
		stmt = parse_if_statement(parser, line);
		break;
	}
	case TOK_while: { /* stat -> whilestat */
		stmt = parse_while_statement(parser, line);
		break;
	}
	case TOK_do: { /* stat -> DO block END */
		stmt = parse_do_statement(parser, line);
		break;
	}
	case TOK_for: { /* stat -> forstat */
		stmt = parse_for_statement(parser, line);
		break;
	}
	case TOK_repeat: { /* stat -> repeatstat */
		stmt = parse_repeat_statement(parser, line);
		break;
	}
	case TOK_function: { /* stat -> funcstat */
		stmt = parse_function_statement(parser, line);
		break;
	}
	case TOK_local: {			/* stat -> localstat */
		raviX_next(ls);			/* skip LOCAL */
		if (testnext(ls, TOK_function)) /* local function? */
			stmt = parse_local_function_statement(parser);
		else
			stmt = parse_local_statement(parser);
		break;
	}
	case TOK_DBCOLON: {	/* stat -> label */
		raviX_next(ls); /* skip double colon */
		stmt = parse_label_statement(parser, check_name_and_next(ls), line);
		break;
	}
	case TOK_return: {	/* stat -> retstat */
		raviX_next(ls); /* skip RETURN */
		stmt = parse_return_statement(parser);
		break;
	}
	case TOK_break:	 /* stat -> breakstat */
	case TOK_goto: { /* stat -> 'goto' NAME */
		stmt = parse_goto_statment(parser);
		break;
	}
	default: { /* stat -> func | assignment */
		stmt = parse_expression_statement(parser);
		break;
	}
	}
	return stmt;
}

/* Parses a sequence of statements */
/* statlist -> { stat [';'] } */
static void parse_statement_list(struct parser_state *parser, AstNodeList **list)
{
	LexerState *ls = parser->ls;
	while (!block_follow(ls, 1)) {
		bool was_return = ls->t.token == TOK_return;
		AstNode *stmt = parse_statement(parser);
		if (stmt)
			add_ast_node(parser->container, list, stmt);
		if (was_return)
			break; /* 'return' must be last statement */
	}
}

/* Starts a new scope. If the current function has no main block
 * defined then the new scope becomes its main block. The new scope
 * gets existing scope as its parent even if that belongs to parent
 * function.
 */
static Scope *new_scope(struct parser_state *parser)
{
	CompilerState *container = parser->container;
	Scope *scope = raviX_allocator_allocate(&container->block_scope_allocator, 0);
	scope->symbol_list = NULL;
	// scope->do_statement_list = NULL;
	scope->function = parser->current_function;
	scope->need_close = 0;
	assert(scope->function && scope->function->type == EXPR_FUNCTION);
	scope->parent = parser->current_scope;
	parser->current_scope = scope;
	if (!parser->current_function->function_expr.main_block)
		parser->current_function->function_expr.main_block = scope;
	return scope;
}

static void end_scope(struct parser_state *parser)
{
	assert(parser->current_scope);
	Scope *scope = parser->current_scope;
	parser->current_scope = scope->parent;
	assert(parser->current_scope != NULL || scope == parser->current_function->function_expr.main_block);
}

/* Creates a new function AST node and starts the function scope.
New function becomes child of current function if any, and scope is linked
to previous scope which may be of parent function.
*/
static AstNode *new_function(struct parser_state *parser)
{
	AstNode *node = allocate_expr_ast_node(parser, EXPR_FUNCTION);
	set_type(&node->function_expr.type, RAVI_TFUNCTION);
	node->function_expr.is_method = false;
	node->function_expr.is_vararg = false;
	node->function_expr.need_close = false;
	node->function_expr.proc_id = 0;
	node->function_expr.args = NULL;
	node->function_expr.child_functions = NULL;
	node->function_expr.upvalues = NULL;
	node->function_expr.locals = NULL;
	node->function_expr.main_block = NULL;
	node->function_expr.function_statement_list = NULL;
	node->function_expr.parent_function = parser->current_function;
	if (parser->current_function) {
		// Make this function a child of current function
		add_ast_node(parser->container, &parser->current_function->function_expr.child_functions, node);
	}
	parser->current_function = node;
	new_scope(parser); /* Start function scope */
	return node;
}

/* Ends the function node and closes the scope for the function. The
 * function being closed becomes the current AST node, while parent function/scope
 * become current function/scope.
 */
static AstNode *end_function(struct parser_state *parser)
{
	assert(parser->current_function);
	end_scope(parser);
	AstNode *function = parser->current_function;
	parser->current_function = function->function_expr.parent_function;
	return function;
}

/* mainfunc() equivalent - parses a Lua script, also known as chunk.
The code is wrapped in a vararg function */
static void parse_lua_chunk(struct parser_state *parser)
{
	raviX_next(parser->ls);					 /* read first token */
	parser->container->main_function = new_function(parser); /* vararg function wrapper */
	parser->container->main_function->function_expr.is_vararg = true;
	add_upvalue_for_ENV(parser);
	parse_statement_list(parser, &parser->container->main_function->function_expr.function_statement_list);
	end_function(parser);
	assert(parser->current_function == NULL);
	assert(parser->current_scope == NULL);
	check(parser->ls, TOK_EOS);
}

static void parser_state_init(struct parser_state *parser, LexerState *ls, CompilerState *container)
{
	parser->ls = ls;
	parser->container = container;
	parser->current_function = NULL;
	parser->current_scope = NULL;
}

/*
** Parse the given source 'chunk' and build an abstract
** syntax tree; return 0 on success / non-zero return code on
** failure
*/
int raviX_parse(CompilerState *container, const char *buffer, size_t buflen, const char *name)
{
	LexerState *lexstate = raviX_init_lexer(container, buffer, buflen, name);
	struct parser_state parser_state;
	parser_state_init(&parser_state, lexstate, container);
	int rc = setjmp(container->env);
	if (rc == 0) {
		parse_lua_chunk(&parser_state);
	}
	raviX_destroy_lexer(lexstate);
	return rc;
}

/*
Return true if two strings are equal, false otherwise.
*/
static int string_equal(const void *a, const void *b)
{
	const StringObject *c1 = (const StringObject *)a;
	const StringObject *c2 = (const StringObject *)b;
	if (c1->len != c2->len || c1->hash != c2->hash)
		return 0;
	return memcmp(c1->str, c2->str, c1->len) == 0;
}

static uint32_t string_hash(const void *c)
{
	const StringObject *c1 = (const StringObject *)c;
	return c1->hash;
}

CompilerState *raviX_init_compiler()
{
	CompilerState *container = (CompilerState *)calloc(1, sizeof(CompilerState));
	raviX_allocator_init(&container->ast_node_allocator, "ast nodes", sizeof(AstNode), sizeof(double),
			     sizeof(AstNode) * 32);
	raviX_allocator_init(&container->ptrlist_allocator, "ptrlists", sizeof(struct ptr_list), sizeof(double),
			     sizeof(struct ptr_list) * 32);
	raviX_allocator_init(&container->block_scope_allocator, "block scopes", sizeof(Scope),
			     sizeof(double), sizeof(Scope) * 32);
	raviX_allocator_init(&container->symbol_allocator, "symbols", sizeof(LuaSymbol), sizeof(double),
			     sizeof(LuaSymbol) * 64);
	raviX_allocator_init(&container->string_allocator, "strings", 0, sizeof(double), 1024);
	raviX_allocator_init(&container->string_object_allocator, "string_objects", sizeof(StringObject),
			     sizeof(double), sizeof(StringObject) * 64);
	raviX_buffer_init(&container->buff, 1024);
	raviX_buffer_init(&container->error_message, 256);
	container->strings = raviX_set_create(string_hash, string_equal);
	container->main_function = NULL;
	container->killed = false;
	container->linearizer = NULL;
	container->_ENV = raviX_create_string(container, "_ENV", 4);
	return container;
}

// static void show_allocations(CompilerState *compiler)
//{
//	raviX_allocator_show_allocations(&compiler->symbol_allocator);
//	raviX_allocator_show_allocations(&compiler->block_scope_allocator);
//	raviX_allocator_show_allocations(&compiler->ast_node_allocator);
//	raviX_allocator_show_allocations(&compiler->ptrlist_allocator);
//	raviX_allocator_show_allocations(&compiler->string_allocator);
//	raviX_allocator_show_allocations(&compiler->string_object_allocator);
//}

void raviX_destroy_compiler(CompilerState *container)
{
	if (!container->killed) {
		// show_allocations(container);
		if (container->linearizer) {
			raviX_destroy_linearizer(container->linearizer);
			free(container->linearizer);
		}
		raviX_set_destroy(container->strings, NULL);
		raviX_buffer_free(&container->buff);
		raviX_buffer_free(&container->error_message);
		raviX_allocator_destroy(&container->symbol_allocator);
		raviX_allocator_destroy(&container->block_scope_allocator);
		raviX_allocator_destroy(&container->ast_node_allocator);
		raviX_allocator_destroy(&container->ptrlist_allocator);
		raviX_allocator_destroy(&container->string_allocator);
		raviX_allocator_destroy(&container->string_object_allocator);
		container->killed = true;
	}
	free(container);
}