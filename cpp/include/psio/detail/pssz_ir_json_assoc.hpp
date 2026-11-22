#pragma once
//
// psio/detail/pssz_ir_json_assoc.hpp — maps and sets in the schema-driven
// JSON / I-JSON codec (included by pssz_ir_json.hpp; docs/json.md "Maps and
// sets").
//
//   set                 an array of its elements
//   map, text key       an object {"k": v, ...}: the key type is a "string"
//                       / "utf8" node, or a custom text presentation whose
//                       handler is registered
//   map, any other key  an array of [k, v] pairs — including a custom
//                       presentation with no handler, which falls back to
//                       the plain form of its wire type
//
// Readers accept any order, reject a repeated key / element, and produce
// the value in canonical order. Writers emit canonical order.

namespace psio::pssz_ir::json_detail
{
// How the key type of a map appears in text.
struct map_key_text
{
   enum class kind : std::uint8_t
   {
      pairs,    // not a string by type: the map is an array of [k, v]
      string,   // a "string" / "utf8" node
      handler,  // a custom presentation with a registered handler
   };
   kind form = kind::pairs;
   const Custom *node = nullptr;
   const CustomHandlers::Handler *handler = nullptr;
   bool missing_handler = false;  // require_all, and the key's id has none
};
inline map_key_text key_text_of(const Schema &schema, const AnyType &key,
                                const custom_lookup &customs)
{
   map_key_text out;
   const AnyType *at = &key;
   for (std::size_t hops = 0; hops <= 4 * schema.types.size() + 64; ++hops)
   {
      at = at->resolve(schema);
      auto custom = at ? std::get_if<Custom>(&at->value) : nullptr;
      if (!custom)
         return out;
      if (detail::text_adapter_id(custom->id))
      {
         out.form = map_key_text::kind::string;
         out.node = custom;
         return out;
      }
      if (detail::wire_custom_id(custom->id) || !custom->type.get())
         return out;
      if (custom->id != "box")
      {
         // A time value is not an object key (docs/json.md).
         if (::psio::parse_time_schema_id(custom->id) ||
             ::psio::reserved_time_schema_id(custom->id))
            return out;
         if (auto handler = customs.find(*custom))
         {
            out.form = map_key_text::kind::handler;
            out.node = custom;
            out.handler = handler;
            return out;
         }
         if (customs.required())
         {
            out.missing_handler = true;
            return out;
         }
      }
      at = custom->type.get();
   }
   return out;
}

// An object member name as a key value: what `read` makes of the same text
// as a JSON string of the key type.
template <JsonCursor C>
bool Reader<C>::key_from_text(const map_key_text &key, std::string_view text, Value &out)
{
   if (key.form == map_key_text::kind::string)
   {
      const byte_shape shape = bytes_of(schema, *key.node);
      if (!shape.valid)
         return c.fail("pssz IR: byte adapter requires byte container");
      if (!shape.admits(text.size()))
         return c.fail("json: string exceeds its bound");
      out = std::string(text);
      return true;
   }
   std::vector<char> bytes;
   if (!key.handler->from_text || !key.handler->from_text(text, bytes).ok())
      return c.fail("json: text is not a value of the custom type");
   bool unknown = false;
   if (!decode_auto(schema, *key.node->type, std::span<const char>{bytes}, out, unknown).ok() ||
       unknown)
      return c.fail("json: custom handler produced bytes that are not the inner type");
   return true;
}

template <JsonCursor C> bool Reader<C>::assoc(const Custom &type, int_form form, Value &out)
{
   detail::Codec<0> codec(schema);
   detail::Codec<0>::AssocShape shape;
   if (auto status = codec.assoc_shape(type, shape); !status.ok())
      return c.fail(status.error().what);
   Sequence items;
   auto element = [&](int_form item_form) {
      return array_items(shape.max, false, [&] {
         Value item;
         if (!read(**shape.element, item_form, 0, item))
            return false;
         items.push_back(std::move(item));
         return true;
      });
   };
   if (!shape.map)
   {
      if (!element(form))
         return false;
   }
   else
   {
      const map_key_text key = key_text_of(schema, *shape.key, customs);
      if (key.missing_handler)
         return c.fail("pssz IR: no handler registered for a custom type");
      if (key.form == map_key_text::kind::pairs)
      {
         if (!element(int_form::natural))
            return false;
      }
      else
      {
         if (!c.begin_object())
            return false;
         while (true)
         {
            bool has;
            std::string_view name;
            if (!c.next_field(has, name))
               return false;
            if (!has)
               break;
            if (shape.max && items.size() >= *shape.max)
               return c.fail("json: array exceeds its bound");
            Sequence entry(2);
            if (!key_from_text(key, name, entry[0]) ||
                !read(*shape.value, int_form::natural, 0, entry[1]))
               return false;
            items.push_back(std::move(entry));
         }
      }
   }
   std::vector<std::size_t> order;
   bool duplicate = false;
   if (auto status = codec.assoc_order(type, items, order, &duplicate); !status.ok())
      return c.fail(!duplicate    ? status.error().what
                    : shape.map   ? std::string_view{"json: duplicate map key"}
                                  : std::string_view{"json: duplicate set element"});
   Sequence sorted;
   sorted.reserve(items.size());
   for (std::size_t index : order)
      sorted.push_back(std::move(items[index]));
   out = std::move(sorted);
   return true;
}

inline void Writer::assoc(const Custom &type, const Value &value, int_form form)
{
   auto items = std::get_if<Sequence>(&value.value);
   if (!items)
      return w.fail("pssz IR: sequence value required");
   detail::Codec<0> codec(schema);
   detail::Codec<0>::AssocShape shape;
   if (auto status = codec.assoc_shape(type, shape); !status.ok())
      return w.fail(status.error().what);
   if (shape.max && items->size() > *shape.max)
      return w.fail("json: array exceeds its bound");
   std::vector<std::size_t> order;
   bool duplicate = false;
   if (auto status = codec.assoc_order(type, *items, order, &duplicate); !status.ok())
      return w.fail(!duplicate    ? status.error().what
                    : shape.map   ? std::string_view{"json: map has duplicate keys"}
                                  : std::string_view{"json: set has duplicate elements"});
   map_key_text key;
   if (shape.map)
   {
      key = key_text_of(schema, *shape.key, customs);
      if (key.missing_handler)
         return w.fail("pssz IR: no handler registered for a custom type");
   }
   const bool object = key.form != map_key_text::kind::pairs;
   w.put(object ? '{' : '[');
   bool first = true;
   for (std::size_t index : order)
   {
      if (!first)
         w.put(',');
      first = false;
      const Value &item = (*items)[index];
      if (!object)
      {
         // A set element, or a map entry as the [k, v] tuple it is.
         write(**shape.element, item, shape.map ? int_form::natural : form, 0);
         continue;
      }
      const Sequence &entry = std::get<Sequence>(item.value);
      if (key.form == map_key_text::kind::string)
      {
         auto text = std::get_if<std::string>(&entry[0].value);
         const byte_shape bytes = bytes_of(schema, *key.node);
         if (!text || !bytes.valid)
            return w.fail("pssz IR: string value required");
         if (!bytes.admits(text->size()))
            return w.fail("json: string exceeds its bound");
         psio::detail::json_impl::write_string(w, *text, ijson);
      }
      else
      {
         std::vector<char> bytes;
         std::string text;
         if (!encode_auto(schema, *key.node->type, entry[0], bytes).ok())
            return w.fail("pssz IR: custom value does not match its inner type");
         if (!key.handler->to_text ||
             !key.handler->to_text(std::span<const char>{bytes}, text).ok())
            return w.fail("json: custom handler has no text for the value");
         psio::detail::json_impl::write_string(w, text, ijson);
      }
      w.put(':');
      write(*shape.value, entry[1], int_form::natural, 0);
   }
   w.put(object ? '}' : ']');
}

} // namespace psio::pssz_ir::json_detail
