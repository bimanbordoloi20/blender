#include "MEM_guardedalloc.h"

#include "BKE_customdata.h"
#include "BKE_mesh.h"

#include "DNA_customdata_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BLI_hash.hh"
#include "BLI_listbase_wrapper.hh"
#include "BLI_map.hh"
#include "BLI_math.h"
#include "BLI_math_vector_types.hh"
#include "BLI_mempool.h"
#include "BLI_set.hh"
#include "BLI_vector.hh"

#include "bmesh.h"
#include "bmesh_idmap.h"
#include "bmesh_log_intern.h"
extern "C" {
#include "bmesh_structure.h"
}

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <type_traits>

#ifdef DO_LOG_PRINT
static int msg_idgen = 1;
static char msg_buffer[256] = {0};

#  define SET_MSG(le) memcpy(le->msg, msg_buffer, sizeof(le->msg))
#  define GET_MSG(le) (le)->msg
#  ifdef DEBUG_LOG_CALL_STACKS
#    define LOGPRINT(entry, ...) \
      fprintf(DEBUG_FILE, "%d: %s: ", entry->id, bm_logstack_head()); \
      fprintf(DEBUG_FILE, __VA_ARGS__)
#  else
#    define LOGPRINT(entry, ...) \
      fprintf(DEBUG_FILE, "%s: ", __func__); \
      fprintf(DEBUG_FILE, __VA_ARGS__)
#  endif
struct Mesh;
#else
#  define GET_MSG(le) le ? "" : " "
#  define SET_MSG(le)
#  define LOGPRINT(...)
#endif

extern "C" void bm_log_message(const char *fmt, ...)
{
  char msg[64];

  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

#ifdef DO_LOG_PRINT
  BLI_snprintf(msg_buffer, 64, "%d %s", msg_idgen, msg);
  msg_idgen++;

  fprintf(DEBUG_FILE, "%s\n", msg);
#endif
}

/* Avoid C++ runtime type ids. */
enum BMLogSetType { LOG_SET_DIFF, LOG_SET_FULL };

namespace blender {

template<typename T> struct BMID {
  int id = -1;

  BMID(int _id) : id(_id)
  {
  }

  T *lookup(BMIdMap *idmap)
  {
    return reinterpret_cast<T *>(BM_idmap_lookup(idmap, id));
  }

  uint64_t hash() const
  {
    return (uint64_t)id;
  }

  bool operator==(const BMID<T> &b) const
  {
    return id == b.id;
  }
};

template<typename T> struct BMLogElem {
  BMID<T> id = BMID<T>(-1);
  void *customdata = nullptr;
  char flag = 0;
};

template<typename T> struct LogElemAlloc {
  BLI_mempool *pool;

  LogElemAlloc()
  {
    /* We need an iterable pool to call individual destructors in ~LogElemAlloc(). */
    pool = BLI_mempool_create(sizeof(T), 0, 256, BLI_MEMPOOL_ALLOW_ITER);
  }

  LogElemAlloc(const LogElemAlloc &b) = delete;

  LogElemAlloc(LogElemAlloc &&b)
  {
    pool = b.pool;
    b.pool = nullptr;
  }

  int calc_size()
  {
    return BLI_mempool_get_size(pool);
  }

  void operator=(LogElemAlloc &&b)
  {
    pool = b.pool;
    b.pool = nullptr;
  }

  ~LogElemAlloc()
  {
    if (pool) {
      BLI_mempool_iter iter;
      BLI_mempool_iternew(pool, &iter);
      while (void *entry = BLI_mempool_iterstep(&iter)) {
        T *ptr = static_cast<T *>(entry);
        ptr->~T();
      }

      BLI_mempool_destroy(pool);
    }
  }

  T *alloc()
  {
    void *mem = BLI_mempool_alloc(pool);
    return new (mem) T();
  }

  void free(T *elem)
  {
    elem->~T();

    BLI_mempool_free(pool, static_cast<void *>(elem));
  }
};

struct BMLogVert : public BMLogElem<BMVert> {
  float3 co;
  float3 no;
};

struct BMLogEdge : public BMLogElem<BMEdge> {
  BMID<BMVert> v1 = BMID<BMVert>(-1);
  BMID<BMVert> v2 = BMID<BMVert>(-1);
};

struct BMLogFace : public BMLogElem<BMFace> {
  Vector<BMID<BMVert>, 5> verts;
  Vector<void *, 5> loop_customdata;
  // int material_index;
};

struct BMLogEntry;

struct BMLogSetBase {
  BMLogSetType type;
  BMLogEntry *entry = nullptr; /* Parent entry */

  BMLogSetBase(BMLogEntry *_entry, BMLogSetType _type) : type(_type), entry(_entry)
  {
  }

  virtual ~BMLogSetBase()
  {
  }

  virtual void undo(BMesh *bm, BMLogCallbacks *callbacks)
  {
  }
  virtual void redo(BMesh *bm, BMLogCallbacks *callbacks)
  {
  }
};

struct BMLogSetDiff : public BMLogSetBase {
  BMLogSetDiff(BMLogEntry *entry) : BMLogSetBase(entry, LOG_SET_DIFF)
  {
  }

  Map<BMID<BMVert>, BMLogVert *> modified_verts;
  Map<BMID<BMEdge>, BMLogEdge *> modified_edges;
  Map<BMID<BMFace>, BMLogFace *> modified_faces;

  Map<BMID<BMVert>, BMLogVert *> removed_verts;
  Map<BMID<BMEdge>, BMLogEdge *> removed_edges;
  Map<BMID<BMFace>, BMLogFace *> removed_faces;

  Map<BMID<BMVert>, BMLogVert *> added_verts;
  Map<BMID<BMEdge>, BMLogEdge *> added_edges;
  Map<BMID<BMFace>, BMLogFace *> added_faces;

  void add_vert(BMesh *bm, BMVert *v);
  void remove_vert(BMesh *bm, BMVert *v);
  void modify_vert(BMesh *bm, BMVert *v);
  void add_edge(BMesh *bm, BMEdge *e);
  void remove_edge(BMesh *bm, BMEdge *e);
  void modify_edge(BMesh *bm, BMEdge *e);
  void add_face(BMesh *bm, BMFace *f);
  void remove_face(BMesh *bm, BMFace *f, bool no_check = false);
  void modify_face(BMesh *bm, BMFace *f);

