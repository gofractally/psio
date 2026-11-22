#pragma once
//
// psio/graphql.hpp — a GraphQL engine over psio reflection and views
// (docs/graphql.md).
//
//    struct Query { ... };                      // reflected fields and methods
//    PSIO_REFLECT(Query, ...)
//    using api = psio::graphql::schema<Query>;
//
//    psio::graphql::document plan;              // reusable, cacheable
//    std::vector<psio::graphql::error> errors;
//    if (api::compile(text, plan, errors))      // parse + validate
//       api::execute(plan, root, out, {.variables = json});
//    else
//       psio::graphql::write_error_response(errors, out);
//
// or, in one call, `api::run(root, text, out, request)`.
//
// The three phases are separate headers: parser.hpp (text -> syntax tree),
// plan.hpp (validation -> plan), execute.hpp (plan + root -> response).
// schema.hpp derives the type system from reflection; introspection.hpp adds
// __schema / __type and SDL; connection.hpp is Relay pagination.

#include <psio/graphql/connection.hpp>
#include <psio/graphql/execute.hpp>
#include <psio/graphql/introspection.hpp>
#include <psio/graphql/plan.hpp>
#include <psio/graphql/schema.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio::graphql
{
   // The schema whose query root is `Query` and whose mutation root is
   // `Mutation` (void: none). Stateless: every member is static.
   //
   // The mutation root's fields are all of its reflected methods (const or
   // not; a void method is `Boolean!` and yields true) and they run one after
   // another, in document order. Subscriptions are not supported.
   //
   // `Introspection = false` leaves out __schema / __type and the code that
   // serves them (a size option for guests).
   template <typename Query, typename Mutation = void, bool Introspection = true>
   struct schema
   {
      static constexpr bool has_mutation = !std::is_void_v<Mutation>;

     private:
      template <typename M>
      static constexpr const type_desc* mutation_desc() noexcept
      {
         if constexpr (std::is_void_v<M>)
            return nullptr;
         else
            return &detail::object_type<M, true>::value;
      }
      static constexpr const type_desc* introspection_root() noexcept
      {
         if constexpr (Introspection)
            return &detail::object_type<introspection::Schema, false>::value;
         else
            return nullptr;
      }

      static constexpr detail::type_table table = detail::collect_types(
         {&detail::object_type<Query, false>::value, mutation_desc<Mutation>()},
         {&detail::leaf_type<std::int32_t>::value, &detail::leaf_type<double>::value,
          &detail::leaf_type<std::string>::value, &detail::leaf_type<bool>::value, &detail::id_type,
          introspection_root()});
      static_assert(!table.conflict,
                    "psio::graphql: two different C++ types of this schema have the same GraphQL "
                    "type name (rename one with psio::graphql::name_as or type_name<T>; an input "
                    "object is named <Type>Input)");
      static_assert(!table.overflow, "psio::graphql: the schema has too many types");

      struct type_names
      {
         static constexpr auto _names_array_ = [] {
            std::array<std::string_view, table.n> out{};
            for (std::size_t i = 0; i < table.n; ++i)
               out[i] = table.t[i]->name;
            return out;
         }();
      };
      static constexpr auto type_list = [] {
         std::array<const type_desc*, table.n> out{};
         for (std::size_t i = 0; i < table.n; ++i)
            out[i] = table.t[i];
         return out;
      }();

      static constexpr schema_desc make_desc() noexcept
      {
         schema_desc d;
         d.query      = &detail::object_type<Query, false>::value;
         d.mutation   = mutation_desc<Mutation>();
         d.types      = type_list.data();
         d.type_count = type_list.size();
         d.find_type  = &detail::find_name<type_names>;
         if constexpr (Introspection)
         {
            d.meta_fields  = introspection::meta_fields;
            d.resolve_meta = &introspection::resolve_meta;
         }
         return d;
      }

      static std::size_t wrong_schema(std::string& out)
      {
         const error e{"Document was compiled for a different schema."};
         write_error_response(std::span<const error>{&e, 1}, out);
         return 1;
      }

     public:
      // Everything the non-template phases need to know about the schema.
      static constexpr schema_desc desc = make_desc();

      // Parses and validates `text`. True: `plan` is ready to execute, any
      // number of times, with any root and variables. False: `errors` says
      // why (write_error_response turns them into a response).
      static bool compile(std::string_view    text,
                          document&           plan,
                          std::vector<error>& errors,
                          const limits&       lim = {})
      {
         return graphql::compile(desc, text, plan, errors, lim);
      }

      // Runs a query operation of `plan` on `root` and appends the response
      // (`{"data":...,"errors":[...]}`) to `out`. Returns the number of
      // errors in the response. A mutation operation needs the overload
      // below.
      static std::size_t execute(const document& plan,
                                 const Query&    root,
                                 std::string&    out,
                                 const request&  req = {},
                                 const limits&   lim = {})
      {
         if (plan.schema != &desc)
            return wrong_schema(out);
         exec e{out, plan, lim, req};
         return detail::respond(e, req, [&] {
            if (e.op->kind != ast::operation_kind::query)
               return e.abort("A mutation needs the mutation root.");
            return detail::run_set(e, e.op->root, &root, detail::handlers<Query, const Query>::table.data(),
                                   desc.query->name);
         });
      }

      // As above, for a document that may hold a mutation: the operation
      // runs on `root` (query) or on `mutation`.
      template <typename M = Mutation>
         requires(!std::is_void_v<M> && std::is_same_v<M, Mutation>)
      static std::size_t execute(const document& plan,
                                 const Query&    root,
                                 M&              mutation,
                                 std::string&    out,
                                 const request&  req = {},
                                 const limits&   lim = {})
      {
         if (plan.schema != &desc)
            return wrong_schema(out);
         exec e{out, plan, lim, req};
         return detail::respond(e, req, [&] {
            if (e.op->kind == ast::operation_kind::query)
               return detail::run_set(e, e.op->root, &root,
                                      detail::handlers<Query, const Query>::table.data(), desc.query->name);
            return detail::run_set(e, e.op->root, &mutation, detail::handlers<M, M>::table.data(),
                                   desc.mutation->name);
         });
      }

      // compile + execute, for a caller that does not keep plans. Always
      // appends a response. Returns the number of errors in it.
      static std::size_t run(const Query&     root,
                             std::string_view text,
                             std::string&     out,
                             const request&   req = {},
                             const limits&    lim = {})
      {
         document           plan;
         std::vector<error> errors;
         if (!compile(text, plan, errors, lim))
         {
            write_error_response(errors, out);
            return errors.size();
         }
         return execute(plan, root, out, req, lim);
      }
      template <typename M = Mutation>
         requires(!std::is_void_v<M> && std::is_same_v<M, Mutation>)
      static std::size_t run(const Query&     root,
                             M&               mutation,
                             std::string_view text,
                             std::string&     out,
                             const request&   req = {},
                             const limits&    lim = {})
      {
         document           plan;
         std::vector<error> errors;
         if (!compile(text, plan, errors, lim))
         {
            write_error_response(errors, out);
            return errors.size();
         }
         return execute(plan, root, mutation, out, req, lim);
      }

      // The schema in SDL.
      static std::string sdl() { return graphql::sdl(desc); }
   };

}  // namespace psio::graphql
