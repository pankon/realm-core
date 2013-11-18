/*************************************************************************
 *
 * TIGHTDB CONFIDENTIAL
 * __________________
 *
 *  [2011] - [2012] TightDB Inc
 *  All Rights Reserved.
 *
 * NOTICE:  All information contained herein is, and remains
 * the property of TightDB Incorporated and its suppliers,
 * if any.  The intellectual and technical concepts contained
 * herein are proprietary to TightDB Incorporated
 * and its suppliers and may be covered by U.S. and Foreign Patents,
 * patents in process, and are protected by trade secret or copyright law.
 * Dissemination of this information or reproduction of this material
 * is strictly forbidden unless prior written permission is obtained
 * from TightDB Incorporated.
 *
 **************************************************************************/

/*
A query consists of node objects, one for each query condition. Each node contains pointers to all other nodes:

node1        node2         node3
------       -----         -----
node2*       node1*        node1*
node3*       node3*        node2*

The construction of all this takes part in query.cpp. Each node has two important functions:

    aggregate(start, end)
    aggregate_local(start, end)

The aggregate() function executes the aggregate of a query. You can call the method on any of the nodes
(except children nodes of OrNode and SubtableNode) - it has the same behaviour. The function contains
scheduling that calls aggregate_local(start, end) on different nodes with different start/end ranges,
depending on what it finds is most optimal.

The aggregate_local() function contains a tight loop that tests the condition of its own node, and upon match
it tests all other conditions at that index to report a full match or not. It will remain in the tight loop
after a full match.

So a call stack with 2 and 9 being local matches of a node could look like this:

aggregate(0, 10)
    node1->aggregate_local(0, 3)
        node2->find_first_local(2, 3)
        node3->find_first_local(2, 3)
    node3->aggregate_local(3, 10)
        node1->find_first_local(4, 5)
        node2->find_first_local(4, 5)
        node1->find_first_local(7, 8)
        node2->find_first_local(7, 8)

find_first_local(n, n + 1) is a function that can be used to test a single row of another condition. Note that
this is very simplified. There are other statistical arguments to the methods, and also, find_first_local() can be
called from a callback function called by an integer Array.


Template arguments in methods:
----------------------------------------------------------------------------------------------------

TConditionFunction: Each node has a condition from query_conditions.c such as Equal, GreaterEqual, etc

TConditionValue:    Type of values in condition column. That is, int64_t, float, int, bool, etc

TAction:            What to do with each search result, from the enums act_ReturnFirst, act_Count, act_Sum, etc

TResult:            Type of result of actions - float, double, int64_t, etc. Special notes: For act_Count it's
                    int64_t, for TDB_FIND_ALL it's int64_t which points at destination array.

TSourceColumn:      Type of source column used in actions, or *ignored* if no source column is used (like for
                    act_Count, act_ReturnFirst)


There are two important classes used in queries:
----------------------------------------------------------------------------------------------------
SequentialGetter    Column iterator used to get successive values with leaf caching. Used both for condition columns
                    and aggregate source column

AggregateState      State of the aggregate - contains a state variable that stores intermediate sum, max, min,
                    etc, etc.

*/

#ifndef TIGHTDB_QUERY_ENGINE_HPP
#define TIGHTDB_QUERY_ENGINE_HPP

#include <string>
#include <functional>
#include <algorithm>

#include <tightdb/meta.hpp>
#include <tightdb/table.hpp>
#include <tightdb/table_view.hpp>
#include <tightdb/column_fwd.hpp>
#include <tightdb/column_string.hpp>
#include <tightdb/column_string_enum.hpp>
#include <tightdb/column_binary.hpp>
#include <tightdb/column_basic.hpp>
#include <tightdb/utf8.hpp>
#include <tightdb/query_conditions.hpp>
#include <tightdb/array_basic.hpp>
#include <tightdb/array_string.hpp>


