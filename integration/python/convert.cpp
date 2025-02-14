#include "convert.hpp"

#include <string>
#include <sstream>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

#include <magic_enum.hpp>

#include <actor-zeta.hpp>

#include <components/document/core/array.hpp>
#include <components/document/core/dict.hpp>

#include <components/document/document_view.hpp>
#include <components/expressions/compare_expression.hpp>
#include <components/expressions/aggregate_expression.hpp>
#include <components/expressions/scalar_expression.hpp>

// The bug related to the use of RTTI by the pybind11 library has been fixed: a
// declaration should be in each translation unit.
PYBIND11_DECLARE_HOLDER_TYPE(T, boost::intrusive_ptr<T>)

using components::document::document_ptr;
using components::document::document_view_t;

::document::retained_const_t<::document::impl::value_t> to_(const py::handle& obj) {
    if (py::isinstance<py::bool_>(obj)) {
        return ::document::impl::new_value(obj.cast<bool>());
    }
    if (py::isinstance<py::int_>(obj)) {
        return ::document::impl::new_value(obj.cast<long>());
    }
    if (py::isinstance<py::float_>(obj)) {
        return ::document::impl::new_value(obj.cast<double>());
    }
    if (py::isinstance<py::bytes>(obj)) {
        py::module base64 = py::module::import("base64");
        return ::document::impl::new_value(base64.attr("b64encode")(obj).attr("decode")("utf-8").cast<std::string>());
    }
    if (py::isinstance<py::str>(obj)) {
        return ::document::impl::new_value(obj.cast<std::string>());
    }
    if (py::isinstance<py::tuple>(obj) || py::isinstance<py::list>(obj)) {
        auto out = ::document::impl::array_t::new_array();
        for (const py::handle value : obj) {
            out->append(to_(value));
        }
        return out->as_array();
    }
    if (py::isinstance<py::dict>(obj)) {
        auto out = ::document::impl::dict_t::new_dict();
        for (const py::handle key : obj) {
            out->set(py::str(key).cast<std::string>(), to_(obj[key]));
        }
        return out->as_dict();
    }
    return nullptr;
}

py::object from_(const ::document::impl::value_t* value) {
    using document::impl::value_type;
    if (!value) {
        return py::none();
    } else if (value->type() == value_type::boolean) {
        return py::bool_(value->as_bool());
    } else if (value->is_unsigned()) {
        return py::int_(value->as_unsigned());
    } else if (value->is_int()) {
        return py::int_(value->as_int());
    } else if (value->is_double()) {
        return py::float_(value->as_double());
    } else if (value->type() == value_type::string) {
        return py::str(value->as_string());
    } else if (value->type() == value_type::array) {
        py::list list;
        for (uint32_t i = 0; i < value->as_array()->count(); ++i) {
            list.append(from_(value->as_array()->get(i)));
        }
        return std::move(list);
    } else if (value->type() == value_type::dict) {
        py::dict dict;
        for (auto it = value->as_dict()->begin(); it; ++it) {
            auto key = static_cast<std::string>(it.key()->as_string());
            dict[py::str(key)] = from_(it.value());
        }
        return std::move(dict);
    }
    return py::none();
}

auto to_document(const py::handle& source) -> components::document::document_ptr {
    return components::document::make_document(to_(source)->as_dict());
}

auto from_document(const document_view_t& document) -> py::object {
    return from_(document.get_value());
}

auto from_object(const document_view_t& document, const std::string& key) -> py::object {
    return from_(document.get(key));
}

auto from_object(const document_view_t& document, uint32_t index) -> py::object {
    return from_(document.get(index));
}

auto to_pylist(const std::pmr::vector<std::string>& src) -> py::list {
    py::list res;
    for (const auto& str : src) {
        res.append(str);
    }
    return res;
}

auto to_pylist(const std::pmr::vector<components::document::document_id_t>& src) -> py::list {
    py::list res;
    for (const auto& str : src) {
        res.append(str.to_string());
    }
    return res;
}

auto to_sorter(const py::handle& sort_dict) -> services::storage::sort::sorter_t {
    services::storage::sort::sorter_t sorter;
    for (const py::handle key : sort_dict) {
        sorter.add(py::str(key).cast<std::string>(), to_order(sort_dict[key]));
    }
    return sorter;
}

auto to_order(const py::object& order) -> services::storage::sort::order {
    return py::int_(order).cast<int>() < 0
               ? services::storage::sort::order::descending
               : services::storage::sort::order::ascending;
}


using components::ql::aggregate_statement;
using components::ql::aggregate::operator_type;

using ex_key_t = components::expressions::key_t;
using components::expressions::expression_ptr;
using components::expressions::sort_order;

using components::expressions::compare_expression_t;
using components::expressions::compare_expression_ptr;
using components::expressions::compare_type;
using components::expressions::get_compare_type;
using components::expressions::make_compare_expression;
using components::expressions::make_compare_union_expression;

