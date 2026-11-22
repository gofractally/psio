#pragma once
//
// psio/graphql/introspection.hpp — `__schema`, `__type` and SDL.
//
// The introspection types (spec section 4) are ordinary reflected C++
// records over the schema's descriptors, resolved by the same executor as
// user types: `__Type` is `introspection::Type`, a method per field. Nothing
// here is instantiated by a schema built with introspection off.

#include <psio/graphql/execute.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace psio::graphql::introspection
{
   enum class TypeKind : std::uint8_t
   {
      SCALAR,
      OBJECT,
      INTERFACE,
      UNION,
      ENUM,
      INPUT_OBJECT,
      LIST,
      NON_NULL,
   };
   PSIO_REFLECT_ENUM(TypeKind, SCALAR, OBJECT, INTERFACE, UNION, ENUM, INPUT_OBJECT, LIST, NON_NULL)

   enum class DirectiveLocation : std::uint8_t
   {
      QUERY,
      MUTATION,
      SUBSCRIPTION,
      FIELD,
      FRAGMENT_DEFINITION,
      FRAGMENT_SPREAD,
      INLINE_FRAGMENT,
      VARIABLE_DEFINITION,
      SCHEMA,
      SCALAR,
      OBJECT,
      FIELD_DEFINITION,
      ARGUMENT_DEFINITION,
      INTERFACE,
      UNION,
      ENUM,
      ENUM_VALUE,
      INPUT_OBJECT,
      INPUT_FIELD_DEFINITION,
   };
   PSIO_REFLECT_ENUM(DirectiveLocation,
                     QUERY,
                     MUTATION,
                     SUBSCRIPTION,
                     FIELD,
                     FRAGMENT_DEFINITION,
                     FRAGMENT_SPREAD,
                     INLINE_FRAGMENT,
                     VARIABLE_DEFINITION,
                     SCHEMA,
                     SCALAR,
                     OBJECT,
                     FIELD_DEFINITION,
                     ARGUMENT_DEFINITION,
                     INTERFACE,
                     UNION,
                     ENUM,
                     ENUM_VALUE,
                     INPUT_OBJECT,
                     INPUT_FIELD_DEFINITION)

   using text = std::optional<std::string_view>;
   inline text text_or_null(std::string_view s) { return s.empty() ? text{} : text{s}; }

   struct Field;
   struct InputValue;
   struct EnumValue;

   // A type reference seen from `level` list wrappers in; `bare` when the
   // non-null wrapper at that level has been taken off (ofType).
   struct Type
   {
      type_ref     ref{};
      std::uint8_t level = 0;
      bool         bare  = false;

      static Type named(const type_desc* d) { return Type{type_ref{d, 0, 0}, 0, true}; }

      bool is_non_null() const noexcept { return !bare && ref.is_nonnull(level); }
      bool is_list() const noexcept { return !is_non_null() && level < ref.depth; }
      const type_desc* desc() const noexcept { return is_non_null() || is_list() ? nullptr : ref.named; }

      TypeKind kind() const noexcept
      {
         return is_non_null() ? TypeKind::NON_NULL
                : is_list()   ? TypeKind::LIST
                              : static_cast<TypeKind>(ref.named->kind);
      }
      text name() const noexcept { return desc() ? text{desc()->name} : text{}; }
      text description() const noexcept { return desc() ? text_or_null(desc()->description) : text{}; }
      text specifiedByURL() const noexcept { return {}; }
      std::optional<std::vector<Field>>      fields(std::optional<bool> includeDeprecated) const;
      std::optional<std::vector<Type>>       interfaces() const;
      std::optional<std::vector<Type>>       possibleTypes() const;
      std::optional<std::vector<EnumValue>>  enumValues(std::optional<bool> includeDeprecated) const;
      std::optional<std::vector<InputValue>> inputFields() const;
      std::optional<Type>                    ofType() const;
   };

   struct InputValue
   {
      const arg_desc*  arg = nullptr;
      std::string_view default_text{};

      std::string_view name() const noexcept { return arg->name; }
      text             description() const noexcept { return text_or_null(arg->description); }
      Type             type() const noexcept { return Type{arg->type}; }
      text             defaultValue() const noexcept { return text_or_null(default_text); }
   };

   struct Field
   {
      const field_desc* field = nullptr;

      std::string_view        name() const noexcept { return field->name; }
      text                    description() const noexcept { return text_or_null(field->description); }
      std::vector<InputValue> args() const
      {
         std::vector<InputValue> out;
         out.reserve(field->arg_count);
         for (std::size_t i = 0; i < field->arg_count; ++i)
            out.push_back(InputValue{&field->args[i]});
         return out;
      }
      Type type() const noexcept { return Type{field->type}; }
      bool isDeprecated() const noexcept { return field->deprecated; }
      text deprecationReason() const noexcept
      {
         return field->deprecated ? text{field->deprecation_reason} : text{};
      }
   };

   struct EnumValue
   {
      std::string_view value{};

      std::string_view name() const noexcept { return value; }
      text             description() const noexcept { return {}; }
      bool             isDeprecated() const noexcept { return false; }
      text             deprecationReason() const noexcept { return {}; }
   };

   inline std::optional<std::vector<Field>> Type::fields(std::optional<bool> includeDeprecated) const
   {
      const type_desc* d = desc();
      if (!d || d->kind != type_kind::object)
         return std::nullopt;
      std::vector<Field> out;
      out.reserve(d->field_count);
      for (std::size_t i = 0; i < d->field_count; ++i)
         if (!d->fields[i].deprecated || includeDeprecated.value_or(false))
            out.push_back(Field{&d->fields[i]});
      return out;
   }
   inline std::optional<std::vector<Type>> Type::interfaces() const
   {
      const type_desc* d = desc();
      if (!d || d->kind != type_kind::object)
         return std::nullopt;
      return std::vector<Type>{};
   }
   inline std::optional<std::vector<Type>> Type::possibleTypes() const
   {
      const type_desc* d = desc();
      if (!d || d->kind != type_kind::union_)
         return std::nullopt;
      std::vector<Type> out;
      for (std::size_t i = 0; i < d->possible_count; ++i)
         out.push_back(Type::named(d->possible[i]));
      return out;
   }
   inline std::optional<std::vector<EnumValue>> Type::enumValues(std::optional<bool>) const
   {
      const type_desc* d = desc();
      if (!d || d->kind != type_kind::enumeration)
         return std::nullopt;
      std::vector<EnumValue> out;
      for (std::size_t i = 0; i < d->value_count; ++i)
         out.push_back(EnumValue{d->values[i]});
      return out;
   }
   inline std::optional<std::vector<InputValue>> Type::inputFields() const
   {
      const type_desc* d = desc();
      if (!d || d->kind != type_kind::input_object)
         return std::nullopt;
      std::vector<InputValue> out;
      for (std::size_t i = 0; i < d->input_count; ++i)
         out.push_back(InputValue{&d->inputs[i]});
      return out;
   }
   inline std::optional<Type> Type::ofType() const
   {
      if (is_non_null())
         return Type{ref, level, true};
      if (is_list())
         return Type{ref, static_cast<std::uint8_t>(level + 1), false};
      return std::nullopt;
   }

   // The directives of every schema.
   struct directive_info
   {
      std::string_view                 name;
      std::string_view                 description;
      std::span<const DirectiveLocation> locations;
      arg_desc                         arg;
      std::string_view                 arg_default{};
   };
   inline constexpr DirectiveLocation selection_locations[] = {
      DirectiveLocation::FIELD, DirectiveLocation::FRAGMENT_SPREAD, DirectiveLocation::INLINE_FRAGMENT};
   inline constexpr DirectiveLocation deprecated_locations[] = {
      DirectiveLocation::FIELD_DEFINITION, DirectiveLocation::ARGUMENT_DEFINITION,
      DirectiveLocation::INPUT_FIELD_DEFINITION, DirectiveLocation::ENUM_VALUE};
   inline constexpr directive_info directive_table[] = {
      {"skip", "Directs the executor to skip this field or fragment when the `if` argument is true.",
       selection_locations, arg_desc{"if", type_ref{&detail::leaf_type<bool>::value, 0, 1}, 0, "Skipped when true."}},
      {"include",
       "Directs the executor to include this field or fragment only when the `if` argument is true.",
       selection_locations, arg_desc{"if", type_ref{&detail::leaf_type<bool>::value, 0, 1}, 0, "Included when true."}},
      {"deprecated", "Marks an element of a GraphQL schema as no longer supported.", deprecated_locations,
       arg_desc{"reason", type_ref{&detail::leaf_type<std::string>::value, 0, 0}, 0,
                "Explains why this element was deprecated."},
       "\"No longer supported\""},
   };

   struct Directive
   {
      const directive_info* info = nullptr;

      std::string_view               name() const noexcept { return info->name; }
      text                           description() const noexcept { return text_or_null(info->description); }
      std::span<const DirectiveLocation> locations() const noexcept { return info->locations; }
      std::vector<InputValue>        args() const { return {InputValue{&info->arg, info->arg_default}}; }
      bool                           isRepeatable() const noexcept { return false; }
   };

   struct Schema
   {
      const schema_desc* schema = nullptr;

      text              description() const noexcept { return {}; }
      std::vector<Type> types() const
      {
         std::vector<Type> out;
         out.reserve(schema->type_count);
         for (std::size_t i = 0; i < schema->type_count; ++i)
            out.push_back(Type::named(schema->types[i]));
         return out;
      }
      Type                queryType() const noexcept { return Type::named(schema->query); }
      std::optional<Type> mutationType() const noexcept
      {
         return schema->mutation ? std::optional<Type>{Type::named(schema->mutation)} : std::nullopt;
      }
      std::optional<Type>    subscriptionType() const noexcept { return std::nullopt; }
      std::vector<Directive> directives() const
      {
         std::vector<Directive> out;
         for (const directive_info& d : directive_table)
            out.push_back(Directive{&d});
         return out;
      }
   };

   PSIO_REFLECT(Type,
                method(kind),
                method(name),
                method(description),
                method(specifiedByURL),
                method(fields, includeDeprecated),
                method(interfaces),
                method(possibleTypes),
                method(enumValues, includeDeprecated),
                method(inputFields),
                method(ofType))
   PSIO_REFLECT(InputValue, method(name), method(description), method(type), method(defaultValue))
   PSIO_REFLECT(Field,
                method(name),
                method(description),
                method(args),
                method(type),
                method(isDeprecated),
                method(deprecationReason))
   PSIO_REFLECT(EnumValue, method(name), method(description), method(isDeprecated), method(deprecationReason))
   PSIO_REFLECT(Directive, method(name), method(description), method(locations), method(args), method(isRepeatable))
   PSIO_REFLECT(Schema,
                method(description),
                method(types),
                method(queryType),
                method(mutationType),
                method(subscriptionType),
                method(directives))
}  // namespace psio::graphql::introspection

