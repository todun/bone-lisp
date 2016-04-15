/* bone.c -- The Bone Lisp interpreter.
 * Copyright (C) 2016 Wolfgang Jaehrling
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE 1 // for mmap()s MAP_ANONYMOUS
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>

#define my static

typedef uint64_t any; // we only support 64 bit currently
my size_t bytes2words(size_t n) { return (n-1)/sizeof(any) + 1; }
typedef enum { t_cons = 0, t_sym = 1, t_uniq = 2, t_str = 3, t_reg = 4, t_sub = 5, t_num = 6, t_other = 7 } type_tag;

#define UNIQ(n) (t_uniq | (010*(n)))
#define NIL       UNIQ(0)
#define BTRUE     UNIQ(1)
#define BFALSE    UNIQ(2)
#define ENDOFFILE UNIQ(3)
#define HASH_SLOT_UNUSED  UNIQ(100)
#define HASH_SLOT_DELETED UNIQ(101)
#define READER_LIST_END   UNIQ(102)
#define BINDING_DEFINED   UNIQ(103)
#define BINDING_DECLARED  UNIQ(104)
#define IN_ARGS           UNIQ(105)
#define IN_ENV            UNIQ(106)
my bool is_nil(any x) { return x == NIL; }
my bool is(any x) { return x != BFALSE; }
my any to_bool(int x) { return x ? BTRUE : BFALSE; }

my void print(any x);
my void generic_error(const char *msg, any x) { printf("%s: ", msg); print(x); printf("\n"); abort(); }
my void type_error(any x, type_tag t) { generic_error("type error", x); } // FIXME: show more info
my type_tag tag_of(any x) { return x & 7; }
my bool is_tagged(any x, type_tag t) { return tag_of(x) == t; }
my void check(any x, type_tag t) { if(!is_tagged(x, t)) type_error(x, t); }
my any tag(any x, type_tag t) { return x | t; }
my any untag(any x) { return x & ~7; }
my any untag_check(any x, type_tag t) { check(x, t); return untag(x); }

my bool is_num(any x) { return is_tagged(x, t_num); }
// FIXME: these assume little-endian
my int32_t any2int(any x) { check(x, t_num); return ((int32_t *) &x)[1]; }
my any int2any(int32_t n) { any r = t_num; ((int32_t *) &r)[1] = n; return r; }

//////////////// regions ////////////////

#define ALLOC_BLOCKS_AT_ONCE 16
my size_t blocksize;  // in bytes
my size_t blockwords; // words per block
my any blockmask; // to get the block an `any` belongs to; is not actually an object!
my any **free_block;
// A block begins with a pointer to the previous block that belongs to the region.
// The metadata of a region (i.e. this struct) is stored in its first block.
typedef struct { any **current_block, **allocp; } *reg;

my any **block(any *x) { return (any **) (blockmask & (any) x); } // get ptr to start of block that x belongs to.
my any **blocks_alloc(int n) { return mmap(NULL, blocksize*n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0); }
my void block_point_to_next(any **p, int i) { p[i*blockwords] = (any *) &p[(i+1)*blockwords]; }
my void blocks_init(any **p, int n) { n--; for(int i = 0; i < n; i++) block_point_to_next(p, i); p[n*blockwords] = NULL; }
my any **fresh_blocks() { any **p = blocks_alloc(ALLOC_BLOCKS_AT_ONCE); blocks_init(p, ALLOC_BLOCKS_AT_ONCE); return p; }
my void ensure_free_block() { if(!free_block) free_block = fresh_blocks(); }
my any **block_new(any **next) { ensure_free_block(); any **r = free_block; free_block = (any **) r[0]; r[0] = (any *) next; return r; }
my void reg_init(reg r, any **b) { r->current_block = b; r->allocp = (any **) &r[1]; }
my reg reg_new() { any **b = block_new(NULL); reg r = (reg) &b[1]; reg_init(r, b); return r; }
my void reg_free(reg r) { block((any *) r)[0] = (any *) free_block; free_block = r->current_block; }
my void blocks_sysfree(any **b) { if(!b) return; any **next = (any **) b[0]; munmap(b, blocksize); blocks_sysfree(next); }
my void reg_sysfree(reg r) { blocks_sysfree(r->current_block); }

my reg permanent_reg;
my reg reg_stack[64];
my reg *reg_sp = reg_stack; // points to tos!
my any **allocp, **current_block; // from currently used reg.
my void load_reg(reg r)  { allocp = r->allocp; current_block = r->current_block; }
my void store_reg(reg r) { r->allocp = allocp; r->current_block = current_block; }
my void reg_push(reg r) { store_reg(*reg_sp); reg_sp++; *reg_sp = r;     load_reg(*reg_sp); }
my reg reg_pop()        { store_reg(*reg_sp); reg r = *reg_sp; reg_sp--; load_reg(*reg_sp); return r; }
my void reg_permanent() { reg_push(permanent_reg); }

my any *reg_alloc(int n) { any *res = (any *) allocp; allocp += n;
  if(block((any *) allocp) == current_block) return res;
  current_block = block_new(current_block); allocp = (any **) &current_block[1]; return reg_alloc(n);
}

my any reg2any(reg r) { return tag((any) r, t_reg); }
my reg any2reg(any x) { return (reg) untag_check(x, t_reg); }

my any copy(any x); // FIXME: to header
my any copy_back(any x) { reg_push(reg_sp[-1]); any y = copy(x); reg_pop(); return y; }

//////////////// conses / lists ////////////////

my any cons(any a, any d) { any *p = reg_alloc(2); p[0] = a; p[1] = d; return (any) p; } // no tag() needed
my any precons(any a) { any *p = reg_alloc(2); p[0] = a; return (any) p; } // for faster list construction
my any far(any x) { return ((any *) x)[0]; } // fast, no typecheck
my any fdr(any x) { return ((any *) x)[1]; } // likewise
my any car(any x) { check(x, t_cons); return far(x); }
my any cdr(any x) { check(x, t_cons); return fdr(x); }
my void set_far(any cell, any x) { ((any *) cell)[0] = x; }
my void set_fdr(any cell, any x) { ((any *) cell)[1] = x; }

my bool is_cons(any x) { return is_tagged(x, t_cons); }
my bool is_single(any x) { return is_cons(x) && is_nil(fdr(x)); }
my any single(any x) { return cons(x, NIL); }
#define foreach(var, lst) for(any p_ = (lst), var; is_cons(p_) && (var = far(p_), 1); p_ = fdr(p_))

my int len(any x) { int n = 0; foreach(e, x) n++; return n; }
my any assoq(any obj, any xs) { foreach(x, xs) if(car(x) == obj) return fdr(x); return BFALSE; }

//////////////// strs ////////////////

my any str(any chrs) { any *p = reg_alloc(1); *p = chrs; return tag((any) p, t_str); }
my any unstr(any s) { return *(any *) untag_check(s, t_str); }
my any charp2list(const char *p) { return !*p ? NIL : cons(int2any(*p), charp2list(p+1)); } // FIXME: for short strings only
my any charp2str(const char *p) { return str(charp2list(p)); }
my char *list2charp(any x) {
  char *res = malloc(len(x) + 1); // FIXME: longer for UTF-8
  char *p = res; foreach(c, x) { *p = any2int(c); p++; }
  *p = '\0'; return res;
}

//////////////// hash tables ////////////////

#define MAXLOAD 175 // Value between 0 and 255; 128 will cause an average of two probes.
typedef struct { unsigned size, taken_slots; any *keys, *vals; any default_value; } *hash;

my hash hash_new(unsigned initsize, any default_val) { hash h = malloc(sizeof(*h));
  h->size = initsize; h->taken_slots = 0; h->default_value = default_val;
  h->keys = malloc(initsize*sizeof(any)); h->vals = malloc(initsize*sizeof(any));
  for(unsigned i = 0; i != initsize; i++) h->keys[i] = HASH_SLOT_UNUSED; return h;
}
my void hash_free(hash h) { free(h->keys); free(h->vals); free(h); }

/* Find the entry in H with KEY and provide the entry number in *POS.
   Return true if there is an entry with this key already.  If there
   is none, *POS will contain the position of the slot we can use to
   add it. */