namespace tightdb {

// Number of matches to find in best condition loop before breaking out to probe other conditions. Too low value gives too many
// constant time overheads everywhere in the query engine. Too high value makes it adapt less rapidly to changes in match
// frequencies.
const size_t findlocals = 64;

// Average match distance in linear searches where further increase in distance no longer increases query speed (because time
// spent on handling each match becomes insignificant compared to time spent on the search).
const size_t bestdist = 512;

// Minimum number of matches required in a certain condition before it can be used to compute statistics. Too high value can spent
// too much time in a bad node (with high match frequency). Too low value gives inaccurate statistics.
const size_t probe_matches = 4;

const size_t bitwidth_time_unit = 64;

typedef bool (*CallbackDummy)(int64_t);

template<class T> struct ColumnTypeTraits;

template<> struct ColumnTypeTraits<int64_t> {
    const static ColumnType ct_id = col_type_Int;
    const static ColumnType ct_id_real = col_type_Int;
    typedef Column column_type;
    typedef Array array_type;
    typedef int64_t sum_type;
    static const DataType id = type_Int;
};
template<> struct ColumnTypeTraits<bool> {
    const static ColumnType ct_id = col_type_Bool;
    const static ColumnType ct_id_real = col_type_Bool;
    typedef Column column_type;
    typedef Array array_type;
    typedef int64_t sum_type;
    static const DataType id = type_Bool;
};
template<> struct ColumnTypeTraits<float> {
    const static ColumnType ct_id = col_type_Float;
    const static ColumnType ct_id_real = col_type_Float;
    typedef ColumnFloat column_type;
    typedef ArrayFloat array_type;
    typedef double sum_type;
    static const DataType id = type_Float;
};
template<> struct ColumnTypeTraits<double> {
    const static ColumnType ct_id = col_type_Double;
    const static ColumnType ct_id_real = col_type_Double;
    typedef ColumnDouble column_type;
    typedef ArrayDouble array_type;
    typedef double sum_type;
    static const DataType id = type_Double;
};
template<> struct ColumnTypeTraits<DateTime> {
    const static ColumnType ct_id = col_type_DateTime;
    const static ColumnType ct_id_real = col_type_Int;
    typedef Column column_type;
    typedef Array array_type;
    typedef int64_t sum_type;
    static const DataType id = type_Int;
};

template<> struct ColumnTypeTraits<StringData> {
    typedef Column column_type;
    typedef Array array_type;
    typedef int64_t sum_type;
    static const DataType id = type_String;
};

// Only purpose is to return 'double' if and only if source column (T) is float and you're doing a sum (A)
template<class T, Action A> struct ColumnTypeTraitsSum {
    typedef T sum_type;
};

template<> struct ColumnTypeTraitsSum<float, act_Sum> {
    typedef double sum_type;
};


struct SequentialGetterBase {
    virtual ~SequentialGetterBase() TIGHTDB_NOEXCEPT {}
};

template<class T>class SequentialGetter : public SequentialGetterBase {
public:
    typedef typename ColumnTypeTraits<T>::column_type ColType;
    typedef typename ColumnTypeTraits<T>::array_type ArrayType;

    SequentialGetter(): m_array((Array::no_prealloc_tag())) {}

    SequentialGetter(const Table& table, size_t column_ndx): m_array((Array::no_prealloc_tag()))
    {
        if (column_ndx != not_found)
            m_column = static_cast<const ColType*>(&table.get_column_base(column_ndx));
        m_leaf_end = 0;
    }

    SequentialGetter(const ColType* column): m_array((Array::no_prealloc_tag()))
    {
        init(column);
    }

    ~SequentialGetter() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {}

    void init(const ColType* column)
    {
        m_column = column;
        m_leaf_end = 0;
    }

    TIGHTDB_FORCEINLINE bool cache_next(size_t index)
    {
        // Return wether or not leaf array has changed (could be useful to know for caller)
        if (index >= m_leaf_end || index < m_leaf_start) {
            // GetBlock() does following: If m_column contains only a leaf, then just return pointer to that leaf and
            // leave m_array untouched. Else call init_from_header() on m_array (more time consuming) and return pointer to m_array.
            m_array_ptr = static_cast<const ArrayType*>(m_column->GetBlock(index, m_array, m_leaf_start, true));
            const size_t leaf_size = m_array_ptr->size();
            m_leaf_end = m_leaf_start + leaf_size;
            return true;
        }
        return false;
    }


    TIGHTDB_FORCEINLINE T get_next(size_t index)
    {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4800)   // Disable the Microsoft warning about bool performance issue.
#endif

        cache_next(index);
        T av = m_array_ptr->get(index - m_leaf_start);
        return av;

#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }

    size_t local_end(size_t global_end)
    {
        if (global_end > m_leaf_end)
            return m_leaf_end - m_leaf_start;
        else
            return global_end - m_leaf_start;
    }

    size_t m_leaf_start;
    size_t m_leaf_end;
    const ColType* m_column;

    // See reason for having both a pointer and instance above
    const ArrayType* m_array_ptr;
private:
    // Never access through m_array because it's uninitialized if column is just a leaf
    ArrayType m_array;
};


class ParentNode {
public:
    ParentNode(): m_is_integer_node(false), m_table(0) {}

    void gather_children(std::vector<ParentNode*>& v)
    {
        m_children.clear();
        ParentNode* p = this;
        size_t i = v.size();
        v.push_back(this);
        p = p->child_criteria();

        if (p)
            p->gather_children(v);

        m_children = v;
        m_children.erase(m_children.begin() + i);
        m_children.insert(m_children.begin(), this);

        m_conds = m_children.size();
    }

    struct score_compare {
        bool operator ()(const ParentNode* a, const ParentNode* b) const { return a->cost() < b->cost(); }
    };

    double cost() const
    {
        return 8 * bitwidth_time_unit / m_dD + m_dT; // dt = 1/64 to 1. Match dist is 8 times more important than bitwidth
    }

    size_t find_first(size_t start, size_t end)
    {
        size_t m = start;
        size_t next_cond = 0;
        size_t first_cond = 0;

        while (start < end) {
            m = m_children[next_cond]->find_first_local(start, end);

            next_cond++;
            if (next_cond == m_conds)
                next_cond = 0;

            if (m == start) {
                if (next_cond == first_cond)
                    return m;
            }
            else {
                first_cond = next_cond;
                start = m;
            }
        }
        return not_found;
    }


    virtual ~ParentNode() TIGHTDB_NOEXCEPT {}

    virtual void init(const Table& table)
    {
        m_table = &table;
        if (m_child)
            m_child->init(table);
    }

    virtual bool is_initialized() const
    {
        return m_table != NULL;
    }

    virtual size_t find_first_local(size_t start, size_t end) = 0;

    virtual ParentNode* child_criteria()
    {
        return m_child;
    }

    // Only purpose is to make all IntegerNode classes have this function (overloaded only in IntegerNode)
    virtual size_t aggregate_call_specialized(Action /*TAction*/, DataType /*TResult*/,
                                              QueryStateBase* /*st*/,
                                              size_t /*start*/, size_t /*end*/, size_t /*local_limit*/,
                                              SequentialGetterBase* /*source_column*/, size_t* /*matchcount*/)
    {
        TIGHTDB_ASSERT(false);
        return 0;
    }

    template<Action TAction, class TResult, class TSourceColumn>
    size_t aggregate_local_selector(ParentNode* node, QueryState<TResult>* st, size_t start, size_t end, size_t local_limit,
                                    SequentialGetter<TSourceColumn>* source_column, size_t* matchcount)
    {
        size_t r;

        if (node->m_is_integer_node)
            // call method in IntegerNode
            r = node->aggregate_call_specialized(TAction, ColumnTypeTraits<TSourceColumn>::id,(QueryStateBase*)st,
                                                 start, end, local_limit, source_column, matchcount);
        else
             // call method in ParentNode
            r = node->aggregate_local<TAction, TResult, TSourceColumn>(st, start, end, local_limit, source_column, matchcount);
        return r;
    }


