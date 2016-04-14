////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "js_collection.hpp"
#include "js_object.hpp"

#include "results.hpp"
#include "list.hpp"
#include "parser.hpp"
#include "query_builder.hpp"

namespace realm {
namespace js {

template<typename T>
struct Results {
    using TContext = typename T::Context;
    using TObject = typename T::Object;
    using TValue = typename T::Value;
    using Object = Object<T>;
    using Value = Value<T>;
    using ReturnValue = ReturnValue<T>;

    static TObject create(TContext, const realm::Results &, bool live = true);
    static TObject create(TContext, const realm::List &, bool live = true);
    static TObject create(TContext, SharedRealm, const std::string &type, bool live = true);
    static TObject create(TContext, SharedRealm, const ObjectSchema &, Query, bool live = true);

    template<typename U>
    static TObject create_filtered(TContext, const U &, size_t, const TValue[]);

    template<typename U>
    static TObject create_sorted(TContext, const U &, size_t, const TValue[]);

    static void GetLength(TContext, TObject, ReturnValue &);
    static void GetIndex(TContext, TObject, uint32_t, ReturnValue &);

    static void StaticResults(TContext, TObject, size_t, const TValue[], ReturnValue &);
    static void Filtered(TContext, TObject, size_t, const TValue[], ReturnValue &);
    static void Sorted(TContext, TObject, size_t, const TValue[], ReturnValue &);
};

template<typename T>
struct ObjectClass<T, realm::Results> : BaseObjectClass<T, Collection> {
    using Results = Results<T>;

    std::string const name = "Results";

    MethodMap<T> const methods = {
        {"snapshot", wrap<Results::StaticResults>},
        {"filtered", wrap<Results::Filtered>},
        {"sorted", wrap<Results::Sorted>},
    };
    
    PropertyMap<T> const properties = {
        {"length", {wrap<Results::GetLength>}},
    };
    