my bool find_slot(hash h, any key, unsigned *pos) { int first_deleted = -1;
  *pos = key % h->size;
  while(1) {
    if(h->keys[*pos] == key) return true;
    if(h->keys[*pos] == HASH_SLOT_UNUSED)  { if(first_deleted != -1) *pos = first_deleted; return false; }
    if(h->keys[*pos] == HASH_SLOT_DELETED) { if(first_deleted == -1) first_deleted = *pos; }
    if(++(*pos) == h->size) *pos = 0;
  }
}

my void hash_set(hash h, any key, any val); // FIXME: to header
my bool slot_used(any x) { return x != HASH_SLOT_UNUSED && x != HASH_SLOT_DELETED; } 
my void enlarge_table(hash h) { hash new = hash_new(h->size * 2 + 1, NIL);
  for(unsigned i = 0; i != h->size; i++) if(slot_used(h->keys[i])) hash_set(new, h->keys[i], h->vals[i]);
  free(h->keys); free(h->vals); h->size = new->size; h->keys = new->keys; h->vals = new->vals; free(new);
}

my void hash_set(hash h, any key, any val) { unsigned pos;
  if(!find_slot(h, key, &pos)) {  // adding a new entry
    h->taken_slots++;
    if(((h->taken_slots << 8) / h->size) > MAXLOAD) { enlarge_table(h); find_slot(h, key, &pos); }
  }
  h->keys[pos] = key; h->vals[pos] = val;
}
my any hash_get(hash h, any key) { unsigned pos; return find_slot(h, key, &pos) ? h->vals[pos] : h->default_value; }
my void hash_rm(hash h, any key) { unsigned pos; if(find_slot(h, key, &pos)) { h->keys[pos] = HASH_SLOT_DELETED; h->taken_slots--; } }
#if 0 // FIXME: hash_iter
my void hash_each(hash h, hash_iter fn, void *hook) {
  for(unsigned i = 0; i != h->size; i++) if(slot_used(h->keys[i])) fn(hook, h->keys[i], h->vals[i]);
}
#endif