    template<Action TAction, class TResult, class TSourceColumn>
    TResult aggregate(QueryState<TResult>* st, size_t start, size_t end, size_t agg_col, size_t* matchcount)
    {
        if (end == not_found)
            end = m_table->size();

        SequentialGetter<TSourceColumn>* source_column = NULL;

        if (agg_col != not_found)
            source_column = new SequentialGetter<TSourceColumn>(*m_table, agg_col);

        size_t td;

        while (start < end) {
            size_t best = std::distance(m_children.begin(), std::min_element(m_children.begin(), m_children.end(), score_compare()));

            // Find a large amount of local matches in best condition
            td = m_children[best]->m_dT == 0.0 ? end : (start + 1000 > end ? end : start + 1000);

            start = aggregate_local_selector<TAction, TResult, TSourceColumn>(m_children[best], st, start, td, findlocals, source_column, matchcount);

            // Make remaining conditions compute their m_dD (statistics)
            for (size_t c = 0; c < m_children.size() && start < end; c++) {
                if (c == best)
                    continue;

                // Skip test if there is no way its cost can ever be better than best node's
                double cost = m_children[c]->cost();
                if (m_children[c]->m_dT < cost) {

                    // Limit to bestdist in order not to skip too large parts of index nodes
                    size_t maxD = m_children[c]->m_dT == 0.0 ? end - start : bestdist;
                    td = m_children[c]->m_dT == 0.0 ? end : (start + maxD > end ? end : start + maxD);
                    start = aggregate_local_selector<TAction, TResult, TSourceColumn>(m_children[c], st, start, td, probe_matches, source_column, matchcount);
                }
            }
        }

        if (matchcount)
            *matchcount = st->m_match_count;
        delete source_column;

        return st->m_state;

    }

    template<Action TAction, class TResult, class TSourceColumn>
    size_t aggregate_local(QueryStateBase* st, size_t start, size_t end, size_t local_limit,
                           SequentialGetterBase* source_column, size_t* matchcount)
    {
        // aggregate called on non-integer column type. Speed of this function is not as critical as speed of the
        // integer version, because find_first_local() is relatively slower here (because it's non-integers).
        //
        // Todo: Two speedups are possible. Simple: Initially test if there are no sub criterias and run find_first_local()
        // in a tight loop if so (instead of testing if there are sub criterias after each match). Harder: Specialize
        // data type array to make array call match() directly on each match, like for integers.

        static_cast<void>(matchcount);
        size_t local_matches = 0;

        size_t r = start - 1;
        for (;;) {
            if (local_matches == local_limit) {
                m_dD = double(r - start) / (local_matches + 1.1);
                return r + 1;
            }

            // Find first match in this condition node
            r = find_first_local(r + 1, end);
            if (r == not_found) {
                m_dD = double(r - start) / (local_matches + 1.1);
                return end;
            }

            local_matches++;

            // Find first match in remaining condition nodes
            size_t m = r;

            for (size_t c = 1; c < m_conds; c++) {
                m = m_children[c]->find_first_local(r, r + 1);
                if (m != r) {
                    break;
                }
            }

            TIGHTDB_STATIC_ASSERT( !(TAction == act_Sum && (SameType<TSourceColumn, float>::value && !SameType<TResult, double>::value)), "");

            // If index of first match in this node equals index of first match in all remaining nodes, we have a final match
            if (m == r) {
                TSourceColumn av = (TSourceColumn)0;
                if (static_cast<QueryState<TResult>*>(st)->template uses_val<TAction>() && source_column != NULL) {
                    TIGHTDB_ASSERT(dynamic_cast<SequentialGetter<TSourceColumn>*>(source_column) != NULL);
                    av = static_cast<SequentialGetter<TSourceColumn>*>(source_column)->get_next(r);
                }
                TIGHTDB_ASSERT(dynamic_cast<QueryState<TResult>*>(st) != NULL);
                bool cont = static_cast<QueryState<TResult>*>(st)->template match<TAction, 0>(r, 0, TResult(av));
                if(!cont)
                    return static_cast<size_t>(-1);
             }
        }
    }


    virtual std::string validate()
    {
        if (error_code != "")
            return error_code;
        if (m_child == 0)
            return "";
        else
            return m_child->validate();
    }

    ParentNode* m_child;
    std::vector<ParentNode*>m_children;

    size_t m_condition_column_idx; // Column of search criteria
    bool m_is_integer_node; // true for IntegerNode, false for any other

    size_t m_conds;
    double m_dD; // Average row distance between each local match at current position
    double m_dT; // Time overhead of testing index i + 1 if we have just tested index i. > 1 for linear scans, 0 for index/tableview

    size_t m_probes;
    size_t m_matches;


protected:
    const Table* m_table;
    std::string error_code;

    const ColumnBase& get_column_base(const Table& table, std::size_t ndx)
    {
        return table.get_column_base(ndx);
    }

    ColumnType get_real_column_type(const Table& table, std::size_t ndx)
    {
        return table.get_real_column_type(ndx);
    }
};


class ArrayNode: public ParentNode {
public:
    ArrayNode(const Array& arr) : m_arr(arr), m_max(0), m_next(0), m_size(arr.size()) {m_child = 0; m_dT = 0.0;}
    ~ArrayNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {}

    void init(const Table& table)
    {
        m_table = &table;

        m_dD =  m_table->size() / (m_arr.size() + 1.0);
        m_probes = 0;
        m_matches = 0;

        m_next = 0;
        if (m_size > 0)
            m_max = to_size_t(m_arr.get(m_size-1));
        if (m_child) m_child->init(table);
    }

    size_t find_first_local(size_t start, size_t end)
    {
        size_t r = m_arr.FindGTE(start, m_next);
        if (r >= end)
            return not_found;

        m_next = r;
        return to_size_t(m_arr.get(r));
    }

protected:
    const Array& m_arr;
    size_t m_max;
    size_t m_next;
    size_t m_size;
};


class SubtableNode: public ParentNode {
public:
    SubtableNode(size_t column): m_column(column) {m_child = 0; m_child2 = 0; m_dT = 100.0;}
    SubtableNode() {};
    ~SubtableNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {}
    void init(const Table& table)
    {
        m_dD = 10.0;
        m_probes = 0;
        m_matches = 0;

        m_table = &table;

        if (m_child) {
            m_child->init(table);
            std::vector<ParentNode*> v;
            m_child->gather_children(v);
        }

        if (m_child2)
            m_child2->init(table);
    }

