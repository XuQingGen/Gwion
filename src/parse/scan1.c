#include "defs.h"
#include "err_msg.h"
#include "absyn.h"
#include "type.h"
#include "traverse.h"

m_bool scan0_class_def(Env env, Class_Def class_def);
static m_bool scan1_exp(Env env, Exp exp);
static m_bool scan1_stmt_list(Env env, Stmt_List list);
m_bool scan1_class_def(Env env, Class_Def class_def);
static m_bool scan1_stmt(Env env, Stmt stmt);

Type scan_type(Env env, Type t, Type_Decl* type) {
  if(GET_FLAG(t, ae_flag_template)) {
    if(!type->types)
      CHECK_BO(err_msg(SCAN1_, type->pos, "you must provide template types"))
    CHECK_BO(template_push_types(env, t->e.def->types, type->types))
    Class_Def a = template_class(env, t->e.def, type->types);
    if(a->type) {
      nspc_pop_type(env->curr);
      return a->type;
    }
    CHECK_BO(scan0_class_def(env, a))
    SET_FLAG(a->type, ae_flag_template);
    SET_FLAG(a->type, ae_flag_ref);
    if(GET_FLAG(t, ae_flag_builtin))
      SET_FLAG(a->type, ae_flag_builtin);
    CHECK_BO(scan1_class_def(env, a))
    nspc_pop_type(env->curr);
    a->tref = t->e.def->types;
    a->base = type->types;
    t = a->type;
  } else if(type->types)
      CHECK_BO(err_msg(SCAN1_, type->pos,
            "type '%s' is not template. You should not provide template types", t->name))
  return t;
}

m_bool type_unknown(ID_List id, m_str orig) {
  char path[id_list_len(id)];
  type_path(path, id);
  CHECK_BB(err_msg(SCAN1_, id->pos,
        "'%s' unknown type in %s", path, orig))
  return -1;
}

static m_bool scan1_exp_decl_template(Env env, Type t, Exp_Decl* decl) {
  CHECK_OB((t = scan_type(env, t, decl->type)))
  if(GET_FLAG(t, ae_flag_template)) {
    decl->base = t->e.def;
    decl->m_type = t;
  }
  if(env->class_def && GET_FLAG(env->class_def, ae_flag_template))
    decl->num_decl = 0;
  return 1;
}

static Type scan1_exp_decl_type(Env env, Exp_Decl* decl) {
  Type t = find_type(env, decl->type->xid);
  if(!t)
    CHECK_BO(type_unknown(decl->type->xid, "declaration"))
  if(!t->size)
    CHECK_BO(err_msg(SCAN1_, decl->pos,
          "cannot declare variables of size '0' (i.e. 'void')..."))
  if(!GET_FLAG(decl->type, ae_flag_ref)) {
    if(env->class_def && (t == env->class_def) && !env->class_scope)
      CHECK_BO(err_msg(SCAN1_, decl->pos,
            "...(note: object of type '%s' declared inside itself)", t->name))
  } else if(isprim(t) > 0 && !decl->type->array)
    CHECK_BO(err_msg(SCAN1_, decl->pos,
          "cannot declare references (@) of primitive type '%s'...\n"
          "\t...(primitive types: 'int', 'float', 'time', 'dur')", t->name))
  return t;
}