namespace psio::graphql
{
   template <>
   struct type_name<introspection::TypeKind>
   {
      static constexpr std::string_view value = "__TypeKind";
   };
   template <>
   struct type_name<introspection::DirectiveLocation>
   {
      static constexpr std::string_view value = "__DirectiveLocation";
   };
   template <>
   struct type_name<introspection::Type>
   {
      static constexpr std::string_view value = "__Type";
   };
   template <>
   struct type_name<introspection::InputValue>
   {
      static constexpr std::string_view value = "__InputValue";
   };
   template <>
   struct type_name<introspection::Field>
   {
      static constexpr std::string_view value = "__Field";
   };
   template <>
   struct type_name<introspection::EnumValue>
   {
      static constexpr std::string_view value = "__EnumValue";
   };
   template <>
   struct type_name<introspection::Directive>
   {
      static constexpr std::string_view value = "__Directive";
   };
   template <>
   struct type_name<introspection::Schema>
   {
      static constexpr std::string_view value = "__Schema";
   };

   namespace introspection
   {
      inline constexpr arg_desc type_field_args[] = {
         arg_desc{"name", type_ref{&detail::leaf_type<std::string>::value, 0, 1}, 0}};
      // `__schema: __Schema!` and `__type(name: String!): __Type`.
      inline constexpr field_desc meta_fields[] = {
         field_desc{.name = "__schema", .type = PSIO_GRAPHQL_OUT_REF(Schema)},
         field_desc{.name      = "__type",
                    .type      = PSIO_GRAPHQL_OUT_REF(std::optional<Type>),
                    .args      = type_field_args,
                    .arg_count = 1},
      };

