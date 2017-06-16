#ifndef __TYPE
#define __TYPE

#include <complex.h>
#include <stdlib.h>
#include <stdio.h>
#include "defs.h"
#include "symbol.h"
#include "nspc.h"
#include "env.h"
#include "value.h"
#include "dl.h"

struct Type_ {
  m_str     name;
  m_uint    size;
  Type      parent;
  te_type   xid;
  Nspc info;
  Nspc owner;
  Func      func;
  Type      actual_type;
  m_uint    array_depth;
  Type      array_type;
  Class_Def def;
  m_str      doc;
  m_uint    obj_size;
//	UGenInfo ugen_info;
  struct VM_Object_ obj;
  m_bool has_constructor;
  m_bool has_destructor;
  m_bool    is_complete;
  m_bool    is_user;
};

m_bool type_engine_check_prog(Env env, Ast ast, m_str str);
Type new_type(te_type xid, m_str name);
Type type_copy(Env env, Type type);
Env type_engine_init(VM* vm, Vector plug_dirs);
/***
  UTILS
    ***/
void start_type_xid();
Value find_value(Type type, S_Symbol xid);
Type find_type(Env env, ID_List list);
int isprim(Type type);
int isa(Type var, Type parent);
int isres(Env env, S_Symbol xid, int pos);
int verify_array(Array_Sub array);
Type new_array_type(Env env, m_uint depth, Type base_type, Nspc owner_nspc);
static inline Type find_common_anc(Type lhs, Type rhs) {
  return isa(lhs, rhs) > 0 ? rhs : isa(rhs, lhs) > 0 ? lhs : NULL;
}
m_str type_path(ID_List path);
m_bool add_global_value(Env env, m_str name, Type type, m_bool is_const,  void* value);
m_bool add_global_type(Env env, Type type);
Kindof kindof(Type type);
m_int str2char(const m_str c, m_int linepos);
m_uint num_digit(m_uint i);

extern struct Type_ t_void;
extern struct Type_ t_int;
extern struct Type_ t_float;
extern struct Type_ t_complex, t_polar, t_vec3, t_vec4;
extern struct Type_ t_function;
extern struct Type_ t_func_ptr;
extern struct Type_ t_dur;
extern struct Type_ t_time;
extern struct Type_ t_now;
extern struct Type_ t_class;
//extern struct Type_ t_std;
extern struct Type_ t_machine;
//extern struct Type_ t_template;
extern struct Type_ t_null;

// base classes
extern struct Type_ t_object, t_string, t_shred, t_event, t_ugen;
// concept
extern struct Type_ t_array;
// event child
extern struct Type_ t_fileio;
extern struct Type_ t_vararg, t_varobj;

extern struct Type_ t_gack;

m_bool name_valid(m_str a); // /07/04/17
m_uint get_type_xid(); // 19/05/17 // should be included in new type.
#endif