  void undo(BMesh *bm, BMLogCallbacks *callbacks) override;
  void redo(BMesh *bm, BMLogCallbacks *callbacks) override;

  void restore_verts(BMesh *bm,
                     blender::Map<BMID<BMVert>, BMLogVert *> verts,
                     BMLogCallbacks *callbacks);
  void remove_verts(BMesh *bm,
                    blender::Map<BMID<BMVert>, BMLogVert *> verts,
                    BMLogCallbacks *callbacks);
  void swap_verts(BMesh *bm,
                  blender::Map<BMID<BMVert>, BMLogVert *> verts,
                  BMLogCallbacks *callbacks);
  void restore_edges(BMesh *bm,
                     blender::Map<BMID<BMEdge>, BMLogEdge *> edges,
                     BMLogCallbacks *callbacks);
  void remove_edges(BMesh *bm,
                    blender::Map<BMID<BMEdge>, BMLogEdge *> edges,
                    BMLogCallbacks *callbacks);
  void swap_edges(BMesh *bm,
                  blender::Map<BMID<BMEdge>, BMLogEdge *> edges,
                  BMLogCallbacks *callbacks);

  void restore_faces(BMesh *bm,
                     blender::Map<BMID<BMFace>, BMLogFace *> faces,
                     BMLogCallbacks *callbacks);
  void remove_faces(BMesh *bm,
                    blender::Map<BMID<BMFace>, BMLogFace *> faces,
                    BMLogCallbacks *callbacks);
  void swap_faces(BMesh *bm,
                  blender::Map<BMID<BMFace>, BMLogFace *> faces,
                  BMLogCallbacks *callbacks);
};

struct BMLogSetFull : public BMLogSetBase {
  BMLogSetFull(BMesh *bm, BMLogEntry *entry) : BMLogSetBase(entry, LOG_SET_FULL)
  {
    BMeshToMeshParams params = {};
    params.update_shapekey_indices = false;
    params.calc_object_remap = false;
    params.copy_temp_cdlayers = true;
    params.ignore_mesh_id_layers = false;

    mesh = BKE_mesh_from_bmesh_nomain(bm, &params, nullptr);
  }

  void ATTR_NO_OPT swap(BMesh *bm)
  {
    CustomData_MeshMasks cd_mask_extra = {CD_MASK_DYNTOPO_VERT | CD_MASK_SHAPEKEY, 0, 0, 0, 0};

    BMeshToMeshParams params = {};
    params.update_shapekey_indices = false;
    params.calc_object_remap = false;
    params.copy_temp_cdlayers = true;
    params.ignore_mesh_id_layers = false;

    Mesh *current_mesh = BKE_mesh_from_bmesh_nomain(bm, &params, nullptr);

    int shapenr = bm->shapenr;
    BMeshFromMeshParams params2 = {};
    params2.copy_temp_cdlayers = true;
    params2.cd_mask_extra = cd_mask_extra;
    params2.calc_face_normal = params2.add_key_index = params2.use_shapekey =
        params2.ignore_id_layers = false;

    BM_mesh_clear(bm);
    BM_mesh_bm_from_me(bm,
                       mesh,  // note we stored shapekeys as customdata layers,
                              // that's why the shapekey params are false
                       &params2);

    bm->shapenr = shapenr;

    bm->elem_index_dirty |= BM_VERT | BM_EDGE | BM_FACE;
    bm->elem_table_dirty |= BM_VERT | BM_EDGE | BM_FACE;

    BM_mesh_elem_table_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);
    BM_mesh_elem_index_ensure(bm, BM_VERT | BM_EDGE | BM_FACE);

    BKE_mesh_free_data_for_undo(mesh);
    mesh = current_mesh;
  }

  void undo(BMesh *bm, BMLogCallbacks *callbacks) override
  {
    swap(bm);
    callbacks->on_full_mesh_load(callbacks->userdata);
  }

  void redo(BMesh *bm, BMLogCallbacks *callbacks) override
  {
    swap(bm);
    callbacks->on_full_mesh_load(callbacks->userdata);
  }

  Mesh *mesh = nullptr;
};

static const char *get_elem_htype_str(int htype)
{
  switch (htype) {
    case BM_VERT:
      return "vertex";
    case BM_EDGE:
      return "edge";
    case BM_LOOP:
      return "loop";
    case BM_FACE:
      return "face";
    default:
      return "unknown type";
  }
}

template<typename T> constexpr char get_elem_type()
{
  if constexpr (std::is_same_v<T, BMVert>) {
    return BM_VERT;
  }
  else if constexpr (std::is_same_v<T, BMEdge>) {
    return BM_EDGE;
  }
  else if constexpr (std::is_same_v<T, BMLoop>) {
    return BM_LOOP;
  }
  else if constexpr (std::is_same_v<T, BMFace>) {
    return BM_FACE;
  }
}

struct BMLogEntry {
  struct BMLogEntry *next = nullptr, *prev = nullptr;

  Vector<BMLogSetBase *> sets;
  LogElemAlloc<BMLogVert> vpool;
  LogElemAlloc<BMLogEdge> epool;
  LogElemAlloc<BMLogFace> fpool;

  CustomData vdata, edata, ldata, pdata;
  BMIdMap *idmap = nullptr;

  BMLog *log = nullptr;

  ATTR_NO_OPT BMLogEntry(BMIdMap *_idmap,
                         CustomData *src_vdata,
                         CustomData *src_edata,
                         CustomData *src_ldata,
                         CustomData *src_pdata)
      : idmap(_idmap)
  {
    CustomData_copy_all_layout(src_vdata, &vdata);
    CustomData_copy_all_layout(src_edata, &edata);
    CustomData_copy_all_layout(src_ldata, &ldata);
    CustomData_copy_all_layout(src_pdata, &pdata);

    CustomData_bmesh_init_pool(&vdata, 0, BM_VERT);
    CustomData_bmesh_init_pool(&edata, 0, BM_EDGE);
    CustomData_bmesh_init_pool(&ldata, 0, BM_LOOP);
    CustomData_bmesh_init_pool(&pdata, 0, BM_FACE);
  }