//////////////// syms ////////////////

my bool is_sym(any x) { return is_tagged(x, t_sym); }
my hash sym_ht;
my any string_hash(const char *s, size_t *len) {  // This is the djb2 algorithm.
  int32_t hash = 5381; *len = 0;
  while(*s) { (*len)++; hash = ((hash << 5) + hash) + *(s++); }
  return int2any(hash);
}
my char *symtext(any sym) { return (char *) untag(sym); }
my any as_sym(char *name) { return tag((any) name, t_sym); } // `name` must be interned
my any add_sym(const char *name, size_t len, any id) {
  reg_permanent(); char *new = (char *) reg_alloc(bytes2words(len+1)); reg_pop();
  memcpy(new, name, len); hash_set(sym_ht, id, (any) new); return as_sym(new);
}
my any intern(const char *name) { size_t len; any id = string_hash(name, &len);
  while(1) {
    char *candidate = (char *) hash_get(sym_ht, id);
    if(candidate == NULL) return add_sym(name, len, id);
    if(!strcmp(candidate, name)) return as_sym(candidate);
    id++;
  }
}
my any intern_from_chars(any chrs) { char *s = list2charp(chrs); any res = intern(s); free(s); return res; }

my any s_quote, s_quasiquote, s_unquote, s_unquote_splicing, s_lambda, s_with, s_if, s_dot;
#define x(name) s_ ## name = intern(#name)
my void init_syms() { x(quote);x(quasiquote);x(unquote);s_unquote_splicing=intern("unquote-splicing");
  x(lambda);x(with);x(if);s_dot=intern("."); }
#undef x

//////////////// subs ////////////////

typedef struct sub_code { // fields are in the order in which we access them.
  int argc; // number of required args
  int has_rest; // accepting rest args? 0=no, 1=yes
  int localc; // for `let` etc.
  any name; // sym for backtraces
  int size_of_env; // so that we can copy subs
  any code[1]; // can be longer
} *sub_code;
#define sub_code_header_size (bytes2words(sizeof(struct sub_code))-1)
my sub_code make_sub_code(any name, int argc, int has_rest, int localc, int size_of_env, int code_size) {
  sub_code code = (sub_code) reg_alloc(sub_code_header_size + code_size);
#define x(f) code->f = f
  x(name); x(argc); x(has_rest); x(localc); x(size_of_env);
#undef x
  return code;
}

typedef struct { sub_code code; any env[0]; } *sub;
my bool is_sub(any x) { return is_tagged(x, t_sub); }
my any sub2any(sub s) { return tag((any) s, t_sub); }
my sub any2sub(any x) { return (sub) untag_check(x, t_sub); }
my any copy(any x);
my any copy_sub(any x) { sub s = any2sub(x); int envsize = s->code->size_of_env;
  any *p = reg_alloc(1+envsize); *p++ = (any) s->code; for(int i = 0; i != envsize; i++) *p++ = copy(s->env[i]);
  return tag((any) p, t_sub);
}
typedef void (*csub)(any *);

//////////////// print ////////////////

