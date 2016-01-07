/*
 * Copyright (c) 2013 Jeremy Yallop.
 *
 * This file is distributed under the terms of the MIT License.
 * See the file LICENSE for details.
 */

#include <errno.h>
#include <assert.h>
#include <string.h>

#include <caml/mlvalues.h>
#include <caml/memory.h>
#include <caml/alloc.h>
#include <caml/custom.h>
#include <caml/callback.h>
#include <caml/fail.h>
#include <caml/hash.h>
#include <caml/unixsupport.h>
#include <caml/threads.h>

#include <ffi.h>

#include "../ctypes/ctypes_managed_buffer_stubs.h"
#include "../ctypes/ctypes_type_info_stubs.h"
#include "../ctypes/ctypes_raw_pointer.h"

/* TODO: support callbacks that raise exceptions?  e.g. using
   caml_callback_exn etc.  */

/* An OCaml function that converts resolves identifiers to OCaml functions */
static value retrieve_closure_;

/* Resolve identifiers to OCaml functions */
static value retrieve_closure(int key)
{
  CAMLparam0 ();
  CAMLlocal1(result);
  result = caml_callback_exn(retrieve_closure_, Val_int(key));

  if (Is_exception_result(result)) {
    caml_raise_constant(*caml_named_value("CallToExpiredClosure"));
  }

  CAMLreturn (result);
}

/* Register the function used to resolve closure identifiers */
/* set_closure_callback : (int -> boxedfn) -> unit */
value ctypes_set_closure_callback(value retrieve)
{
  CAMLparam1(retrieve);

  caml_register_global_root(&retrieve_closure_);
  retrieve_closure_ = retrieve;

  CAMLreturn(Val_unit);
}


void ctypes_check_ffi_status(ffi_status status)
{
  switch (status) {
  case FFI_OK:
    break;
  case FFI_BAD_TYPEDEF:
    raise_with_string(*caml_named_value("FFI_internal_error"),
                      "FFI_BAD_TYPEDEF");
  case FFI_BAD_ABI:
    raise_with_string(*caml_named_value("FFI_internal_error"),
                      "FFI_BAD_ABI");
  default:
    assert(0);
  }
}


/* Given an offset into a fully-aligned buffer, compute the next
   offset that satisfies `alignment'. */
static size_t aligned_offset(size_t offset, size_t alignment)
{
  size_t overhang = offset % alignment;
  return overhang == 0
    ? offset
    : offset - overhang + alignment;
}


static struct callspec {
  /* A description of the buffer used to hold the arguments that we
     pass to C functions via ffi_call.  */

  /* The ffi_cif structure holds some of the information that we're
     maintaining here, but it isn't part of the public interface. */

  /* The space needed to store properly-aligned arguments and return value. */
  size_t bytes;

  /* The number of elements. */
  size_t nelements;

  /* The capacity of the args array, including the terminating null. */
  size_t capacity;

  /* The maximum element alignment */
  size_t max_align;

  /* The state of the callspec value. */
  enum { BUILDING, CALLSPEC } state;

  /* A null-terminated array of size `nelements' types */
  ffi_type **args;

  /* return value offset */
  size_t roffset;

  /* The context in which the call should run: whether errno is
     checked, whether the runtime lock is released, and so on. */
  struct call_context {
    int check_errno;
    int runtime_lock;
  } context;

  /* The libffi call interface structure.  It would be nice for this member to
     be a value rather than a pointer (to save a layer of indirection) but the
     ffi_closure structure keeps the address of the structure, and the GC can
     move callspec values around.
  */
  ffi_cif *cif;

} callspec_prototype = {
  0, 0, 0, 0, BUILDING, NULL, -1, { 0, 0 }, NULL
};


static void finalize_callspec(value v)
{
  struct callspec *callspec = Data_custom_val(v);
  caml_stat_free(callspec->args);
  caml_stat_free(callspec->cif);
}


static struct custom_operations callspec_custom_ops = {
  "ocaml-ctypes:callspec",
  finalize_callspec,
  custom_compare_default,
  custom_hash_default,
  custom_serialize_default,
  custom_deserialize_default,
  custom_compare_ext_default
};

/* We store two things in the callbuffer: a "scratch" area for passing
   arguments and receiving the return value, and an array of pointers into the
   scratch area; we pass that array to ffi_call along with a pointer to the
   return value space.

   The scratch area comes first, followed by the pointer array.

   The incomplete struct type gives a modicum of type safety over void *: the
   compiler should reject incompatible assignments, for example.
 */
typedef struct callbuffer callbuffer;

/* Compute the size of the buffer needed to hold the pointer array used by
   ffi_call, the arguments and the return value */
static size_t compute_arg_buffer_size(struct callspec *callspec,
                                      size_t *arg_array_offset)
{
  assert(callspec->state == CALLSPEC);

  size_t bytes = callspec->bytes;

  *arg_array_offset = aligned_offset(bytes, ffi_type_pointer.alignment);
  bytes = *arg_array_offset + callspec->nelements * sizeof(void *);

  return bytes;
}