  ATTR_NO_OPT ~BMLogEntry()
  {
    for (BMLogSetBase *set : sets) {
      switch (set->type) {
        case LOG_SET_DIFF:
          delete static_cast<BMLogSetDiff *>(set);
          break;
        case LOG_SET_FULL:
          delete static_cast<BMLogSetFull *>(set);
          break;
      }
    }

    if (vdata.pool) {
      BLI_mempool_destroy(vdata.pool);
    }
    if (edata.pool) {
      BLI_mempool_destroy(edata.pool);
    }
    if (ldata.pool) {
      BLI_mempool_destroy(ldata.pool);
    }
    if (pdata.pool) {
      BLI_mempool_destroy(pdata.pool);
    }

    CustomData_free(&vdata, 0);
    CustomData_free(&edata, 0);
    CustomData_free(&ldata, 0);
    CustomData_free(&pdata, 0);
  }

  template<typename T> T *get_elem_from_id(BMesh *bm, BMID<T> id)
  {
    T *elem = reinterpret_cast<T *>(BM_idmap_lookup(idmap, id.id));
    char htype = 0;

    if (!elem) {
      return nullptr;
    }

    if constexpr (std::is_same_v<T, BMVert>) {
      htype = BM_VERT;
    }
    if constexpr (std::is_same_v<T, BMEdge>) {
      htype = BM_EDGE;
    }
    if constexpr (std::is_same_v<T, BMFace>) {
      htype = BM_FACE;
    }

    if (elem->head.htype != htype) {
      printf("%s: error: expected %s, got %s; id: %d\n",
             __func__,
             get_elem_htype_str(elem->head.htype),
             get_elem_htype_str(htype),
             id.id);
      return nullptr;
    }

    return elem;
  }

  template<typename T> void assign_elem_id(BMesh *bm, T *elem, BMID<T> _id, bool check_unique)
  {
    int id = _id.id;

    if (check_unique) {
      BMElem *old;
      old = BM_idmap_lookup(idmap, id);

      if (old && old != (BMElem *)elem) {
        printf(
            "id conflict in BMLogEntry::assign_elem_id; elem %p (a %s) is being reassinged to id "
            "%d.\n",
            elem,
            get_elem_htype_str((int)elem->head.htype),
            (int)id);
        printf(
            "  elem %p (a %s) will get a new id\n", old, get_elem_htype_str((int)old->head.htype));

        BM_idmap_assign(idmap, reinterpret_cast<BMElem *>(elem), id);
        return;
      }
    }

    BM_idmap_assign(idmap, reinterpret_cast<BMElem *>(elem), id);
  }

  template<typename T> BMID<T> get_elem_id(BMesh *bm, T *elem)
  {
    BM_idmap_check_assign(idmap, reinterpret_cast<BMElem *>(elem));
    return BM_idmap_get_id(idmap, reinterpret_cast<BMElem *>(elem));
  }

  ATTR_NO_OPT void push_set(BMesh *bm, BMLogSetType type)
  {
    switch (type) {
      case LOG_SET_DIFF:
        sets.append(static_cast<BMLogSetBase *>(new BMLogSetDiff(this)));
        break;
      case LOG_SET_FULL:
        sets.append(static_cast<BMLogSetBase *>(new BMLogSetFull(bm, this)));
        break;
    }
  }

  BMLogSetDiff *current_diff_set(BMesh *bm)
  {
    if (sets.size() == 0 || sets[sets.size() - 1]->type != LOG_SET_DIFF) {
      push_set(bm, LOG_SET_DIFF);
    }

    return static_cast<BMLogSetDiff *>(sets[sets.size() - 1]);
  }

  ATTR_NO_OPT void update_logvert(BMesh *bm, BMVert *v, BMLogVert *lv)
  {
    if (vdata.pool) {
      CustomData_bmesh_copy_data(&bm->vdata, &vdata, v->head.data, &lv->customdata);
    }

    lv->co = v->co;
    lv->no = v->no;
    lv->flag = v->head.hflag;
  }

  ATTR_NO_OPT void swap_logvert(BMesh *bm, BMID<BMVert> id, BMVert *v, BMLogVert *lv)
  {
    if (v->head.data && lv->customdata) {
      CustomData_bmesh_swap_data(&vdata, &bm->vdata, lv->customdata, &v->head.data);
    }

    std::swap(v->head.hflag, lv->flag);
    swap_v3_v3(v->co, lv->co);
    swap_v3_v3(v->no, lv->no);
  }

  ATTR_NO_OPT void swap_logedge(BMesh *bm, BMID<BMEdge> id, BMEdge *e, BMLogEdge *le)
  {
    if (e->head.data && le->customdata) {
      CustomData_bmesh_swap_data(&edata, &bm->edata, le->customdata, &e->head.data);
    }
  }

  ATTR_NO_OPT void swap_logface(BMesh *bm, BMID<BMFace> id, BMFace *f, BMLogFace *lf)
  {
    if (f->head.data && lf->customdata) {
      CustomData_bmesh_swap_data(&pdata, &bm->pdata, lf->customdata, &f->head.data);
    }

    if (f->len != lf->verts.size()) {
      printf("%s: error: wrong length for face, was %d, should be %d\n",
             __func__,
             f->len,
             (int)lf->verts.size());
      return;
    }

    if (lf->loop_customdata[0]) {
      BMLoop *l = f->l_first;

      int i = 0;
      do {
        CustomData_bmesh_swap_data(&ldata, &bm->ldata, lf->loop_customdata[i], &l->head.data);
        i++;
      } while ((l = l->next) != f->l_first);
    }
    std::swap(f->head.hflag, lf->flag);
  }

  ATTR_NO_OPT BMLogVert *alloc_logvert(BMesh *bm, BMVert *v)
  {
    BMID<BMVert> id = get_elem_id<BMVert>(bm, v);
    BMLogVert *lv = vpool.alloc();

    lv->id = id;

    update_logvert(bm, v, lv);

    return lv;
  }

  ATTR_NO_OPT void free_logvert(BMLogVert *lv)
  {
    if (lv->customdata) {
      BLI_mempool_free(vdata.pool, lv->customdata);
    }

    vpool.free(lv);
  }

  ATTR_NO_OPT void load_vert(BMesh *bm, BMVert *v, BMLogVert *lv)
  {
    if (v->head.data && lv->customdata) {
      CustomData_bmesh_copy_data(&vdata, &bm->vdata, lv->customdata, &v->head.data);
    }

    v->head.hflag = lv->flag;
    copy_v3_v3(v->co, lv->co);
    copy_v3_v3(v->no, lv->no);
  }