my void print_args(any x) {
  if(!is_cons(x)) { if(!is_nil(x)) { printf(". "); print(x); printf(" "); } return; }
  print(far(x)); printf(" "); print_args(fdr(x));
}
my void print(any x) { switch(tag_of(x)) {
  case t_cons:; any a = far(x);
    if(is_sym(far(x))) {
      if(a == s_quote)            { printf("'");  print(fdr(x)); break; }
      if(a == s_quasiquote)       { printf("`");  print(fdr(x)); break; }
      if(a == s_unquote)          { printf(",");  print(fdr(x)); break; }
      if(a == s_unquote_splicing) { printf(",@"); print(fdr(x)); break; }
      if(a == s_lambda && is_cons(fdr(x)) && is_single(fdr(fdr(x))) && is_cons(far(fdr(fdr(x))))) {
	printf("| "); print_args(far(fdr(x))); print(far(fdr(fdr(x)))); break;
      } }
    bool first = true; printf("(");
    do { if(first) first=false; else printf(" "); print(far(x)); x=fdr(x); } while(is_tagged(x, t_cons));
    if(x != NIL) { printf(" . "); print(x); } printf(")"); break;
  case t_sym: printf("%s", symtext(x)); break;
  case t_num: printf("%d", any2int(x)); break;
  case t_uniq:
    switch(x) {
    case NIL: printf("()"); break;
    case BTRUE: printf("#t"); break;
    case BFALSE: printf("#f"); break;
    case ENDOFFILE: printf("#{eof}"); break; // FIXME: should we keep this?
    default: abort(); }
    break;
  case t_str: printf("\"");
    foreach(c, unstr(x))
      switch(any2int(c)) { // FIXME: add more (and add them to reader, too) | FIXME: handle grapheme clusters
      case '"':  printf("\\\""); break;
      case '\\': printf("\\\\"); break;
      case '\n': printf("\\n");  break;
      case '\t': printf("\\t");  break;
      default: putchar(any2int(c)); }
    printf("\""); break;
  case t_reg: printf("#reg(%p)", (void *) x); break;
  case t_sub: printf("#sub(id=%p name=", (void *) x); sub_code code = any2sub(x)->code; print(code->name);
    printf(" argc=%d take-rest?=", code->argc); print(code->has_rest ? BTRUE : BFALSE); printf(")");
    break;
  case t_other: default: abort(); }
}

my void say_chr(any chr) { if(is_num(chr)) putchar(any2int(chr)); else foreach(x, chr) putchar(any2int(x)); }
my void say_str(any s) { foreach(chr, unstr(s)) say_chr(chr); }
my void say(any x) { switch(tag_of(x)) {
  case t_str: say_str(x); break; case t_cons: foreach(e, x) say(e); break; default: print(x); }
}

//////////////// read ////////////////

my void parse_error(const char *text) { printf("parse error: %s\n", text); abort(); } // FIXME

my bool allowed_chars[] = { // these can be used for syms in s-exprs
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,1,0,0,1,1,1,0,0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,0,1,1,
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,0
}; // disallowed are the first 32 and " #'(),@;[]`{}|
my bool is_symchar(int c) { return (c >= 0 && c < 256) ? allowed_chars[c] : c!=EOF; }

#define nextc getchar // FIXME: allow input from other sources
my int look() { int c = getchar(); ungetc(c, stdin); return c; }

