// The previous GraphQL engine (psio1::gql_query), on the shapes of
// graphql_shapes.hpp, for bench_graphql. Built only when its sources are
// present (PSIO_BENCH_OLD_GRAPHQL in CMakeLists.txt); bench_graphql_no_old.cpp
// stands in otherwise.
//
// The old engine has no views, no plans and no generic connection: a paged
// field is a method that returns a Connection of row values, which is what
// its consumer (psibase makeConnection) built from unpacked rows.

#include "graphql_shapes.hpp"

#include <psio1/graphql.hpp>

namespace gqlbench
{
   PSIO1_REFLECT(Small, id, name, a, b, c, score, active)
   PSIO1_REFLECT(Row20, id, name, owner, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17,
                 f18, f19)
   PSIO1_REFLECT(WideRow, id, text1, numbers, text2, strings, count)
   PSIO1_REFLECT(Child, n, label)
   PSIO1_REFLECT(Parent, id, name, children)

   struct OldPageInfo
   {
      bool        hasPreviousPage = false;
      bool        hasNextPage     = false;
      std::string startCursor;
      std::string endCursor;
   };
   PSIO1_REFLECT(OldPageInfo, hasPreviousPage, hasNextPage, startCursor, endCursor)
   struct OldEdge
   {
      Row20       node;
      std::string cursor;
   };
   PSIO1_REFLECT(OldEdge, node, cursor)
   struct OldConnection
   {
      std::vector<OldEdge> edges;
      OldPageInfo          pageInfo;
   };
   PSIO1_REFLECT(OldConnection, edges, pageInfo)

   struct OldQuery
   {
      Small                small;
      std::vector<Row20>   rows100;
      std::vector<Row20>   rows10k;
      std::vector<WideRow> wide;
      std::vector<Parent>  parents;

      // One page of rows10k, as the old consumer produced it: row values
      // and a hex cursor per edge.
      OldConnection page(std::uint32_t first) const
      {
         OldConnection     c;
         const std::size_t n = std::min<std::size_t>(first, rows10k.size());
         c.edges.reserve(n);
         for (std::size_t i = 0; i < n; ++i)
         {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string           cursor(16, '0');
            for (int k = 0; k < 16; ++k)
               cursor[15 - k] = digits[(i >> (4 * k)) & 0xF];
            c.edges.push_back(OldEdge{rows10k[i], std::move(cursor)});
         }
         c.pageInfo.hasNextPage = n < rows10k.size();
         if (n)
         {
            c.pageInfo.startCursor = c.edges.front().cursor;
            c.pageInfo.endCursor   = c.edges.back().cursor;
         }
         return c;
      }
   };
   PSIO1_REFLECT(OldQuery, small, rows100, rows10k, wide, parents, method(page, first))

   bool  old_available() { return true; }
   void* old_root(const Data& d) { return new OldQuery{d.small, d.rows100, d.rows10k, d.wide, d.parents}; }
   std::string old_run(void* root, std::string_view query)
   {
      return psio1::gql_query(*static_cast<const OldQuery*>(root), query, {});
   }
}  // namespace gqlbench