  BMLogEdge *alloc_logedge(BMesh *bm, BMEdge *e)
  {
    BMLogEdge *le = epool.alloc();

    CustomData_bmesh_copy_data(&bm->edata, &edata, e->head.data, &le->customdata);
    le->id = get_elem_id<BMEdge>(bm, e);
    le->v1 = get_elem_id<BMVert>(bm, e->v1);
    le->v2 = get_elem_id<BMVert>(bm, e->v2);

    return le;
  }

  void update_logedge(BMesh *bm, BMEdge *e, BMLogEdge *le)
  {
    le->flag = e->head.hflag;
    CustomData_bmesh_copy_data(&bm->edata, &edata, e->head.data, &le->customdata);
  }

  void free_logedge(BMesh *bm, BMLogEdge *e)
  {
    epool.free(e);
  }

  BMLogFace *alloc_logface(BMesh *bm, BMFace *f)
  {
    BMLogFace *lf = fpool.alloc();

    lf->id = get_elem_id<BMFace>(bm, f);
    lf->flag = f->head.hflag;
    CustomData_bmesh_copy_data(&bm->pdata, &pdata, f->head.data, &lf->customdata);

    BMLoop *l = f->l_first;
    do {
      lf->verts.append(get_elem_id<BMVert>(bm, l->v));
      void *loop_customdata = nullptr;

      if (l->head.data) {
        CustomData_bmesh_copy_data(&bm->ldata, &ldata, l->head.data, &loop_customdata);
      }

      lf->loop_customdata.append(loop_customdata);
    } while ((l = l->next) != f->l_first);

    return lf;
  }

  void update_logface(BMesh *bm, BMLogFace *lf, BMFace *f)
  {
    lf->flag = f->head.hflag;
    CustomData_bmesh_copy_data(&bm->pdata, &pdata, f->head.data, &lf->customdata);

    if (f->len != lf->verts.size()) {
      printf("%s: error: face length changed.\n", __func__);
      return;
    }

    BMLoop *l = f->l_first;
    int i = 0;
    do {
      void *loop_customdata = nullptr;

      if (l->head.data) {
        CustomData_bmesh_copy_data(&bm->ldata, &ldata, l->head.data, &loop_customdata);
      }

      lf->loop_customdata[i++] = loop_customdata;
    } while ((l = l->next) != f->l_first);
  }

  void free_logface(BMesh *bm, BMLogFace *lf)
  {
    if (lf->loop_customdata[0]) {
      for (int i = 0; i < lf->verts.size(); i++) {
        BLI_mempool_free(ldata.pool, lf->loop_customdata[i]);
      }
    }

    if (lf->customdata) {
      BLI_mempool_free(pdata.pool, lf->customdata);
    }

    fpool.free(lf);
  }

  void add_vert(BMesh *bm, BMVert *v)
  {
    current_diff_set(bm)->add_vert(bm, v);
  }

  void remove_vert(BMesh *bm, BMVert *v)
  {
    current_diff_set(bm)->remove_vert(bm, v);
  }

  void modify_vert(BMesh *bm, BMVert *v)
  {
    current_diff_set(bm)->modify_vert(bm, v);
  }

  void add_edge(BMesh *bm, BMEdge *e)
  {
    current_diff_set(bm)->add_edge(bm, e);
  }

  void remove_edge(BMesh *bm, BMEdge *e)
  {
    current_diff_set(bm)->remove_edge(bm, e);
  }

  void modify_edge(BMesh *bm, BMEdge *e)
  {
    current_diff_set(bm)->modify_edge(bm, e);
  }

  void add_face(BMesh *bm, BMFace *f)
  {
    current_diff_set(bm)->add_face(bm, f);
  }
  void remove_face(BMesh *bm, BMFace *f, bool no_check = false)
  {
    current_diff_set(bm)->remove_face(bm, f, no_check);
  }
  void modify_face(BMesh *bm, BMFace *f)
  {
    current_diff_set(bm)->modify_face(bm, f);
  }

  ATTR_NO_OPT void undo(BMesh *bm, BMLogCallbacks *callbacks)
  {
    for (int i = sets.size() - 1; i >= 0; i--) {
      // printf("  - %d of %d\n", i, (int)(sets.size() - 1));
      sets[i]->undo(bm, callbacks);
    }
  }

  ATTR_NO_OPT void redo(BMesh *bm, BMLogCallbacks *callbacks)
  {
    for (int i = 0; i < sets.size(); i++) {
      sets[i]->redo(bm, callbacks);
    }
  }

  int calc_size()
  {
    int ret = 0;

    ret += vdata.pool ? (int)BLI_mempool_get_size(vdata.pool) : 0;
    ret += edata.pool ? (int)BLI_mempool_get_size(edata.pool) : 0;
    ret += ldata.pool ? (int)BLI_mempool_get_size(ldata.pool) : 0;
    ret += pdata.pool ? (int)BLI_mempool_get_size(pdata.pool) : 0;

    ret += vpool.calc_size();
    ret += epool.calc_size();
    ret += fpool.calc_size();

    return ret;
  }
};

struct BMLog {
  BMIdMap *idmap = nullptr;
  BMLogEntry *current_entry = nullptr;
  BMLogEntry *first_entry = nullptr;
  int refcount = 1;
  bool dead = false;

  BMLog(BMIdMap *_idmap) : idmap(_idmap)
  {
  }

  ~BMLog()
  {
  }

  ATTR_NO_OPT void set_idmap(BMIdMap *idmap)
  {
    BMLogEntry *entry = first_entry;
    while (entry) {
      entry->idmap = idmap;
      entry = entry->next;
    }
  }

  ATTR_NO_OPT bool free_all_entries()
  {
    BMLogEntry *entry = first_entry;

    if (!entry) {
      return false;
    }

    while (entry) {
      BMLogEntry *next = entry->next;

      MEM_delete<BMLogEntry>(entry);
      entry = next;
    }

    return true;
  }

  ATTR_NO_OPT BMLogEntry *push_entry(BMesh *bm)
  {
    BMLogEntry *entry = MEM_new<BMLogEntry>(
        "BMLogEntry", idmap, &bm->vdata, &bm->edata, &bm->ldata, &bm->pdata);

    /* Truncate undo list. */
    BMLogEntry *entry2 = current_entry ? current_entry->next : nullptr;
    while (entry2) {
      BMLogEntry *next = entry2->next;
      MEM_delete<BMLogEntry>(entry2);

      entry2 = next;
    }

    entry->prev = current_entry;

    if (!first_entry) {
      first_entry = entry;
    }
    else {
      current_entry->next = entry;
    }

    current_entry = entry;
    return entry;
  }