m_bool scan1_exp_decl(Env env, Exp_Decl* decl) {
  Var_Decl_List list = decl->list;
  Var_Decl var_decl = NULL;
  Type t = scan1_exp_decl_type(env, decl);

  CHECK_OB(t)
  CHECK_BB(scan1_exp_decl_template(env, t, decl))
  if(decl->m_type)
    t = decl->m_type;
  while(list) {
    Value value;
    var_decl = list->self;
    if(isres(list->self->xid, list->self->pos) > 0)
      CHECK_BB(err_msg(SCAN2_, list->self->pos,
            "\t... in variable declaration", s_name(list->self->xid)))
    if((value = nspc_lookup_value0(env->curr, list->self->xid)) &&
      !(env->class_def && GET_FLAG(env->class_def, ae_flag_template)))
        CHECK_BB(err_msg(SCAN1_, list->self->pos,
              "variable %s has already been defined in the same scope...",
              s_name(list->self->xid)))
    decl->num_decl++;
    if(t->array_depth && !var_decl->array) { // catch typedef xxx[] before making array type
      ADD_REF(t);
      if(!t->e.exp_list)
        SET_FLAG(decl->type, ae_flag_ref);
    }
    if(var_decl->array) { // get depth, including typedef
      m_uint depth = list->self->array->depth;
      Type t3, t2 = t;
      t3 = t;
      while(t3) {
        depth += t3->array_depth;
        t3 = t3->d.array_type;
      }
      if(var_decl->array->exp_list)
        CHECK_BB(scan1_exp(env, var_decl->array->exp_list))
      t = new_array_type(env, depth, t2, env->curr);
      if(!list->self->array->exp_list)
        SET_FLAG(decl->type, ae_flag_ref);
      if(t2->array_depth) {
        if(!t2->e.exp_list)
          SET_FLAG(t, ae_flag_ref);
        SET_FLAG(t, ae_flag_typedef);
      }
    }
    if(!list->self->value)
      list->self->value = new_value(t, s_name(list->self->xid));
    else
      list->self->value->m_type = t;
    if(GET_FLAG(decl->type, ae_flag_private)) {
      if(!env->class_def)
        CHECK_BB(err_msg(SCAN2_, list->self->pos,
              "must declare private variables at class scope..."))
      SET_FLAG(list->self->value, ae_flag_private);
    }
    if(GET_FLAG(decl->type, ae_flag_const))
      SET_FLAG(list->self->value, ae_flag_const);
    if(env->class_def && !env->class_scope && !env->func && !GET_FLAG(decl->type, ae_flag_static))
      SET_FLAG(list->self->value, ae_flag_member);
    if(!env->class_def && !env->func && !env->class_scope)
      SET_FLAG(list->self->value, ae_flag_global);
    if(env->func == (Func)1)
      ADD_REF(list->self->value)

    list->self->value->ptr = list->self->addr;
    list->self->value->owner = env->curr;
    list->self->value->owner_class = env->func ? NULL : env->class_def;
    nspc_add_value(env->curr, list->self->xid, list->self->value);
    list = list->next;
  }
  if(!decl->m_type)
    decl->m_type = t;
  return 1;
}

static  m_bool scan1_exp_binary(Env env, Exp_Binary* binary) {
  CHECK_BB(scan1_exp(env, binary->lhs))
  CHECK_BB(scan1_exp(env, binary->rhs))
  return 1;
}

static  m_bool scan1_exp_primary(Env env, Exp_Primary* prim) {
  if(prim->type == ae_primary_hack)
    CHECK_BB(scan1_exp(env, prim->d.exp))
    return 1;
}

static m_bool scan1_exp_array(Env env, Exp_Array* array) {
  CHECK_BB(scan1_exp(env, array->base))
  CHECK_BB(scan1_exp(env, array->indices->exp_list))
  return 1;
}

static m_bool scan1_exp_cast(Env env, Exp_Cast* cast) {
  CHECK_BB(scan1_exp(env, cast->exp))
  return 1;
}

static m_bool scan1_exp_post(Env env, Exp_Postfix* post) {
  CHECK_BB(scan1_exp(env, post->exp))
  if(post->exp->meta != ae_meta_var)
    CHECK_BB(err_msg(SCAN1_, post->exp->pos,
          "post operator '%s' cannot be used"
          " on non-mutable data-type...", op2str(post->op)))
  return 1;
}

static m_bool scan1_exp_dur(Env env, Exp_Dur* dur) {
  CHECK_BB(scan1_exp(env, dur->base))
  CHECK_BB(scan1_exp(env, dur->unit))
  return 1;
}