    size_t find_first_local(size_t start, size_t end)
    {
        TIGHTDB_ASSERT(m_table);
        TIGHTDB_ASSERT(m_child);

        for (size_t s = start; s < end; ++s) {
            const TableRef subtable = ((Table*)m_table)->get_subtable(m_column, s);

            m_child->init(*subtable);
            const size_t subsize = subtable->size();
            const size_t sub = m_child->find_first(0, subsize);

            if (sub != not_found)
                return s;
        }
        return not_found;
    }

    ParentNode* child_criteria()
    {
        return m_child2;
    }

    ParentNode* m_child2;
    size_t m_column;
};

// IntegerNode is for conditions for types stored as integers in a tightdb::Array (int, date, bool)
template <class TConditionValue, class TConditionFunction> class IntegerNode: public ParentNode {
public:
    typedef typename ColumnTypeTraits<TConditionValue>::column_type ColType;

    // NOTE: Be careful to call Array(no_prealloc_tag) constructors on m_array in the initializer list, otherwise
    // their default constructors are called which are slow
    IntegerNode(TConditionValue v, size_t column) : m_value(v), m_array(Array::no_prealloc_tag())
    {
        m_is_integer_node = true;
        m_condition_column_idx = column;
        m_child = 0;
        m_conds = 0;
        m_dT = 1.0 / 4.0;
        m_probes = 0;
        m_matches = 0;
    }
    ~IntegerNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {}

    void init(const Table& table)
    {
        m_dD = 100.0;
        m_condition_column = static_cast<const ColType*>(&get_column_base(table, m_condition_column_idx));
        m_table = &table;
        m_leaf_end = 0;
        if (m_child)
            m_child->init(table);
    }

    // This function is called from Array::find() for each search result if TAction == act_CallbackIdx
    // in the IntegerNode::aggregate_local() call. Used if aggregate source column is different from search criteria column
    // Return value: false means that the query-state (which consumes matches) has signalled to stop searching, perhaps
    template <Action TAction, class TSourceColumn> bool match_callback(int64_t v)
    {
        size_t i = to_size_t(v);
        m_last_local_match = i;
        m_local_matches++;

        typedef typename ColumnTypeTraitsSum<TSourceColumn, TAction>::sum_type QueryStateType;
        QueryState<QueryStateType>* state = static_cast<QueryState<QueryStateType>*>(m_state);
        SequentialGetter<TSourceColumn>* source_column = static_cast<SequentialGetter<TSourceColumn>*>(m_source_column);

        // Test remaining sub conditions of this node. m_children[0] is the node that called match_callback(), so skip it
        for (size_t c = 1; c < m_conds; c++) {
            m_children[c]->m_probes++;
            size_t m = m_children[c]->find_first_local(i, i + 1);
            if (m != i)
                return true;
        }

        bool b;
        if (state->template uses_val<TAction>())    { // Compiler cannot see that Column::Get has no side effect and result is discarded
            TSourceColumn av = source_column->get_next(i);
            b = state->template match<TAction, false>(i, 0, av);
        }
        else {
            b = state->template match<TAction, false>(i, 0, TSourceColumn(0));
        }

        return b;
    }

    size_t aggregate_call_specialized(Action TAction, DataType col_id, QueryStateBase* st,
                                      size_t start, size_t end, size_t local_limit,
                                      SequentialGetterBase* source_column, size_t* matchcount)
    {
        size_t ret;

        if (TAction == act_ReturnFirst)
            ret = aggregate_local<act_ReturnFirst, int64_t, void>(st, start, end, local_limit, source_column, matchcount);

        else if (TAction == act_Count)
            ret = aggregate_local<act_Count, int64_t, void>(st, start, end, local_limit, source_column, matchcount);

        else if (TAction == act_Sum && col_id == type_Int)
            ret = aggregate_local<act_Sum, int64_t, void>(st, start, end, local_limit, source_column, matchcount);
        else if (TAction == act_Sum && col_id == type_Float)
            ret = aggregate_local<act_Sum, float, void>(st, start, end, local_limit, source_column, matchcount);
        else if (TAction == act_Sum && col_id == type_Double)
            ret = aggregate_local<act_Sum, double, void>(st, start, end, local_limit, source_column, matchcount);

        else if (TAction == act_Max && col_id == type_Int)
            ret = aggregate_local<act_Max, int64_t, void>(st, start, end, local_limit, source_column, matchcount);
        else if (TAction == act_Max && col_id == type_Float)
            ret = aggregate_local<act_Max, float, void>(st, start, end, local_limit, source_column, matchcount);
        else if (TAction == act_Max && col_id == type_Double)
            ret = aggregate_local<act_Max, double, void>(st, start, end, local_limit, source_column, matchcount);

        else if (TAction == act_Min && col_id == type_Int)
            ret = aggregate_local<act_Min, int64_t, void>(st, start, end, local_limit, source_column, matchcount);
        else if (TAction == act_Min && col_id == type_Float)
            ret = aggregate_local<act_Min, float, void>(st, start, end, local_limit, source_column, matchcount);
        else if (TAction == act_Min && col_id == type_Double)
            ret = aggregate_local<act_Min, double, void>(st, start, end, local_limit, source_column, matchcount);

        else if (TAction == act_FindAll)
            ret = aggregate_local<act_FindAll, int64_t, void>(st, start, end, local_limit, source_column, matchcount);

        else if (TAction == act_CallbackIdx)
            ret = aggregate_local<act_CallbackIdx, int64_t, void>(st, start, end, local_limit, source_column, matchcount);

        else {
            TIGHTDB_ASSERT(false);
            return 0;
        }
        return ret;
    }


    // source_column: column number in m_table which must act as source for aggreate TAction
    template <Action TAction, class TSourceColumn, class unused>
    size_t aggregate_local(QueryStateBase* st, size_t start, size_t end, size_t local_limit,
                           SequentialGetterBase* source_column, size_t* matchcount)
    {
        typedef typename ColumnTypeTraitsSum<TSourceColumn, TAction>::sum_type QueryStateType;
        TIGHTDB_ASSERT(source_column == NULL || dynamic_cast<SequentialGetter<TSourceColumn>*>(source_column) != NULL);
        TIGHTDB_ASSERT(dynamic_cast<QueryState<QueryStateType>*>(st) != NULL);
        TIGHTDB_ASSERT(m_conds > 0);

        int c = TConditionFunction::condition;
        m_local_matches = 0;
        m_local_limit = local_limit;
        m_last_local_match = start - 1;
        m_state = st;

        for (size_t s = start; s < end; ) {
            // Cache internal leafs
            if (s >= m_leaf_end) {
                m_condition_column->GetBlock(s, m_array, m_leaf_start);
                m_leaf_end = m_leaf_start + m_array.size();
                size_t w = m_array.get_width();
                m_dT = (w == 0 ? 1.0 / TIGHTDB_MAX_LIST_SIZE : w / float(bitwidth_time_unit));
            }

            size_t end2;
            if (end > m_leaf_end)
                end2 = m_leaf_end - m_leaf_start;
            else
                end2 = end - m_leaf_start;

            // If there are no other nodes than us (m_conds == 1) AND the column used for our condition is
            // the same as the column used for the aggregate action, then the entire query can run within scope of that 
            // column only, with no references to other columns:
            if (m_conds == 1 && (source_column == NULL ||
                (SameType<TSourceColumn, int64_t>::value
                 && static_cast<SequentialGetter<int64_t>*>(source_column)->m_column == m_condition_column))) {
                bool cont = m_array.find(c, TAction, m_value, s - m_leaf_start, end2, m_leaf_start, (QueryState<int64_t>*)st);
                if(!cont)
                    return not_found;
            }
            // Else, for each match in this node, call our IntegerNode::match_callback to test remaining nodes and/or extract
            // aggregate payload from aggregate column:
            else {
                m_source_column = source_column;
                bool cont = m_array.find<TConditionFunction, act_CallbackIdx>(m_value, s - m_leaf_start, end2, m_leaf_start, NULL,
                             std::bind1st(std::mem_fun(&IntegerNode::template match_callback<TAction, TSourceColumn>), this));
                if(!cont)
                    return not_found;
            }

            if (m_local_matches == m_local_limit)
                break;

            s = end2 + m_leaf_start;
        }

        if (matchcount)
            *matchcount = int64_t(static_cast< QueryState<QueryStateType>* >(st)->m_match_count);

        if (m_local_matches == m_local_limit) {
            m_dD = (m_last_local_match + 1 - start) / (m_local_matches + 1.0);
            return m_last_local_match + 1;
        }
        else {
            m_dD = (end - start) / (m_local_matches + 1.0);
            return end;
        }
    }

    size_t find_first_local(size_t start, size_t end)
    {
        TConditionFunction condition;
        TIGHTDB_ASSERT(m_table);

        while (start < end) {

            // Cache internal leafs
            if (start >= m_leaf_end) {
                m_condition_column->GetBlock(start, m_array, m_leaf_start);
                m_leaf_end = m_leaf_start + m_array.size();
            }

            // Do search directly on cached leaf array
            if (start + 1 == end) {
                if (condition(m_array.get(start - m_leaf_start), m_value))
                    return start;
                else
                    return not_found;
            }

            size_t end2;
            if (end > m_leaf_end)
                end2 = m_leaf_end - m_leaf_start;
            else
                end2 = end - m_leaf_start;

            size_t s = m_array.find_first<TConditionFunction>(m_value, start - m_leaf_start, end2);

            if (s == not_found) {
                start = m_leaf_end;
                continue;
            }
            else
                return s + m_leaf_start;
        }

        return not_found;
    }

    TConditionValue m_value;

protected:

    size_t m_last_local_match;
    const ColType* m_condition_column;                // Column on which search criteria is applied
    Array m_array;
    size_t m_leaf_start;
    size_t m_leaf_end;
    size_t m_local_end;

    size_t m_local_matches;
    size_t m_local_limit;

    QueryStateBase* m_state;
    SequentialGetterBase* m_source_column; // Column of values used in aggregate (act_FindAll, act_ReturnFirst, act_Sum, etc)
};


template <class TConditionFunction> class StringNode: public ParentNode {
public:
    template <Action TAction>
    int64_t find_all(Array*, size_t, size_t, size_t, size_t)
    {
        TIGHTDB_ASSERT(false);
        return 0;
    }

    StringNode(StringData v, size_t column)
    {
        m_condition_column_idx = column;
        m_child = 0;
        m_dT = 10.0;
        m_leaf = NULL;

        // FIXME: Store these in std::string instead.
        // '*6' because case converted strings can take up more space. Todo, investigate
        char* data = new char[6 * v.size()]; // FIXME: Arithmetic is prone to overflow
        memcpy(data, v.data(), v.size());
        m_value = StringData(data, v.size());
        char* upper = new char[6 * v.size()];
        char* lower = new char[6 * v.size()];

        bool b1 = case_map(v, lower, false);
        bool b2 = case_map(v, upper, true);
        if (!b1 || !b2)
            error_code = "Malformed UTF-8: " + std::string(v);

        m_ucase = upper;
        m_lcase = lower;
    }

    ~StringNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE
    {
        delete[] m_value.data();
        delete[] m_ucase;
        delete[] m_lcase;

        clear_leaf_state();
    }

    void init(const Table& table)
    {
        clear_leaf_state();

        m_dD = 100.0;
        m_probes = 0;
        m_matches = 0;
        m_end_s = 0;
        m_table = &table;
        m_condition_column = &get_column_base(table, m_condition_column_idx);
        m_column_type = get_real_column_type(table, m_condition_column_idx);

        if (m_child)
            m_child->init(table);
    }

    void clear_leaf_state()
    {
        if (!m_leaf)
            return;

        switch (m_leaf_type) {
            case AdaptiveStringColumn::leaf_type_Small:
                delete static_cast<ArrayString*>(m_leaf);
                goto delete_done;
            case AdaptiveStringColumn::leaf_type_Medium:
                delete static_cast<ArrayStringLong*>(m_leaf);
                goto delete_done;
            case AdaptiveStringColumn::leaf_type_Big:
                delete static_cast<ArrayBigBlobs*>(m_leaf);
                goto delete_done;
        }
        TIGHTDB_ASSERT(false);

      delete_done:
        m_leaf = 0;
    }

    size_t find_first_local(size_t start, size_t end)
    {
        TConditionFunction cond;

        for (size_t s = start; s < end; ++s) {
            StringData t;

            if (m_column_type == col_type_StringEnum) {
                // enum
                t = static_cast<const ColumnStringEnum*>(m_condition_column)->get(s);
            }
            else {
                // short or long
                const AdaptiveStringColumn* asc = static_cast<const AdaptiveStringColumn*>(m_condition_column);
                if (s >= m_end_s) {
                    // we exceeded current leaf's range
                    clear_leaf_state();

                    m_leaf_type = asc->GetBlock(s, &m_leaf, m_leaf_start);
                    if (m_leaf_type == AdaptiveStringColumn::leaf_type_Small)
                        m_end_s = m_leaf_start + static_cast<ArrayString*>(m_leaf)->size();
                    else if (m_leaf_type ==  AdaptiveStringColumn::leaf_type_Medium)
                        m_end_s = m_leaf_start + static_cast<ArrayStringLong*>(m_leaf)->size();
                    else
                        m_end_s = m_leaf_start + static_cast<ArrayBigBlobs*>(m_leaf)->size();
                }

                if (m_leaf_type == AdaptiveStringColumn::leaf_type_Small)
                    t = static_cast<ArrayString*>(m_leaf)->get(s - m_leaf_start);
                else if (m_leaf_type ==  AdaptiveStringColumn::leaf_type_Medium)
                    t = static_cast<ArrayStringLong*>(m_leaf)->get(s - m_leaf_start);
                else
                    t = static_cast<ArrayBigBlobs*>(m_leaf)->get_string(s - m_leaf_start);
            }
            if (cond(m_value, m_ucase, m_lcase, t))
                return s;
        }
        return not_found;
    }

private:
    StringData m_value;
    const char* m_lcase;
    const char* m_ucase;

protected:
    const ColumnBase* m_condition_column;
    ColumnType m_column_type;

    ArrayParent *m_leaf;

    AdaptiveStringColumn::LeafType m_leaf_type;
    size_t m_end_s;
//    size_t m_first_s;
    size_t m_leaf_start;
};


// Can be used for simple types (currently float and double)
template <class TConditionValue, class TConditionFunction> class BasicNode: public ParentNode {
public:
    typedef typename ColumnTypeTraits<TConditionValue>::column_type ColType;

    BasicNode(TConditionValue v, size_t column_ndx) : m_value(v)
    {
        m_condition_column_idx = column_ndx;
        m_child = 0;
        m_dT = 1.0;
    }
    ~BasicNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {}

    void init(const Table& table)
    {
        m_dD = 100.0;
        m_table = &table;
        m_condition_column.m_column = static_cast<const ColType*>(&get_column_base(table, m_condition_column_idx));
        m_condition_column.m_leaf_end = 0;

        if (m_child)
            m_child->init(table);
    }

    size_t find_first_local(size_t start, size_t end)
    {
        TConditionFunction cond;

        for (size_t s = start; s < end; ++s) {
            TConditionValue v = m_condition_column.get_next(s);
            if (cond(v, m_value))
                return s;
        }
        return not_found;
    }

protected:
    TConditionValue m_value;
    SequentialGetter<TConditionValue> m_condition_column;
};


template <class TConditionFunction> class BinaryNode: public ParentNode {
public:
    template <Action TAction> int64_t find_all(Array* /*res*/, size_t /*start*/, size_t /*end*/, size_t /*limit*/, size_t /*source_column*/) {TIGHTDB_ASSERT(false); return 0;}

    BinaryNode(BinaryData v, size_t column)
    {
        m_dT = 100.0;
        m_condition_column_idx = column;
        m_child = 0;

        // FIXME: Store this in std::string instead.
        char* data = new char[v.size()];
        memcpy(data, v.data(), v.size());
        m_value = BinaryData(data, v.size());
    }

    ~BinaryNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE
    {
        delete[] m_value.data();
    }

    void init(const Table& table)
    {
        m_dD = 100.0;
        m_table = &table;
        m_condition_column = static_cast<const ColumnBinary*>(&get_column_base(table, m_condition_column_idx));
        m_column_type = get_real_column_type(table, m_condition_column_idx);

        if (m_child)
            m_child->init(table);
    }

    size_t find_first_local(size_t start, size_t end)
    {
        TConditionFunction condition;
        for (size_t s = start; s < end; ++s) {
            BinaryData value = m_condition_column->get(s);
            if (condition(m_value, value))
                return s;
        }
        return not_found;
    }

protected:
private:
    BinaryData m_value;
protected:
    const ColumnBinary* m_condition_column;
    ColumnType m_column_type;
};


template<> class StringNode<Equal>: public ParentNode {
public:
    template <Action TAction>
    int64_t find_all(Array*, size_t, size_t, size_t, size_t)
    {
        TIGHTDB_ASSERT(false);
        return 0;
    }

    StringNode(StringData v, size_t column): m_key_ndx(size_t(-1))
    {
        m_condition_column_idx = column;
        m_child = 0;
        // FIXME: Store this in std::string instead.
        // FIXME: Why are the sizes 6 times the required size?
        char* data = new char[6 * v.size()]; // FIXME: Arithmetic is prone to overflow
        memcpy(data, v.data(), v.size());
        m_value = StringData(data, v.size());
        m_leaf = NULL;
        m_index_getter = 0;
        m_index_matches = 0;
        m_index_matches_destroy = false;
    }
    ~StringNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE
    {
        deallocate();
        delete[] m_value.data();
        clear_leaf_state();
        m_index.destroy();
    }

    void clear_leaf_state()
    {
        if (!m_leaf)
            return;

        switch (m_leaf_type) {
            case AdaptiveStringColumn::leaf_type_Small:
                delete static_cast<ArrayString*>(m_leaf);
                goto delete_done;
            case AdaptiveStringColumn::leaf_type_Medium:
                delete static_cast<ArrayStringLong*>(m_leaf);
                goto delete_done;
            case AdaptiveStringColumn::leaf_type_Big:
                delete static_cast<ArrayBigBlobs*>(m_leaf);
                goto delete_done;
        }
        TIGHTDB_ASSERT(false);

      delete_done:
        m_leaf = 0;
    }

    void deallocate() TIGHTDB_NOEXCEPT
    {
        // Must be called after each query execution too free temporary resources used by the execution. Run in
        // destructor, but also in Init because a user could define a query once and execute it multiple times.
        clear_leaf_state();

        if (m_index_matches_destroy)
            m_index_matches->destroy();

        m_index_matches_destroy = false;

        delete m_index_matches;
        m_index_matches = NULL;

        delete m_index_getter;
        m_index_getter = NULL;
    }

    void init(const Table& table)
    {
        deallocate();
        m_dD = 10.0;
        m_leaf_end = 0;
        m_table = &table;
        m_condition_column = &get_column_base(table, m_condition_column_idx);
        m_column_type = get_real_column_type(table, m_condition_column_idx);

        if (m_column_type == col_type_StringEnum) {
            m_dT = 1.0;
            m_key_ndx = ((const ColumnStringEnum*)m_condition_column)->GetKeyNdx(m_value);
        }
        else if (m_condition_column->has_index()) {
            m_dT = 0.0;
        }
        else {
            m_dT = 10.0;
        }

        if (m_condition_column->has_index()) {
            m_index.clear();

            FindRes fr;
            size_t index_ref;

            if (m_column_type == col_type_StringEnum) {
                fr = static_cast<const ColumnStringEnum*>(m_condition_column)->find_all_indexref(m_value, index_ref);
            }
            else {
                fr = static_cast<const AdaptiveStringColumn*>(m_condition_column)->find_all_indexref(m_value, index_ref);
            }

            m_index_matches_destroy = false;
            if (fr == FindRes_single) {
                m_index_matches = new Column();
                m_index_matches->add(index_ref);
                m_index_matches_destroy = true;        // we own m_index_matches, so we must destroy it
            }
            else if (fr == FindRes_column) {
                // todo: Apparently we can't use m_index.get_alloc() because it uses default allocator which simply makes
                // translate(x) = x. Shouldn't it inherit owner column's allocator?!
                if (m_column_type == col_type_StringEnum) {
                    m_index_matches = new Column(index_ref, 0, 0, static_cast<const ColumnStringEnum*>(m_condition_column)->get_alloc());
                }
                else {
                    m_index_matches = new Column(index_ref, 0, 0, static_cast<const AdaptiveStringColumn*>(m_condition_column)->get_alloc());
                }
            }
            else if (fr == FindRes_not_found) {
                m_index_matches = new Column;
                m_index_matches_destroy = true;        // we own m_index_matches, so we must destroy it
            }

            last_indexed = 0;

            m_index_getter = new SequentialGetter<int64_t>(m_index_matches);
            m_index_size = m_index_getter->m_column->size();

        }
        else if (m_column_type != col_type_String) {
            m_cse.m_column = static_cast<const ColumnStringEnum*>(m_condition_column);
            m_cse.m_leaf_end = 0;
            m_cse.m_leaf_start = 0;
        }

        if (m_child)
            m_child->init(table);
    }

    size_t find_first_local(size_t start, size_t end)
    {
        TIGHTDB_ASSERT(m_table);

        for (size_t s = start; s < end; ++s) {
            if (m_condition_column->has_index()) {

                // Indexed string column
                size_t f = not_found;

                while (f == not_found && last_indexed < m_index_size) {
                    m_index_getter->cache_next(last_indexed);
                    f = m_index_getter->m_array_ptr->FindGTE(s, last_indexed - m_index_getter->m_leaf_start);

                    if (f == not_found) {
                        last_indexed = m_index_getter->m_leaf_end;
                    }
                    else {
                        s = to_size_t(m_index_getter->m_array_ptr->get(f));
                        if (s > end)
                            return not_found;
                        else {
                            last_indexed = f + m_index_getter->m_leaf_start;
                            return s;
                        }
                    }
                }
                return not_found;
            }
            else {
                if (m_column_type != col_type_String) {

                    // Enum string column
                    if (m_key_ndx == not_found)
                        s = end; // not in key set
                    else {
                        m_cse.cache_next(s);
                        s = m_cse.m_array_ptr->find_first(m_key_ndx, s - m_cse.m_leaf_start, m_cse.local_end(end));
                        if (s == not_found)
                            s = m_cse.m_leaf_end - 1;
                        else
                            return s + m_cse.m_leaf_start;
                    }
                }
                else {

                    // Normal string column, with long or short leaf
                    AdaptiveStringColumn* asc = (AdaptiveStringColumn*)m_condition_column;
                    if (s >= m_leaf_end) {
                        clear_leaf_state();
                        m_leaf_type = asc->GetBlock(s, &m_leaf, m_leaf_start);
                        if (m_leaf_type == AdaptiveStringColumn::leaf_type_Small)
                            m_leaf_end = m_leaf_start + static_cast<ArrayString*>(m_leaf)->size();
                        else if (m_leaf_type ==  AdaptiveStringColumn::leaf_type_Medium)
                            m_leaf_end = m_leaf_start + static_cast<ArrayStringLong*>(m_leaf)->size();
                        else
                            m_leaf_end = m_leaf_start + static_cast<ArrayBigBlobs*>(m_leaf)->size();
                    }
                    size_t end2 = (end > m_leaf_end ? m_leaf_end - m_leaf_start : end - m_leaf_start);

                    if (m_leaf_type == AdaptiveStringColumn::leaf_type_Small)
                        s = static_cast<ArrayString*>(m_leaf)->find_first(m_value, s - m_leaf_start, end2);
                    else if (m_leaf_type ==  AdaptiveStringColumn::leaf_type_Medium)
                        s = static_cast<ArrayStringLong*>(m_leaf)->find_first(m_value, s - m_leaf_start, end2);
                    else
                        s = static_cast<ArrayBigBlobs*>(m_leaf)->find_first(str_to_bin(m_value), true, s - m_leaf_start, end2);

                    if (s == not_found)
                        s = m_leaf_end - 1;
                    else
                        return s + m_leaf_start;
                }
            }
        }
        return not_found;
    }

private:
    inline BinaryData str_to_bin(const StringData& s) TIGHTDB_NOEXCEPT
    {
        return BinaryData(s.data(), s.size());
    }

    StringData m_value;
    const ColumnBase* m_condition_column;
    ColumnType m_column_type;
    size_t m_key_ndx;
    Array m_index;
    size_t last_indexed;

    // Used for linear scan through enum-string
    SequentialGetter<int64_t> m_cse;

    // Used for linear scan through short/long-string
    ArrayParent* m_leaf;
    AdaptiveStringColumn::LeafType m_leaf_type;
    size_t m_leaf_end;
//    size_t m_first_s;
    size_t m_leaf_start;

    // Used for index lookup
    Column* m_index_matches;
    bool m_index_matches_destroy;
    SequentialGetter<int64_t>* m_index_getter;
    size_t m_index_size;
};


class OrNode: public ParentNode {
public:
    template <Action TAction> int64_t find_all(Array*, size_t, size_t, size_t, size_t)
    {
        TIGHTDB_ASSERT(false);
        return 0;
    }

