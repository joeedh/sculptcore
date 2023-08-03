#pragma once

#include "attribute.h"
#include "elem_data.h"
#include "mesh_iter.h"
#include "mesh_types.h"

#include <concepts>
#include <span>
#include <utility>

#include "math/vector.h"
#include "util/compiler_util.h"
#include "util/vector.h"

namespace sculptcore::mesh {

/* Have to package proxies inside a single struct
 * to properly resolve circular depenencies.
 */
struct Proxies {
  enum AssignMode {
    ASSIGN_PROXY = 0,
    ASSIGN_SRC = 1,
  };

  template <AssignMode is_inline> struct VertProxy;
  template <AssignMode is_inline> struct EdgeProxy;
  template <AssignMode is_inline> struct CornerProxy;
  template <AssignMode is_inline> struct ListProxy;
  template <AssignMode is_inline> struct FaceProxy;

  struct EdgeVertProxyIter {
    inline EdgeVertProxyIter(MeshBase *m, int v, int e) : iter(m, v, e)
    {
    }

    inline EdgeVertProxyIter(const EdgeOfVertIter &b) : iter(b)
    {
    }

    inline EdgeVertProxyIter(const EdgeVertProxyIter &b) : iter(b.iter)
    {
    }

    inline bool operator==(const EdgeVertProxyIter &b) const
    {
      return iter == b.iter;
    }
    inline bool operator!=(const EdgeVertProxyIter &b) const
    {
      return iter != b.iter;
    }

    inline EdgeVertProxyIter &operator++()
    {
      ++iter;
      return *this;
    }

    inline EdgeProxy<ASSIGN_PROXY> operator*() const
    {
      return EdgeProxy<ASSIGN_PROXY>(iter.m, *iter);
    }

    inline EdgeVertProxyIter begin() const
    {
      return EdgeVertProxyIter(iter.begin());
    }

    inline EdgeVertProxyIter end() const
    {
      return EdgeVertProxyIter(iter.end());
    }

  private:
    EdgeOfVertIter iter;
  };

  /** is_inline: Whether assigning to this proxy should
   *  update the original index it came from (the src_idx pointer).
   */
  template <AssignMode is_inline = ASSIGN_PROXY> struct VertProxy {
    int i;

    inline operator int()
    {
      return i;
    }

    inline VertProxy(MeshBase *m_, int v, int *src_idx_ = nullptr)
        : i(v), m(m_), src_idx(src_idx_)
    {
    }

    inline VertProxy &operator=(int idx)
    {
      if constexpr (is_inline == ASSIGN_SRC) {
        *src_idx = i = idx;
        printf("updating src vindex\n");
      } else {
        printf("updating proxy vindex\n");
        i = idx;
      }

      return *this;
    }

    template <typename VertProxyObject>
    inline VertProxy &operator=(const VertProxyObject &b)
    {
      if constexpr (is_inline == ASSIGN_SRC) {
        *src_idx = i = b.i;
        printf("updating src vindex\n");
      } else {
        i = b.i;
        printf("updating proxy vindex\n");
      }

      return *this;
    }

    int valence()
    {
      int e = m->v.e[i];

      if (e == ELEM_NONE) {
        return 0;
      }

      int count = 0;
      for (int e : EdgeOfVertIter(m, i, e)) {
        count++;
      }

      return count;
    }

    inline EdgeProxy<ASSIGN_SRC> e()
    {
      return EdgeProxy<ASSIGN_SRC>(m, m->v.e[i], &m->v.e[i]);
    }

    inline math::float3 &co()
    {
      return m->v.co[i];
    }

    inline math::float3 &no()
    {
      return m->v.no[i];
    }

    inline bool select() const
    {
      return m->v.select[i];
    }

    inline void select_set(bool val)
    {
      m->v.select.set(i, val);
    }

    inline EdgeVertProxyIter edges()
    {
      return EdgeVertProxyIter(m, i, m->v.e[i]);
    }

  private:
    MeshBase *m;
    int *src_idx;
  };