my void skip_until(char end) { int c; do { c = nextc(); } while(c!=end && c!=EOF); }
my int find_token() {
  while(1) { int c = nextc();
    switch(c) {
    case ';': skip_until('\n'); break;
    case ' ': case '\t': case '\n': case '\f': case '\r': break;
    default: return c;
    }
  }
}
my int digit2int (any chr) { int dig = any2int(chr) - '0'; return (dig >= 0 && dig <= 9) ? dig : -1; }
my any chars2num(any chrs) { int ires = 0, pos = 0;
  bool is_positive = true, is_num = false; // to catch "", "+" and "-"
  foreach(chr, chrs) {
    int dig = digit2int(chr); pos++;
    if(dig == -1) {
      if(pos != 1) return BFALSE;
      if(any2int(chr) == '-') { is_positive = false; continue; }
      if(any2int(chr) == '+') continue;
      return BFALSE;
    }
    is_num = true; ires *= 10; ires += dig;
  }
  return !is_num ? BFALSE : int2any(is_positive ? ires : -ires);
}
my any chars_to_num_or_sym(any cs) { any num = chars2num(cs); return is(num) ? num : intern_from_chars(cs); }
my any read_sym_chars(int start_char) {
  any res = precons(int2any(start_char)); any curr = res; int c;
  while(is_symchar(c = look())) { any next = precons(int2any(nextc())); set_fdr(curr, next); curr = next; }
  set_fdr(curr, NIL); return res;
}
my any read_str() { any curr, res = NIL;
  while(1) { int c = nextc();
    if(c == '"') { if(!is_nil(res)) set_fdr(curr, NIL); return str(res); }
    if(c == EOF) parse_error("end of file inside of a str");
    if(c == '\\') switch(c = nextc()) {
      case '\\': case '\'': break;
      case 'n': c = '\n'; break;
      case 't': c = '\t'; break;
      case EOF: parse_error("end of file after backslash in str");
      default: parse_error("invalid character after backslash in str");
      }
    any now = precons(int2any(c));
    if(is_nil(res)) res = curr = now; else { set_fdr(curr, now); curr = now; } 
  }
}
my any reader(); // for mutual recursion
my any read_list() { any x = reader();
  if(x == READER_LIST_END) return NIL;
  if(x == ENDOFFILE) parse_error("end of file in list");
  if(x == s_dot) { x = reader(); if(reader() != READER_LIST_END) parse_error("invalid improper list"); return x; }
  return cons(x, read_list());
}
my any lambda_parser(any *body) { any x = reader();
  if(is_cons(x)) { *body = x; return NIL; }
  if(x == s_dot) { any rest = reader(); *body = reader(); return rest; }
  if(is_nil(x)) { parse_error("empty body expression not allowed in lambda short form"); }
  return cons(x, lambda_parser(body));
}
my any read_lambda_short_form() { any body, args = lambda_parser(&body); return cons(s_lambda, cons(args, single(body))); }
my any read_unquote() { any q = s_unquote; int c = look();
  if(c == '@') { nextc(); q = s_unquote_splicing; }
  return cons(q, reader());
}
my any reader() { int c = find_token(); switch(c) { // FIXME: should have arg "allow_closing_paren"
  case ')': return READER_LIST_END;
  case '(': return read_list();
  case '|': return read_lambda_short_form();
  case '\'': return cons(s_quote, reader());
  case '`': return cons(s_quasiquote, reader());
  case ',': return read_unquote();
  case '"': return read_str();
  case '#': switch(c = nextc()) {
    case 'f': return BFALSE;
    case 't': return BTRUE;
    case '!': skip_until('\n'); return reader(); // ignore Unix-style script header
    default: parse_error("invalid character after #"); }
  case EOF: return ENDOFFILE;
  default: return(chars_to_num_or_sym(read_sym_chars(c))); }
}
my any bone_read() {
  any x = reader();
  if(x == READER_LIST_END) parse_error("unexpected closing parenthesis");
  return x;
}

//////////////// evaluator ////////////////

typedef enum {
  OP_CONST = 1, OP_GET_ENV, OP_GET_ARG, OP_SET_LOCAL, OP_WRAP, OP_PREPARE_CALL, OP_CALL, OP_TAILCALL, OP_ADD_ARG,
  OP_JMP_IF, OP_JMP, OP_RET, OP_PREPARE_SUB, OP_ADD_ENV, OP_MAKE_SUB
} opcode;

my any last_value;
struct call_stack_entry { sub subr; int tail_calls; } call_stack[256], *call_stack_sp;
struct upcoming_call {
  sub to_be_called;
  int nonrest_args_left; any rest_constructor;
  any *the_args, *next_arg; // `next_arg` points into `the_args`.
} upcoming_calls[256], *next_call; // stack pointer

my void args_error(sub_code sc, any xs) { generic_error("wrong number of args", cons(sc->name, xs)); }
my void args_error_unspecific(sub_code sc) { args_error(sc, single(intern("..."))); }

