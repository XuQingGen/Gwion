#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "traverse.h"
#include "parse.h"
#include "gwion.h"
#include "clean.h"
#include "object.h"

ANN static inline m_bool freeable(const Type a) {
  return !(tflag(a, tflag_force) || tflag(a, tflag_nonnull)) && (tflag(a, tflag_tmpl) ||GET_FLAG(a, global));
}

ANN void free_type(const Type a, struct Gwion_ *const gwion) {
  if(freeable(a)) {
    if(tflag(a, tflag_udef))
      free_union_def(gwion->mp, a->info->udef);
    if(tflag(a, tflag_cdef))
      class_def_cleaner(gwion, a->info->cdef);
  }
  if(a->nspc)
    nspc_remref(a->nspc, gwion);
  if(a->info->tuple)
    free_tupleform(a->info->tuple, gwion);
  mp_free(gwion->mp, TypeInfo, a->info);
  mp_free(gwion->mp, Type, a);

}

Type new_type(MemPool p, const m_uint xid, const m_str name, const Type parent) {
  const Type type = mp_calloc(p, Type);
  type->xid  = xid;
  type->name = name;
  type->info = mp_calloc(p, TypeInfo);
  type->info->parent = parent;
  if(parent)
    type->size = parent->size;
  type->ref = 1;
  return type;
}

ANN Type type_copy(MemPool p, const Type type) {
  const Type a = new_type(p, type->xid, type->name, type->info->parent);
  a->nspc           = type->nspc;
  a->info->owner       = type->info->owner;
  a->info->owner_class = type->info->owner_class;
  a->size           = type->size;
  a->info->base_type = type->info->base_type;
  a->array_depth    = type->array_depth;
  a->info->gack        = type->info->gack;
  return a;
}

ANN m_bool isa(const restrict Type var, const restrict Type parent) {
  return (var->xid == parent->xid) ? GW_OK : var->info->parent ? isa(var->info->parent, parent) : GW_ERROR;
}

ANN Type find_common_anc(const restrict Type lhs, const restrict Type rhs) {
  return isa(lhs, rhs) > 0 ? rhs : isa(rhs, lhs) > 0 ? lhs : NULL;
}

#define describe_find(name, t)                                       \
ANN t find_##name(const Type type, const Symbol xid) {               \
  if(type->nspc) {                                                   \
  const t val = nspc_lookup_##name##2(type->nspc, xid);              \
  if(val)                                                            \
    return val;                                                      \
  }                                                                  \
  return type->info->parent ? find_##name(type->info->parent, xid) : NULL; \
}
describe_find(value, Value)
//describe_find(func,  Func)

ANN Type typedef_base(Type t) {
  while(tflag(t, tflag_typedef))
    t = t->info->parent;
  return t;
}

ANN Type array_base(Type type) {
  const Type t = typedef_base(type);
  return t->array_depth ? t->info->base_type : t;
}

ANN static Symbol array_sym(const Env env, const Type src, const m_uint depth) {
  size_t len = strlen(src->name);
  char name[len + 2* depth + 1];
  strcpy(name, src->name);
  m_uint i = depth + 1;
  while(--i) {
    strcpy(name+len, "[]");
    len += 2;
  }
  return insert_symbol(name);
}

ANN Type array_type(const Env env, const Type src, const m_uint depth) {
  const Symbol sym = array_sym(env, src, depth);
  const Type type = nspc_lookup_type1(src->info->owner, sym);
  if(type)
    return type;
  const Type t = new_type(env->gwion->mp, env->gwion->type[et_array]->xid,
      s_name(sym), env->gwion->type[et_array]);
  t->array_depth = depth + src->array_depth;
  t->info->base_type = array_base(src) ?: src;
  t->info->owner = src->info->owner;
  if(depth > 1 || isa(src, env->gwion->type[et_compound]) > 0) {
    t->nspc = new_nspc(env->gwion->mp, s_name(sym));
    inherit(t);
    t->nspc->info->class_data_size = SZ_INT;
    nspc_allocdata(env->gwion->mp, t->nspc);
    *(f_release**)(t->nspc->info->class_data) = (depth > 1 || !tflag(src, tflag_struct)) ?
      object_release : struct_release;
  } else
  nspc_addref((t->nspc = env->gwion->type[et_array]->nspc));
  mk_class(env, t);
  nspc_add_type_front(src->info->owner, sym, t);
  return t;
}

ANN m_bool type_ref(Type t) {
  do {
    if(tflag(t, tflag_empty))
      return GW_OK;
    if(tflag(t, tflag_typedef) && tflag(t, tflag_cdef)) {
      if(t->info->cdef->base.ext && t->info->cdef->base.ext->array) {
        if(!t->info->cdef->base.ext->array->exp)
          return GW_OK;
        else {
          const Type type = t->info->parent->info->base_type;
          if(tflag(type, tflag_empty))
            return GW_OK;
        }
      }
    }
  } while((t = t->info->parent));
  return 0;
}


ANN m_str get_type_name(const Env env, const Type t, const m_uint index) {
  if(!index)
    return NULL;
  m_str name = strchr(t->name, ':');
  if(!name)
    return NULL;
  name += 2;
  const size_t slen = strlen(name);
  m_uint lvl = 0;
  m_uint n = 1;
  char c, buf[slen + 1], *tmp = buf;
  while((c = *name)) {
    if(c == ':')
      ++lvl;
    else if(c == ']') {
      if(!lvl-- && n == index)
        break;
    } else if(c == ',') {
      if(!lvl && n++ == index)
        break;
      if(!lvl)
        ++name;
    }
    if(n == index)
      *tmp++ = *name;
    ++name;
  }
  *tmp = '\0';
  return tmp - buf ? s_name(insert_symbol(buf)) : NULL;
}

ANN m_uint get_depth(const Type type) {
  m_uint depth = 0;
  Type t = type;
  do {
    if(t->array_depth) {
      depth += t->array_depth;
      t = t->info->base_type;
    } else
      t = t->info->parent;
  } while(t);
  return depth;
}

ANN m_bool is_fptr(const struct Gwion_* gwion, const Type t) {
  return isa(actual_type(gwion, t), gwion->type[et_fptr]) > 0;
}
ANN inline m_bool is_class(const struct Gwion_* gwion, const Type t) {
  return isa(t, gwion->type[et_class]) > 0;
}

ANN Type actual_type(const struct Gwion_* gwion, const Type t) {
  return is_class(gwion, t) ? t->info->base_type : t;
}

ANN void inherit(const Type t) {
  const Nspc nspc = t->nspc, parent = t->info->parent->nspc;
  if(!nspc || !parent)
    return;
  nspc->info->offset = parent->info->offset;
  if(parent->info->vtable.ptr)
    vector_copy2(&parent->info->vtable, &nspc->info->vtable);
}
