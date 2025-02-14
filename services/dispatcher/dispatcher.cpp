#include "dispatcher.hpp"

#include <core/tracy/tracy.hpp>
#include <core/system_command.hpp>

#include <components/document/document.hpp>
#include <components/ql/statements.hpp>
#include <components/translator/ql_translator.hpp>

#include <services/collection/route.hpp>
#include <services/database/database.hpp>
#include <services/database/route.hpp>
#include <services/disk/manager_disk.hpp>
#include <services/disk/route.hpp>
#include <services/wal/route.hpp>

using namespace components::ql;

namespace services::dispatcher {

    key_collection_t::key_collection_t(database_name_t  database, collection_name_t  collection)
        : database_(std::move(database))
        , collection_(std::move(collection)) {
    }

    const database_name_t& key_collection_t::database() const {
        return database_;
    }

    const collection_name_t& key_collection_t::collection() const {
        return collection_;
    }

    bool key_collection_t::operator==(const key_collection_t& other) const {
        return database_ == other.database_ && collection_ == other.collection_;
    }

    std::size_t key_collection_t::hash::operator()(const key_collection_t& key) const {
        return std::hash<std::string>()(key.database()) ^ std::hash<std::string>()(key.collection());
    }


    dispatcher_t::dispatcher_t(
        manager_dispatcher_t* manager_dispatcher,
        std::pmr::memory_resource *resource,
        actor_zeta::address_t mdb,
        actor_zeta::address_t mwal,
        actor_zeta::address_t mdisk,
        log_t& log,
        std::string name)
        : actor_zeta::basic_async_actor(manager_dispatcher, std::move(name))
        , log_(log.clone())
        , resource_(resource)
        , manager_dispatcher_(manager_dispatcher->address())
        , manager_database_(std::move(mdb))
        , manager_wal_(std::move(mwal))
        , manager_disk_(std::move(mdisk)) {
        trace(log_, "dispatcher_t::dispatcher_t start name:{}", type());
        add_handler(core::handler_id(core::route::load), &dispatcher_t::load);
        add_handler(disk::handler_id(disk::route::load_finish), &dispatcher_t::load_from_disk_result);
        add_handler(database::handler_id(database::route::create_databases_finish), &dispatcher_t::load_create_databases_result);
        add_handler(database::handler_id(database::route::create_collections_finish), &dispatcher_t::load_create_collections_result);
        add_handler(collection::handler_id(collection::route::create_documents_finish), &dispatcher_t::load_create_documents_result);
        add_handler(wal::handler_id(wal::route::load_finish), &dispatcher_t::load_from_wal_result);
        add_handler(database::handler_id(database::route::create_database), &dispatcher_t::create_database);
        add_handler(database::handler_id(database::route::create_database_finish), &dispatcher_t::create_database_finish);
        add_handler(database::handler_id(database::route::create_collection), &dispatcher_t::create_collection);
        add_handler(database::handler_id(database::route::create_collection_finish), &dispatcher_t::create_collection_finish);
        add_handler(database::handler_id(database::route::drop_collection), &dispatcher_t::drop_collection);
        add_handler(database::handler_id(database::route::drop_collection_finish), &dispatcher_t::drop_collection_finish);
        add_handler(collection::handler_id(collection::route::drop_collection_finish), &dispatcher_t::drop_collection_finish_collection);
        add_handler(disk::handler_id(disk::route::remove_collection_finish), &dispatcher_t::drop_collection_finish_from_disk);
        add_handler(collection::handler_id(collection::route::insert_documents), &dispatcher_t::insert_documents);
        add_handler(collection::handler_id(collection::route::insert_finish), &dispatcher_t::insert_finish);
        add_handler(collection::handler_id(collection::route::find), &dispatcher_t::find);
        add_handler(collection::handler_id(collection::route::find_finish), &dispatcher_t::find_finish);
        add_handler(collection::handler_id(collection::route::find_one), &dispatcher_t::find_one);
        add_handler(collection::handler_id(collection::route::find_one_finish), &dispatcher_t::find_one_finish);
        add_handler(collection::handler_id(collection::route::delete_documents), &dispatcher_t::delete_documents);
        add_handler(collection::handler_id(collection::route::delete_finish), &dispatcher_t::delete_finish);
        add_handler(collection::handler_id(collection::route::update_documents), &dispatcher_t::update_documents);
        add_handler(collection::handler_id(collection::route::update_finish), &dispatcher_t::update_finish);
        add_handler(collection::handler_id(collection::route::size), &dispatcher_t::size);
        add_handler(collection::handler_id(collection::route::size_finish), &dispatcher_t::size_finish);
        add_handler(collection::handler_id(collection::route::close_cursor), &dispatcher_t::close_cursor);
        add_handler(collection::handler_id(collection::route::create_index), &dispatcher_t::create_index);
        add_handler(collection::handler_id(collection::route::create_index_finish), &dispatcher_t::create_index_finish);
        add_handler(collection::handler_id(collection::route::drop_index), &dispatcher_t::drop_index);
        add_handler(collection::handler_id(collection::route::drop_index_finish), &dispatcher_t::drop_index_finish);
        add_handler(wal::handler_id(wal::route::success), &dispatcher_t::wal_success);
        trace(log_, "dispatcher_t::dispatcher_t finish name:{}", type());
    }