/* Set the pointers in `arg_array' to the addresses of the argument slots in
   `callbuffer' as indicated by the elements of the ffitype array in the
   callspec.  */
static void populate_arg_array(struct callspec *callspec,
                               callbuffer *callbuffer, void **arg_array)
{
  size_t i = 0, offset = 0;
  for (; i < callspec->nelements; i++) {
    offset = aligned_offset(offset, callspec->args[i]->alignment);
    arg_array[i] = (char *)callbuffer + offset;
    offset += callspec->args[i]->size;
  }
}


/* Allocate a new C call specification */
/* allocate_callspec : check_errno:bool -> runtime_lock:bool -> callspec */
value ctypes_allocate_callspec(value check_errno, value runtime_lock)
{
  struct call_context context = {
    Int_val(check_errno),
    Int_val(runtime_lock),
  };

  value block = caml_alloc_custom(&callspec_custom_ops,
                                  sizeof(struct callspec), 0, 1);
  struct callspec *spec = Data_custom_val(block);
  memcpy(spec, &callspec_prototype, sizeof(struct callspec));
  spec->context = context;
  return block;
}



/* Add an argument to the C call specification */
/* add_argument : callspec -> 'a ffitype -> int */
value ctypes_add_argument(value callspec_, value argument_)
{
  static const size_t increment_size = 8;

  CAMLparam2(callspec_, argument_);
  struct callspec *callspec = Data_custom_val(callspec_);
  ffi_type *argtype = CTYPES_TO_PTR(argument_);

  assert (callspec->state == BUILDING);

  /* If there's a possibility that this spec represents an argument list or
     a struct we might pass by value then we have to take care to maintain
     the args, capacity and nelements members. */
  int offset = aligned_offset(callspec->bytes, argtype->alignment);
  callspec->bytes = offset + argtype->size;

  if (callspec->nelements + 2 >= callspec->capacity) {
    size_t new_size = ((callspec->capacity + increment_size)
                       * sizeof *callspec->args);
    callspec->args = caml_stat_resize(callspec->args, new_size);
    callspec->capacity += increment_size;
  }
  callspec->args[callspec->nelements] = argtype;
  callspec->args[callspec->nelements + 1] = NULL;
  callspec->nelements += 1;
  callspec->max_align = argtype->alignment > callspec->max_align
    ? argtype->alignment
    : callspec->max_align;
  CAMLreturn(Val_int(offset));
}


/* Pass the return type and conclude the specification preparation */
/* prep_callspec : callspec -> 'a ffitype -> int -> unit */
value ctypes_prep_callspec(value callspec_, value abi_, value rtype)
{
  CAMLparam3(callspec_, abi_, rtype);

  struct callspec *callspec = Data_custom_val(callspec_);
  ffi_type *rffitype = CTYPES_TO_PTR(rtype);
  ffi_abi abi = Int_val(abi_);

  /* Allocate the cif structure */
  callspec->cif = caml_stat_alloc(sizeof *callspec->cif);

  /* Add the (aligned) space needed for the return value */
  callspec->roffset = aligned_offset(callspec->bytes,
                                     rffitype->alignment);
  callspec->bytes = callspec->roffset + rffitype->size;

  /* Allocate an extra word after the return value space to work
     around a bug in libffi which causes it to write past the return
     value space.

     https://github.com/atgreen/libffi/issues/35
  */
  callspec->bytes = aligned_offset(callspec->bytes,
                                              ffi_type_pointer.alignment);
  callspec->bytes += ffi_type_pointer.size;

  ffi_status status = ffi_prep_cif(callspec->cif,
                                   abi,
                                   callspec->nelements,
                                   rffitype,
                                   callspec->args);

  ctypes_check_ffi_status(status);

  callspec->state = CALLSPEC;
  CAMLreturn(Val_unit);
}

/* Call the function specified by `callspec', passing arguments and return
   values in `buffer' */
/* call : string -> _ fn Fat.t -> callspec ->
          (raw_pointer -> Obj.t array -> unit) -> (raw_pointer -> 'a) -> 'a */