  /** is_inline: Whether assigning to this proxy should
   *  update the original index it came from (the src_idx_ pointer).
   */
  template <AssignMode is_inline = ASSIGN_PROXY> struct EdgeProxy {
    int i;
    MeshBase *m;

    inline EdgeProxy(MeshBase *m_, int e, int *src_idx_ = nullptr)
        : m(m_), i(e), src_idx(src_idx_)
    {
    }

    EdgeProxy(const EdgeProxy &) = delete;
    EdgeProxy(const EdgeProxy &&) = delete;

    inline EdgeProxy &operator=(int idx)
    {
      if constexpr (is_inline == ASSIGN_SRC) {
        *src_idx = i = idx;
        printf("updating src eindex\n");
      } else {
        printf("updating proxy eindex\n");
        i = idx;
      }

      return *this;
    }

    template <typename EdgeProxyObject>
    inline EdgeProxy &operator=(const EdgeProxyObject &b)
    {
      if constexpr (is_inline == ASSIGN_SRC) {
        *src_idx = i = b.i;
        printf("updating src eindex\n");
      } else {
        i = b.i;
        printf("updating proxy eindex\n");
      }

      return *this;
    }

    inline VertProxy<ASSIGN_PROXY> other_vert(int v)
    {
      int v2 = ELEM_NONE;

      if (v == m->e.vs[i][0]) {
        v2 = m->e.vs[i][1];
      } else if (v == m->e.vs[i][1]) {
        v2 = m->e.vs[i][0];
      }

      return VertProxy<ASSIGN_PROXY>(m, v2);
    }

    inline VertProxy<ASSIGN_SRC> v1()
    {
      return VertProxy<ASSIGN_SRC>(m, m->e.vs[i][0], &m->e.vs[i][0]);
    }

    inline VertProxy<ASSIGN_SRC> v2()
    {
      return VertProxy<ASSIGN_SRC>(m, m->e.vs[i][1], &m->e.vs[i][1]);
    }

  private:
    int *src_idx;
  };

  template <typename Derived, AssignMode is_inline> struct ProxyBase {
    inline operator int() const
    {
      return get_this().i;
    }

    ProxyBase(MeshBase *m, int i, int *src_idx_) : src_idx(src_idx_)
    {
      get_this().m = m;
      get_this().i = i;
    }

    force_inline Derived &get_this()
    {
      return *(reinterpret_cast<Derived *>(this));
    }

    force_inline const Derived &get_this() const
    {
      return *(reinterpret_cast<const Derived *>(this));
    }

    ProxyBase(const ProxyBase &) = delete;

    template <typename ProxyImpl> inline void operator=(const ProxyImpl &b)
    {
      if constexpr (is_inline == ASSIGN_SRC) {
        *src_idx = get_this().i = b.i;
        printf("updating src index\n");
      } else {
        get_this().i = b.i;
        printf("updating proxy index\n");
      }
    }

    inline void operator=(int idx)
    {
      if constexpr (is_inline == ASSIGN_SRC) {
        *src_idx = get_this().i = idx;
        printf("updating src index\n");
      } else {
        printf("updating proxy index\n");
        get_this().i = idx;
      }
    }

  private:
    int *src_idx;
  };

  template <AssignMode is_inline = ASSIGN_PROXY>
  struct CornerProxy : public ProxyBase<CornerProxy<is_inline>, is_inline> {
    using ProxyBaseCls = ProxyBase<CornerProxy, is_inline>;

    int i = ELEM_NONE;
    MeshBase *m;

    inline CornerProxy(MeshBase *m, int c, int *src_idx = nullptr)
        : ProxyBaseCls(m, c, src_idx), i(c)
    {
    }

    inline CornerProxy<ASSIGN_SRC> prev()
    {
      return CornerProxy<ASSIGN_SRC>(m, m->c.prev[i], &m->c.prev[i]);
    }
    inline CornerProxy<ASSIGN_SRC> next()
    {
      return CornerProxy<ASSIGN_SRC>(m, m->c.next[i], &m->c.next[i]);
    }

    /* Returns true if the number of corners
     * around the edge is greater than 3.
     */
    inline bool manifold()
    {
      bool ok = m->c.radial_next[i] == i;
      ok = ok || (m->c.radial_next[m->c.radial_next[i]] == i);

      return ok;
    }

    inline CornerProxy<ASSIGN_SRC> radial_prev()
    {
      return CornerProxy<ASSIGN_SRC>(m, m->c.radial_prev[i], &m->c.radial_prev[i]);
    }
    inline CornerProxy<ASSIGN_SRC> radial_next()
    {
      return CornerProxy<ASSIGN_SRC>(m, m->c.radial_next[i], &m->c.radial_next[i]);
    }

    inline VertProxy<ASSIGN_SRC> v()
    {
      return VertProxy<ASSIGN_SRC>(m, m->c.v[i], &m->c.v[i]);
    }

    inline EdgeProxy<ASSIGN_SRC> e()
    {
      return EdgeProxy<ASSIGN_SRC>(m, m->c.e[i], &m->c.e[i]);
    }

    template <typename ProxyImpl> inline void operator=(const ProxyImpl &b)
    {
      ProxyBaseCls::operator=<ProxyImpl>(b);
      return *this;
    }

