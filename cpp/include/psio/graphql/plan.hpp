#pragma once
//
// psio/graphql/plan.hpp — phase 2: validation (GraphQL October 2021,
// section 5) of a parsed document against a schema, producing the plan the
// executor runs.
//
// The plan is the validated document with everything resolved:
//
//   * fragments are expanded and fields with one response key are merged
//     (CollectFields), once per concrete object type — a field whose type is
//     a union has one selection set per member, indexed by the member;
//   * a field is {response key as ready JSON text, resolver slot, argument
//     values in the order of the resolver's parameters, child set};
//   * @skip / @include that depend on variables become guards evaluated per
//     request; everything else is decided here.
//
// So execution compares no names and never looks at the query text. A plan
// holds every operation of its document, does not refer to the query text or
// to a root value, and can be reused for any number of requests (and shared
// between threads: it is immutable once built).
//
// Validation reports up to limits::max_errors errors, each with a line and
// column. Limits enforced here: selections after fragment expansion and
// selection depth after fragment expansion (alias and fragment bombs), and,
// for an operation whose cost does not depend on variables, complexity.

#include <psio/graphql/parser.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace psio::graphql
{
   enum class meta_field : std::uint8_t
   {
      none,
      type_name,  // __typename
      schema,     // __schema
      type,       // __type
   };

   struct plan_field
   {
      // `,"key":` in document::pool (written without the comma for the
      // first field of an object).
      std::uint32_t key     = 0;
      std::uint32_t key_len = 0;
      std::uint16_t slot    = 0;
      meta_field    meta    = meta_field::none;
      // desc->arg_count value ids in document::args (nil: not given).
      std::uint32_t args = nil;
      // The selection set below: a set id; for a union, the first of one set
      // per member. nil for a leaf.
      std::uint32_t child = nil;
      // The field is selected when it has no guards or any guard passes.
      std::uint32_t guards = 0, guard_count = 0;
      // A response key selected more than once, the first time conditionally,
      // appears at the position of the first selection that is included
      // (CollectFields runs per request in the spec). Such a key has one
      // entry per candidate position, all with this non-zero `group` (unique
      // in the set) and each guarded by its own selection; the first entry
      // that passes is written. `alternate` marks all but the first.
      std::uint8_t  group     = 0;
      bool          alternate = false;
      std::uint32_t     line = 0, column = 0;
      const field_desc* desc = nullptr;
   };
   struct plan_set
   {
      std::uint32_t first = 0, count = 0;
   };
   // A conjunction of literals in document::literals.
   struct plan_guard
   {
      std::uint32_t first = 0, count = 0;
   };
   // Passes when the operation's variable `var` equals `expect`; var == nil:
   // passes iff `expect`.
   struct plan_literal
   {
      std::uint32_t var    = nil;
      bool          expect = true;
   };
   struct plan_variable
   {
      std::string      name;
      std::string      type_name;
      std::uint8_t     depth   = 0;
      std::uint8_t     nonnull = 0;
      std::uint32_t    default_value = nil;
      const type_desc* type          = nullptr;
      std::uint32_t    line = 0, column = 0;

      type_ref ref() const noexcept { return type_ref{type, depth, nonnull}; }
   };
   struct plan_operation
   {
      ast::operation_kind kind = ast::operation_kind::query;
      std::string         name;
      std::uint32_t       root       = nil;
      std::uint32_t       vars_first = 0, vars_count = 0;
      std::uint32_t       line = 0, column = 0;
   };

   // A validated, executable document. Opaque to callers: build it with
   // schema<...>::compile, run it with schema<...>::execute.
   struct document
   {
      const schema_desc*          schema = nullptr;
      in_store                    values;
      std::string                 pool;
      std::vector<plan_field>     fields;
      std::vector<plan_set>       sets;
      std::vector<plan_guard>     guards;
      std::vector<plan_literal>   literals;
      std::vector<std::uint32_t>  args;
      std::vector<plan_variable>  variables;
      std::vector<plan_operation> operations;

      std::string_view key_of(const plan_field& f) const noexcept
      {
         return std::string_view{pool}.substr(f.key + 2, f.key_len - 4);
      }
   };

   namespace detail
   {
      template <typename... A>
      std::string cat(const A&... a)
      {
         std::string out;
         (out.append(std::string_view{a}), ...);
         return out;
      }

      inline std::string type_string(std::string_view name, unsigned depth, unsigned nonnull)
      {
         std::string out(depth, '[');
         out += name;
         for (unsigned level = depth + 1; level-- > 0;)
         {
            if ((nonnull >> level) & 1)
               out += '!';
            if (level != 0)
               out += ']';
         }
         return out;
      }
      inline std::string type_string(type_ref t, unsigned level = 0)
      {
         return type_string(t.named->name, t.depth - level, static_cast<unsigned>(t.nonnull >> level));
      }

      // Type checks of an input value (spec 5.6, and 6.1.2 for variable
      // values): shared by argument literals, variable defaults and the
      // variable values of a request.
      class value_checker
      {
        public:
         in_store& values;
         // Values came from JSON: an enum value is a string.
         bool from_json = false;

         explicit value_checker(in_store& v) : values(v) {}

         // `exact`: the reference comes from the C++ parameter, so its scalar
         // check is that parameter's own (range of a std::uint8_t, length of
         // a utf8_string<N>). Otherwise it comes from a type NAME, which
         // built-in scalars share between C++ types: unless every such type
         // accepts the same values (type_desc::check_by_name), only the
         // scalar's class is checked here and the typed check runs when the
         // value is decoded for a parameter (a failure is then a field error).
         bool check(std::uint32_t id, type_ref t, unsigned level, bool exact)
         {
            in_node& n = values.nodes[id];
            if (n.kind == value_kind::variable)
               return variable(id, t, level);
            if (n.kind == value_kind::null)
               return !t.is_nonnull(level) ||
                      fail(n.pos, cat("Expected value of type \"", type_string(t, level), "\", found null."));
            if (level < t.depth)
            {
               if (n.kind != value_kind::list)
                  return check(id, t, level + 1, exact);  // one item coerces to a list of it
               bool ok = true;
               for (std::uint32_t c = n.child; c != nil; c = values.nodes[c].next)
                  ok = check(c, t, level + 1, exact) && ok;
               return ok;
            }
            const type_desc& d = *t.named;
            switch (d.kind)
            {
               case type_kind::enumeration:
                  if (n.kind == value_kind::enumeration || (from_json && n.kind == value_kind::string))
                  {
                     if (const std::size_t at = d.find(values.text_of(n)); at < d.value_count)
                     {
                        n.kind  = value_kind::enumeration;
                        n.index = static_cast<std::uint16_t>(at);
                        return true;
                     }
                     return fail(n.pos, cat("Value \"", values.text_of(n), "\" does not exist in \"",
                                            d.name, "\" enum."));
                  }
                  return mismatch(n, d);
               case type_kind::input_object: return object(id, d);
               case type_kind::scalar: return scalar(n, d, exact);
               default: return mismatch(n, d);
            }
         }

        protected:
         virtual bool fail(std::uint32_t pos, std::string message)                = 0;
         virtual bool variable(std::uint32_t id, type_ref t, unsigned level)      = 0;
         ~value_checker() = default;

        private:
         std::string_view describe(const in_node& n) const
         {
            switch (n.kind)
            {
               case value_kind::list: return "a list";
               case value_kind::object: return "an object";
               case value_kind::boolean: return n.flag ? "true" : "false";
               case value_kind::null: return "null";
               default: return values.text_of(n);
            }
         }
         bool mismatch(const in_node& n, const type_desc& d)
         {
            const bool quoted = n.kind == value_kind::string;
            return fail(n.pos, cat("Expected value of type \"", d.name, "\", found ", quoted ? "\"" : "",
                                   describe(n).substr(0, 60), quoted ? "\"" : "", "."));
         }
         bool scalar(const in_node& n, const type_desc& d, bool exact)
         {
            const value_kind k = n.kind;
            bool             ok = false;
            switch (d.scalar)
            {
               case scalar_class::integer: ok = k == value_kind::integer; break;
               case scalar_class::floating:
                  ok = k == value_kind::integer || k == value_kind::floating;
                  break;
               case scalar_class::string: ok = k == value_kind::string; break;
               case scalar_class::boolean: ok = k == value_kind::boolean; break;
               case scalar_class::id: ok = k == value_kind::string || k == value_kind::integer; break;
               case scalar_class::custom:
                  ok = k == value_kind::integer || k == value_kind::floating ||
                       k == value_kind::string || k == value_kind::boolean;
                  break;
               default: break;
            }
            if (!ok)
               return mismatch(n, d);
            const leaf_token token{k, values.text_of(n), n.flag};
            if (exact || !d.builtin || d.check_by_name)
               ok = !d.check || d.check(token);
            else if (d.scalar == scalar_class::integer)
            {
               // Int by name: any value some <= 32-bit integer holds.
               std::int64_t v = 0;
               ok = read_leaf<std::int64_t>(token, &v) && v >= -2147483648ll && v <= 4294967295ll;
            }
            return ok || mismatch(n, d);
         }
         bool object(std::uint32_t id, const type_desc& d)
         {
            in_node& n = values.nodes[id];
            if (n.kind != value_kind::object)
               return mismatch(n, d);
            bool              ok = true;
            std::vector<char> seen(d.input_count, 0);
            for (std::uint32_t c = n.child; c != nil; c = values.nodes[c].next)
            {
               in_node&          m    = values.nodes[c];
               const std::size_t at   = d.find(values.name_of(m));
               if (at >= d.input_count)
               {
                  ok = fail(m.name_pos, cat("Field \"", values.name_of(m), "\" is not defined by type \"",
                                            d.name, "\"."));
                  continue;
               }
               if (seen[at])
               {
                  ok = fail(m.name_pos, cat("There can be only one input field named \"",
                                            values.name_of(m), "\"."));
                  continue;
               }
               seen[at] = 1;
               m.slot   = d.inputs[at].slot;
               ok       = check(c, d.inputs[at].type, 0, true) && ok;
            }
            for (std::size_t i = 0; i < d.input_count; ++i)
               if (!seen[i] && d.inputs[i].type.is_nonnull())
                  ok = fail(n.pos, cat("Field \"", d.name, ".", d.inputs[i].name, "\" of required type \"",
                                       type_string(d.inputs[i].type), "\" was not provided."));
            return ok;
         }
      };

      class planner final : value_checker
      {
         const schema_desc&  s_;
         const limits&       lim_;
         ast::document&      a_;
         document&           d_;
         std::vector<error>& errs_;
         line_index          lines_;
         bool                fatal_ = false;
         std::size_t         visits_ = 0;
         std::size_t         work_   = 0;

         // Fragments, by index into a_.fragments.
         std::vector<std::uint32_t>    by_name_;
         std::vector<const type_desc*> frag_type_;
         std::vector<char>             frag_used_, frag_active_;

         // The operation being planned.
         const ast::operation* op_ = nullptr;
         std::vector<char>     var_used_;

         // Guard chains: a literal and the chain it extends.
         struct chain
         {
            std::uint32_t parent;
            plan_literal  literal;
         };
         std::vector<chain> chains_;

         struct occurrence
         {
            std::uint32_t    selection;
            std::uint32_t    chain;
            const type_desc* scope;
            std::uint32_t    next;   // in its group
            std::uint32_t    group;
         };
         struct group
         {
            std::string_view key;
            std::uint32_t    first, last, count;
         };
         struct source
         {
            std::uint32_t first_selection;
            std::uint32_t chain;
         };

         // ── Errors ────────────────────────────────────────────────────────
         bool fail(std::uint32_t pos, std::string message) override
         {
            error e;
            e.message = std::move(message);
            lines_.locate(pos, e.line, e.column);
            for (const error& seen : errs_)
               if (seen.line == e.line && seen.column == e.column && seen.message == e.message)
                  return false;
            if (errs_.size() < lim_.max_errors)
               errs_.push_back(std::move(e));
            return false;
         }
         void fatal(std::uint32_t pos, std::string message)
         {
            if (!fatal_)
            {
               fatal_ = true;
               if (errs_.size() >= lim_.max_errors && !errs_.empty())
                  errs_.pop_back();
               fail(pos, std::move(message));
            }
         }

         std::string_view op_name() const { return op_->name; }

         // ── Variables ─────────────────────────────────────────────────────
         bool variable(std::uint32_t id, type_ref t, unsigned level) override
         {
            in_node&               n    = d_.values.nodes[id];
            const std::string_view name = d_.values.text_of(n);
            if (!op_)
               return fail(n.pos, cat("Variable \"$", name, "\" is not allowed here."));
            for (std::uint32_t i = 0; i < op_->vars_count; ++i)
            {
               const plan_variable& v = d_.variables[d_.operations.back().vars_first + i];
               if (v.name != name)
                  continue;
               var_used_[i] = 1;
               n.index      = static_cast<std::uint16_t>(i);
               if (!v.type)
                  return false;  // its definition was already reported
               bool allowed = v.depth == t.depth - level && v.type_name == t.named->name;
               for (unsigned k = 0; allowed && k <= v.depth; ++k)
                  if (t.is_nonnull(level + k) && !((v.nonnull >> k) & 1))
                     allowed = k == 0 && v.default_value != nil &&
                               d_.values.nodes[v.default_value].kind != value_kind::null;
               return allowed ||
                      fail(n.pos, cat("Variable \"$", name, "\" of type \"",
                                      type_string(v.type_name, v.depth, v.nonnull),
                                      "\" used in position expecting type \"", type_string(t, level),
                                      "\"."));
            }
            return fail(n.pos, op_->name.empty()
                                  ? cat("Variable \"$", name, "\" is not defined.")
                                  : cat("Variable \"$", name, "\" is not defined by operation \"",
                                        op_->name, "\"."));
         }

         // ── Directives (@skip, @include) ──────────────────────────────────
         static constexpr type_ref boolean_ref{&leaf_type<bool>::value, 0, 1};

         // `location`: null where @skip / @include apply, else the name of
         // the location for the error.
         void directives(std::uint32_t first, const char* location, std::uint32_t& guard_chain)
         {
            bool seen[2] = {false, false};
            for (std::uint32_t i = first; i != nil; i = a_.directives[i].next)
            {
               const ast::directive& d = a_.directives[i];
               const bool include = d.name == "include";
               if (!include && d.name != "skip")
               {
                  if (d.name == "deprecated" || d.name == "specifiedBy")
                     fail(d.pos, cat("Directive \"@", d.name, "\" may not be used on ",
                                     location ? location : "FIELD", "."));
                  else
                     fail(d.pos, cat("Unknown directive \"@", d.name, "\"."));
                  continue;
               }
               if (location)
               {
                  fail(d.pos, cat("Directive \"@", d.name, "\" may not be used on ", location, "."));
                  continue;
               }
               if (seen[include])
               {
                  fail(d.pos, cat("The directive \"@", d.name, "\" can only be used once at this location."));
                  continue;
               }
               seen[include]       = true;
               std::uint32_t value = nil;
               for (std::uint32_t a = d.args; a != nil; a = a_.arguments[a].next)
               {
                  const ast::argument& arg = a_.arguments[a];
                  if (arg.name != "if")
                     fail(arg.pos, cat("Unknown argument \"", arg.name, "\" on directive \"@", d.name, "\"."));
                  else if (value != nil)
                     fail(arg.pos, "There can be only one argument named \"if\".");
                  else
                     value = arg.value;
               }
               if (value == nil)
               {
                  fail(d.pos, cat("Directive \"@", d.name,
                                  "\" argument \"if\" of type \"Boolean!\" is required, but it was not provided."));
                  continue;
               }
               if (!check(value, boolean_ref, 0, true))
                  continue;
               const in_node& n = d_.values.nodes[value];
               plan_literal   lit;
               if (n.kind == value_kind::variable)
               {
                  lit.var    = n.index;
                  lit.expect = include;
               }
               else
               {
                  lit.expect = n.flag == include;
                  if (lit.expect)
                     continue;  // a constant that always passes
               }
               chains_.push_back(chain{guard_chain, lit});
               guard_chain = static_cast<std::uint32_t>(chains_.size() - 1);
            }
         }

         // ── Fragments ─────────────────────────────────────────────────────
         std::uint32_t find_fragment(std::string_view name) const
         {
            auto it = std::lower_bound(by_name_.begin(), by_name_.end(), name,
                                       [&](std::uint32_t i, std::string_view n) {
                                          return a_.fragments[i].name < n;
                                       });
            return it != by_name_.end() && a_.fragments[*it].name == name ? *it : nil;
         }
         static bool same_type(const type_desc* a, const type_desc* b) noexcept
         {
            return a == b || a->name == b->name;
         }
         // Objects of type `concrete` are of type `t`.
         static bool applies(const type_desc* t, const type_desc* concrete) noexcept
         {
            if (t->kind != type_kind::union_)
               return same_type(t, concrete);
            for (std::size_t i = 0; i < t->possible_count; ++i)
               if (same_type(t->possible[i], concrete))
                  return true;
            return false;
         }
         // Some object type is both of type `a` and of type `b` (spec 5.5.2.3).
         static bool overlap(const type_desc* a, const type_desc* b) noexcept
         {
            if (a->kind != type_kind::union_)
               return applies(b, a);
            for (std::size_t i = 0; i < a->possible_count; ++i)
               if (applies(b, a->possible[i]))
                  return true;
            return false;
         }

         // CollectFields for one concrete object type: every field selection
         // that applies, grouped by response key in order of appearance.
         struct collector
         {
            std::vector<occurrence>    occurrences;
            std::vector<group>         groups;
            std::vector<std::uint32_t> table;  // open addressing over groups

            static std::uint64_t hash(std::string_view s) noexcept
            {
               return ::psio::detail::lookup::full_hash(s);
            }
            void rehash()
            {
               table.assign(std::max<std::size_t>(16, std::bit_ceil(groups.size() * 4)), nil);
               for (std::uint32_t g = 0; g < groups.size(); ++g)
                  place(g);
            }
            void place(std::uint32_t g)
            {
               std::size_t at = hash(groups[g].key) & (table.size() - 1);
               while (table[at] != nil)
                  at = (at + 1) & (table.size() - 1);
               table[at] = g;
            }
            void add(std::string_view key, occurrence o)
            {
               if (table.empty() || groups.size() * 2 >= table.size())
                  rehash();
               std::size_t at = hash(key) & (table.size() - 1);
               o.next         = nil;
               const auto index = static_cast<std::uint32_t>(occurrences.size());
               occurrences.push_back(o);
               while (table[at] != nil)
               {
                  group& g = groups[table[at]];
                  if (g.key == key)
                  {
                     occurrences[index].group = table[at];
                     occurrences[g.last].next = index;
                     g.last                   = index;
                     ++g.count;
                     return;
                  }
                  at = (at + 1) & (table.size() - 1);
               }
               table[at]                = static_cast<std::uint32_t>(groups.size());
               occurrences[index].group = table[at];
               groups.push_back(group{key, index, index, 1});
            }
         };

         void collect(collector& out, std::uint32_t first, const type_desc* scope,
                      const type_desc* concrete, std::uint32_t guard_chain, std::size_t nesting)
         {
            for (std::uint32_t i = first; i != nil && !fatal_; i = a_.selections[i].next)
            {
               const ast::selection& s = a_.selections[i];
               if (++visits_ > lim_.max_fields)
                  return fatal(s.pos, "Query selects more fields than the limit allows.");
               std::uint32_t c = guard_chain;
               directives(s.directives, nullptr, c);
               if (s.kind == ast::selection_kind::field)
               {
                  out.add(s.alias.empty() ? s.name : s.alias, occurrence{i, c, scope, nil, 0});
                  continue;
               }
               const type_desc* on  = scope;
               std::uint32_t    set = s.set;
               std::uint32_t    f   = nil;
               if (s.kind == ast::selection_kind::spread)
               {
                  f = find_fragment(s.name);
                  if (f == nil)
                  {
                     fail(s.pos, cat("Unknown fragment \"", s.name, "\"."));
                     continue;
                  }
                  frag_used_[f] = 1;
                  if (frag_active_[f])
                  {
                     fail(s.pos, cat("Cannot spread fragment \"", s.name, "\" within itself."));
                     continue;
                  }
                  on  = frag_type_[f];
                  set = a_.fragments[f].set;
                  if (!on)
                     continue;  // its type condition was already reported
               }
               else if (!s.name.empty())
               {
                  on = s_.type_named(s.name);
                  if (!on)
                  {
                     fail(s.pos, cat("Unknown type \"", s.name, "\"."));
                     continue;
                  }
                  if (!on->is_composite())
                  {
                     fail(s.pos, cat("Fragment cannot condition on non composite type \"", s.name, "\"."));
                     continue;
                  }
               }
               if (!overlap(scope, on))
               {
                  fail(s.pos, f != nil ? cat("Fragment \"", s.name, "\" cannot be spread here as objects of type \"",
                                             scope->name, "\" can never be of type \"", on->name, "\".")
                                       : cat("Fragment cannot be spread here as objects of type \"", scope->name,
                                             "\" can never be of type \"", on->name, "\"."));
                  continue;
               }
               if (!applies(on, concrete))
                  continue;
               if (nesting >= lim_.max_depth)
                  return fatal(s.pos, "Fragments are nested deeper than the limit allows.");
               if (f != nil)
                  frag_active_[f] = 1;
               // On the object type itself, look fields up in the descriptor
               // being planned (one name can have several, all equivalent).
               collect(out, set, same_type(on, concrete) ? concrete : on, concrete, c, nesting + 1);
               if (f != nil)
                  frag_active_[f] = 0;
            }
         }

         // ── Arguments ─────────────────────────────────────────────────────
         bool values_equal(std::uint32_t a, std::uint32_t b) const
         {
            if (a == nil || b == nil)
               return a == b;
            const in_node& x = d_.values.nodes[a];
            const in_node& y = d_.values.nodes[b];
            if (x.kind != y.kind || x.flag != y.flag || d_.values.text_of(x) != d_.values.text_of(y))
               return false;
            if (x.kind == value_kind::object)
            {
               std::size_t nx = 0, ny = 0;
               for (std::uint32_t c = y.child; c != nil; c = d_.values.nodes[c].next)
                  ++ny;
               for (std::uint32_t c = x.child; c != nil; c = d_.values.nodes[c].next, ++nx)
               {
                  std::uint32_t other = y.child;
                  while (other != nil && d_.values.name_of(d_.values.nodes[other]) !=
                                            d_.values.name_of(d_.values.nodes[c]))
                     other = d_.values.nodes[other].next;
                  if (other == nil || !values_equal(c, other))
                     return false;
               }
               return nx == ny;
            }
            std::uint32_t cx = x.child, cy = y.child;
            for (; cx != nil && cy != nil; cx = d_.values.nodes[cx].next, cy = d_.values.nodes[cy].next)
               if (!values_equal(cx, cy))
                  return false;
            return cx == cy;
         }

         // The values of `s`'s arguments in the order of `f`'s arguments.
         bool arguments(const ast::selection& s, const type_desc* parent, const field_desc* f,
                        std::vector<std::uint32_t>& out)
         {
            const std::size_t count = f ? f->arg_count : 0;
            out.assign(count, nil);
            bool ok = true;
            for (std::uint32_t a = s.args; a != nil; a = a_.arguments[a].next)
            {
               const ast::argument& arg = a_.arguments[a];
               std::size_t          at  = 0;
               while (at < count && f->args[at].name != arg.name)
                  ++at;
               if (at == count)
                  ok = fail(arg.pos, cat("Unknown argument \"", arg.name, "\" on field \"", parent->name, ".",
                                         s.name, "\"."));
               else if (out[at] != nil)
                  ok = fail(arg.pos, cat("There can be only one argument named \"", arg.name, "\"."));
               else
               {
                  out[at] = arg.value;
                  ok      = check(arg.value, f->args[at].type, 0, true) && ok;
               }
            }
            for (std::size_t at = 0; at < count; ++at)
               if (out[at] == nil && f->args[at].type.is_nonnull())
                  ok = fail(s.pos, cat("Field \"", s.name, "\" argument \"", f->args[at].name, "\" of type \"",
                                       type_string(f->args[at].type),
                                       "\" is required, but it was not provided."));
            return ok;
         }

         // ── Selection sets ────────────────────────────────────────────────
         static constexpr field_desc typename_field{.name = "__typename",
                                                    .type = {&leaf_type<std::string>::value, 0, 1},
                                                    .cost = 0};

         const field_desc* lookup(const ast::selection& s, const type_desc* scope, meta_field& meta)
         {
            meta = meta_field::none;
            if (s.name == "__typename")
            {
               meta = meta_field::type_name;
               return &typename_field;
            }
            if (s_.meta_fields && scope == s_.query)
            {
               if (s.name == "__schema")
               {
                  meta = meta_field::schema;
                  return &s_.meta_fields[0];
               }
               if (s.name == "__type")
               {
                  meta = meta_field::type;
                  return &s_.meta_fields[1];
               }
            }
            if (scope->kind == type_kind::object)
            {
               const std::size_t at = scope->find(s.name);
               if (at < scope->field_count)
                  return &scope->fields[at];
            }
            fail(s.pos, scope->kind == type_kind::union_
                           ? cat("Cannot query field \"", s.name, "\" on type \"", scope->name,
                                 "\". Did you mean to use an inline fragment on one of its member types?")
                           : cat("Cannot query field \"", s.name, "\" on type \"", scope->name, "\"."));
            return nullptr;
         }

         void flatten(std::uint32_t guard_chain)
         {
            plan_guard g;
            g.first = static_cast<std::uint32_t>(d_.literals.size());
            for (std::uint32_t c = guard_chain; c != nil; c = chains_[c].parent)
               d_.literals.push_back(chains_[c].literal);
            g.count = static_cast<std::uint32_t>(d_.literals.size()) - g.first;
            d_.guards.push_back(g);
         }

         std::uint32_t set_count(const type_desc* t) const noexcept
         {
            return t->kind == type_kind::union_ ? t->possible_count : 1;
         }

         // Fields with one response key in the sets of two different object
         // types must have the same shape (spec 5.3.2, SameResponseShape).
         void same_shape(std::uint32_t set_a, std::uint32_t set_b, std::size_t depth)
         {
            const plan_set a = d_.sets[set_a];
            const plan_set b = d_.sets[set_b];
            for (std::uint32_t i = 0; i < a.count && !fatal_; ++i)
               for (std::uint32_t j = 0; j < b.count && !fatal_; ++j)
               {
                  const plan_field& x = d_.fields[a.first + i];
                  const plan_field& y = d_.fields[b.first + j];
                  if (++work_ > lim_.max_fields * 64)
                     return fatal(0, "Query is too complex to validate.");
                  if (x.alternate || y.alternate || d_.key_of(x) != d_.key_of(y))
                     continue;
                  const type_ref tx = x.desc->type, ty = y.desc->type;
                  const bool     leaf = tx.named->is_leaf() || ty.named->is_leaf();
                  if (tx.depth != ty.depth || tx.nonnull != ty.nonnull ||
                      (leaf && tx.named->name != ty.named->name))
                  {
                     error e;
                     e.message = cat("Fields \"", d_.key_of(x), "\" conflict because they return conflicting types \"",
                                     type_string(tx), "\" and \"", type_string(ty),
                                     "\". Use different aliases on the fields to fetch both if this was intentional.");
                     e.line    = y.line;
                     e.column  = y.column;
                     bool seen = false;
                     for (const error& o : errs_)
                        seen = seen || (o.line == e.line && o.column == e.column && o.message == e.message);
                     if (!seen && errs_.size() < lim_.max_errors)
                        errs_.push_back(std::move(e));
                  }
                  else if (!leaf && x.child != nil && y.child != nil && depth < lim_.max_depth)
                     for (std::uint32_t p = 0; p < set_count(tx.named); ++p)
                        for (std::uint32_t q = 0; q < set_count(ty.named); ++q)
                           same_shape(x.child + p, y.child + q, depth + 1);
               }
         }

         // The selection sets of a field of composite type `type`: one, or
         // one per union member. Returns the first set's id.
         std::uint32_t build(const type_desc* type, const std::vector<source>& sources,
                             std::uint32_t pos, std::size_t depth)
         {
            if (depth >= lim_.max_depth)
            {
               fatal(pos, "Query is nested deeper than the limit allows.");
               return nil;
            }
            const std::uint32_t count = set_count(type);
            const auto          first = static_cast<std::uint32_t>(d_.sets.size());
            d_.sets.resize(d_.sets.size() + count);
            for (std::uint32_t i = 0; i < count && !fatal_; ++i)
               fill(first + i, type->kind == type_kind::union_ ? type->possible[i] : type, type, sources, depth);
            for (std::uint32_t i = 0; i < count && !fatal_; ++i)
               for (std::uint32_t j = i + 1; j < count && !fatal_; ++j)
                  same_shape(first + i, first + j, depth);
            return first;
         }

         void fill(std::uint32_t set_id, const type_desc* concrete, const type_desc* scope,
                   const std::vector<source>& sources, std::size_t depth)
         {
            collector found;
            for (const source& src : sources)
               collect(found, src.first_selection, scope, concrete, src.chain, 0);
            if (fatal_)
               return;

            // 1. Resolve and check each response key.
            struct resolved
            {
               const field_desc* desc = nullptr;
               meta_field        meta = meta_field::none;
               std::uint32_t     args = nil;
               bool              ok            = false;
               bool              unconditional = false;
               std::uint8_t      bit           = 0;      // plan_field::group, when positions vary
               bool              closed        = false;  // no later position is needed
               std::uint32_t     first_entry = nil, entries = 0;
            };
            std::vector<resolved>      keys(found.groups.size());
            std::vector<std::uint32_t> ids, other;
            unsigned                   bits = 0;
            for (std::uint32_t gi = 0; gi < found.groups.size(); ++gi)
            {
               const group&          g  = found.groups[gi];
               const occurrence&     o0 = found.occurrences[g.first];
               const ast::selection& s0 = a_.selections[o0.selection];
               resolved&             r  = keys[gi];
               const field_desc*     f  = lookup(s0, o0.scope, r.meta);
               bool                  ok = f && arguments(s0, o0.scope, f, ids);
               for (std::uint32_t oi = g.first; oi != nil; oi = found.occurrences[oi].next)
               {
                  const occurrence&     o = found.occurrences[oi];
                  const ast::selection& s = a_.selections[o.selection];
                  r.unconditional         = r.unconditional || o.chain == nil;
                  const field_desc* fo    = f;
                  if (oi != g.first)
                  {
                     meta_field m2;
                     fo = lookup(s, o.scope, m2);
                     if (!fo)
                        ok = false;
                     else if (f && s.name != s0.name)
                        ok = fail(s.pos, cat("Fields \"", g.key, "\" conflict because \"", s0.name, "\" and \"",
                                             s.name, "\" are different fields. Use different aliases on the "
                                                     "fields to fetch both if this was intentional."));
                     else if (!arguments(s, o.scope, fo, other))
                        ok = false;
                     else if (f)
                        for (std::size_t k = 0; k < ids.size() && k < other.size(); ++k)
                           if (!values_equal(ids[k], other[k]))
                           {
                              ok = fail(s.pos, cat("Fields \"", g.key, "\" conflict because they have differing "
                                                   "arguments. Use different aliases on the fields to fetch both "
                                                   "if this was intentional."));
                              break;
                           }
                  }
                  if (fo)
                  {
                     if (fo->type.named->is_leaf() && s.has_set)
                        ok = fail(s.pos, cat("Field \"", s.name, "\" must not have a selection since type \"",
                                             type_string(fo->type), "\" has no subfields."));
                     else if (!fo->type.named->is_leaf() && !s.has_set)
                        ok = fail(s.pos, cat("Field \"", s.name, "\" of type \"", type_string(fo->type),
                                             "\" must have a selection of subfields. Did you mean \"", s.name,
                                             " { ... }\"?"));
                  }
               }
               if (!ok)
                  continue;
               r.ok   = true;
               r.desc = f;
               if (!ids.empty())
               {
                  r.args = static_cast<std::uint32_t>(d_.args.size());
                  d_.args.insert(d_.args.end(), ids.begin(), ids.end());
               }
               // Selected again after a conditional first selection: the key's
               // position depends on the request (63 such keys per set get
               // exact positions; any beyond keep the first one).
               if (g.count > 1 && o0.chain != nil && bits < 63)
                  r.bit = static_cast<std::uint8_t>(++bits);
            }

            // 2. One entry per key, in order of first selection (and one per
            // further candidate position of a key whose position varies).
            const auto first_field = static_cast<std::uint32_t>(d_.fields.size());
            for (std::uint32_t oi = 0; oi < found.occurrences.size(); ++oi)
            {
               const occurrence& o  = found.occurrences[oi];
               const group&      g  = found.groups[o.group];
               resolved&         r  = keys[o.group];
               const bool        first = oi == g.first;
               if (!r.ok || (!first && (!r.bit || r.closed)))
                  continue;
               plan_field p;
               p.key = static_cast<std::uint32_t>(d_.pool.size());
               d_.pool += ",\"";
               d_.pool += g.key;
               d_.pool += "\":";
               p.key_len = static_cast<std::uint32_t>(d_.pool.size()) - p.key;
               p.slot    = r.desc->slot;
               p.meta    = r.meta;
               p.desc    = r.desc;
               p.args    = r.args;
               lines_.locate(a_.selections[o.selection].pos, p.line, p.column);
               if (r.bit)
               {
                  p.group     = r.bit;
                  p.alternate = !first;
                  r.closed    = o.chain == nil;
                  if (o.chain != nil)
                  {
                     p.guards = static_cast<std::uint32_t>(d_.guards.size());
                     flatten(o.chain);
                     p.guard_count = 1;
                  }
               }
               else if (!r.unconditional)
               {
                  p.guards = static_cast<std::uint32_t>(d_.guards.size());
                  for (std::uint32_t k = g.first; k != nil; k = found.occurrences[k].next)
                     flatten(found.occurrences[k].chain);
                  p.guard_count = static_cast<std::uint32_t>(d_.guards.size()) - p.guards;
               }
               if (r.first_entry == nil)
                  r.first_entry = static_cast<std::uint32_t>(d_.fields.size());
               ++r.entries;
               d_.fields.push_back(p);
            }
            const auto field_count = static_cast<std::uint32_t>(d_.fields.size()) - first_field;
            d_.sets[set_id]        = plan_set{first_field, field_count};

            // 3. The selection sets below, once per key.
            std::vector<source> below;
            for (std::uint32_t gi = 0; gi < found.groups.size() && !fatal_; ++gi)
            {
               const resolved& r = keys[gi];
               if (!r.ok || r.desc->type.named->is_leaf())
                  continue;
               const group& g = found.groups[gi];
               below.clear();
               for (std::uint32_t oi = g.first; oi != nil; oi = found.occurrences[oi].next)
               {
                  const occurrence& o = found.occurrences[oi];
                  // The only occurrence: the field's own guard already holds.
                  below.push_back(source{a_.selections[o.selection].set, g.count == 1 ? nil : o.chain});
               }
               const std::uint32_t child = build(r.desc->type.named, below,
                                                 a_.selections[found.occurrences[g.first].selection].pos, depth + 1);
               for (std::uint32_t k = 0, seen = 0; seen < r.entries; ++k)
               {
                  plan_field& p = d_.fields[r.first_entry + k];
                  if (p.desc == r.desc && d_.key_of(p) == g.key)
                  {
                     p.child = child;
                     ++seen;
                  }
               }
            }
         }

         // ── Definitions ───────────────────────────────────────────────────
         void fragments()
         {
            const std::size_t n = a_.fragments.size();
            by_name_.resize(n);
            for (std::uint32_t i = 0; i < n; ++i)
               by_name_[i] = i;
            std::stable_sort(by_name_.begin(), by_name_.end(), [&](std::uint32_t x, std::uint32_t y) {
               return a_.fragments[x].name < a_.fragments[y].name;
            });
            for (std::size_t i = 1; i < n; ++i)
               if (a_.fragments[by_name_[i]].name == a_.fragments[by_name_[i - 1]].name)
                  fail(a_.fragments[by_name_[i]].pos,
                       cat("There can be only one fragment named \"", a_.fragments[by_name_[i]].name, "\"."));
            frag_type_.assign(n, nullptr);
            frag_used_.assign(n, 0);
            frag_active_.assign(n, 0);
            for (std::size_t i = 0; i < n; ++i)
            {
               const ast::fragment& f = a_.fragments[i];
               const type_desc*     t = s_.type_named(f.on);
               if (!t)
                  fail(f.on_pos, cat("Unknown type \"", f.on, "\"."));
               else if (!t->is_composite())
                  fail(f.on_pos, cat("Fragment \"", f.name, "\" cannot condition on non composite type \"",
                                     f.on, "\"."));
               else
                  frag_type_[i] = t;
               std::uint32_t none = nil;
               directives(f.directives, "FRAGMENT_DEFINITION", none);
            }
         }

         void operation(const ast::operation& op)
         {
            plan_operation po;
            po.kind       = op.kind;
            po.name       = std::string{op.name};
            po.vars_first = static_cast<std::uint32_t>(d_.variables.size());
            po.vars_count = op.vars_count;
            lines_.locate(op.pos, po.line, po.column);
            d_.operations.push_back(std::move(po));
            const std::size_t self = d_.operations.size() - 1;

            for (std::size_t i = 0; i + 1 < d_.operations.size(); ++i)
               if (d_.operations[i].name == op.name && !op.name.empty())
                  fail(op.pos, cat("There can be only one operation named \"", op.name, "\"."));
            if (op.name.empty() && a_.operations.size() > 1)
               fail(op.pos, "This anonymous operation must be the only defined operation.");

            const type_desc* root     = nullptr;
            const char*      location = "QUERY";
            switch (op.kind)
            {
               case ast::operation_kind::query: root = s_.query; break;
               case ast::operation_kind::mutation:
                  location = "MUTATION";
                  root     = s_.mutation;
                  if (!root)
                     fail(op.pos, "Schema is not configured for mutations.");
                  break;
               case ast::operation_kind::subscription:
                  location = "SUBSCRIPTION";
                  fail(op.pos, "Subscriptions are not supported.");
                  break;
            }

            op_ = nullptr;  // no variable is in scope inside variable definitions
            for (std::uint32_t i = 0; i < op.vars_count; ++i)
            {
               const ast::variable& v = a_.variables[op.vars_first + i];
               plan_variable        pv;
               pv.name      = std::string{v.name};
               pv.type_name = std::string{v.type.name};
               pv.depth     = v.type.depth;
               pv.nonnull   = v.type.nonnull;
               lines_.locate(v.pos, pv.line, pv.column);
               for (std::uint32_t k = 0; k < i; ++k)
                  if (a_.variables[op.vars_first + k].name == v.name)
                  {
                     fail(v.pos, cat("There can be only one variable named \"$", v.name, "\"."));
                     break;
                  }
               const type_desc* t = s_.type_named(v.type.name);
               if (!t)
                  fail(v.type.pos, cat("Unknown type \"", v.type.name, "\"."));
               else if (!t->is_input())
                  fail(v.type.pos, cat("Variable \"$", v.name, "\" cannot be non-input type \"",
                                       type_string(v.type.name, v.type.depth, v.type.nonnull), "\"."));
               else
               {
                  pv.type = t;
                  if (v.default_value != nil && check(v.default_value, pv.ref(), 0, false))
                     pv.default_value = v.default_value;
               }
               std::uint32_t none = nil;
               directives(v.directives, "VARIABLE_DEFINITION", none);
               d_.variables.push_back(std::move(pv));
            }

            op_ = &op;
            var_used_.assign(op.vars_count, 0);
            std::uint32_t none = nil;
            directives(op.directives, location, none);
            if (root)
            {
               const std::vector<source> sources{source{op.set, nil}};
               d_.operations[self].root = build(root, sources, op.pos, 0);
            }
            for (std::uint32_t i = 0; i < op.vars_count && !fatal_; ++i)
               if (!var_used_[i])
               {
                  const ast::variable& v = a_.variables[op.vars_first + i];
                  fail(v.pos, op.name.empty()
                                 ? cat("Variable \"$", v.name, "\" is never used.")
                                 : cat("Variable \"$", v.name, "\" is never used in operation \"", op.name,
                                       "\"."));
               }
            op_ = nullptr;
         }

        public:
         planner(const schema_desc& s, const limits& lim, ast::document& a, document& d,
                 std::vector<error>& errs)
             : value_checker(d.values), s_(s), lim_(lim), a_(a), d_(d), errs_(errs), lines_(a.source)
         {
         }

         bool run()
         {
            d_.schema = &s_;
            d_.values = std::move(a_.values);
            fragments();
            for (const ast::operation& op : a_.operations)
            {
               if (fatal_)
                  break;
               operation(op);
            }
            if (!fatal_)
               for (std::size_t i = 0; i < a_.fragments.size(); ++i)
                  if (!frag_used_[i])
                     fail(a_.fragments[i].pos, cat("Fragment \"", a_.fragments[i].name, "\" is never used."));
            return errs_.empty();
         }
      };
   }  // namespace detail

   // Phases 1 and 2: parse `text` and validate it against `schema`. Returns
   // true with the plan in `out`; false with the errors in `errors` (`out` is
   // then unusable). The plan does not refer to `text`.
   inline bool compile(const schema_desc&  schema,
                       std::string_view    text,
                       document&           out,
                       std::vector<error>& errors,
                       const limits&       lim = {})
   {
      out = document{};
      errors.clear();
      ast::document tree;
      error         syntax;
      if (!parse(text, lim, tree, syntax))
      {
         errors.push_back(std::move(syntax));
         return false;
      }
      return detail::planner{schema, lim, tree, out, errors}.run();
   }

   // A small cache of plans keyed by query text (64-bit hash, confirmed by
   // comparing the text). Least recently used entry is replaced. Not
   // synchronised: one per thread, or guard it. A plan depends on the schema
   // and on the limits it was compiled under; use one cache per pair.
   class plan_cache
   {
      struct entry
      {
         std::uint64_t hash = 0;
         std::uint64_t used = 0;
         std::string   text;
         document      plan;
      };
      std::vector<entry> entries_;
      std::size_t        capacity_;
      std::uint64_t      clock_ = 0;

     public:
      explicit plan_cache(std::size_t capacity = 64) : capacity_(capacity ? capacity : 1)
      {
         entries_.reserve(capacity_);
      }

      std::size_t size() const noexcept { return entries_.size(); }
      void        clear() { entries_.clear(); }

      const document* find(std::string_view text)
      {
         const std::uint64_t h = ::psio::detail::lookup::full_hash(text);
         for (entry& e : entries_)
            if (e.hash == h && e.text == text)
            {
               e.used = ++clock_;
               return &e.plan;
            }
         return nullptr;
      }

      // The plan for `text`: the cached one, or a freshly compiled one (which
      // is cached when valid). nullptr with `errors` filled when invalid. The
      // pointer stays valid until the entry is replaced by a later get().
      const document* get(const schema_desc&  schema,
                          std::string_view    text,
                          std::vector<error>& errors,
                          const limits&       lim = {})
      {
         if (const document* hit = find(text))
         {
            errors.clear();
            return hit;
         }
         document plan;
         if (!compile(schema, text, plan, errors, lim))
            return nullptr;
         entry* slot = nullptr;
         if (entries_.size() < capacity_)
            slot = &entries_.emplace_back();
         else
            slot = &*std::min_element(entries_.begin(), entries_.end(),
                                      [](const entry& a, const entry& b) { return a.used < b.used; });
         slot->hash = ::psio::detail::lookup::full_hash(text);
         slot->used = ++clock_;
         slot->text.assign(text);
         slot->plan = std::move(plan);
         return &slot->plan;
      }
   };

}  // namespace psio::graphql