my void add_nonrest_arg() { *(next_call->next_arg++) = last_value; }
my void add_rest_arg() { sub_code sc = next_call->to_be_called->code;
  if(!sc->has_rest) args_error_unspecific(sc);
  if(next_call->rest_constructor == NIL) { // first rest arg
    next_call->rest_constructor = next_call->the_args[sc->argc] = single(last_value);
  } else { // adding another rest arg
    any next = single(last_value); set_fdr(next_call->rest_constructor, next); next_call->rest_constructor = next;
  }
}
my void verify_argc(struct upcoming_call *the_call) {
  if(the_call->nonrest_args_left) args_error_unspecific(the_call->to_be_called->code);
}
my void call(sub subr, any *args) { // FIXME: move call_stack_sp++ into OP_CALL?
  call_stack_sp++; call_stack_sp->tail_calls = 0; call_stack_sp->subr = subr; sub lambda; any *lambda_envp;
start:;
  any *env = subr->env; any *ip = subr->code->code;
  while(1) switch(*ip++) {
    case OP_CONST: last_value = *ip++; break;
    case OP_GET_ENV: last_value = env[any2int(*ip++)]; break;
    case OP_GET_ARG: last_value = args[any2int(*ip++)]; break; // args+locals
    case OP_SET_LOCAL: args[any2int(*ip++)] = last_value; break;
    case OP_WRAP: ((csub) *ip)(args); return;
    case OP_PREPARE_CALL: { sub to_be_called = any2sub(last_value); sub_code sc = to_be_called->code;
	next_call++; next_call->to_be_called = to_be_called;
	next_call->nonrest_args_left = sc->argc;
	next_call->next_arg = next_call->the_args = reg_alloc(sc->argc+sc->has_rest+sc->localc);
	if(sc->has_rest) { next_call->rest_constructor = next_call->the_args[sc->argc] = NIL; }
	break; }
    case OP_CALL: { struct upcoming_call *the_call = next_call--; verify_argc(the_call);
	call(the_call->to_be_called, the_call->the_args); break; }
    case OP_TAILCALL: { struct upcoming_call *the_call = next_call--; verify_argc(the_call);
	subr = the_call->to_be_called; args = the_call->the_args; call_stack_sp->tail_calls++; goto start; }
    case OP_ADD_ARG:
      if(next_call->nonrest_args_left) { next_call->nonrest_args_left--; add_nonrest_arg(); } else add_rest_arg();
      break;
    case OP_JMP_IF: if(!is(last_value)) { ip++; break; } // else fall through
    case OP_JMP: ip += any2int(*ip); break;
    case OP_RET: return;
    case OP_PREPARE_SUB: { sub_code lc = (sub_code) *ip++; lambda = (sub) reg_alloc(1+lc->size_of_env);
	lambda->code = lc; lambda_envp = lambda->env; break; }
    case OP_ADD_ENV: *(lambda_envp++) = last_value; break;
    case OP_MAKE_SUB: last_value = sub2any(lambda); break;
    default: printf("unknown vm instruction\n"); abort(); // FIXME
  }
}

my void apply(sub subr, any xs) { sub_code sc = subr->code; int argc = sc->argc, pos = 0; any p;
  any *args = reg_alloc(argc + sc->has_rest + sc->localc); // FIXME: allocate on stack
  foreach(x, xs) {
    if(pos <  argc) { args[pos] = x; pos++; continue; } // non-rest arg
    if(pos == argc) { // starting rest args
      if(sc->has_rest) { args[pos] = precons(x); p = args[pos]; pos++; continue; } else args_error(sc, xs);
    }
    any next = precons(x); set_fdr(p, next); p = next; pos++; // adding another rest arg
  }
  if(pos < argc) args_error(sc, xs);
  if(pos == argc) args[argc] = NIL; else set_fdr(p, NIL);
  call(subr, args);
}

//////////////// bindings ////////////////

my hash bindings; // FIXME: does it need mutex protection?
my void bind(any name, any subr) { reg_permanent(); hash_set(bindings, name, cons(BINDING_DEFINED, subr)); reg_pop(); } // FIXME: reg-change?
my any get_binding(any name) { return hash_get(bindings, name); }

//////////////// compiler ////////////////

my void emit(any x, any *dst) { any next = single(x); set_fdr(*dst, next); *dst = next; }
my void compile_expr(any e, any env, bool tail_context, any *dst) {
  switch(tag_of(e)) {
  case t_num: case t_uniq: case t_str: emit(OP_CONST, dst); emit(e, dst); break;
  case t_cons:; any first = far(e);
    if(first == s_quote) { emit(OP_CONST, dst); emit(fdr(e), dst); break; } // FIXME: copy()?
    //if(first == s_if) { break; }
    compile_expr(first, env, false, dst); emit(OP_PREPARE_CALL, dst);
    foreach(arg, fdr(e)) { compile_expr(arg, env, false, dst); emit(OP_ADD_ARG, dst); }
    emit(tail_context ? OP_TAILCALL : OP_CALL, dst); break;
  case t_sym:; any local = assoq(e, env);
    if(is(local)) { emit(far(local) == IN_ARGS ? OP_GET_ARG : OP_GET_ENV, dst); emit(fdr(local), dst); break; }
    any global = get_binding(e); if(!is_cons(global)) generic_error("unbound sym", e);
    emit(OP_CONST, dst); emit(fdr(global), dst); break;
  }
}
my any compile2list(any expr) { any res = single(BFALSE); any buf = res;
  compile_expr(expr, NIL, true, &buf); emit(OP_RET, &buf); return fdr(res);
}
my sub_code compile2sub_code(any e) { any raw = compile2list(e);
  reg_permanent(); sub_code code = make_sub_code(BFALSE, 0, false, 0, 0, len(raw)); reg_pop(); // FIXME: we ignore env size
  any *p = code->code; foreach(x, raw) *p++ = x; return code;
} // result is in permanent region.

