#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "env.h"
#include "type.h"
#include "nspc.h"
#include "value.h"
#include "optim.h"
#include "parse.h"

#define FAKE_FUNC ((Func)1)

ANN m_bool scan0_class_def(const Env env, const Class_Def class_def);
ANN static m_bool scan1_exp(const Env env, Exp exp);
ANN static m_bool scan1_stmt_list(const Env env, Stmt_List list);
ANN m_bool scan1_class_def(const Env env, const Class_Def class_def);
ANN static m_bool scan1_stmt(const Env env, Stmt stmt);

ANN static inline m_bool check_array_empty(const Array_Sub a, const uint pos) {
  if(!a->exp)
    return 1;
  ERR_B(pos, "type must be defined with empty []'s")
}

ANN static void scan1_exp_decl_template(const Type t, const Exp_Decl* decl) {
  Exp_Decl* d = (Exp_Decl*)decl;
  d->base = t->def;
  d->type = t;
}

ANN static Type scan1_exp_decl_type(const Env env, const Exp_Decl* decl) {
  const Type t = known_type(env, decl->td, "declaration");
  CHECK_OO(t)
  if(!t->size)
    ERR_O(decl->self->pos, "cannot declare variables of size '0' (i.e. 'void')...")
  if(GET_FLAG(t, private) && t->owner != env->curr)
    ERR_O(decl->self->pos, "can't use private type %s", t->name)
  if(GET_FLAG(t, protect) &&
    (!env->class_def || isa(t, env->class_def) < 0))
    ERR_O(decl->self->pos, "can't use protected type %s", t->name)
  if(env->class_def) {
    if(!env->scope) {
      if(!env->func && !GET_FLAG(decl->td, static))
        SET_FLAG(decl->td, member);
      if(!GET_FLAG(decl->td, ref) && t == env->class_def)
        ERR_O(decl->self->pos, "...(note: object of type '%s' declared inside itself)", t->name)
    }
    if(GET_FLAG(decl->td, global) && env->class_def)
      UNSET_FLAG(decl->td, global);
  } else if(GET_FLAG(decl->td, private))
      ERR_O(decl->self->pos, "must declare private variables at class scope...")
  if(GET_FLAG(t, template))
    scan1_exp_decl_template(t, decl);
  return t;
}

ANN m_bool scan1_exp_decl(const Env env, const Exp_Decl* decl) { GWDEBUG_EXE
  CHECK_BB(env_access(env, decl->td->flag))
  env_storage(env, &decl->td->flag);
  Var_Decl_List list = decl->list;
  m_uint scope;
  ((Exp_Decl*)decl)->type = scan1_exp_decl_type(env, decl);
  CHECK_OB(decl->type)
  const m_bool global = GET_FLAG(decl->td, global);
  const Nspc nspc = !global ? env->curr : env->global_nspc;
  if(global)
    env_push(env, NULL, env->global_nspc, &scope);
  do {
    Type t;
    const Var_Decl var = list->self;
    const Value value = nspc_lookup_value0(env->curr, var->xid);
    if(isres(var->xid) > 0)
      ERR_B(var->pos, "\t... in variable declaration", s_name(var->xid))
    if(value && (!env->class_def ||
        (!GET_FLAG(env->class_def, template) || !GET_FLAG(env->class_def, scan1))))
      ERR_B(var->pos, "variable %s has already been defined in the same scope...",
              s_name(var->xid))
    if(!var->array)
      t = decl->type;
    else {
      if(var->array->exp)
        CHECK_BB(scan1_exp(env, var->array->exp))
      t = array_type(decl->type, var->array->depth);
    }
    var->value = value ? value : new_value(env->gwion, t, s_name(var->xid));
    nspc_add_value(nspc, var->xid, var->value);
    var->value->flag = decl->td->flag;
    if(var->array && !var->array->exp)
      SET_FLAG(var->value, ref);
    if(!env->func && !env->scope && !env->class_def)
      SET_FLAG(var->value, global);
    var->value->d.ptr = var->addr;
    var->value->owner = !env->func ? env->curr : NULL;
    var->value->owner_class = env->scope ? NULL : env->class_def;
  } while((list = list->next));
  ((Exp_Decl*)decl)->type = decl->list->self->value->type;
  if(global)
    env_pop(env, scope);
  return 1;
}