  void load_entries(BMLogEntry *entry)
  {
    first_entry = current_entry = entry;
    while (first_entry->prev) {
      first_entry = first_entry->prev;
    }

    entry = first_entry;
    while (entry) {
      entry->log = this;
      entry->idmap = idmap;
      entry = entry->next;
    }
  }

  ATTR_NO_OPT void ensure_entry(BMesh *bm)
  {
    if (!current_entry) {
      push_entry(bm);
    }
  }

  void add_vert(BMesh *bm, BMVert *v)
  {
    ensure_entry(bm);
    current_entry->add_vert(bm, v);
  }

  void remove_vert(BMesh *bm, BMVert *v)
  {
    ensure_entry(bm);
    current_entry->remove_vert(bm, v);
  }

  void modify_vert(BMesh *bm, BMVert *v)
  {
    ensure_entry(bm);
    current_entry->modify_vert(bm, v);
  }

  void add_edge(BMesh *bm, BMEdge *e)
  {
    ensure_entry(bm);
    current_entry->add_edge(bm, e);
  }

  void remove_edge(BMesh *bm, BMEdge *e)
  {
    ensure_entry(bm);
    current_entry->remove_edge(bm, e);
  }

  void modify_edge(BMesh *bm, BMEdge *e)
  {
    ensure_entry(bm);
    current_entry->modify_edge(bm, e);
  }

  void add_face(BMesh *bm, BMFace *f)
  {
    ensure_entry(bm);
    current_entry->add_face(bm, f);
  }

  void remove_face(BMesh *bm, BMFace *f, bool no_check = false)
  {
    ensure_entry(bm);
    current_entry->remove_face(bm, f, no_check);
  }

  void modify_face(BMesh *bm, BMFace *f)
  {
    ensure_entry(bm);
    current_entry->modify_face(bm, f);
  }

  void full_mesh(BMesh *bm)
  {
    ensure_entry(bm);
    current_entry->push_set(bm, LOG_SET_FULL);
  }

  void skip(int dir)
  {
    if (current_entry) {
      current_entry = dir > 0 ? current_entry->next : current_entry->prev;
    }
  }

  void undo(BMesh *bm, BMLogCallbacks *callbacks)
  {
    current_entry->undo(bm, callbacks);
    current_entry = current_entry->prev;
  }

  void redo(BMesh *bm, BMLogCallbacks *callbacks)
  {
    current_entry = current_entry->next;
    if (current_entry) {
      current_entry->redo(bm, callbacks);
    }
  }
};

void BMLogSetDiff::add_vert(BMesh *bm, BMVert *v)
{
  BMID<BMVert> id = entry->get_elem_id(bm, v);

  BMLogVert *lv = nullptr;
  BMLogVert **modified_lv = modified_verts.lookup_ptr(id);

  if (modified_lv) {
    modified_verts.remove(id);
    lv = *modified_lv;
  }

  if (added_verts.contains(id)) {
    if (lv) {
      entry->free_logvert(lv);
    }
    return;
  }

  if (!lv) {
    lv = entry->alloc_logvert(bm, v);
  }

  added_verts.add(id, lv);
}

void BMLogSetDiff::remove_vert(BMesh *bm, BMVert *v)
{
  BMID<BMVert> id = entry->get_elem_id(bm, v);

  BMLogVert **added_lv = added_verts.lookup_ptr(id);
  if (added_lv) {
    added_verts.remove(id);
    entry->free_logvert(*added_lv);
    return;
  }

  if (removed_verts.contains(id)) {
    return;
  }

  BMLogVert *lv;
  BMLogVert **modified_lv = modified_verts.lookup_ptr(id);
  if (modified_lv) {
    modified_verts.remove(id);
    lv = *modified_lv;
  }
  else {
    lv = entry->alloc_logvert(bm, v);
  }

  removed_verts.add(id, lv);
}

void BMLogSetDiff::modify_vert(BMesh *bm, BMVert *v)
{
  BMID<BMVert> id = entry->get_elem_id(bm, v);
  if (modified_verts.contains(id)) {
    return;
  }

  modified_verts.add(id, entry->alloc_logvert(bm, v));
}

void BMLogSetDiff::add_edge(BMesh *bm, BMEdge *e)
{
  BMID<BMEdge> id = entry->get_elem_id(bm, e);
  BMLogEdge *le;

  BMLogEdge **modified_le = modified_edges.lookup_ptr(id);
  if (modified_le) {
    le = *modified_le;
    modified_edges.remove(id);
  }
  else {
    le = entry->alloc_logedge(bm, e);
  }

  added_edges.add_or_modify(
      id, [&](BMLogEdge **le_out) { *le_out = le; }, [&](BMLogEdge **le_out) { *le_out = le; });
}

void BMLogSetDiff::remove_edge(BMesh *bm, BMEdge *e)
{
  BMID<BMEdge> id = entry->get_elem_id(bm, e);

  if (added_edges.remove(id) || removed_edges.contains(id)) {
    return;
  }

  BMLogEdge *le;
  BMLogEdge **modified_le = modified_edges.lookup_ptr(id);
  if (modified_le) {
    le = *modified_le;
    modified_edges.remove(id);
  }
  else {
    le = entry->alloc_logedge(bm, e);
  }

  removed_edges.add(id, le);
}

void BMLogSetDiff::modify_edge(BMesh *bm, BMEdge *e)
{
}

void BMLogSetDiff::add_face(BMesh *bm, BMFace *f)
{
  BM_idmap_check_assign(entry->idmap, (BMElem *)f);

  BMID<BMFace> id = entry->get_elem_id<BMFace>(bm, f);

  if (added_faces.contains(id)) {
    return;
  }

  BMLogFace *lf;

  if (BMLogFace **ptr = modified_faces.lookup_ptr(id)) {
    lf = *ptr;
    modified_faces.remove(id);
    entry->update_logface(bm, lf, f);
  }
  else {
    lf = entry->alloc_logface(bm, f);
  }

  added_faces.add(id, lf);
}

