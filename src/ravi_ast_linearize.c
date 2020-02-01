/*
Copyright (C) 2018-2020 Dibyendu Majumdar
*/

#include <ravi_ast.h>
#include <ptrlist.h>

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/* Linearizer - WIP  */
static void ravi_init_linearizer(struct linearizer *linearizer, struct ast_container *container) {
  memset(linearizer, 0, sizeof *linearizer);
  linearizer->ast_container = container;
  dmrC_allocator_init(&linearizer->edge_allocator, "edge_allocator", sizeof(struct edge), sizeof(double), CHUNK);
  dmrC_allocator_init(&linearizer->instruction_allocator, "instruction_allocator", sizeof(struct instruction),
                      sizeof(double), CHUNK);
  dmrC_allocator_init(&linearizer->ptrlist_allocator, "ptrlist_allocator", sizeof(struct ptr_list), sizeof(double),
                      CHUNK);
  dmrC_allocator_init(&linearizer->pseudo_allocator, "pseudo_allocator", sizeof(struct pseudo), sizeof(double), CHUNK);
  dmrC_allocator_init(&linearizer->basic_block_allocator, "basic_block_allocator", sizeof(struct basic_block),
                      sizeof(double), CHUNK);
  dmrC_allocator_init(&linearizer->proc_allocator, "proc_allocator", sizeof(struct proc), sizeof(double), CHUNK);
}

static void ravi_destroy_linearizer(struct linearizer *linearizer) {
  dmrC_allocator_destroy(&linearizer->edge_allocator);
  dmrC_allocator_destroy(&linearizer->instruction_allocator);
  dmrC_allocator_destroy(&linearizer->ptrlist_allocator);
  dmrC_allocator_destroy(&linearizer->pseudo_allocator);
  dmrC_allocator_destroy(&linearizer->basic_block_allocator);
  dmrC_allocator_destroy(&linearizer->proc_allocator);
}

/**
 * Allocate a new proc. If there is a current proc, then the new proc gets added to the
 * current procs children.
 */
static struct proc *allocate_proc(struct linearizer *linearizer, struct ast_node *function_expr) {
  assert(function_expr->type == AST_FUNCTION_EXPR);
  struct proc *proc = dmrC_allocator_allocate(&linearizer->proc_allocator, 0);
  proc->function_expr = function_expr;
  ptrlist_add(&linearizer->all_procs, proc, &linearizer->ptrlist_allocator);
  if (linearizer->current_proc) {
    proc->parent = linearizer->current_proc;
    ptrlist_add(&linearizer->current_proc, proc, &linearizer->ptrlist_allocator);
  }
  return proc;
}

static void set_main_proc(struct linearizer *linearizer, struct proc *proc) {
  assert(linearizer->main_proc == NULL);
  assert(linearizer->current_proc == NULL);
  linearizer->main_proc = proc;
  assert(proc->function_expr->function_expr.parent_function == NULL);
}

static inline void set_current_proc(struct linearizer *linearizer, struct proc *proc) {
  linearizer->current_proc = proc;
}

void raviA_ast_linearize(struct linearizer *linearizer, struct ast_container *container) {
  ravi_init_linearizer(linearizer, container);
  struct proc *proc = allocate_proc(linearizer, container->main_function);

  ravi_destroy_linearizer(linearizer);
}