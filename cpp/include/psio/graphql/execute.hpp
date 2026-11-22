#pragma once
//
// psio/graphql/execute.hpp — phase 3: run a plan against a root value and
// write the response.
//
// The executor is written against a *source*: a C++ value, or a psio view
// over encoded bytes (`view<T, Fmt, Store>`; a `buffer<T, Fmt, Store>` is
// viewed). The two differ in four accessors only —
//
//                       value                 view
//   record field I      obj.*member           v.get<I>()  (sub-view / scalar)
//   optional            has_value(), *o       has_value(), *v
//   list                range-for             range-for (elements are views)
//   variant             index, get_if<I>      index(), visit
//
// — so a selected field of an encoded row is read straight from the buffer:
// unselected members are never touched, strings and byte sequences are
// escaped / hex-encoded from the buffer into the response, nested records and
// lists stay sub-views, and a trailing optional the writer of an older row
// did not know reads as null (pssz_view.hpp). A view must be over validated
// bytes, as for every psio view.
//
// Each object type has a table of resolvers indexed by slot; the plan gives
// the slot, so a field costs one indirect call and no name lookup. Leaves
// are written by psio's I-JSON writer (json.hpp) into the response string;
// input scalars are read by its typed reader.
//
// Errors follow the spec (6.4.4): a field error is recorded with its
// location and response path, the field becomes null, and null propagates to
// the nearest nullable parent. Nothing here throws or aborts.

#include <psio/cpo.hpp>
#include <psio/graphql/plan.hpp>