ATTR_NO_OPT void BMLogSetDiff::remove_face(BMesh *bm, BMFace *f, bool no_check)
{
  BMID<BMFace> id = entry->get_elem_id<BMFace>(bm, f);

  if (!no_check && (added_faces.remove(id) || removed_faces.contains(id))) {
    return;
  }

  BMLogFace *lf;

  if (BMLogFace **ptr = modified_faces.lookup_ptr(id)) {
    lf = *ptr;
    modified_faces.remove(id);
    entry->update_logface(bm, lf, f);
  }
  else {
    lf = entry->alloc_logface(bm, f);
  }

  removed_faces.add(id, lf);
}

void BMLogSetDiff::modify_face(BMesh *bm, BMFace *f)
{
  BMID<BMFace> id = entry->get_elem_id<BMFace>(bm, f);

  BMLogFace *lf;

  if (BMLogFace **ptr = modified_faces.lookup_ptr(id)) {
    lf = *ptr;
    entry->update_logface(bm, lf, f);
  }
  else {
    lf = entry->alloc_logface(bm, f);
    modified_faces.add(id, lf);
  }
}

ATTR_NO_OPT void BMLogSetDiff::swap_verts(BMesh *bm,
                                          blender::Map<BMID<BMVert>, BMLogVert *> verts,
                                          BMLogCallbacks *callbacks)
{
  void *old_customdata = entry->vdata.pool ? BLI_mempool_alloc(bm->vdata.pool) : nullptr;

  const int cd_id = entry->idmap->cd_id_off[BM_VERT];

  if (old_customdata) {
    CustomData_bmesh_asan_unpoison(&bm->vdata, old_customdata);
  }

  for (BMLogVert *lv : verts.values()) {
    BMVert *v = entry->get_elem_from_id<BMVert>(bm, lv->id);

    if (!v) {
      printf("modified_verts: invalid vertex %d\n", lv->id.id);
      continue;
    }

    if (old_customdata) {
      CustomData_bmesh_asan_unpoison(&bm->vdata, v->head.data);
      memcpy(old_customdata, v->head.data, bm->vdata.totsize);
      CustomData_bmesh_asan_poison(&bm->vdata, v->head.data);
    }

    entry->swap_logvert(bm, lv->id, v, lv);

    /* Ensure id wasn't mangled in customdata swap. */
    BM_ELEM_CD_SET_INT(v, cd_id, lv->id.id);

    if (callbacks) {
      callbacks->on_vert_change(v, callbacks->userdata, old_customdata);
    }
  }

  if (old_customdata) {
    BLI_mempool_free(bm->vdata.pool, old_customdata);
  }
}

ATTR_NO_OPT void BMLogSetDiff::restore_verts(BMesh *bm,
                                             blender::Map<BMID<BMVert>, BMLogVert *> verts,
                                             BMLogCallbacks *callbacks)
{
  for (BMLogVert *lv : verts.values()) {
    BMVert *v = BM_vert_create(bm, lv->co, nullptr, BM_CREATE_NOP);

    v->head.hflag = lv->flag;
    copy_v3_v3(v->no, lv->no);

    CustomData_bmesh_copy_data(&entry->vdata, &bm->vdata, lv->customdata, &v->head.data);
    entry->assign_elem_id<BMVert>(bm, v, lv->id, true);

    if (callbacks && callbacks->on_vert_add) {
      callbacks->on_vert_add(v, callbacks->userdata);
    }
  }

  bm->elem_index_dirty |= BM_VERT | BM_EDGE;
  bm->elem_table_dirty |= BM_VERT | BM_EDGE;
}

ATTR_NO_OPT void BMLogSetDiff::remove_verts(BMesh *bm,
                                            blender::Map<BMID<BMVert>, BMLogVert *> verts,
                                            BMLogCallbacks *callbacks)
{
  for (BMLogVert *lv : verts.values()) {
    BMVert *v = entry->get_elem_from_id(bm, lv->id);

    if (!v) {
      printf("%s: Failed to find vertex %d\b", __func__, lv->id.id);
      continue;
    }

    if (callbacks && callbacks->on_vert_kill) {
      callbacks->on_vert_kill(v, callbacks->userdata);
    }

    BM_idmap_release(entry->idmap, (BMElem *)v, false);
    BM_vert_kill(bm, v);
  }

  bm->elem_index_dirty |= BM_VERT | BM_EDGE;
  bm->elem_table_dirty |= BM_VERT | BM_EDGE;
}

void BMLogSetDiff::restore_edges(BMesh *bm,
                                 blender::Map<BMID<BMEdge>, BMLogEdge *> edges,
                                 BMLogCallbacks *callbacks)
{
  for (BMLogEdge *le : edges.values()) {
    BMVert *v1 = entry->get_elem_from_id<BMVert>(bm, le->v1);
    BMVert *v2 = entry->get_elem_from_id<BMVert>(bm, le->v2);

    if (!v1) {
      printf("%s: missing vertex v1 %d\n", __func__, le->v1.id);
      continue;
    }

    if (!v2) {
      printf("%s: missing vertex v2 %d\n", __func__, le->v2.id);
      continue;
    }

    BMEdge *e = BM_edge_create(bm, v1, v2, nullptr, BM_CREATE_NOP);
    CustomData_bmesh_copy_data(&entry->edata, &bm->edata, le->customdata, &e->head.data);

    entry->assign_elem_id<BMEdge>(bm, e, le->id, true);

    if (callbacks->on_edge_add) {
      callbacks->on_edge_add(e, callbacks->userdata);
    }
  }
}

void BMLogSetDiff::remove_edges(BMesh *bm,
                                blender::Map<BMID<BMEdge>, BMLogEdge *> edges,
                                BMLogCallbacks *callbacks)
{
  for (BMLogEdge *le : edges.values()) {
    BMEdge *e = entry->get_elem_from_id<BMEdge>(bm, le->id);

    if (!e) {
      printf("%s: failed to find edge %d\n", __func__, le->id.id);
      continue;
    }

    if (callbacks && callbacks->on_edge_kill) {
      callbacks->on_edge_kill(e, callbacks->userdata);
    }

    BM_idmap_release(entry->idmap, reinterpret_cast<BMElem *>(e), true);
    BM_edge_kill(bm, e);
  }
}