    inline CornerProxy &operator=(int idx)
    {
      ProxyBaseCls::operator=(idx);
      return *this;
    }
  };

  template <AssignMode is_inline = ASSIGN_PROXY>
  struct ListProxy : public ProxyBase<ListProxy<is_inline>, is_inline> {
    struct iterator {
      inline iterator(MeshBase *m_, int c_) : m(m_), c(c_), start_c(c_)
      {
      }

      inline iterator(const iterator &b) : m(b.m), c(b.c), start_c(b.start_c)
      {
      }

      inline bool operator==(const iterator &b)
      {
        return b.c == c;
      }

      inline bool operator!=(const iterator &b)
      {
        return !operator==(b);
      }

      CornerProxy<ASSIGN_PROXY> operator*()
      {
        return CornerProxy<ASSIGN_PROXY>(m, c);
      }

      iterator &operator++()
      {
        c = m->c.next[c];

        if (c == start_c) {
          c = ELEM_NONE;
        }

        return *this;
      }

      MeshBase *m;
      int c, start_c;
    };

    using ProxyBaseCls = ProxyBase<ListProxy, is_inline>;

    int i = ELEM_NONE;
    MeshBase *m;

    inline ListProxy(MeshBase *m, int l, int *src_idx = nullptr)
        : ProxyBaseCls(m, l, src_idx), i(l)
    {
    }

    inline ListProxy<ASSIGN_SRC> next()
    {
      return ListProxy<ASSIGN_SRC>(m, m->l.next[i], &m->l.next[i]);
    }

    inline int size()
    {
      return m->l.size[i];
    }

    inline CornerProxy<ASSIGN_SRC> c()
    {
      return CornerProxy<ASSIGN_SRC>(m, m->l.c[i], &m->l.c[i]);
    }

    inline FaceProxy<ASSIGN_SRC> f()
    {
      return FaceProxy<ASSIGN_SRC>(m, m->l.f[i], &m->l.f[i]);
    }

    iterator begin()
    {
      return iterator(m, m->l.c[i]);
    }

    iterator end()
    {
      return iterator(m, ELEM_NONE);
    }
  };

  template <AssignMode is_inline = ASSIGN_PROXY>
  struct FaceProxy : public ProxyBase<FaceProxy<is_inline>, is_inline> {
    struct FaceListIterator {
      FaceListIterator(MeshBase *m_, int l_) : m(m_), l(l_)
      {
      }

      FaceListIterator(const FaceListIterator &b) : m(b.m), l(b.l)
      {
      }

      bool operator==(const FaceListIterator &b)
      {
        return b.l == l;
      }
      bool operator!=(const FaceListIterator &b)
      {
        return !operator==(b);
      }

      ListProxy<ASSIGN_PROXY> operator*()
      {
        return ListProxy<ASSIGN_PROXY>(m, l);
      }

      FaceListIterator &operator++()
      {
        l = m->l.next[l];

        return *this;
      }

      FaceListIterator begin()
      {
        return *this;
      }

      FaceListIterator end()
      {
        return FaceListIterator(m, ELEM_NONE);
      }

      int l;
      MeshBase *m;
    };

    using ProxyBaseCls = ProxyBase<FaceProxy, is_inline>;

    int i = ELEM_NONE;
    MeshBase *m;

    inline FaceProxy(MeshBase *m, int f, int *src_idx = nullptr)
        : ProxyBaseCls(m, f, src_idx), i(f)
    {
    }

    inline CornerProxy<ASSIGN_SRC> l()
    {
      return CornerProxy<ASSIGN_SRC>(m, m->f.l[i], &m->f.l[i]);
    }

    inline math::float3 &no()
    {
      return m->f.no[i];
    }

    inline int &list_count()
    {
      return m->f.list_count[i];
    }

    FaceListIterator lists()
    {
      return FaceListIterator(m, m->f.l[i]);
    }

    math::float3 calc_center()
    {
      float tot = 0.0f;
      math::float3 cent(0.0f);

      for (auto list : lists()) {
        for (auto c : list) {
          cent += c.v().co();
        }
      }

      return cent / tot;
    }
  };
};

using EdgeProxy = Proxies::EdgeProxy<Proxies::ASSIGN_PROXY>;
using VertProxy = Proxies::VertProxy<Proxies::ASSIGN_PROXY>;
using CornerProxy = Proxies::CornerProxy<Proxies::ASSIGN_PROXY>;
using ListProxy = Proxies::ListProxy<Proxies::ASSIGN_PROXY>;
using FaceProxy = Proxies::FaceProxy<Proxies::ASSIGN_PROXY>;
} // namespace sculptcore::mesh