#include <charconv>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio::graphql
{
   // One request against a plan.
   struct request
   {
      // Which operation of the document to run; may be empty when the
      // document has exactly one.
      std::string_view operation_name{};
      // The variable values: the text of a JSON object, or empty.
      std::string_view variables{};

      // Attaches a per-request object that reflected methods taking a
      // `psio::graphql::context&` reach with `context::user<T>()`.
      template <typename T>
      request& user(T& object) noexcept
      {
         user_ = const_cast<void*>(static_cast<const void*>(&object));
         tag_  = &detail::user_tag<T>;
         return *this;
      }

      void*       user_ = nullptr;
      const void* tag_  = nullptr;
   };

   // A (possibly absent) input value: a node of the plan's literals or of
   // the request's variable values.
   struct in_ref
   {
      const in_store* store = nullptr;
      std::uint32_t   id    = nil;

      explicit       operator bool() const noexcept { return id != nil; }
      const in_node& node() const noexcept { return store->nodes[id]; }
      leaf_token     token() const noexcept
      {
         const in_node& n = node();
         return leaf_token{n.kind, store->text_of(n), n.flag};
      }
   };

   // The state of one request. Internal; reflected methods see its
   // `context` base.
   struct exec final : context
   {
      struct segment
      {
         const plan_field* field;  // null: a list index
         std::size_t       index;
      };
      // The response path of the value being written. Inline storage for
      // every plan under the default depth limit; the heap beyond it.
      class path_stack
      {
         static constexpr std::size_t inline_capacity = 64;
         segment              inline_[inline_capacity];
         std::vector<segment> heap_;
         std::size_t          size_ = 0;

        public:
         void push_back(segment s)
         {
            if (size_ < inline_capacity)
               inline_[size_] = s;
            else
               heap_.push_back(s);
            ++size_;
         }
         void pop_back() noexcept
         {
            if (size_-- > inline_capacity)
               heap_.pop_back();
         }
         segment& back() noexcept { return (*this)[size_ - 1]; }
         segment& operator[](std::size_t i) noexcept
         {
            return i < inline_capacity ? inline_[i] : heap_[i - inline_capacity];
         }
         std::size_t size() const noexcept { return size_; }
      };

      detail::ji::writer    w;
      const document&       doc;
      const limits&         lim;
      const plan_operation* op = nullptr;
      std::vector<error>    errors;
      in_store              request_values;
      std::vector<in_ref>   vars;
      path_stack            path;
      std::uint64_t         fields  = 0;
      bool                  aborted = false;

      exec(std::string& out, const document& d, const limits& l, const request& r)
          : w{out, "graphql", false}, doc(d), lim(l)
      {
         user_ = r.user_;
         tag_  = r.tag_;
      }

      void add_error(std::string message, std::uint32_t line, std::uint32_t column, bool with_path)
      {
         if (errors.size() >= lim.max_errors)
            return;
         error e;
         e.message = std::move(message);
         e.line    = line;
         e.column  = column;
         if (with_path)
         {
            e.path = "[";
            for (std::size_t i = 0; i < path.size(); ++i)
            {
               const segment& s = path[i];
               if (e.path.size() > 1)
                  e.path += ',';
               if (s.field)
               {
                  e.path += '"';
                  e.path += doc.key_of(*s.field);
                  e.path += '"';
               }
               else
                  e.path += std::to_string(s.index);
            }
            e.path += ']';
         }
         errors.push_back(std::move(e));
      }

      // A field error at the current path. Returns false: "this value is
      // null; propagate".
      bool fail_field(const plan_field& f, std::string message)
      {
         add_error(std::move(message), f.line, f.column, true);
         return false;
      }
      // Ends the request: no further output, `data` is null.
      bool abort(std::string message)
      {
         if (!aborted)
         {
            aborted = true;
            if (errors.size() >= lim.max_errors && !errors.empty())
               errors.pop_back();
            add_error(std::move(message), 0, 0, false);
         }
         return false;
      }

      // The value a node stands for: itself, or its variable's value.
      in_ref resolve(in_ref r) const noexcept
      {
         if (r && r.node().kind == value_kind::variable)
            return vars[r.node().index];
         return r;
      }
      // Argument `index` of field `f` (absent when not given).
      in_ref arg(const plan_field& f, std::size_t index) const noexcept
      {
         return f.args == nil ? in_ref{} : resolve(in_ref{&doc.values, doc.args[f.args + index]});
      }

      bool literal(const plan_literal& l) const noexcept
      {
         if (l.var == nil)
            return l.expect;
         const in_ref v = vars[l.var];
         return v && v.node().flag == l.expect;
      }
      bool selected(const plan_field& f) const noexcept
      {
         for (std::uint32_t g = 0; g < f.guard_count; ++g)
         {
            const plan_guard& guard = doc.guards[f.guards + g];
            bool              pass  = true;
            for (std::uint32_t i = 0; i < guard.count && pass; ++i)
               pass = literal(doc.literals[guard.first + i]);
            if (pass)
               return true;
         }
         return false;
      }

      void null() { w.append(std::string_view{"null"}); }
   };

   namespace detail
   {
      using handler = bool (*)(exec&, const void* source, const plan_field&);

      // Writes the selection set `set_id` of one object: per selected field
      // its key and the value its resolver writes.
      inline bool run_set(exec&            e,
                          std::uint32_t    set_id,
                          const void*      source,
                          const handler*   table,
                          std::string_view type_name)
      {
         const plan_set          set = e.doc.sets[set_id];
         const plan_field*       f   = e.doc.fields.data() + set.first;
         const plan_field* const end = f + set.count;
         const char* const       pool = e.doc.pool.data();
         e.w.put('{');
         std::size_t   skip    = 1;  // the first key is written without its comma
         std::uint64_t written = 0;  // keys that have several positions (plan_field::group)
         for (; f != end; ++f)
         {
            if (f->guard_count && !e.selected(*f))
               continue;
            if (f->group)
            {
               if (written & (std::uint64_t{1} << f->group))
                  continue;
               written |= std::uint64_t{1} << f->group;
            }
            if (++e.fields > e.lim.max_response_fields || e.w.n > e.lim.max_response_bytes)
               return e.abort("Response is larger than the limit allows.");
            e.w.append(std::string_view{pool + f->key + skip, f->key_len - skip});
            skip = 0;
            if (f->meta == meta_field::type_name)
            {
               char* out = e.w.reserve(type_name.size() + 2);
               *out++    = '"';
               std::memcpy(out, type_name.data(), type_name.size());
               out += type_name.size();
               *out++ = '"';
               e.w.commit(out);
               continue;
            }
            e.path.push_back(exec::segment{f, 0});
            const bool ok = f->meta == meta_field::none
                               ? table[f->slot](e, source, *f)
                               : e.doc.schema->resolve_meta(e, *f, f->meta == meta_field::schema ? 0 : 1);
            e.path.pop_back();
            if (!ok)
               return false;
         }
         e.w.put('}');
         return true;
      }

      template <typename T, typename Src>
      struct handlers;

      template <typename S>
      bool write(exec& e, S& s, const plan_field& f, std::uint32_t set);

      // ── Leaves ───────────────────────────────────────────────────────────
      inline bool leaf_failed(exec& e, const plan_field& f, std::size_t mark)
      {
         if (e.w.err.empty())
            return false;
         // "json: string is not valid UTF-8" -> "string is not valid UTF-8".
         std::string_view what = e.w.err;
         if (const auto colon = what.find(": "); colon != std::string_view::npos && colon < 8)
            what.remove_prefix(colon + 2);
         std::string message = cat("Cannot represent value: ", what);
         e.w.err           = {};
         e.w.has_invariant = false;
         e.w.n   = mark;
         e.fail_field(f, std::move(message));
         return true;
      }

      template <typename L>
      bool write_leaf(exec& e, const L& v, const plan_field& f)
      {
         const std::size_t mark = e.w.n;
         if constexpr (std::is_same_v<L, std::string_view>)
            ji::write_string(e.w, v, true);
         else if constexpr (std::is_same_v<L, page_cursor>)
            ji::write_hex(e.w, reinterpret_cast<const unsigned char*>(v.key.data()), v.key.size());
         else if constexpr (ReflectedEnum<L> && !is_text_adapter_v<L>)
         {
            // A GraphQL enum has names only (I-JSON would write the number).
            const auto name = reflect_enum<L>::name_of(v);
            if (!name)
               return e.fail_field(f, cat("Enum \"", leaf_meta_of<L>().name, "\" has no name for this value."));
            char* out = e.w.reserve(name->size() + 2);
            *out++    = '"';
            std::memcpy(out, name->data(), name->size());
            out += name->size();
            *out++ = '"';
            e.w.commit(out);
         }
         else
            ji::encode_value<ijson, ji::int_mode::natural>(v, e.w);
         return !leaf_failed(e, f, mark);
      }

      // A leaf read from encoded bytes: strings and byte sequences go from
      // the buffer to the response; anything else is a small value.
      template <typename L, typename V>
      bool write_leaf_view(exec& e, const V& v, const plan_field& f)
      {
         const std::size_t mark = e.w.n;
         if constexpr (requires { v.view_(); })
         {
            ji::write_string(e.w, v.view_(), true);
            return !leaf_failed(e, f, mark);
         }
         else if constexpr (is_bytes_v<L> && !requires { v.get(); })
         {
            const std::span<const char> bytes = v._psio_data();
            ji::write_hex(e.w, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
            return true;
         }
         else if constexpr (requires {
                               { v.get() } -> std::convertible_to<L>;
                            })
         {
            const L value = v.get();
            return write_leaf<L>(e, value, f);
         }
         else
         {
            // A leaf with a wire form of its own and no value accessor (a
            // record or enum with a text adapter): decode that one value.
            const L value = ::psio::decode<L>(typename V::format_type{}, v._psio_data());
            return write_leaf<L>(e, value, f);
         }
      }

      // ID: a string as it is, an integer as a string of its digits.
      template <typename S>
      bool write_id(exec& e, S& s, const plan_field& f)
      {
         using U = std::remove_cv_t<S>;
         using L = logical_t<U>;
         if constexpr (is_buffer_v<U>)
         {
            auto v = as_view(s);
            return write_id(e, v, f);
         }
         else if constexpr (shape_of<L>() == shape::nullable)
         {
            if (!s)
            {
               e.null();
               return true;
            }
            auto&& inner = *s;
            return write_id(e, inner, f);
         }
         else if constexpr (std::is_integral_v<L> && !std::is_same_v<L, bool>)
         {
            L value;
            if constexpr (is_view_v<U>)
               value = s.get();
            else
               value = s;
            char* out = e.w.reserve(24);
            *out++    = '"';
            out       = std::to_chars(out, out + 21, value).ptr;
            *out++    = '"';
            e.w.commit(out);
            return true;
         }
         else
            return write(e, s, f, nil);
      }

      // ── Values ───────────────────────────────────────────────────────────
      template <typename S>
      bool write_nullable_inner(exec& e, S& inner, const plan_field& f, std::uint32_t set)
      {
         const std::size_t mark = e.w.n;
         if (write(e, inner, f, set))
            return true;
         if (e.aborted)
            return false;
         e.w.n = mark;  // the value failed: null stops here
         e.null();
         return true;
      }

      template <typename S>
      bool write_list(exec& e, S& s, const plan_field& f, std::uint32_t set)
      {
         e.w.put('[');
         e.path.push_back(exec::segment{nullptr, 0});
         std::size_t i = 0;
         for (auto&& item : s)
         {
            if (++e.fields > e.lim.max_response_fields || e.w.n > e.lim.max_response_bytes)
            {
               e.path.pop_back();
               return e.abort("Response is larger than the limit allows.");
            }
            if (i)
               e.w.put(',');
            e.path.back().index = i++;
            if (!write(e, item, f, set))
            {
               e.path.pop_back();
               return false;
            }
         }
         e.path.pop_back();
         e.w.put(']');
         return true;
      }

      template <typename S, std::size_t... Is>
      bool write_tuple_list(exec& e, S& s, const plan_field& f, std::uint32_t set, std::index_sequence<Is...>)
      {
         using L = logical_t<std::remove_cv_t<S>>;
         e.w.put('[');
         e.path.push_back(exec::segment{nullptr, 0});
         const bool ok = ([&] {
            if (Is)
               e.w.put(',');
            e.path.back().index = Is;
            auto&& item         = object_def<L>::template get<Is>(s);
            return write(e, item, f, set);
         }() && ...);
         e.path.pop_back();
         if (ok)
            e.w.put(']');
         return ok;
      }

      template <typename S, std::size_t... Is>
      bool write_union(exec& e, S& s, const plan_field& f, std::uint32_t set, std::index_sequence<Is...>)
      {
         using U = std::remove_cv_t<S>;
         if constexpr (is_view_v<U>)
         {
            const std::size_t index = s.index();
            if (index >= sizeof...(Is))
               return e.fail_field(f, "Union value has no member.");
            return s.visit([&](auto alt) {
               return write(e, alt, f, set + static_cast<std::uint32_t>(index));
            });
         }
         else
         {
            bool ok      = false;
            bool matched = ((s.index() == Is
                                ? (ok = write(e, *std::get_if<Is>(&s), f, set + static_cast<std::uint32_t>(Is)), true)
                                : false) ||
                            ...);
            return matched ? ok : e.fail_field(f, "Union value has no member.");
         }
      }

      template <typename S>
      bool write(exec& e, S& s, const plan_field& f, std::uint32_t set)
      {
         using U = std::remove_cv_t<S>;
         using L = logical_t<U>;
         if constexpr (is_buffer_v<U>)
         {
            auto v = as_view(s);
            return write(e, v, f, set);
         }
         else if constexpr (is_reference_wrapper_v<U>)
            return write(e, s.get(), f, set);
         else if constexpr (is_nested_source_v<U>)
         {
            // A nested value resolves as its inner value, read in place.
            if constexpr (is_view_v<U>)
            {
               auto v = s.get();
               return write(e, v, f, set);
            }
            else
            {
               auto v = s.view();
               return write(e, v, f, set);
            }
         }
         else if constexpr (shape_of<L>() == shape::leaf)
         {
            if constexpr (is_view_v<U>)
               return write_leaf_view<L>(e, s, f);
            else
               return write_leaf<L>(e, s, f);
         }
         else if constexpr (shape_of<L>() == shape::nullable)
         {
            if (!s)
            {
               e.null();
               return true;
            }
            auto&& inner = *s;
            return write_nullable_inner(e, inner, f, set);
         }
         else if constexpr (shape_of<L>() == shape::result)
         {
            if (!s.has_value())
            {
               e.fail_field(f, std::string{std::string_view{s.error()}});
               e.null();
               return true;
            }
            auto&& inner = *s;
            return write_nullable_inner(e, inner, f, set);
         }
         else if constexpr (shape_of<L>() == shape::union_)
            return write_union(e, s, f, set, std::make_index_sequence<std::variant_size_v<L>>{});
         else if constexpr (shape_of<L>() == shape::tuple_list)
            return write_tuple_list(e, s, f, set, std::make_index_sequence<std::tuple_size_v<L>>{});
         else if constexpr (shape_of<L>() == shape::object)
         {
            if constexpr (is_view_v<U>)
               return run_set(e, set, &s, handlers<L, const U>::table.data(), object_name<L>());
            else
               return run_set(e, set, &s, handlers<L, const L>::table.data(), object_name<L>());
         }
         else
         {
            static_assert(shape_of<L>() == shape::list, "psio::graphql: this type has no GraphQL mapping");
            return write_list(e, s, f, set);
         }
      }

      // ── Arguments ────────────────────────────────────────────────────────
      template <typename P>
      bool decode_input(exec& e, in_ref r, P& out)
      {
         r = e.resolve(r);
         if constexpr (ji::is_std_optional<P>::value)
         {
            if (!r || r.node().kind == value_kind::null)
            {
               out.reset();
               return true;
            }
            return decode_input(e, r, out.emplace());
         }
         else
         {
            if (!r || r.node().kind == value_kind::null)
               return false;
            if constexpr (ReflectedEnum<P> && !is_text_adapter_v<P>)
            {
               // Validation resolved the name to its index.
               if (r.node().kind != value_kind::enumeration || r.node().index >= reflect_enum<P>::count)
                  return false;
               out = reflect_enum<P>::values[r.node().index];
               return true;
            }
            else if constexpr (is_leaf_v<P>)
               return read_leaf<P>(r.token(), &out);
            else if constexpr (is_std_vector_v<P>)
            {
               out.clear();
               if (r.node().kind != value_kind::list)
                  return decode_input(e, r, out.emplace_back());
               for (std::uint32_t c = r.node().child; c != nil; c = r.store->nodes[c].next)
                  if (!decode_input(e, in_ref{r.store, c}, out.emplace_back()))
                     return false;
               return true;
            }
            else
            {
               if (r.node().kind != value_kind::object)
                  return false;
               using R = reflect<P>;
               for (std::uint32_t c = r.node().child; c != nil; c = r.store->nodes[c].next)
               {
                  const std::size_t slot = r.store->nodes[c].slot;
                  bool              ok   = false;
                  [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                     (void)((slot == Is ? (ok = decode_input(e, in_ref{r.store, c},
                                                             out.*(R::template member_pointer<Is>)),
                                           true)
                                        : false) ||
                            ...);
                  }(std::make_index_sequence<R::member_count>{});
                  if (!ok)
                     return false;
               }
               return true;
            }
         }
      }

      struct context_slot
      {
      };
      template <typename A>
      using arg_storage_t =
         std::conditional_t<is_context_param_v<A>, context_slot, std::remove_cvref_t<A>>;

      // ── Resolvers ────────────────────────────────────────────────────────
      template <typename T, typename Src>
      struct handlers
      {
         using def = object_def<T>;
         // The mutation root is the one object resolved through a non-const
         // value; its non-const methods are fields.
         static constexpr bool by_view  = is_view_v<std::remove_cv_t<Src>>;
         static constexpr bool mutation = !by_view && !std::is_const_v<Src>;

         template <std::size_t I, typename Obj, typename Storage, std::size_t... Ks>
         static decltype(auto) call(exec& e, Obj& obj, Storage& args, std::index_sequence<Ks...>)
         {
            using Args                    = typename slot_types<T, I>::args;
            constexpr auto ptr            = def::template pointer<I>;
            [[maybe_unused]] auto forward = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) -> decltype(auto) {
               using A = std::tuple_element_t<K, Args>;
               if constexpr (is_context_param_v<A>)
                  return static_cast<context&>(e);
               else
                  return static_cast<A&&>(std::get<K>(args));
            };
            if constexpr (slot_types<T, I>::is_static)
               return ptr(forward(std::integral_constant<std::size_t, Ks>{})...);
            else
               return (obj.*ptr)(forward(std::integral_constant<std::size_t, Ks>{})...);
         }

         template <std::size_t I, typename Obj>
         static bool invoke(exec& e, Obj& obj, const plan_field& f)
         {
            using S    = slot_types<T, I>;
            using Args = typename S::args;
            return [&]<std::size_t... Ks>(std::index_sequence<Ks...>) {
               std::tuple<arg_storage_t<std::tuple_element_t<Ks, Args>>...> args;
               std::size_t                                                  bad = static_cast<std::size_t>(-1);
               (void)(([&] {
                         using A = std::tuple_element_t<Ks, Args>;
                         if constexpr (!is_context_param_v<A>)
                         {
                            constexpr std::size_t at = slot_args<T, I>::template arg_index<Ks>;
                            if (!decode_input(e, e.arg(f, at), std::get<Ks>(args)))
                            {
                               bad = at;
                               return false;
                            }
                         }
                         return true;
                      }()) &&
                      ...);
               if (bad != static_cast<std::size_t>(-1))
                  return e.fail_field(f, cat("Argument \"", f.desc->args[bad].name, "\" has an invalid value."));
               if constexpr (std::is_void_v<typename S::result>)
               {
                  call<I>(e, obj, args, std::index_sequence<Ks...>{});
                  e.w.append(std::string_view{"true"});
                  return true;
               }
               else
               {
                  decltype(auto) value = call<I>(e, obj, args, std::index_sequence<Ks...>{});
                  if constexpr (def::template meta<I>().is_id)
                     return write_id(e, value, f);
                  else
                     return write(e, value, f, f.child);
               }
            }(std::make_index_sequence<std::tuple_size_v<Args>>{});
         }

         template <std::size_t I>
         static bool slot(exec& e, const void* source, const plan_field& f)
         {
            if constexpr (!slot_visible<T, mutation, I>())
               return e.fail_field(f, "Field has no resolver.");
            else
            {
               Src& src = *static_cast<Src*>(const_cast<void*>(source));
               if constexpr (!def::template is_method<I>)
               {
                  decltype(auto) value = def::template get<I>(src);
                  if constexpr (def::template meta<I>().is_id)
                     return write_id(e, value, f);
                  else
                     return write(e, value, f, f.child);
               }
               else if constexpr (by_view)
               {
                  // A method needs the object: decode it from the view.
                  const T object =
                     ::psio::decode<T>(typename std::remove_cv_t<Src>::format_type{}, src._psio_data());
                  return invoke<I>(e, object, f);
               }
               else
                  return invoke<I>(e, src, f);
            }
         }

         static constexpr auto table = []<std::size_t... Is>(std::index_sequence<Is...>) {
            return std::array<handler, sizeof...(Is)>{&slot<Is>...};
         }(std::make_index_sequence<def::slot_count>{});
      };

      // ── Requests ─────────────────────────────────────────────────────────
      inline void write_error(exec& e, const error& err)
      {
         e.w.append(std::string_view{"{\"message\":"});
         const std::size_t mark = e.w.n;
         ji::write_string(e.w, err.message, false);
         if (!e.w.err.empty())
         {
            e.w.err = {};
            e.w.n   = mark;
            e.w.append(std::string_view{"\"(error message is not valid UTF-8)\""});
         }
         if (err.line)
         {
            e.w.append(std::string_view{",\"locations\":[{\"line\":"});
            e.w.append(std::to_string(err.line));
            e.w.append(std::string_view{",\"column\":"});
            e.w.append(std::to_string(err.column));
            e.w.append(std::string_view{"}]"});
         }
         if (!err.path.empty())
         {
            e.w.append(std::string_view{",\"path\":"});
            e.w.append(err.path);
         }
         e.w.put('}');
      }
      inline void write_errors(exec& e)
      {
         e.w.append(std::string_view{"\"errors\":["});
         bool first = true;
         for (const error& err : e.errors)
         {
            if (!first)
               e.w.put(',');
            first = false;
            write_error(e, err);
         }
         e.w.put(']');
      }

      // JSON text -> input value nodes (untyped; the checker types them).
      inline bool json_value(json_text_cursor& c, in_store& out, std::size_t depth, std::size_t max_depth,
                             std::uint32_t& id)
      {
         json_kind kind;
         if (!c.kind(kind))
            return false;
         switch (kind)
         {
            case json_kind::null:
               id = out.add(value_kind::null, 0);
               return c.null();
            case json_kind::boolean:
            {
               bool v = false;
               if (!c.boolean(v))
                  return false;
               id                 = out.add(value_kind::boolean, 0);
               out.nodes[id].flag = v;
               return true;
            }
            case json_kind::number:
            {
               std::string_view text;
               if (!c.number(text))
                  return false;
               const bool integer = text.find_first_of(".eE") == std::string_view::npos;
               id = out.add(integer ? value_kind::integer : value_kind::floating, 0);
               out.set_text(id, text);
               return true;
            }
            case json_kind::string:
            {
               std::string_view text;
               bool             escaped = false;
               if (!c.string(text, escaped))
                  return false;
               id = out.add(value_kind::string, 0);
               out.set_text(id, text);
               return true;
            }
            case json_kind::array:
            case json_kind::object:
            {
               if (depth >= max_depth)
                  return c.fail("graphql: variable value is nested deeper than the limit allows");
               const bool list = kind == json_kind::array;
               if (!(list ? c.begin_array() : c.begin_object()))
                  return false;
               id                 = out.add(list ? value_kind::list : value_kind::object, 0);
               std::uint32_t last = nil;
               while (true)
               {
                  bool             more = false;
                  std::string_view key;
                  std::string      name;
                  if (!(list ? c.next_element(more) : c.next_field(more, key)))
                     return false;
                  if (!more)
                     return true;
                  name.assign(key);  // the cursor reuses the key's storage
                  std::uint32_t child = nil;
                  if (!json_value(c, out, depth + 1, max_depth, child))
                     return false;
                  if (!list)
                     out.set_name(child, name);
                  (last == nil ? out.nodes[id].child : out.nodes[last].next) = child;
                  last = child;
               }
            }
         }
         return false;
      }

      class variable_checker final : public value_checker
      {
         exec&                e_;
         const plan_variable& v_;

         bool fail(std::uint32_t, std::string message) override
         {
            e_.add_error(cat("Variable \"$", v_.name, "\" got invalid value; ", message), v_.line, v_.column,
                         false);
            return false;
         }
         bool variable(std::uint32_t, type_ref, unsigned) override { return false; }

        public:
         variable_checker(exec& e, const plan_variable& v) : value_checker(e.request_values), e_(e), v_(v)
         {
            from_json = true;
         }
      };

      // Operation selection and variable coercion (spec 6.1).
      inline bool select_operation(exec& e, const request& req)
      {
         const auto& ops = e.doc.operations;
         if (req.operation_name.empty())
         {
            if (ops.size() == 1)
               e.op = &ops.front();
         }
         else
            for (const plan_operation& op : ops)
               if (op.name == req.operation_name)
                  e.op = &op;
         if (e.op)
            return true;
         if (req.operation_name.empty())
            e.add_error(ops.empty() ? "Document has no operation."
                                    : "Must provide operation name if query contains multiple operations.",
                        0, 0, false);
         else
            e.add_error(cat("Unknown operation named \"", req.operation_name, "\"."), 0, 0, false);
         return false;
      }

      inline bool coerce_variables(exec& e, const request& req)
      {
         const plan_operation& op = *e.op;
         e.vars.assign(op.vars_count, in_ref{});
         bool ok = true;
         if (req.variables.size() > e.lim.max_variables_bytes)
         {
            e.add_error("Variables are larger than the limit allows.", 0, 0, false);
            return false;
         }
         if (!req.variables.empty())
         {
            json_text_cursor c{std::span<const char>{req.variables.data(), req.variables.size()}, true};
            json_kind        kind;
            bool             more = false;
            std::string_view key;
            bool             syntax = c.kind(kind) && (kind == json_kind::null ? c.null() : c.begin_object());
            if (syntax && kind != json_kind::null)
            {
               e.request_values.nodes.reserve(16);
               while ((syntax = c.next_field(more, key)) && more)
               {
                  std::uint32_t index = 0;
                  while (index < op.vars_count && e.doc.variables[op.vars_first + index].name != key)
                     ++index;
                  if (index == op.vars_count)
                  {
                     if (!(syntax = c.skip()))
                        break;
                     continue;
                  }
                  std::uint32_t id = nil;
                  if (!(syntax = json_value(c, e.request_values, 0, e.lim.max_input_depth, id)))
                     break;
                  e.vars[index] = in_ref{&e.request_values, id};
               }
            }
            if (!syntax || !c.finish())
            {
               e.add_error("Variables are not a valid JSON object.", 0, 0, false);
               return false;
            }
         }
         for (std::uint32_t i = 0; i < op.vars_count; ++i)
         {
            const plan_variable& v = e.doc.variables[op.vars_first + i];
            if (e.vars[i])
            {
               variable_checker checker{e, v};
               ok = checker.check(e.vars[i].id, v.ref(), 0, false) && ok;
            }
            else if (v.default_value != nil)
               e.vars[i] = in_ref{&e.doc.values, v.default_value};
            else if (v.nonnull & 1)
            {
               e.add_error(cat("Variable \"$", v.name, "\" of required type \"",
                               type_string(v.type_name, v.depth, v.nonnull), "\" was not provided."),
                           v.line, v.column, false);
               ok = false;
            }
         }
         return ok;
      }

      // Complexity of a selection set (limits::max_complexity); saturates
      // just above the limit.
      inline std::uint64_t page_size(const exec& e, const plan_field& f)
      {
         const field_desc& d     = *f.desc;
         std::int64_t      given = -1;
         for (const std::int8_t at : {d.first_arg, d.last_arg})
            if (at >= 0)
               if (const in_ref r = e.arg(f, static_cast<std::size_t>(at)); r && r.node().kind == value_kind::integer)
               {
                  std::int64_t v = 0;
                  if (read_leaf<std::int64_t>(r.token(), &v))
                     given = std::max<std::int64_t>(given, std::max<std::int64_t>(v, 0));
               }
         const bool paged = d.first_arg >= 0 || d.last_arg >= 0;
         if (d.type.depth == 0 && !paged)
            return 1;
         std::uint64_t size = given >= 0 ? static_cast<std::uint64_t>(given) : e.lim.default_list_size;
         for (unsigned level = 1; level < d.type.depth; ++level)
            size *= e.lim.default_list_size;
         return size;
      }
      inline std::uint64_t complexity(const exec& e, std::uint32_t set_id)
      {
         const std::uint64_t cap  = e.lim.max_complexity + 1;
         const plan_set      set  = e.doc.sets[set_id];
         std::uint64_t       cost = 0;
         for (std::uint32_t i = 0; i < set.count && cost < cap; ++i)
         {
            const plan_field& f = e.doc.fields[set.first + i];
            if (f.alternate)
               continue;
            const field_desc& d = *f.desc;
            cost += d.cost;
            std::uint64_t below = 0;
            if (f.child != nil)
            {
               const std::uint32_t count =
                  d.type.named->kind == type_kind::union_ ? d.type.named->possible_count : 1;
               for (std::uint32_t k = 0; k < count; ++k)
                  below = std::max(below, complexity(e, f.child + k));
            }
            // Each element of a list costs at least one.
            if (d.type.depth != 0 || d.first_arg >= 0 || d.last_arg >= 0)
               below = std::max<std::uint64_t>(below, 1);
            if (below == 0)
               continue;
            const std::uint64_t size = page_size(e, f);
            if (size > cap / below)
               return cap;
            cost += size * below;
         }
         return std::min(cost, cap);
      }

      inline bool prepare(exec& e, const request& req)
      {
         if (!select_operation(e, req) || !coerce_variables(e, req))
            return false;
         if (e.op->root == nil)
         {
            e.add_error("Operation cannot be executed.", 0, 0, false);
            return false;
         }
         if (complexity(e, e.op->root) > e.lim.max_complexity)
         {
            e.add_error(cat("Query complexity exceeds the limit of ", std::to_string(e.lim.max_complexity), "."),
                        e.op->line, e.op->column, false);
            return false;
         }
         return true;
      }

      // The response: {"data":...} then ,"errors":[...] when there are any.
      template <typename Run>
      std::size_t respond(exec& e, const request& req, Run&& run_root)
      {
         e.w.put('{');
         if (prepare(e, req))
         {
            e.w.append(std::string_view{"\"data\":"});
            const std::size_t mark = e.w.n;
            if (!run_root())
            {
               e.w.n = mark;
               e.null();
            }
            if (!e.errors.empty())
               e.w.put(',');
         }
         if (!e.errors.empty())
            write_errors(e);
         e.w.put('}');
         e.w.finish();
         return e.errors.size();
      }
   }  // namespace detail

   // Appends to `out` the response for errors found before execution (the
   // parse / validation errors of a document): {"errors":[...]}.
   inline void write_error_response(std::span<const error> errors, std::string& out)
   {
      static const document none{};
      static const limits   lim{};
      exec                  e{out, none, lim, request{}};
      e.errors.assign(errors.begin(), errors.end());
      e.w.put('{');
      detail::write_errors(e);
      e.w.put('}');
      e.w.finish();
   }

}  // namespace psio::graphql