ANN static inline m_bool scan1_exp_binary(const Env env, const Exp_Binary* bin) { GWDEBUG_EXE
  CHECK_BB(scan1_exp(env, bin->lhs))
  return scan1_exp(env, bin->rhs);
}

ANN static inline m_bool scan1_exp_primary(const Env env, const Exp_Primary* prim) { GWDEBUG_EXE
  if(prim->primary_type == ae_primary_hack)
    CHECK_BB(scan1_exp(env, prim->d.exp))
  return 1;
}

ANN static inline m_bool scan1_exp_array(const Env env, const Exp_Array* array) { GWDEBUG_EXE
  CHECK_BB(scan1_exp(env, array->base))
  return scan1_exp(env, array->array->exp);
}

ANN static inline m_bool scan1_exp_cast(const Env env, const Exp_Cast* cast) { GWDEBUG_EXE
  return scan1_exp(env, cast->exp);
}

ANN static m_bool scan1_exp_post(const Env env, const Exp_Postfix* post) { GWDEBUG_EXE
  CHECK_BB(scan1_exp(env, post->exp))
  return post->exp->meta == ae_meta_var ? 1 :
    err_msg(post->exp->pos, "post operator '%s' cannot be used"
          " on non-mutable data-type...", op2str(post->op));
}

ANN static inline m_bool scan1_exp_dur(const Env env, const Exp_Dur* dur) { GWDEBUG_EXE
  CHECK_BB(scan1_exp(env, dur->base))
  return scan1_exp(env, dur->unit);
}

ANN static m_bool scan1_exp_call(const Env env, const Exp_Call* exp_call) { GWDEBUG_EXE
  if(exp_call->tmpl)
    return 1;
  CHECK_BB(scan1_exp(env, exp_call->func))
  const Exp args = exp_call->args;
  return args ? scan1_exp(env, args) : 1;
}

ANN static inline m_bool scan1_exp_dot(const Env env, const Exp_Dot* member) { GWDEBUG_EXE
  return scan1_exp(env, member->base);
}

ANN static m_bool scan1_exp_if(const Env env, const Exp_If* exp_if) { GWDEBUG_EXE
  CHECK_BB(scan1_exp(env, exp_if->cond))
  CHECK_BB(scan1_exp(env, exp_if->if_exp))
  return scan1_exp(env, exp_if->else_exp);
}

ANN static inline m_bool scan1_exp_unary(const restrict Env env, const Exp_Unary * unary) {
  if(unary->op == op_spork && unary->code)
    return scan1_stmt(env, unary->code);
  return 1;
}

HANDLE_EXP_FUNC(scan1, m_bool, 1)

#define describe_ret_nspc(name, type, prolog, exp) describe_stmt_func(scan1, name, type, prolog, exp)
describe_ret_nspc(flow, Stmt_Flow,, !(scan1_exp(env, stmt->cond) < 0 ||
    scan1_stmt(env, stmt->body) < 0) ? 1 : -1)
describe_ret_nspc(for, Stmt_For,, !(scan1_stmt(env, stmt->c1) < 0 ||
    scan1_stmt(env, stmt->c2) < 0 ||
    (stmt->c3 && scan1_exp(env, stmt->c3) < 0) ||
    scan1_stmt(env, stmt->body) < 0) ? 1 : -1)
describe_ret_nspc(auto, Stmt_Auto,, !(scan1_exp(env, stmt->exp) < 0 ||
    scan1_stmt(env, stmt->body) < 0) ? 1 : -1)
describe_ret_nspc(loop, Stmt_Loop,, !(scan1_exp(env, stmt->cond) < 0 ||
    scan1_stmt(env, stmt->body) < 0) ? 1 : -1)
describe_ret_nspc(switch, Stmt_Switch,, scan1_exp(env, stmt->val))
describe_ret_nspc(if, Stmt_If,, !(scan1_exp(env, stmt->cond) < 0 ||
    scan1_stmt(env, stmt->if_body) < 0 ||
    (stmt->else_body && scan1_stmt(env, stmt->else_body) < 0)) ? 1 : -1)

ANN static inline m_bool scan1_stmt_code(const Env env, const Stmt_Code stmt) { GWDEBUG_EXE
  if(stmt->stmt_list) {
    RET_NSPC(scan1_stmt_list(env, stmt->stmt_list))
  }
  return 1;
}

ANN static inline m_bool scan1_stmt_exp(const Env env, const Stmt_Exp stmt) { GWDEBUG_EXE
  return stmt->val ? scan1_exp(env, stmt->val) : 1;
}