      inline bool resolve_meta(exec& e, const plan_field& f, std::size_t which)
      {
         const schema_desc& s = *e.doc.schema;
         if (which == 0)
         {
            const Schema value{&s};
            return detail::write(e, value, f, f.child);
         }
         std::string name;
         if (!detail::decode_input(e, e.arg(f, 0), name))
            return e.fail_field(f, "Argument \"name\" has an invalid value.");
         std::optional<Type> value;
         if (const type_desc* d = s.type_named(name))
            value = Type::named(d);
         return detail::write(e, value, f, f.child);
      }
   }  // namespace introspection

   // ── SDL ───────────────────────────────────────────────────────────────
   namespace detail
   {
      inline bool sdl_omits(const type_desc& d) noexcept
      {
         return d.name.starts_with("__") || d.name == "Int" || d.name == "Float" || d.name == "String" ||
                d.name == "Boolean" || d.name == "ID";
      }
      inline void sdl_string(std::string& out, std::string_view s)
      {
         out += '"';
         for (const char c : s)
         {
            if (c == '"' || c == '\\')
               out += '\\';
            if (c == '\n')
               out += "\\n";
            else
               out += c;
         }
         out += '"';
      }
      inline void sdl_description(std::string& out, std::string_view text, std::string_view indent)
      {
         if (text.empty())
            return;
         out += indent;
         sdl_string(out, text);
         out += '\n';
      }
      inline void sdl_args(std::string& out, const arg_desc* args, std::size_t count)
      {
         if (!count)
            return;
         out += '(';
         for (std::size_t i = 0; i < count; ++i)
         {
            if (i)
               out += ", ";
            out += args[i].name;
            out += ": ";
            out += type_string(args[i].type);
         }
         out += ')';
      }
   }  // namespace detail

