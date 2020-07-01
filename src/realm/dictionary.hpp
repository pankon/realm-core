/*************************************************************************
 *
 * Copyright 2019 Realm Inc.
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

#ifndef REALM_DICTIONARY_HPP
#define REALM_DICTIONARY_HPP

#include <realm/array.hpp>
#include <realm/obj.hpp>
#include <realm/mixed.hpp>
#include <realm/cluster_tree.hpp>

namespace realm {

class DictionaryClusterTree;

class Dictionary : public ArrayParent {
public:
    class Iterator;

    Dictionary() {}
    ~Dictionary();

    Dictionary(const Obj& obj, ColKey col_key);
    Dictionary(const Dictionary& other)
    {
        *this = other;
    }
    Dictionary& operator=(const Dictionary& other);

    bool is_attached() const
    {
        return m_obj.is_valid();
    }

    size_t size() const;

    void create();

    // first points to inserted/updated element.
    // second is true if the element was inserted
    std::pair<Iterator, bool> insert(Mixed key, Mixed value);
    std::pair<Iterator, bool> insert(Mixed key, const Obj& obj);

    // throws std::out_of_range if key is not found
    Mixed get(Mixed key) const;
    // adds entry if key is not found
    const Mixed operator[](Mixed key);

    Iterator find(Mixed key);

    void erase(Mixed key);
    void erase(Iterator it);

    void nullify(Mixed);

    void clear();

    Iterator begin() const;
    Iterator end() const;

private:
    friend class MixedRef;

    mutable DictionaryClusterTree* m_clusters = nullptr;
    Obj m_obj;
    ColKey m_col_key;
    mutable uint_fast64_t m_content_version = 0;

    void update_content_version() const
    {
        m_content_version = m_obj.get_alloc().get_content_version();
    }

    void update_if_needed() const
    {
        auto content_version = m_obj.get_alloc().get_content_version();
        if (m_obj.update_if_needed() || content_version != m_content_version) {
            init_from_parent();
        }
    }
    void init_from_parent() const;

    void update_child_ref(size_t ndx, ref_type new_ref) override;
    ref_type get_child_ref(size_t ndx) const noexcept override;
};

class Dictionary::Iterator : public ClusterTree::Iterator {
public:
    typedef std::forward_iterator_tag iterator_category;
    typedef std::pair<const Mixed, Mixed> value_type;
    typedef ptrdiff_t difference_type;
    typedef const value_type* pointer;
    typedef const value_type& reference;

    value_type operator*();

private:
    friend class Dictionary;
    using ClusterTree::Iterator::get_position;

    ColumnType m_key_type;

    Iterator(const Dictionary* dict, size_t pos);
};

inline std::pair<Dictionary::Iterator, bool> Dictionary::insert(Mixed key, const Obj& obj)
{
    return insert(key, Mixed(obj.get_link()));
}

} // namespace realm

#endif /* SRC_REALM_DICTIONARY_HPP_ */