static m_bool scan1_exp_call(Env env, Exp_Func* exp_func) {
  Exp args = exp_func->args;
  if(exp_func->types)
    return 1;
  CHECK_BB(scan1_exp(env, exp_func->func))
  CHECK_BB(args && scan1_exp(env, args))
  return 1;
}

static m_bool scan1_exp_dot(Env env, Exp_Dot* member) {
  CHECK_BB(scan1_exp(env, member->base));
  return 1;
}

static m_bool scan1_exp_if(Env env, Exp_If* exp_if) {
  CHECK_BB(scan1_exp(env, exp_if->cond))
  CHECK_BB(scan1_exp(env, exp_if->if_exp))
  CHECK_BB(scan1_exp(env, exp_if->else_exp))
  return 1;
}

static m_bool scan1_exp_spork(Env env, Stmt code) {
  Func f = env->func;
  env->func = f ? f: (Func)2;
  CHECK_BB(scan1_stmt(env, code))
  env->func = f;
  return 1;
}

static m_bool scan1_exp(Env env, Exp exp) {
  while(exp) {
    switch(exp->exp_type) {
      case ae_exp_primary:
        CHECK_BB(scan1_exp_primary(env, &exp->d.exp_primary))
        break;
      case ae_exp_decl:
        CHECK_BB(scan1_exp_decl(env, &exp->d.exp_decl))
        break;
      case ae_exp_unary:
        if(exp->d.exp_unary.op == op_spork && exp->d.exp_unary.code)
          CHECK_BB(scan1_exp_spork(env, exp->d.exp_unary.code))
        break;
      case ae_exp_binary:
        CHECK_BB(scan1_exp_binary(env, &exp->d.exp_binary))
        break;
      case ae_exp_post:
        CHECK_BB(scan1_exp_post(env, &exp->d.exp_post))
        break;
      case ae_exp_cast:
        CHECK_BB(scan1_exp_cast(env, &exp->d.exp_cast))
        break;
      case ae_exp_call:
        CHECK_BB(scan1_exp_call(env, &exp->d.exp_func))
        break;
      case ae_exp_array:
        CHECK_BB(scan1_exp_array(env, &exp->d.exp_array))
        break;
      case ae_exp_dot:
        CHECK_BB(scan1_exp_dot(env, &exp->d.exp_dot))
        break;
      case ae_exp_dur:
        CHECK_BB(scan1_exp_dur(env, &exp->d.exp_dur))
        break;
      case ae_exp_if:
        CHECK_BB(scan1_exp_if(env, &exp->d.exp_if))
        break;
    }
    exp = exp->next;
    if(exp&& exp->exp_type == ae_exp_decl)
      CHECK_BB(err_msg(SCAN1_, exp->pos, "can't declare after expression"))
  }
  return 1;
}

static m_bool scan1_stmt_code(Env env, Stmt_Code stmt, m_bool push) {
  m_bool ret;
  env->class_scope++;
  if(push)
    nspc_push_value(env->curr);
  ret = scan1_stmt_list(env, stmt->stmt_list);
  if(push)
    nspc_pop_value(env->curr);
  env->class_scope--;
  return ret;
}

static m_bool scan1_stmt_return(Env env, Stmt_Return stmt) {
  return stmt->val ? scan1_exp(env, stmt->val) : 1;
}

static m_bool scan1_stmt_flow(Env env, struct Stmt_Flow_* stmt) {
  CHECK_BB(scan1_exp(env, stmt->cond))
  CHECK_BB(scan1_stmt(env, stmt->body))
  return 1;
}