   // The schema in the GraphQL schema definition language: every named type
   // except the built-in scalars and the introspection types, in the order
   // of the schema's type list.
   inline std::string sdl(const schema_desc& s)
   {
      std::string out;
      if (s.query->name != "Query" || (s.mutation && s.mutation->name != "Mutation"))
      {
         out += "schema {\n  query: ";
         out += s.query->name;
         if (s.mutation)
         {
            out += "\n  mutation: ";
            out += s.mutation->name;
         }
         out += "\n}\n\n";
      }
      for (std::size_t i = 0; i < s.type_count; ++i)
      {
         const type_desc& d = *s.types[i];
         if (detail::sdl_omits(d))
            continue;
         detail::sdl_description(out, d.description, "");
         switch (d.kind)
         {
            case type_kind::scalar:
               out += "scalar ";
               out += d.name;
               out += '\n';
               break;
            case type_kind::enumeration:
               out += "enum ";
               out += d.name;
               out += " {\n";
               for (std::size_t k = 0; k < d.value_count; ++k)
               {
                  out += "  ";
                  out += d.values[k];
                  out += '\n';
               }
               out += "}\n";
               break;
            case type_kind::union_:
               out += "union ";
               out += d.name;
               out += " = ";
               for (std::size_t k = 0; k < d.possible_count; ++k)
               {
                  if (k)
                     out += " | ";
                  out += d.possible[k]->name;
               }
               out += '\n';
               break;
            case type_kind::input_object:
               out += "input ";
               out += d.name;
               out += " {\n";
               for (std::size_t k = 0; k < d.input_count; ++k)
               {
                  detail::sdl_description(out, d.inputs[k].description, "  ");
                  out += "  ";
                  out += d.inputs[k].name;
                  out += ": ";
                  out += detail::type_string(d.inputs[k].type);
                  out += '\n';
               }
               out += "}\n";
               break;
            default:
               out += "type ";
               out += d.name;
               out += " {\n";
               for (std::size_t k = 0; k < d.field_count; ++k)
               {
                  const field_desc& f = d.fields[k];
                  detail::sdl_description(out, f.description, "  ");
                  out += "  ";
                  out += f.name;
                  detail::sdl_args(out, f.args, f.arg_count);
                  out += ": ";
                  out += detail::type_string(f.type);
                  if (f.deprecated)
                  {
                     out += " @deprecated(reason: ";
                     detail::sdl_string(out, f.deprecation_reason);
                     out += ')';
                  }
                  out += '\n';
               }
               out += "}\n";
               break;
         }
         out += '\n';
      }
      if (!out.empty())
         out.pop_back();
      return out;
   }

}  // namespace psio::graphql
