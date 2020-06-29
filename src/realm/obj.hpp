/*************************************************************************
 *
 * Copyright 2016 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#ifndef REALM_OBJ_HPP
#define REALM_OBJ_HPP

#include <realm/array.hpp>
#include <realm/cluster.hpp>
#include <realm/table_ref.hpp>
#include <realm/keys.hpp>
#include <map>

#define REALM_CLUSTER_IF

namespace realm {

class TableClusterTree;
class Replication;
class TableView;
class ConstLstBase;
class LstBase;
struct GlobalKey;

template <class>
class ConstLstIf;

template <class>
class Lst;
template <class T>
using LstPtr = std::unique_ptr<Lst<T>>;
using LstBasePtr = std::unique_ptr<LstBase>;

class LnkLst;
using LnkLstPtr = std::unique_ptr<LnkLst>;

// 'Object' would have been a better name, but it clashes with a class in ObjectStore
class Obj {
public:
    Obj()
        : m_table(nullptr)
        , m_row_ndx(size_t(-1))
        , m_storage_version(-1)
        , m_valid(false)
    {
    }
    Obj(TableRef table, MemRef mem, ObjKey key, size_t row_ndx);

    TableRef get_table()
    {
        return m_table.cast_away_const();
    }

    ConstTableRef get_table() const
    {
        return m_table;
    }

    Allocator& get_alloc() const;

    bool operator==(const Obj& other) const;

    ObjKey get_key() const
    {
        return m_key;
    }

    GlobalKey get_object_id() const;

    Replication* get_replication() const;

    // Check if this object is default constructed
    explicit operator bool() const
    {
        return m_table != nullptr;
    }

    // Check if the object is still alive
    bool is_valid() const;
    // Will throw if object is not valid
    void check_valid() const;
    // Delete object from table. Object is invalid afterwards.
    void remove();
    // Invalidate
    //  - this turns the object into a tombstone if links to the object exist.
    //  - deletes the object is no links to the object exist.
    //  - To be used by the Sync client.
    void invalidate();

    template <typename U>
    U get(ColKey col_key) const;

    Mixed get_any(ColKey col_key) const;

    template <typename U>
    U get(StringData col_name) const
    {
        return get<U>(get_column_key(col_name));
    }
    bool is_unresolved(ColKey col_key) const;
    int cmp(const Obj& other, ColKey col_key) const;

    size_t get_link_count(ColKey col_key) const;

    bool is_null(ColKey col_key) const;
    bool is_null(StringData col_name) const
    {
        return is_null(get_column_key(col_name));
    }
    bool has_backlinks(bool only_strong_links) const;
    size_t get_backlink_count() const;
    size_t get_backlink_count(const Table& origin, ColKey origin_col_key) const;
    ObjKey get_backlink(const Table& origin, ColKey origin_col_key, size_t backlink_ndx) const;
    TableView get_backlink_view(TableRef src_table, ColKey src_col_key);

    // To be used by the query system when a single object should
    // be tested. Will allow a function to be called in the context
    // of the owning cluster.
    template <class T>
    bool evaluate(T func) const;

    void to_json(std::ostream& out, size_t link_depth, std::map<std::string, std::string>& renames,
                 std::vector<ColKey>& followed) const;
    void to_json(std::ostream& out, size_t link_depth = 0,
                 std::map<std::string, std::string>* renames = nullptr) const
    {
        std::map<std::string, std::string> renames2;
        renames = renames ? renames : &renames2;

        std::vector<ColKey> followed;
        to_json(out, link_depth, *renames, followed);
    }

    std::string to_string() const;

    // Get the path in a minimal format without including object accessors.
    // If you need to obtain additional information for each object in the path,
    // you should use get_fat_path() or traverse_path() instead (see below).
    struct PathElement;
    struct Path {
        TableKey top_table;
        ObjKey top_objkey;
        std::vector<PathElement> path_from_top;
    };
    Path get_path() const;

    // Get the fat path to this object expressed as a vector of fat path elements.
    // each Fat path elements include a Obj allowing for low cost access to the
    // objects data.
    // For a top-level object, the returned vector will be empty.
    // For an embedded object, the vector has the top object as first element,
    // and the embedded object itself is not included in the path.
    struct FatPathElement;
    using FatPath = std::vector<FatPathElement>;
    FatPath get_fat_path() const;

    // For an embedded object, traverse the path leading to this object.
    // The PathSizer is called first to set the size of the path
    // Then there is one call for each object on that path, starting with the top level object
    // The embedded object itself is not considered part of the path.
    // Note: You should never provide the path_index for calls to traverse_path.
    using Visitor = std::function<void(const Obj&, ColKey, size_t)>;
    using PathSizer = std::function<void(size_t)>;
    void traverse_path(Visitor v, PathSizer ps, size_t path_index = 0) const;

    template <typename U>
    Obj& set(ColKey col_key, U value, bool is_default = false);
    // Create a new object and link it. If an embedded object
    // is already set, it will be removed. If a non-embedded
    // object is already set, we throw LogicError (to prevent
    // dangling objects, since they do not delete automatically
    // if they are not embedded...)
    Obj create_and_set_linked_object(ColKey col_key, bool is_default = false);
    // Clear all fields of a linked object returning it to its
    // default state. If the object does not exist, create a
    // new object and link it. (To Be Implemented)
    Obj clear_linked_object(ColKey col_key);
    Obj& set(ColKey col_key, Mixed value);

    template <typename U>
    Obj& set(StringData col_name, U value, bool is_default = false)
    {
        return set(get_column_key(col_name), value, is_default);
    }

    Obj& set_null(ColKey col_key, bool is_default = false);
    Obj& set_null(StringData col_name, bool is_default = false)
    {
        return set_null(get_column_key(col_name), is_default);
    }

    Obj& add_int(ColKey col_key, int64_t value);
    Obj& add_int(StringData col_name, int64_t value)
    {
        return add_int(get_column_key(col_name), value);
    }

    template <typename U>
    Obj& set_list_values(ColKey col_key, const std::vector<U>& values);

    template <typename U>
    std::vector<U> get_list_values(ColKey col_key) const;

    template <class Head, class... Tail>
    Obj& set_all(Head v, Tail... tail);

    void assign(const Obj& other);

    Obj get_linked_object(ColKey link_col_key) const;

    template <typename U>
    Lst<U> get_list(ColKey col_key) const;
    template <typename U>
    LstPtr<U> get_list_ptr(ColKey col_key) const;

    template <typename U>
    Lst<U> get_list(StringData col_name) const
    {
        return get_list<U>(get_column_key(col_name));
    }

    LnkLst get_linklist(ColKey col_key) const;
    LnkLstPtr get_linklist_ptr(ColKey col_key) const;
    LnkLst get_linklist(StringData col_name) const;

    LstBasePtr get_listbase_ptr(ColKey col_key) const;
    void assign_pk_and_backlinks(const Obj& other);

private:
    friend class ArrayBacklink;
    friend class CascadeState;
    friend class Cluster;
    friend class ColumnListBase;
    friend class ConstLstBase;
    friend class ConstTableView;
    template <class>
    friend class Lst;
    friend class LnkLst;
    friend class LinkMap;
    friend class Table;
    friend class Transaction;
    friend struct ClusterNode::IteratorState;

    mutable TableRef m_table;
    ObjKey m_key;
    mutable MemRef m_mem;
    mutable size_t m_row_ndx;
    mutable uint64_t m_storage_version;
    mutable bool m_valid;

    Allocator& _get_alloc() const;
    bool update() const;
    // update if needed - with and without check of table instance version:
    bool update_if_needed() const;
    bool _update_if_needed() const; // no check, use only when already checked
    template <class T>
    bool do_is_null(ColKey::Idx col_ndx) const;

    const TableClusterTree* get_tree_top() const;
    ColKey get_column_key(StringData col_name) const;
    TableKey get_table_key() const;
    TableRef get_target_table(ColKey col_key) const;
    TableRef get_target_table(ObjLink link) const;
    const Spec& get_spec() const;

    template <typename U>
    U _get(ColKey::Idx col_ndx) const;

    template <class T>
    int cmp(const Obj& other, ColKey::Idx col_ndx) const;
    int cmp(const Obj& other, ColKey::Idx col_ndx) const;
    ObjKey get_backlink(ColKey backlink_col, size_t backlink_ndx) const;
    // Return all backlinks from a specific backlink column
    std::vector<ObjKey> get_all_backlinks(ColKey backlink_col) const;
    // Return number of backlinks from a specific backlink column
    size_t get_backlink_cnt(ColKey backlink_col) const;
    ObjKey get_unfiltered_link(ColKey col_key) const;

    template <class Val>
    Obj& _set(size_t col_ndx, Val v);
    template <class Head, class... Tail>
    Obj& _set(size_t col_ndx, Head v, Tail... tail);
    ColKey spec_ndx2colkey(size_t col_ndx);
    bool ensure_writeable();
    void bump_content_version();
    void bump_both_versions();
    template <class T>
    void do_set_null(ColKey col_key);

    void set_int(ColKey col_key, int64_t value);
    void add_backlink(ColKey backlink_col, ObjKey origin_key);
    bool remove_one_backlink(ColKey backlink_col, ObjKey origin_key);
    void nullify_link(ColKey origin_col, ObjLink target_key);
    // Used when inserting a new link. You will not remove existing links in this process
    void set_backlink(ColKey col_key, ObjLink new_link);
    // Used when replacing a link, return true if CascadeState contains objects to remove
    bool replace_backlink(ColKey col_key, ObjLink old_link, ObjLink new_link, CascadeState& state);
    // Used when removing a backlink, return true if CascadeState contains objects to remove
    bool remove_backlink(ColKey col_key, ObjLink old_link, CascadeState& state);
    template <class T>
    inline void set_spec(T&, ColKey);
};

struct Obj::FatPathElement {
    Obj obj;        // Object which embeds...
    ColKey col_key; // Column holding link or link list which embeds...
    size_t index;   // index into link list (or 0)
};

struct Obj::PathElement {
    ColKey col_key; // Column holding link or link list which embeds...
    size_t index;   // index into link list (or 0)
};

template <>
Obj& Obj::set(ColKey, int64_t value, bool is_default);

template <>
Obj& Obj::set(ColKey, ObjKey value, bool is_default);


template <>
inline Obj& Obj::set(ColKey col_key, int value, bool is_default)
{
    return set(col_key, int_fast64_t(value), is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, uint_fast64_t value, bool is_default)
{
    int_fast64_t value_2 = 0;
    if (REALM_UNLIKELY(util::int_cast_with_overflow_detect(value, value_2))) {
        REALM_TERMINATE("Unsigned integer too big.");
    }
    return set(col_key, value_2, is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, const char* str, bool is_default)
{
    return set(col_key, StringData(str), is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, char* str, bool is_default)
{
    return set(col_key, StringData(str), is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, std::string str, bool is_default)
{
    return set(col_key, StringData(str), is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, realm::null, bool is_default)
{
    return set_null(col_key, is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, util::Optional<bool> value, bool is_default)
{
    return value ? set(col_key, *value, is_default) : set_null(col_key, is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, util::Optional<int64_t> value, bool is_default)
{
    return value ? set(col_key, *value, is_default) : set_null(col_key, is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, util::Optional<float> value, bool is_default)
{
    return value ? set(col_key, *value, is_default) : set_null(col_key, is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, util::Optional<double> value, bool is_default)
{
    return value ? set(col_key, *value, is_default) : set_null(col_key, is_default);
}

template <>
inline Obj& Obj::set(ColKey col_key, util::Optional<ObjectId> value, bool is_default)
{
    return value ? set(col_key, *value, is_default) : set_null(col_key, is_default);
}

template <typename U>
Obj& Obj::set_list_values(ColKey col_key, const std::vector<U>& values)
{
    size_t sz = values.size();
    auto list = get_list<U>(col_key);
    list.resize(sz);
    for (size_t i = 0; i < sz; i++)
        list.set(i, values[i]);

    return *this;
}

template <typename U>
std::vector<U> Obj::get_list_values(ColKey col_key) const
{
    std::vector<U> values;
    auto list = get_list<U>(col_key);
    for (auto v : list)
        values.push_back(v);

    return values;
}

template <class Val>
inline Obj& Obj::_set(size_t col_ndx, Val v)
{
    return set(spec_ndx2colkey(col_ndx), v);
}

template <class Head, class... Tail>
inline Obj& Obj::_set(size_t col_ndx, Head v, Tail... tail)
{
    set(spec_ndx2colkey(col_ndx), v);
    return _set(col_ndx + 1, tail...);
}

template <class Head, class... Tail>
inline Obj& Obj::set_all(Head v, Tail... tail)
{
    return _set(0, v, tail...);
}
}

#endif // REALM_OBJ_HPP