static m_bool scan1_stmt_for(Env env, Stmt_For stmt) {
  Func f = env->func;
  if(env->class_def && !env->func)
   env->func = (Func)2;
  nspc_push_value(env->curr);
  CHECK_BB(scan1_stmt(env, stmt->c1))
  CHECK_BB(scan1_stmt(env, stmt->c2))
  CHECK_BB(scan1_exp(env, stmt->c3))
  CHECK_BB(scan1_stmt(env, stmt->body))
  nspc_pop_value(env->curr);
  env->func = f;
  return 1;
}

static m_bool scan1_stmt_auto(Env env, Stmt_Auto stmt) {
  CHECK_BB(scan1_exp(env, stmt->exp))
  CHECK_BB(scan1_stmt(env, stmt->body))
  return 1;
}

static m_bool scan1_stmt_loop(Env env, Stmt_Loop stmt) {
  CHECK_BB(scan1_exp(env, stmt->cond))
  CHECK_BB(scan1_stmt(env, stmt->body))
  return 1;
}

static m_bool scan1_stmt_switch(Env env, Stmt_Switch stmt) {
  return scan1_exp(env, stmt->val);
}

static m_bool scan1_stmt_case(Env env, Stmt_Case stmt) {
  return scan1_exp(env, stmt->val);
}

static m_bool scan1_stmt_if(Env env, Stmt_If stmt) {
  CHECK_BB(scan1_exp(env, stmt->cond))
  CHECK_BB(scan1_stmt(env, stmt->if_body))
  if(stmt->else_body)
    CHECK_BB(scan1_stmt(env, stmt->else_body))
    return 1;
}

m_bool scan1_stmt_enum(Env env, Stmt_Enum stmt) {
  ID_List list = stmt->list;
  m_uint count = 1;
  while(list) {
    Value v;
    if(nspc_lookup_value0(env->curr, list->xid))
      CHECK_BB(err_msg(SCAN1_, stmt->pos,
            "in enum argument %i '%s' already declared as variable",
            count, s_name(list->xid)))
    v = new_value(stmt->t, s_name(list->xid));
    if(env->class_def) {
      v->owner_class = env->class_def;
      SET_FLAG(v, ae_flag_static);
      if(GET_FLAG(stmt, ae_flag_private))
       SET_FLAG(v, ae_flag_private);
    }
    SET_FLAG(v, ae_flag_const | ae_flag_enum | ae_flag_checked);
    nspc_add_value(env->curr, list->xid, v);
    vector_add(&stmt->values, (vtype)v);
    list = list->next;
    count++;
  }
  return 1;
}

static m_int scan1_func_def_args(Env env, Arg_List arg_list) {
  m_int count = 0;
  while(arg_list) {
    count++;
    if(!(arg_list->type = find_type(env, arg_list->type_decl->xid)))
      CHECK_BB(type_unknown(arg_list->type_decl->xid, "function argument")) // TODO: use count
    CHECK_OB((arg_list->type = scan_type(env, arg_list->type, arg_list->type_decl)))
    if(arg_list->type_decl->types)
      ADD_REF(arg_list->type)
    arg_list = arg_list->next;
  }
  return count;
}

m_bool scan1_stmt_fptr(Env env, Stmt_Ptr ptr) {
  if(!(ptr->ret_type = find_type(env, ptr->type->xid)))
      CHECK_BB(type_unknown(ptr->type->xid, "function pointer return type")) // TODO: use count
  CHECK_OB((ptr->ret_type = scan_type(env, ptr->ret_type, ptr->type)))
  if(!env->class_def && GET_FLAG(ptr, ae_flag_static))
    CHECK_BB(err_msg(SCAN1_, ptr->pos,
          "can't declare func pointer static outside of a class"))
  if(scan1_func_def_args(env, ptr->args) < 0)
    CHECK_BB(err_msg(SCAN1_, ptr->pos,
          "\t... in typedef '%s'...", s_name(ptr->xid)))
  return 1;
}

static m_bool scan1_stmt_type(Env env, Stmt_Typedef stmt) {
  if(stmt->type->array)
    return scan1_exp(env, stmt->type->array->exp_list);
  return 1;
}