    dispatcher_t::~dispatcher_t() {
        trace(log_, "delete dispatcher_t");
    }

    void dispatcher_t::load(components::session::session_id_t &session, actor_zeta::address_t sender) {
        trace(log_, "dispatcher_t::load, session: {}", session.data());
        load_session_ = session;
        make_session(session_to_address_, session, session_t(std::move(sender)));
        actor_zeta::send(manager_disk_, dispatcher_t::address(), disk::handler_id(disk::route::load), session);
    }

    void dispatcher_t::load_from_disk_result(components::session::session_id_t &session, const disk::result_load_t &result) {
        trace(log_, "dispatcher_t::load_from_disk_result, session: {}, wal_id: {}", session.data(), result.wal_id());
        if ((*result).empty()) {
            actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), core::handler_id(core::route::load_finish));
            remove_session(session_to_address_, session);
            load_result_.clear();
        } else {
            load_count_answers_ = result.count_collections();
            load_result_ = result;
            actor_zeta::send(manager_database_, address(), database::handler_id(database::route::create_databases), session, result.name_databases());
        }
    }

    void dispatcher_t::load_create_databases_result(components::session::session_id_t &session, const std::vector<actor_zeta::base::address_t> &result) {
        trace(log_, "dispatcher_t::load_create_databases_result, session: {}", session.data());
        for (std::size_t i = 0; i < result.size(); ++i) {
            database_address_book_.emplace((*load_result_).at(i).name, result.at(i));
            actor_zeta::send(result.at(i), dispatcher_t::address(), database::handler_id(database::route::create_collections), session, (*load_result_).at(i).name_collections(), manager_disk_);
        }
    }

    void dispatcher_t::load_create_collections_result(components::session::session_id_t &session, const database_name_t &database_name, const std::vector<actor_zeta::base::address_t> &result) {
        trace(log_, "dispatcher_t::load_create_collections_result, session: {}, database: {}", session.data(), database_name);
        auto it_database = std::find_if((*load_result_).begin(), (*load_result_).end(), [&database_name](const services::disk::result_database_t &database) {
            return database.name == database_name;
        });
        if (it_database != (*load_result_).end()) {
            for (std::size_t i = 0; i < result.size(); ++i) {
                collection_address_book_.emplace(key_collection_t(database_name, it_database->collections.at(i).name), result.at(i));
                actor_zeta::send(result.at(i), dispatcher_t::address(), collection::handler_id(collection::route::create_documents), session, it_database->collections.at(i).documents);
            }
        }
    }

    void dispatcher_t::load_create_documents_result(components::session::session_id_t &session) {
        trace(log_, "dispatcher_t::load_create_documents_result, session: {}, wait answers: {}", session.data(), --load_count_answers_);
        if (load_count_answers_ == 0) {
            actor_zeta::send(manager_disk_, address(), disk::handler_id(disk::route::load_indexes), session);
            actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::load), session, load_result_.wal_id());
            load_result_.clear();
        }
    }

    void dispatcher_t::load_from_wal_result(components::session::session_id_t& session, std::vector<services::wal::record_t> &records) {
        load_count_answers_ = records.size();
        trace(log_, "dispatcher_t::load_from_wal_result, session: {}, count commands: {}", session.data(), load_count_answers_);
        if (load_count_answers_ == 0) {
            actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), core::handler_id(core::route::load_finish));
            remove_session(session_to_address_, session);
            return;
        }
        last_wal_id_ = records[load_count_answers_ - 1].id;
        for (const auto &record : records) {
            switch (record.type) {
                case statement_type::create_database: {
                    auto data = std::get<create_database_t>(record.data);
                    components::session::session_id_t session_database;
                    create_database(session_database, &data, manager_wal_);
                    break;
                }
                case statement_type::drop_database: {
                    break;
                }
                case statement_type::create_collection: {
                    auto data = std::get<create_collection_t>(record.data);
                    components::session::session_id_t session_collection;
                    create_collection(session_collection, &data, manager_wal_);
                    break;
                }
                case statement_type::drop_collection: {
                    auto data = std::get<drop_collection_t>(record.data);
                    components::session::session_id_t session_collection;
                    drop_collection(session_collection, &data, manager_wal_);
                    break;
                }
                case statement_type::insert_one: {
                    auto ql = std::get<insert_one_t>(record.data);
                    components::session::session_id_t session_insert;
                    insert_documents(session_insert, &ql, manager_wal_);
                    break;
                }
                case statement_type::insert_many: {
                    auto ql = std::get<insert_many_t>(record.data);
                    components::session::session_id_t session_insert;
                    insert_documents(session_insert, &ql, manager_wal_);
                    break;
                }
                case statement_type::delete_one: {
                    auto ql = std::get<delete_one_t>(record.data);
                    components::session::session_id_t session_delete;
                    delete_documents(session_delete, &ql, manager_wal_);
                    break;
                }
                case statement_type::delete_many: {
                    auto ql = std::get<delete_many_t>(record.data);
                    components::session::session_id_t session_delete;
                    delete_documents(session_delete, &ql, manager_wal_);
                    break;
                }
                case statement_type::update_one: {
                    auto ql = std::get<update_one_t>(record.data);
                    components::session::session_id_t session_update;
                    update_documents(session_update, &ql, manager_wal_);
                    break;
                }
                case statement_type::update_many: {
                    auto ql = std::get<update_many_t>(record.data);
                    components::session::session_id_t session_update;
                    update_documents(session_update, &ql, manager_wal_);
                    break;
                }
                case statement_type::create_index: {
                    auto data = std::get<create_index_t>(record.data);
                    components::session::session_id_t session_create_index;
                    create_index(session_create_index, std::move(data), manager_wal_);
                    break;
                }
                default:
                    break;
            }
        }
    }

    void dispatcher_t::create_database(components::session::session_id_t& session, components::ql::ql_statement_t* statement, actor_zeta::address_t address) {
        trace(log_, "dispatcher_t::create_database: session {} , name {}", session.data(), statement->database_);
        make_session(session_to_address_, session, session_t(std::move(address), *static_cast<components::ql::create_database_t*>(statement)));
        actor_zeta::send(manager_database_, dispatcher_t::address(), database::handler_id(database::route::create_database), session, std::move(statement));
    }

    void dispatcher_t::create_database_finish(components::session::session_id_t& session, const database::database_create_result& result) {
        trace(log_, "dispatcher_t::create_database_finish: session {} , name {}", session.data(), result.statement_->database_);
        if (result.created_) {
            database_address_book_.emplace(result.statement_->database_, result.address_);
            actor_zeta::send(manager_disk_, dispatcher_t::address(), disk::handler_id(disk::route::append_database), session, result.statement_->database_);
            if (find_session(session_to_address_, session).address().get() == manager_wal_.get()) {
                wal_success(session, last_wal_id_);
            } else {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::create_database), session, *static_cast<create_database_t*>(result.statement_));
            }
        }
        if (!check_load_from_wal(session)) {
            actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), database::handler_id(database::route::create_database_finish), session, result);
            remove_session(session_to_address_, session);
        }
    }

    void dispatcher_t::create_collection(components::session::session_id_t& session, components::ql::ql_statement_t* statement, const actor_zeta::address_t& address) {
        trace(log_, "dispatcher_t::create_collection: session {} , database_name {} , collection_name {}", session.data(), statement->database_, statement->collection_);
        make_session(session_to_address_, session, session_t(address, *static_cast<components::ql::create_collection_t*>(statement)));
        actor_zeta::send(database_address_book_.at(statement->database_), dispatcher_t::address(), database::handler_id(database::route::create_collection), session, std::move(statement), manager_disk_);
    }

    void dispatcher_t::create_collection_finish(components::session::session_id_t& session, const database::collection_create_result& result) {
        trace(log_, "create_collection_finish: session {} , database_name {} , collection_name {}", session.data(), result.statement_->database_, result.statement_->collection_);
        if (result.created_) {
            collection_address_book_.emplace(key_collection_t(result.statement_->database_, result.statement_->collection_), result.address_);
            actor_zeta::send(manager_disk_, dispatcher_t::address(), disk::handler_id(disk::route::append_collection), session, result.statement_->database_, result.statement_->collection_);
            if (find_session(session_to_address_, session).address().get() == manager_wal_.get()) {
                wal_success(session, last_wal_id_);
            } else {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::create_collection), session, *static_cast<create_collection_t*>(result.statement_));
            }
        }
        if (!check_load_from_wal(session)) {
            actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), database::handler_id(database::route::create_collection_finish), session, result);
            remove_session(session_to_address_, session);
        }
    }

    void dispatcher_t::drop_collection(components::session::session_id_t& session, components::ql::ql_statement_t* statement, actor_zeta::address_t address) {
        trace(log_, "dispatcher_t::drop_collection: session {} , database_name {} , collection_name {}", session.data(), statement->database_, statement->collection_);
        auto it_collection = collection_address_book_.find({statement->database_, statement->collection_});
        if (it_collection != collection_address_book_.end()) {
            make_session(session_to_address_, session, session_t(std::move(address), *static_cast<components::ql::drop_collection_t*>(statement)));
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::drop_collection), session);
        } else {
            actor_zeta::send(address, dispatcher_t::address(), database::handler_id(database::route::drop_collection_finish), session, result_drop_collection(false));
        }
    }

    void dispatcher_t::drop_collection_finish_collection(components::session::session_id_t& session, result_drop_collection& result, std::string& database_name, std::string& collection_name) {
        trace(log_, "dispatcher_t::drop_collection_finish_collection: database_name {} , collection_name {}, result: {}", session.data(), database_name, collection_name, result.is_success());
        if (result.is_success()) {
            actor_zeta::send(database_address_book_.at(database_name), dispatcher_t::address(), database::handler_id(database::route::drop_collection), session, collection_name);
        } else {
            if (!check_load_from_wal(session)) {
                actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), database::handler_id(database::route::drop_collection_finish), session, result);
                remove_session(session_to_address_, session);
            }
        }
    }

    void dispatcher_t::drop_collection_finish(components::session::session_id_t& session, result_drop_collection& result, std::string& database_name, std::string& collection_name, const actor_zeta::address_t&) {
        trace(log_, "drop_collection_finish: {}", collection_name);
        if (result.is_success()) {
            collection_address_book_.erase({database_name, std::string(collection_name)});
            actor_zeta::send(manager_disk_, dispatcher_t::address(), disk::handler_id(disk::route::remove_collection), session, database_name, std::string(collection_name));
            if (find_session(session_to_address_, session).address().get() == manager_wal_.get()) {
                wal_success(session, last_wal_id_);
            } else {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::drop_collection), session, components::ql::drop_collection_t(database_name, collection_name));
            }
        } else if (!check_load_from_wal(session)) {
            actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), database::handler_id(database::route::drop_collection_finish), session, result);
            remove_session(session_to_address_, session);
        }
    }

    void dispatcher_t::drop_collection_finish_from_disk(components::session::session_id_t& session, std::string& collection_name) {
        trace(log_, "drop_collection_finish_from_disk: {}", collection_name);
        if (!check_load_from_wal(session)) {
            result_drop_collection result{true};
            actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), database::handler_id(database::route::drop_collection_finish), session, result);
            remove_session(session_to_address_, session);
        }
    }

    void dispatcher_t::insert_documents(components::session::session_id_t& session, ql_statement_t* statement, actor_zeta::address_t address) {
        trace(log_, "dispatcher_t::insert: session:{}, database: {}, collection: {}", session.data(), statement->database_, statement->collection_);
        key_collection_t key{statement->database_, statement->collection_};
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            if (statement->type() == statement_type::insert_one) {
                make_session(session_to_address_, session, session_t(std::move(address), *static_cast<insert_one_t*>(statement)));
            } else {
                make_session(session_to_address_, session, session_t(std::move(address), *static_cast<insert_many_t*>(statement)));
            }
            auto logic_plan = create_logic_plan(statement).first;
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::insert_documents), session, logic_plan, components::ql::storage_parameters{});
        } else {
            actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::insert_finish), session, result_insert(resource_));
        }
    }

    void dispatcher_t::insert_finish(components::session::session_id_t& session, result_insert& result) {
        trace(log_, "dispatcher_t::insert_finish session: {}", session.data());
        auto& s = find_session(session_to_address_, session);
        if (s.address().get() == manager_wal_.get()) {
            wal_success(session, last_wal_id_);
        } else {
            if (s.type() == statement_type::insert_one) {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::insert_one), session, s.get<insert_one_t>());
            } else {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::insert_many), session, s.get<insert_many_t>());
            }
        }
        if (!check_load_from_wal(session)) {
            actor_zeta::send(s.address(), dispatcher_t::address(), collection::handler_id(collection::route::insert_finish), session, result);
            remove_session(session_to_address_, session);
        }
    }

    void dispatcher_t::find(components::session::session_id_t& session, components::ql::ql_statement_t* statement, actor_zeta::address_t address) {
        debug(log_, "dispatcher_t::find: session:{}, database: {}, collection: {}", session.data(), statement->database_, statement->collection_);

        key_collection_t key(statement->database_, statement->collection_);
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            make_session(session_to_address_, session, session_t(std::move(address)));
            auto logic_plan = create_logic_plan(statement);
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::find), session, std::move(logic_plan.first), std::move(logic_plan.second));
        } else {
            delete statement;
            actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::find_finish), session, result_find(resource_));
        }
    }

    void dispatcher_t::find_finish(components::session::session_id_t& session, components::cursor::sub_cursor_t* cursor) {
        trace(log_, "dispatcher_t::find_finish session: {}", session.data());
        auto result = new components::cursor::cursor_t(resource_);
        if (cursor) {
            result->push(cursor);
        }
        actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), collection::handler_id(collection::route::find_finish), session, result);
        remove_session(session_to_address_, session);
    }

    void dispatcher_t::find_one(components::session::session_id_t& session, components::ql::ql_statement_t* statement, actor_zeta::address_t address) {
        debug(log_, "dispatcher_t::find_one: session:{}, database: {}, collection: {}", session.data(), statement->database_, statement->collection_);
        key_collection_t key(statement->database_, statement->collection_);
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            make_session(session_to_address_, session, session_t(std::move(address)));
            auto logic_plan = create_logic_plan(statement);
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::find_one), session, std::move(logic_plan.first), std::move(logic_plan.second));
        } else {
            delete statement;
            actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::find_one_finish), session, result_find_one());
        }
    }

    void dispatcher_t::find_one_finish(components::session::session_id_t& session, result_find_one& result) {
        trace(log_, "dispatcher_t::find_one_finish session: {}", session.data());
        actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), collection::handler_id(collection::route::find_one_finish), session, result);
        remove_session(session_to_address_, session);
    }

    void dispatcher_t::delete_documents(components::session::session_id_t& session, components::ql::ql_statement_t* statement, actor_zeta::address_t address) {
        debug(log_, "dispatcher_t::delete_one: session:{}, database: {}, collection: {}", session.data(), statement->database_, statement->collection_);
        key_collection_t key(statement->database_, statement->collection_);
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            if (statement->type() == statement_type::delete_one) {
                make_session(session_to_address_, session, session_t(address, *static_cast<delete_one_t*>(statement)));
            } else {
                make_session(session_to_address_, session, session_t(address, *static_cast<delete_many_t*>(statement)));
            }
            auto logic_plan = create_logic_plan(statement);
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::delete_documents), session, std::move(logic_plan.first), std::move(logic_plan.second));
        } else {
            delete statement;
            actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::delete_finish), session, result_delete(resource_));
        }
    }

    void dispatcher_t::delete_finish(components::session::session_id_t& session, result_delete& result) {
        trace(log_, "dispatcher_t::delete_finish session: {}", session.data());
        auto& s = find_session(session_to_address_, session);
        if (s.address().get() == manager_wal_.get()) {
            wal_success(session, last_wal_id_);
        } else {
            if (s.type() == statement_type::delete_one) {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::delete_one), session, s.get<delete_one_t>());
            } else {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::delete_many), session, s.get<delete_many_t>());
            }
        }
        if (!check_load_from_wal(session)) {
            actor_zeta::send(s.address(), dispatcher_t::address(), collection::handler_id(collection::route::delete_finish), session, result);
            remove_session(session_to_address_, session);
        }
    }

    void dispatcher_t::update_documents(components::session::session_id_t& session, components::ql::ql_statement_t* statement, actor_zeta::address_t address) {
        debug(log_, "dispatcher_t::update_one: session:{}, database: {}, collection: {}", session.data(), statement->database_, statement->collection_);
        key_collection_t key(statement->database_, statement->collection_);
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            if (statement->type() == statement_type::update_one) {
                make_session(session_to_address_, session, session_t(address, *static_cast<update_one_t*>(statement)));
            } else {
                make_session(session_to_address_, session, session_t(address, *static_cast<update_many_t*>(statement)));
            }
            auto logic_plan = create_logic_plan(statement);
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::update_documents), session, std::move(logic_plan.first), std::move(logic_plan.second));
        } else {
            delete statement;
            actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::update_finish), session, result_update(resource_));
        }
    }

    void dispatcher_t::update_finish(components::session::session_id_t& session, result_update& result) {
        trace(log_, "dispatcher_t::update_finish session: {}", session.data());
        auto& s = find_session(session_to_address_, session);
        if (s.address().get() == manager_wal_.get()) {
            wal_success(session, last_wal_id_);
        } else {
            if (s.type() == statement_type::update_one) {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::update_one), session, s.get<update_one_t>());
            } else {
                actor_zeta::send(manager_wal_, dispatcher_t::address(), wal::handler_id(wal::route::update_many), session, s.get<update_many_t>());
            }
        }
        if (!check_load_from_wal(session)) {
            actor_zeta::send(s.address(), dispatcher_t::address(), collection::handler_id(collection::route::update_finish), session, result);
            remove_session(session_to_address_, session);
        }
    }

    void dispatcher_t::size(components::session::session_id_t& session, std::string& database_name, std::string& collection, actor_zeta::address_t address) {
        trace(log_, "dispatcher_t::size: session:{}, database: {}, collection: {}", session.data(), database_name, collection);
        key_collection_t key(database_name, collection);
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            make_session(session_to_address_, session, session_t(std::move(address)));
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::size), session);
        } else {
            actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::size_finish), session, result_size());
        }
    }

    void dispatcher_t::size_finish(components::session::session_id_t& session, result_size& result) {
        trace(log_, "dispatcher_t::size_finish session: {}", session.data());
        actor_zeta::send(find_session(session_to_address_, session).address(), dispatcher_t::address(), collection::handler_id(collection::route::size_finish), session, result);
        remove_session(session_to_address_, session);
    }

    void dispatcher_t::create_index(components::session::session_id_t &session, components::ql::create_index_t index, actor_zeta::address_t address) {
        debug(log_, "dispatcher_t::create_index: session:{}, index: {}", session.data(), index.name());
        key_collection_t key(index.database_, index.collection_);
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            make_session(session_to_address_, session_key_t{session, index.name()}, session_t(address, index));
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::create_index), session, std::move(index));
        } else {
            if (address.get() != dispatcher_t::address().get()) {
                actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::create_index_finish), session, result_create_index());
            }
        }
    }

    void dispatcher_t::create_index_finish(components::session::session_id_t &session, const std::string& name, result_create_index& result) {
        trace(log_, "dispatcher_t::create_index_finish session: {}, index: {}", session.data(), name);
        if (find_session(session_to_address_, session, name).address().get() == manager_wal_.get()) {
            wal_success(session, last_wal_id_);
        }
        if (find_session(session_to_address_, session, name).address().get() != dispatcher_t::address().get()) {
            actor_zeta::send(find_session(session_to_address_, session, name).address(), dispatcher_t::address(), collection::handler_id(collection::route::create_index_finish), session, result);
        }
        remove_session(session_to_address_, session, name);
    }

    void dispatcher_t::drop_index(components::session::session_id_t &session, components::ql::drop_index_t drop_index, actor_zeta::address_t address) {
        debug(log_, "dispatcher_t::drop_index: session: {}, index: {}", session.data(), drop_index.name());
        key_collection_t key(drop_index.database_, drop_index.collection_);
        auto it_collection = collection_address_book_.find(key);
        if (it_collection != collection_address_book_.end()) {
            make_session(session_to_address_, session_key_t{session, drop_index.name()}, session_t(address, drop_index));
            actor_zeta::send(it_collection->second, dispatcher_t::address(), collection::handler_id(collection::route::drop_index), session, std::move(drop_index));
        } else {
            if (address.get() != dispatcher_t::address().get()) {
                actor_zeta::send(address, dispatcher_t::address(), collection::handler_id(collection::route::drop_index_finish), session, result_drop_index());
            }
        }
    }

    void dispatcher_t::drop_index_finish(components::session::session_id_t &session, const std::string& name, result_drop_index& result) {
        trace(log_, "dispatcher_t::drop_index_finish session: {}, index: {}", session.data(), name);
        if (find_session(session_to_address_, session, name).address().get() == manager_wal_.get()) {
            wal_success(session, last_wal_id_);
        }
        if (find_session(session_to_address_, session, name).address().get() != dispatcher_t::address().get()) {
            actor_zeta::send(find_session(session_to_address_, session, name).address(), dispatcher_t::address(), collection::handler_id(collection::route::drop_index_finish), session, result);
        }
        remove_session(session_to_address_, session, name);
    }

    void dispatcher_t::close_cursor(components::session::session_id_t& session) {
        trace(log_, " dispatcher_t::close_cursor ");
        trace(log_, "Session : {}", session.data());
        auto it = cursor_.find(session);
        if (it != cursor_.end()) {
            for (auto& i : *it->second) {
                actor_zeta::send(i->address(), address(), collection::handler_id(collection::route::close_cursor), session);
            }
            cursor_.erase(it);
        } else {
            error(log_, "Not find session : {}", session.data());
        }
    }

    void dispatcher_t::wal_success(components::session::session_id_t& session, services::wal::id_t wal_id) {
        actor_zeta::send(manager_disk_, dispatcher_t::address(), disk::handler_id(disk::route::flush), session, wal_id);
    }

    bool dispatcher_t::check_load_from_wal(components::session::session_id_t& session) {
        if (find_session(session_to_address_, session).address().get() == manager_wal_.get()) {
            if (--load_count_answers_ == 0) {
                actor_zeta::send(find_session(session_to_address_, load_session_).address(), dispatcher_t::address(), core::handler_id(core::route::load_finish));
                remove_session(session_to_address_, load_session_);
            }
            return true;
        }
        return false;
    }

    std::pair<components::logical_plan::node_ptr, components::ql::storage_parameters> dispatcher_t::create_logic_plan(
            ql_statement_t* statement) {
        //todo: cache logical plans
        auto logic_plan = components::translator::ql_translator(resource_, statement);
        auto parameters = statement->is_parameters()
                ? static_cast<components::ql::ql_param_statement_t*>(statement)->take_parameters()
                : components::ql::storage_parameters{};
        return {logic_plan, parameters};
    }


    manager_dispatcher_t::manager_dispatcher_t(
        actor_zeta::detail::pmr::memory_resource* mr,
        actor_zeta::scheduler_raw scheduler,
        log_t& log)
        : actor_zeta::cooperative_supervisor<manager_dispatcher_t>(mr, "manager_dispatcher")
        , log_(log.clone())
        , e_(scheduler) {
        ZoneScoped;
        trace(log_, "manager_dispatcher_t::manager_dispatcher_t ");
        add_handler(handler_id(route::create), &manager_dispatcher_t::create);
        add_handler(core::handler_id(core::route::load), &manager_dispatcher_t::load);
        add_handler(database::handler_id(database::route::create_database), &manager_dispatcher_t::create_database);
        add_handler(database::handler_id(database::route::create_collection), &manager_dispatcher_t::create_collection);
        add_handler(database::handler_id(database::route::drop_collection), &manager_dispatcher_t::drop_collection);
        add_handler(collection::handler_id(collection::route::insert_documents), &manager_dispatcher_t::insert_documents);
        add_handler(collection::handler_id(collection::route::find), &manager_dispatcher_t::find);
        add_handler(collection::handler_id(collection::route::find_one), &manager_dispatcher_t::find_one);
        add_handler(collection::handler_id(collection::route::delete_documents), &manager_dispatcher_t::delete_documents);
        add_handler(collection::handler_id(collection::route::update_documents), &manager_dispatcher_t::update_documents);
        add_handler(collection::handler_id(collection::route::size), &manager_dispatcher_t::size);
        add_handler(collection::handler_id(collection::route::close_cursor), &manager_dispatcher_t::close_cursor);
        add_handler(collection::handler_id(collection::route::create_index), &manager_dispatcher_t::create_index);
        add_handler(collection::handler_id(collection::route::drop_index), &manager_dispatcher_t::drop_index);
        add_handler(core::handler_id(core::route::sync), &manager_dispatcher_t::sync);
        trace(log_, "manager_dispatcher_t finish");
    }

    manager_dispatcher_t::~manager_dispatcher_t() {
        ZoneScoped;
        trace(log_, "delete manager_dispatcher_t");
    }

    auto manager_dispatcher_t::scheduler_impl() noexcept -> actor_zeta::scheduler_abstract_t* {
        return e_;
    }

    //NOTE: behold thread-safety!
    auto manager_dispatcher_t::enqueue_impl(actor_zeta::message_ptr msg, actor_zeta::execution_unit*) -> void {
        ZoneScoped;
        std::unique_lock<spin_lock> _(lock_);
        set_current_message(std::move(msg));
        execute(this, current_message());
    }

    void manager_dispatcher_t::create(components::session::session_id_t& session, std::string& name) {
        trace(log_, "manager_dispatcher_t::create session: {} , name: {} ", session.data(), name);
        auto target = spawn_actor<dispatcher_t>(
            [this, name](dispatcher_t* ptr) {
                dispatchers_.emplace_back(dispatcher_ptr(ptr));
            },
            resource(), manager_database_, manager_wal_, manager_disk_, log_, std::string(name));
    }

    void manager_dispatcher_t::load(components::session::session_id_t &session) {
        trace(log_, "manager_dispatcher_t::load session: {}", session.data());
        return actor_zeta::send(dispatcher(), address(), core::handler_id(core::route::load), session, current_message()->sender());
    }

    void manager_dispatcher_t::create_database(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::create_database session: {} , name: {} ", session.data(), statement->database_);
        return actor_zeta::send(dispatcher(), address(), database::handler_id(database::route::create_database), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::create_collection(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::create_collection session: {} , database name: {} , collection name: {} ", session.data(), statement->database_, statement->collection_);
        return actor_zeta::send(dispatcher(), address(), database::handler_id(database::route::create_collection), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::drop_collection(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::drop_collection session: {} , database name: {} , collection name: {} ", session.data(), statement->database_, statement->collection_);
        return actor_zeta::send(dispatcher(), address(), database::handler_id(database::route::drop_collection), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::insert_documents(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::insert_documents session: {}, database: {}, collection name: {} ", session.data(), statement->database_, statement->collection_);
        return actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::insert_documents), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::find(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::find session: {}, database: {}, collection name: {} ", session.data(), statement->database_, statement->collection_);
        return actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::find), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::find_one(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::find_one session: {}, database: {}, collection name: {} ", session.data(), statement->database_, statement->collection_);
        return actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::find_one), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::delete_documents(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::delete_documents session: {}, database: {}, collection name: {} ", session.data(), statement->database_, statement->collection_);
        return actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::delete_documents), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::update_documents(components::session::session_id_t& session, components::ql::ql_statement_t* statement) {
        trace(log_, "manager_dispatcher_t::update_documents session: {}, database: {}, collection name: {} ", session.data(), statement->database_, statement->collection_);
        return actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::update_documents), session, std::move(statement), current_message()->sender());
    }

    void manager_dispatcher_t::size(components::session::session_id_t& session, std::string& database_name, std::string& collection) {
        trace(log_, "manager_dispatcher_t::size session: {} , database: {}, collection name: {} ", session.data(), database_name, collection);
        actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::size), session, std::move(database_name), std::move(collection), current_message()->sender());
    }

    void manager_dispatcher_t::close_cursor(components::session::session_id_t&) {
    }

    void manager_dispatcher_t::create_index(components::session::session_id_t &session, components::ql::create_index_t index) {
        trace(log_, "manager_dispatcher_t::create_index session: {} , index: {}", session.data(), index.name());
        actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::create_index), session, std::move(index), current_message()->sender());
    }

    void manager_dispatcher_t::drop_index(components::session::session_id_t& session, components::ql::drop_index_t drop_index) {
        trace(log_, "manager_dispatcher_t::drop_index session: {} , index: {}", session.data(), drop_index.name());
        actor_zeta::send(dispatcher(), address(), collection::handler_id(collection::route::drop_index), session, std::move(drop_index), current_message()->sender());
    }

    auto manager_dispatcher_t::dispatcher() -> actor_zeta::address_t {
        return dispatchers_[0]->address();
    }

} // namespace services::dispatcher
