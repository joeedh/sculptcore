#pragma once

#include "attribute.h"
#include "elem_data.h"
#include "mesh_iter.h"
#include "mesh_types.h"

#include <concepts>
#include <span>
#include <utility>

#include "math/vector.h"
#include "util/vector.h"

/** DO NOT USE AUTO WITH PROXIES! */

namespace sculptcore::mesh {

/* Have to package proxies inside a single struct
 * to properly resolve circular depenencies.
 */
struct Proxies {
  template <bool is_inline> struct EdgeProxy;
  template <bool is_inline> struct VertProxy;

  /** is_inline: Whether assigning to this proxy should
   *  update the original index it came from (the src_idx pointer).
   */
  template <bool is_inline = false> struct VertProxy {
    int v;

    VertProxy(MeshBase *m_, int v_, int *src_idx_ = nullptr)
        : v(v_), m(m_), src_idx(src_idx_)
    {
    }

    VertProxy &operator=(int idx)
    {
      if constexpr (is_inline) {
        *src_idx = e = idx;
        printf("updating src vindex\n");
      } else {
        printf("updating proxy vindex\n");
        e = idx;
      }

      return *this;
    }

    template <typename EdgeProxyObject> VertProxy &operator=(const EdgeProxyObject &b)
    {
      if constexpr (is_inline) {
        *src_idx = e = b.e;
        printf("updating src vindex\n");
      } else {
        e = b.e;
        printf("updating proxy vindex\n");
      }

      return *this;
    }

    EdgeProxy<true> e()
    {
      return EdgeProxy<true>(m, m->v.e[v], &m->v.e[v]);
    }

    math::float3 &co()
    {
      return m->v.co[v];
    }

    math::float3 &no()
    {
      return m->v.no[v];
    }

    bool select() const
    {
      return m->v.select[v];
    }

    void select_set(bool val)
    {
      m->v.select.set(v, val);
    }

    EdgeOfVertIter edges()
    {
      return EdgeOfVertIter(m, v, m->v.e[v]);
    }

  private:
    MeshBase *m;
    int *src_idx;
  };

  /** is_inline: Whether assigning to this proxy should
   *  update the original index it came from (the src_idx_ pointer).
   */
  template <bool is_inline = false> struct EdgeProxy {
    int e;

    EdgeProxy(MeshBase *m_, int e_, int *src_idx_ = nullptr)
        : m(m_), e(e_), src_idx(src_idx_)
    {
    }

    EdgeProxy(const EdgeProxy &) = delete;
    EdgeProxy(const EdgeProxy &&) = delete;

    EdgeProxy &operator=(int idx)
    {
      if constexpr (is_inline) {
        *src_idx = e = idx;
        printf("updating src eindex\n");
      } else {
        printf("updating proxy eindex\n");
        e = idx;
      }

      return *this;
    }

    template <typename EdgeProxyObject> EdgeProxy &operator=(const EdgeProxyObject &b)
    {
      if constexpr (is_inline) {
        *src_idx = e = b.e;
        printf("updating src eindex\n");
      } else {
        e = b.e;
        printf("updating proxy eindex\n");
      }

      return *this;
    }

    VertProxy<true> v1()
    {
      return VertProxy<true>(m, m->e.vs[e][0], &m->e.vs[e][0]);
    }

    VertProxy<true> v2()
    {
      return VertProxy<true>(m, m->e.vs[e][1], &m->e.vs[e][1]);
    }

  private:
    MeshBase *m;
    int *src_idx;
  };
};

using EdgeProxy = Proxies::EdgeProxy<false>;
using VertProxy = Proxies::VertProxy<false>;

} // namespace sculptcore::mesh