ANN static inline m_bool scan1_stmt_case(const Env env, const Stmt_Exp stmt) { GWDEBUG_EXE
  return scan1_exp(env, stmt->val);
}

ANN m_bool scan1_stmt_enum(const Env env, const Stmt_Enum stmt) { GWDEBUG_EXE
  ID_List list = stmt->list;
  do {
    CHECK_BB(already_defined(env, list->xid, stmt->self->pos))
    const Value v = new_value(env->gwion, stmt->t, s_name(list->xid));
    if(env->class_def) {
      v->owner_class = env->class_def;
      v->owner = env->curr;
      SET_FLAG(v, static);
      if(GET_FLAG(stmt, private))
        SET_FLAG(v, private);
    }
    SET_FLAG(v, const | ae_flag_enum | ae_flag_checked);
    nspc_add_value(stmt->t->owner, list->xid, v);
    vector_add(&stmt->values, (vtype)v);
  } while((list = list->next));
  return 1;
}

ANN static m_bool scan1_func_def_args(const Env env, Arg_List list) { GWDEBUG_EXE
  do {
    if(list->var_decl->array)
      CHECK_BB(check_array_empty(list->var_decl->array, list->var_decl->pos))
    CHECK_OB((list->type = known_type(env, list->td, "function argument")))
  } while((list = list->next));
  return 1;
}

ANN m_bool scan1_stmt_fptr(const Env env, const Stmt_Fptr ptr) { GWDEBUG_EXE
  if(ptr->td->array)
    CHECK_BB(check_array_empty(ptr->td->array, ptr->td->xid->pos))
  CHECK_OB((ptr->ret_type = known_type(env, ptr->td, "func pointer definition")))
  if(ptr->args && scan1_func_def_args(env, ptr->args) < 0)
    ERR_B(ptr->td->xid->pos, "\t... in typedef '%s'...", s_name(ptr->xid))
  return 1;
}

ANN static inline m_bool scan1_stmt_type(const Env env, const Stmt_Type stmt) { GWDEBUG_EXE
  return stmt->type->def ? scan1_class_def(env, stmt->type->def) : 1;
}

ANN static inline m_bool scan1_stmt_union_array(const Array_Sub array) { GWDEBUG_EXE
  if(array->exp)
    ERR_B(array->exp->pos, "array declaration must be empty in union.")
  return 1;
}

ANN m_bool scan1_stmt_union(const Env env, const Stmt_Union stmt) { GWDEBUG_EXE
  Decl_List l = stmt->l;
  const m_uint scope = union_push(env, stmt);
  if(stmt->xid)
    UNSET_FLAG(stmt, private);
  else if(stmt->type_xid)
    UNSET_FLAG(stmt, private);
  do {
    const Exp_Decl decl = l->self->d.exp_decl;
    Var_Decl_List list = decl.list;
    SET_FLAG(decl.td, checked | stmt->flag);
    if(GET_FLAG(stmt, static))
      SET_FLAG(decl.td, static);
    do {
      const Var_Decl var_decl = list->self;
      if(var_decl->array)
        CHECK_BB(scan1_stmt_union_array(var_decl->array))
    } while((list = list->next));
    CHECK_BB(scan1_exp_decl(env, &l->self->d.exp_decl))
  } while((l = l->next));
  union_pop(env, stmt, scope);
  return 1;
}

static const _exp_func stmt_func[] = {
  (_exp_func)scan1_stmt_exp,  (_exp_func)scan1_stmt_flow, (_exp_func)scan1_stmt_flow,
  (_exp_func)scan1_stmt_for,  (_exp_func)scan1_stmt_auto, (_exp_func)scan1_stmt_loop,
  (_exp_func)scan1_stmt_if,   (_exp_func)scan1_stmt_code, (_exp_func)scan1_stmt_switch,
  (_exp_func)dummy_func,      (_exp_func)dummy_func,      (_exp_func)scan1_stmt_exp,
  (_exp_func)scan1_stmt_case, (_exp_func)dummy_func,      (_exp_func)scan1_stmt_enum,
  (_exp_func)scan1_stmt_fptr, (_exp_func)scan1_stmt_type, (_exp_func)scan1_stmt_union,
};

ANN static inline m_bool scan1_stmt(const Env env, const Stmt stmt) { GWDEBUG_EXE
  return stmt_func[stmt->stmt_type](env, &stmt->d);
}