//////////////// library ////////////////

#define DEFSUB(name) my void CSUB_ ## name(any *args)
DEFSUB(simpleplus) { last_value = int2any(any2int(args[0]) + any2int(args[1])); }
DEFSUB(fullplus) { int ires = 0; foreach(n, args[0]) ires += any2int(n); last_value = int2any(ires); }
DEFSUB(cons) { last_value = cons(args[0], args[1]); }
DEFSUB(print) { print(args[0]); last_value = single(args[0]); }
DEFSUB(apply) { apply(any2sub(args[0]), args[1]); } // FIXME: (apply foo a b c xs)
DEFSUB(id) { last_value = args[0]; }
DEFSUB(nilp) { last_value = to_bool(args[0] == NIL); }
DEFSUB(eqp) { last_value = to_bool(args[0] == args[1]); }
DEFSUB(not) { last_value = to_bool(args[0] == BFALSE); }
DEFSUB(car) { last_value = car(args[0]); }
DEFSUB(cdr) { last_value = cdr(args[0]); }
DEFSUB(consp) { last_value = to_bool(is_tagged(args[0], t_cons)); }
DEFSUB(symp) { last_value = to_bool(is_tagged(args[0], t_sym)); }
DEFSUB(subp) { last_value = to_bool(is_tagged(args[0], t_sub)); }
DEFSUB(nump) { last_value = to_bool(is_tagged(args[0], t_num)); }
DEFSUB(strp) { last_value = to_bool(is_tagged(args[0], t_str)); }
DEFSUB(str) { last_value = str(args[0]); }
DEFSUB(unstr) { last_value = unstr(args[0]); }
DEFSUB(len) { last_value = int2any(len(args[0])); }
DEFSUB(assoq) { last_value = assoq(args[0], args[1]); }
DEFSUB(intern) { last_value = intern_from_chars(unstr(args[0])); }
DEFSUB(copy) { last_value = copy(args[0]); }
DEFSUB(say) { foreach(x, args[0]) say(x); last_value = args[0]; }
DEFSUB(unaryminus) { last_value = int2any(-any2int(args[0])); }
DEFSUB(simpleminus) { last_value = int2any(any2int(args[0]) - any2int(args[1])); }
DEFSUB(fullminus) { int res = any2int(args[0]); foreach(x, args[1]) res -= any2int(x); last_value = int2any(res); }
DEFSUB(simple_num_eqp) { last_value = to_bool(any2int(args[0]) == any2int(args[1])); }
DEFSUB(simple_num_neqp) { last_value = to_bool(any2int(args[0]) != any2int(args[1])); }
DEFSUB(simple_num_gtp) { last_value = to_bool(any2int(args[0]) > any2int(args[1])); }
DEFSUB(simple_num_ltp) { last_value = to_bool(any2int(args[0]) < any2int(args[1])); }
DEFSUB(simple_num_geqp) { last_value = to_bool(any2int(args[0]) >= any2int(args[1])); }
DEFSUB(simple_num_leqp) { last_value = to_bool(any2int(args[0]) <= any2int(args[1])); }
// FIXME: the set_far() below may be dangerous when we have `car!`, because a sub that takes only
// rest args will see the cons `arg` directly and could let it escape.  In the sense of:
//   (each xs (lambda rest (car! elsewhere rest))) ;; but taking rest args here is not sane.
DEFSUB(each) { sub subr = any2sub(args[1]); any arg = single(BFALSE); foreach(x, args[0]) { set_far(arg, x); apply(subr, arg); } }