    OrNode(ParentNode* p1) {m_child = NULL; m_cond[0] = p1; m_cond[1] = NULL; m_dT = 50.0;}
    ~OrNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE {}

    void init(const Table& table)
    {
        m_dD = 10.0;

        std::vector<ParentNode*> v;

        for (size_t c = 0; c < 2; ++c) {
            m_cond[c]->init(table);
            v.clear();
            m_cond[c]->gather_children(v);
            m_last[c] = 0;
            m_was_match[c] = false;
        }

        if (m_child)
            m_child->init(table);

        m_table = &table;
    }

    size_t find_first_local(size_t start, size_t end)
    {
        for (size_t s = start; s < end; ++s) {
            size_t f[2];

            for (size_t c = 0; c < 2; ++c) {
                if (m_last[c] >= end)
                    f[c] = end;
                else if (m_was_match[c] && m_last[c] >= s)
                    f[c] = m_last[c];
                else {
                    size_t fmax = m_last[c] > s ? m_last[c] : s;
                    f[c] = m_cond[c]->find_first(fmax, end);
                    m_was_match[c] = (f[c] != not_found);
                    m_last[c] = f[c] == not_found ? end : f[c];
                }
            }

            s = f[0] < f[1] ? f[0] : f[1];
            s = s >= end ? not_found : s;

            return s;
        }
        return not_found;
    }