static m_bool scan1_stmt_union_array(Array_Sub array) {
  if(array->exp_list)
    CHECK_BB(err_msg(SCAN1_, array->pos,
      "array declaration must be empty in union."))
  return 1;
}

m_bool scan1_stmt_union(Env env, Stmt_Union stmt) {
  Decl_List l = stmt->l;

  if(stmt->xid) {
    UNSET_FLAG(stmt, ae_flag_private);
    env_push_class(env, stmt->value->m_type);
  }
  while(l) {
    Var_Decl_List list = l->self->d.exp_decl.list;

    if(l->self->exp_type != ae_exp_decl)
      CHECK_BB(err_msg(SCAN1_, stmt->pos,
            "invalid expression type '%i' in union declaration."))
    SET_FLAG(l->self->d.exp_decl.type, ae_flag_checked | stmt->flag);
    if(GET_FLAG(stmt, ae_flag_static))
      SET_FLAG(l->self->d.exp_decl.type, ae_flag_static);
    while(list) {
      Var_Decl var_decl = list->self;
      if(var_decl->array)
        CHECK_BB(scan1_stmt_union_array(var_decl->array))
      list = list->next;
    }
    CHECK_BB(scan1_exp_decl(env, &l->self->d.exp_decl))
    l = l->next;
  }
  if(stmt->xid)
    env_pop_class(env);
  return 1;
}

static m_bool scan1_stmt(Env env, Stmt stmt) {
  m_bool ret = -1;

  switch(stmt->type) {
    case ae_stmt_exp:
      ret = scan1_exp(env, stmt->d.stmt_exp.val);
      break;
    case ae_stmt_code:
      SCOPE(ret = scan1_stmt_code(env, &stmt->d.stmt_code, 1))
      break;
    case ae_stmt_return:
      ret = scan1_stmt_return(env, &stmt->d.stmt_return);
      break;
    case ae_stmt_if:
      NSPC(ret = scan1_stmt_if(env, &stmt->d.stmt_if))
      break;
    case ae_stmt_while:
      NSPC(ret = scan1_stmt_flow(env, &stmt->d.stmt_while))
      break;
    case ae_stmt_for:
      NSPC(ret = scan1_stmt_for(env, &stmt->d.stmt_for))
      break;
    case ae_stmt_auto:
      NSPC(ret = scan1_stmt_auto(env, &stmt->d.stmt_auto))
      break;
    case ae_stmt_until:
      NSPC(ret = scan1_stmt_flow(env, &stmt->d.stmt_until))
      break;
    case ae_stmt_loop:
      NSPC(ret = scan1_stmt_loop(env, &stmt->d.stmt_loop))
      break;
    case ae_stmt_switch:
      NSPC(ret = scan1_stmt_switch(env, &stmt->d.stmt_switch))
      break;
    case ae_stmt_case:
      ret = scan1_stmt_case(env, &stmt->d.stmt_case);
      break;
    case ae_stmt_enum:
      ret = scan1_stmt_enum(env, &stmt->d.stmt_enum);
      break;
    case ae_stmt_continue:
    case ae_stmt_break:
    case ae_stmt_gotolabel:
      ret = 1;
      break;
    case ae_stmt_funcptr:
      ret = scan1_stmt_fptr(env, &stmt->d.stmt_ptr);
      break;
    case ae_stmt_typedef:
      ret = scan1_stmt_type(env, &stmt->d.stmt_type);
      break;
    case ae_stmt_union:
      ret = scan1_stmt_union(env, &stmt->d.stmt_union);
      break;
  }
  return ret;
}

static m_bool scan1_stmt_list(Env env, Stmt_List list) {
  Stmt_List curr = list;
  while(curr) {
    CHECK_BB(scan1_stmt(env, curr->stmt))
    curr = curr->next;
  }
  return 1;
}