using components::expressions::aggregate_expression_t;
using components::expressions::aggregate_type;
using components::expressions::is_aggregate_type;
using components::expressions::get_aggregate_type;
using components::expressions::make_aggregate_expression;

using components::expressions::scalar_expression_t;
using components::expressions::scalar_type;
using components::expressions::is_scalar_type;
using components::expressions::get_scalar_type;
using components::expressions::make_scalar_expression;

void normalize(compare_expression_ptr& expr) {
    if (expr->type() == compare_type::invalid && !expr->key().is_null()) {
        expr->set_type(compare_type::eq);
    }
}

void normalize_union(compare_expression_ptr& expr) {
    if (expr->type() == compare_type::invalid && expr->is_union()) {
        expr->set_type(compare_type::union_and);
    }
}

void parse_find_condition_dict_(std::pmr::memory_resource *resource, compare_expression_t* parent_condition, const py::handle& condition, const std::string& prev_key, aggregate_statement* aggregate);
void parse_find_condition_array_(std::pmr::memory_resource *resource, compare_expression_t* parent_condition, const py::handle& condition, const std::string& prev_key, aggregate_statement* aggregate);

void parse_find_condition_(std::pmr::memory_resource *resource, compare_expression_t* parent_condition, const py::handle& condition, const std::string& prev_key, const std::string& key_word, aggregate_statement* aggregate) {
    auto real_key = prev_key;
    auto type = get_compare_type(key_word);
    if (type == compare_type::invalid) {
        type = get_compare_type(prev_key);
        if (type != compare_type::invalid) {
            real_key = key_word;
        }
    }
    if (py::isinstance<py::dict>(condition)) {
        parse_find_condition_dict_(resource, parent_condition, condition, real_key, aggregate);
    } else if (py::isinstance<py::list>(condition) || py::isinstance<py::tuple>(condition)) {
        parse_find_condition_array_(resource, parent_condition, condition, real_key, aggregate);
    } else {
        auto value = aggregate->add_parameter(to_(condition).detach());
        auto sub_condition = make_compare_expression(resource, type, ex_key_t(real_key), value);
        if (sub_condition->is_union()) {
            parse_find_condition_(resource, sub_condition.get(), condition, real_key, std::string(), aggregate);
        }
        normalize(sub_condition);
        parent_condition->append_child(sub_condition);
    }
}

void parse_find_condition_dict_(std::pmr::memory_resource *resource, compare_expression_t* parent_condition, const py::handle& condition, const std::string& prev_key, aggregate_statement* aggregate) {
    for (const auto &it : condition) {
        auto key = py::str(it).cast<std::string>();
        auto type = get_compare_type(key);
        auto union_condition = parent_condition;
        if (is_union_compare_condition(type)) {
            parent_condition->append_child(make_compare_union_expression(resource, type));
            union_condition = parent_condition->children().at(parent_condition->children().size() - 1).get();
        }
        if (prev_key.empty()) {
            parse_find_condition_(resource, union_condition, condition[it], key, std::string(), aggregate);
        } else {
            parse_find_condition_(resource, union_condition, condition[it], prev_key, key, aggregate);
        }
    }
}

void parse_find_condition_array_(std::pmr::memory_resource *resource, compare_expression_t* parent_condition, const py::handle& condition, const std::string& prev_key, aggregate_statement* aggregate) {
    for (const auto &it : condition) {
        parse_find_condition_(resource, parent_condition, it, prev_key, std::string(), aggregate);
    }
}

expression_ptr parse_find_condition_(std::pmr::memory_resource *resource, const py::handle& condition, aggregate_statement* aggregate) {
    auto res_condition = make_compare_union_expression(resource, compare_type::union_and);
    for (const auto &it : condition) {
        if (py::len(condition) == 1) {
            res_condition->set_type(get_compare_type(py::str(it).cast<std::string>()));
        }
        parse_find_condition_(resource, res_condition.get(), condition[it], py::str(it).cast<std::string>(), std::string(), aggregate);
    }
    if (res_condition->children().size() == 1) {
        compare_expression_ptr child = res_condition->children()[0];
        normalize(child);
        return child;
    }
    normalize_union(res_condition);
    return res_condition;
}

aggregate_expression_t::param_storage parse_aggregate_param(const py::handle& condition, aggregate_statement* aggregate) {
    auto value = to_(condition);
    if (value->type() == document::impl::value_type::string
        && !value->as_string().empty()
        && value->as_string().at(0) == '$') {
        return ex_key_t(value->as_string().substr(1));
    } else {
        return aggregate->add_parameter(value);
    }
}

scalar_expression_t::param_storage parse_scalar_param(const py::handle& condition, aggregate_statement* aggregate) {
    auto value = to_(condition);
    if (value->type() == document::impl::value_type::string
        && !value->as_string().empty()
        && value->as_string().at(0) == '$') {
        return ex_key_t(value->as_string().substr(1));
    } else {
        return aggregate->add_parameter(value);
    }
}