    virtual std::string validate()
    {
        if (error_code != "")
            return error_code;
        if (m_cond[0] == 0)
            return "Missing left-hand side of OR";
        if (m_cond[1] == 0)
            return "Missing right-hand side of OR";
        std::string s;
        if (m_child != 0)
            s = m_child->validate();
        if (s != "")
            return s;
        s = m_cond[0]->validate();
        if (s != "")
            return s;
        s = m_cond[1]->validate();
        if (s != "")
            return s;
        return "";
    }

    ParentNode* m_cond[2];
private:
    size_t m_last[2];
    bool m_was_match[2];
};




template <class TConditionValue, class TConditionFunction> class TwoColumnsNode: public ParentNode {
public:
    template <Action TAction> int64_t find_all(Array* /*res*/, size_t /*start*/, size_t /*end*/, size_t /*limit*/, size_t /*source_column*/) {TIGHTDB_ASSERT(false); return 0;}

    TwoColumnsNode(size_t column1, size_t column2)
    {
        m_dT = 100.0;
        m_condition_column_idx1 = column1;
        m_condition_column_idx2 = column2;
        m_child = 0;
    }

    ~TwoColumnsNode() TIGHTDB_NOEXCEPT TIGHTDB_OVERRIDE
    {
        delete[] m_value.data();
    }

    void init(const Table& table)
    {
        typedef typename ColumnTypeTraits<TConditionValue>::column_type ColType;
        m_dD = 100.0;
        m_table = &table;

        const ColType* c = static_cast<const ColType*>(&get_column_base(table, m_condition_column_idx1));
        m_getter1.init(c);

        c = static_cast<const ColType*>(&get_column_base(table, m_condition_column_idx2));
        m_getter2.init(c);

        if (m_child)
            m_child->init(table);
    }

    size_t find_first_local(size_t start, size_t end)
    {
        size_t s = start;

        while (s < end) {
            if (SameType<TConditionValue, int64_t>::value) {
                // For int64_t we've created an array intrinsics named CompareLeafs which template expands bitwidths
                // of boths arrays to make Get faster.
                m_getter1.cache_next(s);
                m_getter2.cache_next(s);

                QueryState<int64_t> qs;
                bool resume = m_getter1.m_array_ptr->template CompareLeafs<TConditionFunction, act_ReturnFirst>(m_getter2.m_array_ptr, s - m_getter1.m_leaf_start, m_getter1.local_end(end), 0, &qs, CallbackDummy());

                if (resume)
                    s = m_getter1.m_leaf_end;
                else
                    return to_size_t(qs.m_state) + m_getter1.m_leaf_start;
            }
            else {
                // This is for float and double.
                TConditionValue v1 = m_getter1.get_next(s);
                TConditionValue v2 = m_getter2.get_next(s);
                TConditionFunction C;

                if (C(v1, v2))
                    return s;
                else
                    s++;
            }
        }
        return not_found;
    }

protected:
    BinaryData m_value;
    const ColumnBinary* m_condition_column;
    ColumnType m_column_type;

    size_t m_condition_column_idx1;
    size_t m_condition_column_idx2;

    SequentialGetter<TConditionValue> m_getter1;
    SequentialGetter<TConditionValue> m_getter2;
};

// todo, fixme: move this up! There are just some annoying compiler errors that need to be resolved when doing this
#include "query_expression.hpp"


// For expressions like col1 / col2 + 123 > col4 * 100
class ExpressionNode: public ParentNode {

public:
    ~ExpressionNode() TIGHTDB_NOEXCEPT
    {
        if(m_auto_delete)
            delete m_compare, m_compare = NULL;
    }

    ExpressionNode(Expression* compare, bool auto_delete) 
    {
        m_auto_delete = auto_delete;
        m_child = 0;
        m_compare = compare;
    }

    void init(const Table& table) 
    {
        m_compare->set_table(&table);
        if (m_child)
            m_child->init(table);
    }

    size_t find_first_local(size_t start, size_t end)
    {
        size_t res = m_compare->find_first(start, end);
        return res;
    }
    
    bool m_auto_delete;
    Expression* m_compare;
};

} // namespace tightdb

#endif // TIGHTDB_QUERY_ENGINE_HPP