void BMLogSetDiff::swap_edges(BMesh *bm,
                              blender::Map<BMID<BMEdge>, BMLogEdge *> edges,
                              BMLogCallbacks *callbacks)
{
  void *old_customdata = entry->edata.pool ? BLI_mempool_alloc(bm->edata.pool) : nullptr;

  if (old_customdata) {
    CustomData_bmesh_asan_unpoison(&bm->edata, old_customdata);
  }

  for (BMLogEdge *le : edges.values()) {
    BMEdge *e = entry->get_elem_from_id(bm, le->id);

    if (!e) {
      printf("%s: failed to find edge %d\n", __func__, le->id.id);
      continue;
    }

    if (old_customdata) {
      CustomData_bmesh_asan_unpoison(&bm->edata, e->head.data);
      memcpy(old_customdata, e->head.data, bm->edata.totsize);
      CustomData_bmesh_asan_poison(&bm->edata, e->head.data);
    }

    entry->swap_logedge(bm, le->id, e, le);

    if (callbacks->on_edge_change) {
      callbacks->on_edge_change(e, callbacks->userdata, old_customdata);
    }
  }

  if (old_customdata) {
    BLI_mempool_free(bm->edata.pool, old_customdata);
  }
}

ATTR_NO_OPT void BMLogSetDiff::restore_faces(BMesh *bm,
                                             blender::Map<BMID<BMFace>, BMLogFace *> faces,
                                             BMLogCallbacks *callbacks)
{
  Vector<BMVert *, 16> verts;

  for (BMLogFace *lf : faces.values()) {
    bool ok = true;
    verts.clear();

    for (BMID<BMVert> v_id : lf->verts) {
      BMVert *v = entry->get_elem_from_id<BMVert>(bm, v_id);

      if (!v) {
        printf("%s: Error looking up vertex %d\n", __func__, v_id.id);
        ok = false;
        continue;
      }

      verts.append(v);
    }

    if (!ok) {
      continue;
    }

    BMFace *f = BM_face_create_verts(bm, verts.data(), verts.size(), nullptr, BM_CREATE_NOP, true);

    f->head.hflag = lf->flag;
    CustomData_bmesh_copy_data(&entry->pdata, &bm->pdata, lf->customdata, &f->head.data);
    entry->assign_elem_id<BMFace>(bm, f, lf->id, true);

    BMLoop *l = f->l_first;
    int i = 0;

    if (lf->loop_customdata[0]) {
      do {
        CustomData_bmesh_copy_data(
            &entry->ldata, &bm->ldata, lf->loop_customdata[i], &l->head.data);
        i++;
      } while ((l = l->next) != f->l_first);
    }

    if (callbacks && callbacks->on_face_add) {
      callbacks->on_face_add(f, callbacks->userdata);
    }
  }

  bm->elem_index_dirty |= BM_FACE;
  bm->elem_table_dirty |= BM_FACE;
}

ATTR_NO_OPT void BMLogSetDiff::remove_faces(BMesh *bm,
                                            blender::Map<BMID<BMFace>, BMLogFace *> faces,
                                            BMLogCallbacks *callbacks)
{
  for (BMLogFace *lf : faces.values()) {
    BMFace *f = entry->get_elem_from_id<BMFace>(bm, lf->id);

    if (!f) {
      printf("%s: error finding face %d\n", __func__, lf->id.id);
      continue;
    }

    if (callbacks && callbacks->on_face_kill) {
      callbacks->on_face_kill(f, callbacks->userdata);
    }

    BM_idmap_release(entry->idmap, reinterpret_cast<BMElem *>(f), true);
    BM_face_kill(bm, f);
  }

  bm->elem_index_dirty |= BM_FACE;
  bm->elem_table_dirty |= BM_FACE;
}

ATTR_NO_OPT void BMLogSetDiff::swap_faces(BMesh *bm,
                                          blender::Map<BMID<BMFace>, BMLogFace *> faces,
                                          BMLogCallbacks *callbacks)
{
  void *old_customdata = entry->pdata.pool ? BLI_mempool_alloc(bm->pdata.pool) : nullptr;

  const int cd_id = entry->idmap->cd_id_off[BM_FACE];

  if (old_customdata) {
    CustomData_bmesh_asan_unpoison(&bm->pdata, old_customdata);
  }

  for (BMLogFace *lf : faces.values()) {
    BMFace *f = entry->get_elem_from_id<BMFace>(bm, lf->id);

    if (!f) {
      printf("modified_verts: invalid vertex %d\n", lf->id.id);
      continue;
    }

    if (old_customdata) {
      CustomData_bmesh_asan_unpoison(&bm->pdata, f->head.data);
      memcpy(old_customdata, f->head.data, bm->pdata.totsize);
      CustomData_bmesh_asan_poison(&bm->pdata, f->head.data);
    }

    entry->swap_logface(bm, lf->id, f, lf);

    /* Ensure id wasn't mangled in customdata swap. */
    BM_ELEM_CD_SET_INT(f, cd_id, lf->id.id);

    if (callbacks) {
      callbacks->on_face_change(f, callbacks->userdata, old_customdata);
    }
  }

  if (old_customdata) {
    BLI_mempool_free(bm->pdata.pool, old_customdata);
  }
}

ATTR_NO_OPT void BMLogSetDiff::undo(BMesh *bm, BMLogCallbacks *callbacks)
{
  remove_faces(bm, added_faces, callbacks);
  remove_edges(bm, added_edges, callbacks);
  remove_verts(bm, added_verts, callbacks);

  restore_verts(bm, removed_verts, callbacks);
  restore_edges(bm, removed_edges, callbacks);
  restore_faces(bm, removed_faces, callbacks);

  swap_faces(bm, modified_faces, callbacks);
  swap_verts(bm, modified_verts, callbacks);
}

ATTR_NO_OPT void BMLogSetDiff::redo(BMesh *bm, BMLogCallbacks *callbacks)
{
  remove_faces(bm, removed_faces, callbacks);
  remove_edges(bm, removed_edges, callbacks);
  remove_verts(bm, removed_verts, callbacks);

  restore_verts(bm, added_verts, callbacks);
  restore_edges(bm, added_edges, callbacks);
  restore_faces(bm, added_faces, callbacks);

  swap_faces(bm, modified_faces, callbacks);
  swap_edges(bm, modified_edges, callbacks);
  swap_verts(bm, modified_verts, callbacks);
}
}  // namespace blender

ATTR_NO_OPT BMLog *BM_log_from_existing_entries_create(BMesh *bm,
                                                       BMIdMap *idmap,
                                                       BMLogEntry *entry)
{
  BMLog *log = BM_log_create(bm, idmap);
  log->load_entries(entry);

  return log;
}