value ctypes_call(value fnname, value function, value callspec_,
                  value argwriter, value rvreader)
{
  CAMLparam5(fnname, function, callspec_, argwriter, rvreader);
  CAMLlocal3(callback_arg_buf, callback_val_arr, callback_rv_buf);

  struct callspec *callspec = Data_custom_val(callspec_);
  int roffset = callspec->roffset;
  struct call_context context = callspec->context;
  size_t nelements = callspec->nelements;
  ffi_cif *cif = callspec->cif;

  assert(callspec->state == CALLSPEC);

  size_t arg_array_offset;
  size_t bytes = compute_arg_buffer_size(callspec, &arg_array_offset);

  char *callbuffer = alloca(bytes);
  char *return_slot = callbuffer + roffset;

  populate_arg_array(callspec, (struct callbuffer *)callbuffer,
                     (void **)(callbuffer + arg_array_offset));
  callback_arg_buf = CTYPES_FROM_PTR(callbuffer);

  callback_val_arr = caml_alloc_tuple(nelements);
  caml_callback2(argwriter, callback_arg_buf, callback_val_arr);

  void **val_refs = alloca(sizeof(void*) * nelements);

  unsigned arg_idx;
  for(arg_idx = 0; arg_idx < Wosize_val(callback_val_arr); arg_idx++) {
    value arg_tuple = Field(callback_val_arr, arg_idx);
    /* <4.02 initialize to 0; >=4.02 initialize to unit. */
    if(arg_tuple == 0 || arg_tuple == Val_unit) continue;

    value arg_ptr    = Field(arg_tuple, 0);
    value arg_offset = Field(arg_tuple, 1);

    /* Only strings have defined semantics for now. */
    assert(Is_block(arg_ptr) && Tag_val(arg_ptr) == String_tag);
    val_refs[arg_idx] = String_val(arg_ptr) + Int_val(arg_offset);

    ((void**)(callbuffer + arg_array_offset))[arg_idx] = &val_refs[arg_idx];
  }

  void (*cfunction)(void) = (void (*)(void)) CTYPES_ADDR_OF_FATPTR(function);
  int check_errno = context.check_errno;
  int saved_errno = 0;
  if (context.runtime_lock)
  {
    caml_release_runtime_system();
  }

  if (check_errno)
  {
    errno=0;
  }

  ffi_call(cif,
           cfunction,
           return_slot,
           (void **)(callbuffer + arg_array_offset));
  if (check_errno)
  {
    saved_errno=errno;
  }

  if (context.runtime_lock)
  {
    caml_acquire_runtime_system();
  }

  if (check_errno && saved_errno != 0)
  {
    char *buffer = alloca(caml_string_length(fnname) + 1);
    strcpy(buffer, String_val(fnname));
    unix_error(saved_errno, buffer, Nothing);
  }

  callback_rv_buf = CTYPES_FROM_PTR(return_slot);
  CAMLreturn(caml_callback(rvreader, callback_rv_buf));
}

typedef struct closure closure;
struct closure
{
  ffi_closure         closure;
  int                 fnkey;
  struct call_context context;
};

enum boxedfn_tags { Done, Fn };

static void callback_handler(ffi_cif *cif,
                             void *ret,
                             void **args,
                             void *user_data)
{
  CAMLparam0 ();

  CAMLlocal2(boxedfn, argptr);
  closure *closure = user_data;

  if (closure->context.runtime_lock)
  {
    caml_acquire_runtime_system();
  }

  boxedfn = retrieve_closure(closure->fnkey);

  int i, arity = cif->nargs;

  switch (arity)
  {
    case 0:
    {
      assert (Tag_val(boxedfn) == Fn);
      boxedfn = caml_callback(Field(boxedfn, 0), Val_unit);
      break;
    }
    default:
    {
      for (i = 0; i < arity; i++)
      {
        void *cvalue = args[i];
        assert (Tag_val(boxedfn) == Fn);
        /* unbox and call */
        argptr = CTYPES_FROM_PTR(cvalue);
        boxedfn = caml_callback(Field(boxedfn, 0), argptr);
      }
      break;
    }
  }

  /* now store the return value */
  assert (Tag_val(boxedfn) == Done);
  argptr = CTYPES_FROM_PTR(ret);
  caml_callback(Field(boxedfn, 0), argptr);

  if (closure->context.runtime_lock)
  {
    caml_release_runtime_system();
  }

  CAMLreturn0;
}


/* Construct a pointer to an OCaml function represented by an identifier */
/* make_function_pointer : callspec -> int -> raw_pointer */
value ctypes_make_function_pointer(value callspec_, value fnid)
{
  CAMLparam2(callspec_, fnid);
  CAMLlocal1(codeptr);
  struct callspec *callspec = Data_custom_val(callspec_);

  assert(callspec->state == CALLSPEC);

  void (*code_address)(void) = NULL;

  /* TODO: we need to call ffi_closure_free at some point.  This function
     should return a managed object to which we can attach a finaliser for the
     closure.
  */
  closure *closure = ffi_closure_alloc(sizeof *closure, (void *)&code_address);

  if (closure == NULL) {
    caml_raise_out_of_memory();
  } else {
    closure->fnkey = Int_val(fnid);
    closure->context = callspec->context;

    ffi_status status =  ffi_prep_closure_loc
      ((ffi_closure *)closure,
       callspec->cif,
       callback_handler,
       closure,
       (void *)code_address);

    ctypes_check_ffi_status(status);

    codeptr = CTYPES_FROM_PTR((void *)code_address);
    CAMLreturn (codeptr);
  }
}