expression_ptr parse_group_expr(std::pmr::memory_resource *resource, const std::string& key, const py::handle& condition, aggregate_statement* aggregate) {
    if (py::isinstance<py::dict>(condition)) {
        for (const auto &it : condition) {
            auto key_type = py::str(it).cast<std::string>().substr(1);
            if (is_aggregate_type(key_type)) {
                auto type = get_aggregate_type(key_type);
                auto expr = make_aggregate_expression(resource, type, key.empty() ? ex_key_t() : ex_key_t(key));
                if (py::isinstance<py::dict>(condition[it])) {
                    expr->append_param(parse_group_expr(resource, {}, condition[it], aggregate));
                } else if (py::isinstance<py::list>(condition[it]) || py::isinstance<py::tuple>(condition[it])) {
                    for (const auto& value : condition[it]) {
                        expr->append_param(parse_aggregate_param(value, aggregate));
                    }
                } else {
                    expr->append_param(parse_aggregate_param(condition[it], aggregate));
                }
                return expr;
            } else if (is_scalar_type(key_type)) {
                auto type = get_scalar_type(key_type);
                auto expr = make_scalar_expression(resource, type, key.empty() ? ex_key_t() : ex_key_t(key));
                if (py::isinstance<py::dict>(condition[it])) {
                    expr->append_param(parse_group_expr(resource, {}, condition[it], aggregate));
                } else if (py::isinstance<py::list>(condition[it]) || py::isinstance<py::tuple>(condition[it])) {
                    for (const auto& value : condition[it]) {
                        expr->append_param(parse_scalar_param(value, aggregate));
                    }
                } else {
                    expr->append_param(parse_scalar_param(condition[it], aggregate));
                }
                return expr;
            }
        }
    } else {
        auto expr = make_scalar_expression(resource, scalar_type::get_field, key.empty() ? ex_key_t() : ex_key_t(key));
        expr->append_param(parse_scalar_param(condition, aggregate));
        return expr;
    }
    return nullptr;
}

components::ql::aggregate::group_t parse_group(std::pmr::memory_resource *resource, const py::handle& condition, aggregate_statement* aggregate) {
    components::ql::aggregate::group_t group;
    for (const auto &it : condition) {
        components::ql::aggregate::append_expr(group, parse_group_expr(resource, py::str(it).cast<std::string>(), condition[it], aggregate));
    }
    return group;
}

components::ql::aggregate::sort_t parse_sort(const py::handle& condition) {
    components::ql::aggregate::sort_t sort;
    for (const auto &it : condition) {
        components::ql::aggregate::append_sort(sort, ex_key_t(py::str(it).cast<std::string>()), sort_order(condition[it].cast<int>()));
    }
    return sort;
}

auto to_statement(const py::handle& source, aggregate_statement* aggregate) -> void {
    auto *resource = actor_zeta::detail::pmr::get_default_resource(); //todo
    auto is_sequence = py::isinstance<py::sequence>(source);

    if (!is_sequence) {
        throw py::type_error(" not list ");
    }

    auto size = py::len(source);

    if (size == 0) {
        throw py::value_error(" len == 0 ");
    }

    aggregate->reserve(size);

    for (const py::handle obj : source) {
        auto is_mapping = py::isinstance<py::dict>(obj);
        if (!is_mapping) {
            throw py::type_error(" not mapping ");
        }

        for (const py::handle key : obj) {
            auto name = py::str(key).cast<std::string>();
            constexpr static std::string_view prefix = "$";
            std::string result = name.substr(prefix.length());
            operator_type op_type = components::ql::get_aggregate_type(result);
            switch (op_type) {
                case operator_type::invalid:
                    break;
                case operator_type::count: {
                    break;
                }
                case operator_type::group: {
                    aggregate->append(operator_type::group, parse_group(resource, obj[key], aggregate));
                    break;
                }
                case operator_type::limit: {
                    break;
                }
                case operator_type::match: {
                    aggregate->append(operator_type::match, components::ql::aggregate::make_match(parse_find_condition_(resource, obj[key], aggregate)));
                    break;
                }
                case operator_type::merge: {
                    break;
                }
                case operator_type::out: {
                    break;
                }
                case operator_type::project: {
                    break;
                }
                case operator_type::skip: {
                    break;
                }
                case operator_type::sort: {
                    aggregate->append(operator_type::sort, parse_sort(obj[key]));
                    break;
                }
                case operator_type::unset: {
                    break;
                }
                case operator_type::unwind: {
                    break;
                }
                case operator_type::finish: {
                    break;
                }
            }
        }
    }
}

auto test_to_statement(const py::handle& source) -> py::str {
    aggregate_statement aggregate("database", "collection");
    to_statement(source, &aggregate);
    std::stringstream stream;
    stream << aggregate;
    return stream.str();
}

pybind11::list pack_to_match(const pybind11::object &object) {
    py::dict match;
    match["$match"] = object;
    py::list list;
    list.append(match);
    return list;
}