    IndexPropertyType<T> const index_accessor = {wrap<Results::GetIndex>};
};

template<typename T>
typename T::Object Results<T>::create(TContext ctx, const realm::Results &results, bool live) {
    auto new_results = new realm::Results(results);
    new_results->set_live(live);

    return create_object<T, realm::Results>(ctx, new_results);
}

template<typename T>
typename T::Object Results<T>::create(TContext ctx, const realm::List &list, bool live) {
    return create(ctx, list.get_realm(), list.get_object_schema(), list.get_query(), live);
}

template<typename T>
typename T::Object Results<T>::create(TContext ctx, SharedRealm realm, const std::string &type, bool live) {
    auto table = ObjectStore::table_for_object_type(realm->read_group(), type);
    auto &schema = realm->config().schema;
    auto object_schema = schema->find(type);

    if (object_schema == schema->end()) {
        throw std::runtime_error("Object type '" + type + "' not present in Realm.");
    }

    auto results = new realm::Results(realm, *object_schema, *table);
    results->set_live(live);

    return create_object<T, realm::Results>(ctx, results);
}

template<typename T>
typename T::Object Results<T>::create(TContext ctx, SharedRealm realm, const ObjectSchema &object_schema, Query query, bool live) {
    auto results = new realm::Results(realm, object_schema, std::move(query));
    results->set_live(live);

    return create_object<T, realm::Results>(ctx, results);
}

template<typename T>
template<typename U>
typename T::Object Results<T>::create_filtered(TContext ctx, const U &collection, size_t argc, const TValue arguments[]) {
    auto query_string = Value::validated_to_string(ctx, arguments[0], "predicate");
    auto query = collection.get_query();
    auto const &realm = collection.get_realm();
    auto const &object_schema = collection.get_object_schema();

    std::vector<TValue> args;
    args.reserve(argc - 1);

    for (size_t i = 1; i < argc; i++) {
        args.push_back(arguments[i]);
    }

    parser::Predicate predicate = parser::parse(query_string);
    query_builder::ArgumentConverter<TValue, TContext> converter(ctx, args);
    query_builder::apply_predicate(query, predicate, converter, *realm->config().schema, object_schema.name);

    return create(ctx, realm, object_schema, std::move(query));
}

template<typename T>
template<typename U>
typename T::Object Results<T>::create_sorted(TContext ctx, const U &collection, size_t argc, const TValue arguments[]) {
    auto const &realm = collection.get_realm();
    auto const &object_schema = collection.get_object_schema();
    std::vector<std::string> prop_names;
    std::vector<bool> ascending;
    size_t prop_count;

    if (Value::is_array(ctx, arguments[0])) {
        validate_argument_count(argc, 1, "Second argument is not allowed if passed an array of sort descriptors");

        TObject js_prop_names = Value::validated_to_object(ctx, arguments[0]);
        prop_count = Object::validated_get_length(ctx, js_prop_names);
        if (!prop_count) {
            throw std::invalid_argument("Sort descriptor array must not be empty");
        }

        prop_names.resize(prop_count);
        ascending.resize(prop_count);

        for (unsigned int i = 0; i < prop_count; i++) {
            TValue value = Object::validated_get_property(ctx, js_prop_names, i);

            if (Value::is_array(ctx, value)) {
                TObject array = Value::to_array(ctx, value);
                prop_names[i] = Object::validated_get_string(ctx, array, 0);
                ascending[i] = !Object::validated_get_boolean(ctx, array, 1);
            }
            else {
                prop_names[i] = Value::validated_to_string(ctx, value);
                ascending[i] = true;
            }
        }
    }
    else {
        validate_argument_count(argc, 1, 2);

        prop_count = 1;
        prop_names.push_back(Value::validated_to_string(ctx, arguments[0]));
        ascending.push_back(argc == 1 ? true : !Value::to_boolean(ctx, arguments[1]));
    }

    std::vector<size_t> columns;
    columns.reserve(prop_count);

    for (std::string &prop_name : prop_names) {
        const Property *prop = object_schema.property_for_name(prop_name);
        if (!prop) {
            throw std::runtime_error("Property '" + prop_name + "' does not exist on object type '" + object_schema.name + "'");
        }
        columns.push_back(prop->table_column);
    }

    auto results = new realm::Results(realm, object_schema, collection.get_query(), {std::move(columns), std::move(ascending)});
    return create_object<T, realm::Results>(ctx, results);
}

template<typename T>
void Results<T>::GetLength(TContext ctx, TObject object, ReturnValue &return_value) {
    auto results = get_internal<T, realm::Results>(object);
    return_value.set((uint32_t)results->size());
}

template<typename T>
void Results<T>::GetIndex(TContext ctx, TObject object, uint32_t index, ReturnValue &return_value) {
    auto results = get_internal<T, realm::Results>(object);
    auto row = results->get(index);

    // Return null for deleted objects in a snapshot.
    if (!row.is_attached()) {
        return_value.set_null();
        return;
    }

    auto realm_object = realm::Object(results->get_realm(), results->get_object_schema(), results->get(index));
    return_value.set(RealmObject<T>::create(ctx, realm_object));
}

template<typename T>
void Results<T>::StaticResults(TContext ctx, TObject this_object, size_t argc, const TValue arguments[], ReturnValue &return_value) {
    validate_argument_count(argc, 0);

    auto results = get_internal<T, realm::Results>(this_object);
    return_value.set(Results<T>::create(ctx, *results, false));
}

template<typename T>
void Results<T>::Filtered(TContext ctx, TObject this_object, size_t argc, const TValue arguments[], ReturnValue &return_value) {
    validate_argument_count_at_least(argc, 1);

    auto results = get_internal<T, realm::Results>(this_object);
    return_value.set(create_filtered(ctx, *results, argc, arguments));
}

template<typename T>
void Results<T>::Sorted(TContext ctx, TObject this_object, size_t argc, const TValue arguments[], ReturnValue &return_value) {
    validate_argument_count(argc, 1, 2);

    auto results = get_internal<T, realm::Results>(this_object);
    return_value.set(create_sorted(ctx, *results, argc, arguments));
}

} // js
} // realm