my void register_csub(csub cptr, const char *name, int argc, int has_rest) {
  any name_sym = intern(name); sub_code code = make_sub_code(name_sym, argc, has_rest, 0, 0, 2);
  code->code[0] = OP_WRAP; code->code[1] = (any) cptr;
  sub subr = (sub) reg_alloc(1); subr->code = code; bind(name_sym, sub2any(subr));
}
my void init_csubs() {
  register_csub(CSUB_simpleplus, "simple+", 2, false);
  register_csub(CSUB_fullplus, "full+", 0, 1); register_csub(CSUB_fullplus, "+", 0, 1);
  register_csub(CSUB_cons, "cons", 2, false);
  register_csub(CSUB_print, "print", 1, false);
  register_csub(CSUB_apply, "apply", 2, false);
  register_csub(CSUB_id, "id", 1, false); register_csub(CSUB_id, "list", 0, 1);
  register_csub(CSUB_nilp, "nil?", 1, false);
  register_csub(CSUB_eqp, "eq?", 2, false);
  register_csub(CSUB_not, "not", 1, false);
  register_csub(CSUB_car, "car", 1, false);
  register_csub(CSUB_cdr, "cdr", 1, false);
  register_csub(CSUB_consp, "cons?", 1, false);
  register_csub(CSUB_symp, "sym?", 1, false);
  register_csub(CSUB_subp, "sub?", 1, false);
  register_csub(CSUB_nump, "num?", 1, false);
  register_csub(CSUB_strp, "str?", 1, false);
  register_csub(CSUB_str, "str", 1, false);
  register_csub(CSUB_unstr, "unstr", 1, false);
  register_csub(CSUB_len, "len", 1, false);
  register_csub(CSUB_assoq, "assoq", 2, false);
  register_csub(CSUB_intern, "intern", 1, false); register_csub(CSUB_intern, "str->sym", 1, false);
  register_csub(CSUB_copy, "copy", 1, false);
  register_csub(CSUB_say, "say", 0, 1);
  register_csub(CSUB_unaryminus, "unary-", 1, false);
  register_csub(CSUB_simpleminus, "simple-", 2, false);
  register_csub(CSUB_fullminus, "full-", 1, 1); register_csub(CSUB_fullminus, "-", 1, 1);
  // FIXME: Add the full versions and bind canonical names to to them
  register_csub(CSUB_simple_num_eqp, "simple=?", 2, false); register_csub(CSUB_simple_num_eqp, "=?", 2, false);
  register_csub(CSUB_simple_num_neqp, "simple<>?", 2, false); register_csub(CSUB_simple_num_neqp, "<>?", 2, false);
  register_csub(CSUB_simple_num_gtp, "simple>?", 2, false); register_csub(CSUB_simple_num_gtp, ">?", 2, false);
  register_csub(CSUB_simple_num_ltp, "simple<?", 2, false); register_csub(CSUB_simple_num_ltp, "<?", 2, false);
  register_csub(CSUB_simple_num_geqp, "simple>=?", 2, false); register_csub(CSUB_simple_num_geqp, ">=?", 2, false);
  register_csub(CSUB_simple_num_leqp, "simple<=?", 2, false); register_csub(CSUB_simple_num_leqp, "<=?", 2, false);

  register_csub(CSUB_each, "each", 2, false);
}

//////////////// misc ////////////////

my any copy(any x) {
  switch(tag_of(x)) {
  case t_cons: return cons(copy(far(x)), copy(fdr(x))); // FIXME: optimize
  case t_str: return str(copy(unstr(x)));
  case t_sym: case t_num: case t_uniq: return x;
  case t_sub: return copy_sub(x);
  default: return x; } // FIXME: should be an error
}

my void bone_init_thread() {
  call_stack_sp = call_stack; call_stack->subr = NULL; call_stack->tail_calls = 0; // FIXME: all needed?
  next_call = upcoming_calls;
}

void bone_init() {
  blocksize = sysconf(_SC_PAGESIZE); blockmask = ~(blocksize - 1); blockwords = blocksize/sizeof(any);
  free_block = fresh_blocks();
  permanent_reg = reg_new(); reg_stack[0] = permanent_reg; load_reg(permanent_reg);
  sym_ht = hash_new(997, (any) NULL); init_syms();
  bindings = hash_new(997, BFALSE); init_csubs();
  bone_init_thread();
}
void bone_repl() { int line = 0;
  while(1) { printf("\n@%d: ", line++);
    any e = bone_read(); sub_code code = compile2sub_code(e); call((sub) &code, NULL); print(last_value);
  }
}

// FIXME: should have its own file, to allow embedding.
int main() {
  printf("Bone Lisp 0.1"); bone_init(); bone_repl();

  //reg_push(reg_new());
  //reg_free(reg_pop());
  return 0;
}