static m_int scan1_func_def_array(Env env, Func_Def f) {
  Type t = NULL;
  Type t2 = f->ret_type;

  if(f->type_decl->array->exp_list)
    CHECK_BB(err_msg(SCAN1_, f->type_decl->array->pos,
      "in function '%s':\n\treturn array type must be defined with empty []'s",
      s_name(f->name)))
  t = new_array_type(env, f->type_decl->array->depth, t2, env->curr);
  f->ret_type = t;
  return 1;
}

static m_bool scan1_func_def_type(Env env, Func_Def f) {
  if(!(f->ret_type = find_type(env, f->type_decl->xid)))
      CHECK_BB(type_unknown(f->type_decl->xid, "function return type"))
  CHECK_OB((f->ret_type = scan_type(env, f->ret_type, f->type_decl)))
  if(f->type_decl->array)
    CHECK_BB(scan1_func_def_array(env, f))
  return 1;
}

static m_bool scan1_func_def_op(Env env, Func_Def f) {
  m_int count = 0;
  Arg_List list = f->arg_list;
  while(list) {
    count++;
    list = list->next;
  }
  if(count > (GET_FLAG(f, ae_flag_unary) ? 1 : 2) || !count)
    CHECK_BB(err_msg(SCAN1_, f->pos,
          "operators can only have one or two arguments"))
  return 1;
}

static m_bool scan1_func_def_flag(Env env, Func_Def f) {
  if(GET_FLAG(f, ae_flag_dtor) && !env->class_def)
    CHECK_BB(err_msg(SCAN1_, f->pos, "dtor must be in class def!!"))
  if(GET_FLAG(f, ae_flag_op))
    CHECK_BB(scan1_func_def_op(env, f))
/*  else if(name2op(s_name(f->name)) > 0)
      CHECK_BB(err_msg(SCAN1_, f->pos,
                "'%s' is a reserved operator name", s_name(f->name))) */
  return 1;
}

static m_bool scan1_func_def_code(Env env, Func_Def f) {
  m_bool ret;
  nspc_push_value(env->curr);
  ret = scan1_stmt_code(env, &f->code->d.stmt_code, 0);
  nspc_pop_value(env->curr);
  return ret;
}

m_bool scan1_func_def(Env env, Func_Def f) {
  if(!env->class_def && GET_FLAG(f, ae_flag_private))
    CHECK_BB(err_msg(SCAN1_, f->pos, "can't declare func '%s' private outside of class", s_name(f->name)))
  if(f->types)
    return 1;
  env->func = (Func)2;
  if(scan1_func_def_flag(env, f) < 0 ||
     scan1_func_def_type(env, f) < 0 ||
    (f->arg_list && scan1_func_def_args(env, f->arg_list) < 0) ||
    (f->code && scan1_func_def_code(env, f) < 0))
    CHECK_BB(err_msg(SCAN1_, f->pos, "\t...in function '%s'", s_name(f->name)))
  env->func = NULL;
  return 1;
}

static m_bool scan1_section(Env env, Section* section) {
  ae_Section_Type t = section->type;
  if(t == ae_section_stmt)
    CHECK_BB(scan1_stmt_list(env, section->d.stmt_list))
  else if(t == ae_section_func)
    CHECK_BB(scan1_func_def(env, section->d.func_def))
  else if(t == ae_section_class)
    CHECK_BB(scan1_class_def(env, section->d.class_def))
  return 1;
}

m_bool scan1_class_def(Env env, Class_Def class_def) {
  Class_Body body = class_def->body;

  if(class_def->types)
    return 1;
  CHECK_BB(env_push_class(env, class_def->type))
  while(body) {
    CHECK_BB(scan1_section(env, body->section))
    body = body->next;
  }
  CHECK_BB(env_pop_class(env))
  return 1;
}

m_bool scan1_ast(Env env, Ast ast) {
  while(ast) {
    CHECK_BB(scan1_section(env, ast->section))
    ast = ast->next;
  }
  return 1;
}