ATTR_NO_OPT BMLog *BM_log_create(BMesh *bm, BMIdMap *idmap)
{
  BMLog *log = MEM_new<BMLog>("BMLog", idmap);

  return log;
}

ATTR_NO_OPT void BM_log_set_idmap(BMLog *log, struct BMIdMap *idmap)
{
  log->set_idmap(idmap);
}

/* Free all the data in a BMLog including the log itself
 * safe_mode means log->refcount will be checked, and if nonzero log will not be freed
 */
ATTR_NO_OPT static bool bm_log_free_direct(BMLog *log, bool safe_mode)
{
  if (safe_mode && log->refcount) {
    return false;
  }

  log->dead = true;
  log->free_all_entries();

  return true;
}

// if true, make sure to call BM_log_free on the log
ATTR_NO_OPT bool BM_log_is_dead(BMLog *log)
{
  return log->dead;
}

ATTR_NO_OPT bool BM_log_free(BMLog *log, bool safe_mode)
{
  if (log->dead) {
    MEM_delete<BMLog>(log);
    return true;
  }

  if (bm_log_free_direct(log, safe_mode)) {
    MEM_delete<BMLog>(log);
    return true;
  }

  return false;
}

ATTR_NO_OPT BMLogEntry *BM_log_entry_add_ex(BMesh *bm, BMLog *log, bool combine_with_last)
{
  if (combine_with_last && log->current_entry) {
    log->current_entry->push_set(bm, LOG_SET_DIFF);
  }
  else {
    log->push_entry(bm);
  }

  log->current_entry->push_set(bm, LOG_SET_DIFF);
  return log->current_entry;
}

ATTR_NO_OPT BMLogEntry *BM_log_entry_add(BMesh *bm, BMLog *log)
{
  log->push_entry(bm)->push_set(bm, LOG_SET_DIFF);
  return log->current_entry;
}

void BM_log_vert_added(BMesh *bm, BMLog *log, BMVert *v)
{
  log->add_vert(bm, v);
}

void BM_log_vert_removed(BMesh *bm, BMLog *log, BMVert *v)
{
  log->remove_vert(bm, v);
}

void BM_log_vert_before_modified(BMesh *bm, BMLog *log, BMVert *v)
{
  log->modify_vert(bm, v);
}

BMLogEntry *BM_log_entry_check_customdata(BMesh *bm, BMLog *log)
{
  BMLogEntry *entry = log->current_entry;

  if (!entry) {
    fprintf(stdout, "no current entry; creating...\n");
    fflush(stdout);
    return BM_log_entry_add_ex(bm, log, false);
  }

  CustomData *cd1[4] = {&bm->vdata, &bm->edata, &bm->ldata, &bm->pdata};
  CustomData *cd2[4] = {&entry->vdata, &entry->edata, &entry->ldata, &entry->pdata};

  for (int i = 0; i < 4; i++) {
    if (!CustomData_layout_is_same(cd1[i], cd2[i])) {
      fprintf(stdout, "Customdata changed for undo\n");
      fflush(stdout);
      return BM_log_entry_add_ex(bm, log, false);
    }
  }

  return entry;
}

void BM_log_edge_added(BMesh *bm, BMLog *log, BMEdge *e)
{
  log->add_edge(bm, e);
}
void BM_log_edge_modified(BMesh *bm, BMLog *log, BMEdge *e)
{
  log->modify_edge(bm, e);
}
void BM_log_edge_removed(BMesh *bm, BMLog *log, BMEdge *e)
{
  log->remove_edge(bm, e);
}

ATTR_NO_OPT void BM_log_face_added(BMesh *bm, BMLog *log, BMFace *f)
{
  log->add_face(bm, f);
}
ATTR_NO_OPT void BM_log_face_modified(BMesh *bm, BMLog *log, BMFace *f)
{
  log->modify_face(bm, f);
}
void BM_log_face_removed(BMesh *bm, BMLog *log, BMFace *f)
{
  log->remove_face(bm, f);
}
void BM_log_face_removed_no_check(BMesh *bm, BMLog *log, BMFace *f)
{
  log->remove_face(bm, f, true);
}

void BM_log_full_mesh(BMesh *bm, BMLog *log)
{
  log->full_mesh(bm);
}

BMVert *BM_log_id_vert_get(BMesh *bm, BMLog *log, uint id)
{
  return reinterpret_cast<BMVert *>(BM_idmap_lookup(log->idmap, id));
}

uint BM_log_vert_id_get(BMesh *bm, BMLog *log, BMVert *v)
{
  return BM_idmap_get_id(log->idmap, reinterpret_cast<BMElem *>(v));
}

BMFace *BM_log_id_face_get(BMesh *bm, BMLog *log, uint id)
{
  return reinterpret_cast<BMFace *>(BM_idmap_lookup(log->idmap, id));
}

uint BM_log_face_id_get(BMesh *bm, BMLog *log, BMFace *f)
{
  return BM_idmap_get_id(log->idmap, reinterpret_cast<BMElem *>(f));
}

int BM_log_entry_size(BMLogEntry *entry)
{
  return entry->calc_size();
}

void BM_log_undo(BMesh *bm, BMLog *log, BMLogCallbacks *callbacks)
{
  log->undo(bm, callbacks);
}

void BM_log_redo(BMesh *bm, BMLog *log, BMLogCallbacks *callbacks)
{
  log->redo(bm, callbacks);
}

void BM_log_undo_skip(BMesh *bm, BMLog *log)
{
  log->skip(-1);
}

void BM_log_redo_skip(BMesh *bm, BMLog *log)
{
  log->skip(1);
}

BMLogEntry *BM_log_entry_prev(BMLogEntry *entry)
{
  return entry->prev;
}

BMLogEntry *BM_log_entry_next(BMLogEntry *entry)
{
  return entry->next;
}

void BM_log_set_current_entry(BMLog *log, BMLogEntry *entry)
{
  log->current_entry = entry;
}

bool BM_log_entry_drop(BMLogEntry *entry)
{
  if (entry->prev) {
    entry->prev->next = entry->next;
  }
  if (entry->next) {
    entry->next->prev = entry->prev;
  }
  if (entry->log && entry == entry->log->current_entry) {
    entry->log->current_entry = entry->prev;
  }

  MEM_delete<BMLogEntry>(entry);
  return true;
}

void BM_log_print_entry(BMLog *log, BMLogEntry *entry)
{
}