ANN static m_bool scan1_stmt_list(const Env env, Stmt_List l) { GWDEBUG_EXE
  do {
    CHECK_BB(scan1_stmt(env, l->stmt))
    if(l->next) {
      if(l->stmt->stmt_type != ae_stmt_return) {
        if(l->next->stmt->stmt_type == ae_stmt_exp &&
          !l->next->stmt->d.stmt_exp.val) {
           Stmt_List next = l->next;
           l->next = l->next->next;
           next->next = NULL;
           free_stmt_list(next);
        }
      } else {
        Stmt_List tmp = l->next;
        l->next = NULL;
        free_stmt_list(tmp);
      }
    }
  } while((l = l->next));
  return 1;
}

ANN static m_bool scan1_func_def_type(const Env env, const Func_Def f) { GWDEBUG_EXE
  if(f->td->array)
    CHECK_BB(check_array_empty(f->td->array, f->td->xid->pos))
  CHECK_OB((f->ret_type = known_type(env, f->td, "function return")))
  return 1;
}

ANN static m_bool scan1_func_def_op(const Func_Def f) { GWDEBUG_EXE
  m_int count = 0;
  Arg_List list = f->arg_list;
  while(list) {
    ++count;
    list = list->next;
  }
  if(count > (GET_FLAG(f, unary) ? 1 : 2) || !count)
    ERR_B(f->td->xid->pos, "operators can only have one or two arguments")
  return 1;
}

ANN static m_bool scan1_func_def_flag(const Env env, const Func_Def f) { GWDEBUG_EXE
  if(GET_FLAG(f, dtor) && !env->class_def)
    ERR_B(f->td->xid->pos, "dtor must be in class def!!")
  else if(GET_FLAG(f, op))
    CHECK_BB(scan1_func_def_op(f))
  return 1;
}

ANN m_bool scan1_func_def(const Env env, const Func_Def f) { GWDEBUG_EXE
  CHECK_BB(env_access(env, f->flag))
  env_storage(env, &f->flag);
  if(tmpl_list_base(f->tmpl))
    return 1;
  const Func former = env->func;
  env->func = FAKE_FUNC;
  ++env->scope;
  CHECK_BB(scan1_func_def_flag(env, f))
  CHECK_BB(scan1_func_def_type(env, f))
  if(f->arg_list)
    CHECK_BB(scan1_func_def_args(env, f->arg_list))
  if(!GET_FLAG(f, builtin))
    CHECK_BB(scan1_stmt_code(env, &f->d.code->d.stmt_code))
  if(GET_FLAG(f, op) && env->class_def)
    SET_FLAG(f, static);
  env->func = former;
  --env->scope;
  return 1;
}

DECL_SECTION_FUNC(scan1)

ANN static m_bool scan1_class_parent(const Env env, const Class_Def class_def) {
  if(class_def->ext->array) {
    if(class_def->ext->array->exp)
      CHECK_BB(scan1_exp(env, class_def->ext->array->exp))
    else
      ERR_B(class_def->ext->xid->pos, "can't use empty []'s in class extend")
  }
  const Type parent = class_def->type->parent = known_type(env, class_def->ext, "class definition");
  CHECK_OB(parent)
  if(!GET_FLAG(parent, scan1) && parent->def)
    CHECK_BB(scan1_class_def(env, parent->def))
  if(type_ref(parent))
    ERR_B(class_def->ext->xid->pos, "can't use ref type in class extend")
  return 1;
}

ANN static m_bool scan1_class_body(const Env env, const Class_Def class_def) {
  m_uint scope;
  env_push(env, class_def->type, class_def->type->nspc, &scope);
  Class_Body body = class_def->body;
  do CHECK_BB(scan1_section(env, body->section))
  while((body = body->next));
  env_pop(env, scope);
  return 1;
}

ANN m_bool scan1_class_def(const Env env, const Class_Def class_def) { GWDEBUG_EXE
  if(tmpl_class_base(class_def->tmpl))
    return 1;
  if(class_def->ext)
    CHECK_BB(scan1_class_parent(env, class_def))
  if(class_def->body)
    CHECK_BB(scan1_class_body(env, class_def))
  SET_FLAG(class_def->type, scan1);
  return 1;
}

ANN m_bool scan1_ast(const Env env, Ast ast) { GWDEBUG_EXE
  do CHECK_BB(scan1_section(env, ast->section))
  while((ast = ast->next));
  return 1;
}
